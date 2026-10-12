#include "controller.h"

#include <cstdio>
#include <sstream>

namespace qnu {

namespace {

std::string jsonString(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

void appendDb(std::string* out, float db) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.1f", std::max(db, -90.0f));
  *out += buf;
}

}  // namespace

Controller::Controller(Mixer& mixer, ParamRegistry& reg, Labels& labels, SceneStore& scenes,
                       std::string backendName)
    : mixer_(mixer), reg_(reg), labels_(labels), scenes_(scenes),
      backendName_(std::move(backendName)) {}

Controller::Reply Controller::handle(const std::string& message) {
  Reply reply;
  std::istringstream in(message);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) handleLine(line, &reply);
  }
  return reply;
}

void Controller::handleLine(const std::string& line, Reply* reply) {
  std::istringstream in(line);
  std::string cmd;
  in >> cmd;

  if (cmd == "set") {
    std::string path, value;
    in >> path >> value;
    Param* p = reg_.find(path);
    if (!p) {
      reply->toSender.push_back("e unknown parameter " + path);
      return;
    }
    float v;
    try {
      v = std::stof(value);
    } catch (...) {
      reply->toSender.push_back("e bad value for " + path);
      return;
    }
    reply->broadcast.push_back("p " + path + " " + formatValue(p->set(v)));
  } else if (cmd == "label") {
    std::string key, text;
    in >> key;
    std::getline(in >> std::ws, text);
    if (!labels_.set(key, text)) {
      reply->toSender.push_back("e unknown label " + key);
      return;
    }
    reply->broadcast.push_back("l " + key + " " + labels_.get(key));
  } else if (cmd == "state") {
    reply->toSender.push_back(stateMessage());
  } else if (cmd == "info") {
    reply->toSender.push_back(infoMessage());
    reply->toSender.push_back(metaMessage());
  } else if (cmd == "scene") {
    std::string sub, name;
    in >> sub;
    std::getline(in >> std::ws, name);
    std::string err;
    if (sub == "list") {
      reply->toSender.push_back(sceneListMessage());
    } else if (sub == "save") {
      if (!scenes_.save(name, serializeScene(reg_, labels_), &err)) {
        reply->toSender.push_back("e " + err);
        return;
      }
      currentScene_ = name;
      reply->broadcast.push_back(sceneListMessage());
    } else if (sub == "load") {
      std::string text;
      if (!scenes_.load(name, &text, &err)) {
        reply->toSender.push_back("e " + err);
        return;
      }
      applyScene(text, reg_, labels_);
      currentScene_ = name;
      reply->broadcast.push_back(stateMessage());
      reply->broadcast.push_back(sceneListMessage());
    } else {
      reply->toSender.push_back("e unknown scene command");
    }
  } else if (cmd == "rec") {
    std::string sub;
    in >> sub;
    if (!recorder_) {
      reply->toSender.push_back("e recording is not available");
      return;
    }
    std::string err;
    if (sub == "multi" || sub == "stereo") {
      if (!recorder_->start(sub == "multi" ? Recorder::kMultitrack : Recorder::kStereo,
                            mixer_.sampleRate(), &err)) {
        reply->toSender.push_back("e " + err);
        return;
      }
    } else if (sub == "stop") {
      recorder_->stop();
    } else {
      reply->toSender.push_back("e unknown rec command");
      return;
    }
    reply->broadcast.push_back(recorderMessage());
  } else {
    reply->toSender.push_back("e unknown command " + cmd);
  }
}

std::string Controller::infoMessage() const {
  std::ostringstream o;
  o << "i {\"name\":\"QNU16\",\"version\":\"0.2.0\",\"sampleRate\":" << mixer_.sampleRate()
    << ",\"backend\":" << jsonString(backendName_) << ",\"physInputs\":" << kPhysInputs
    << ",\"physOutputs\":" << kPhysOutputs << ",\"dcas\":" << kDcas
    << ",\"muteGroups\":" << kMuteGroups << ",\"geqBands\":" << kGeqBands << ",\"inputs\":[";
  const char* inputKinds[] = {"mono", "stereo", "fxret"};
  for (size_t i = 0; i < inputDefs().size(); ++i) {
    const InputDef& d = inputDefs()[i];
    o << (i ? "," : "") << "{\"id\":" << jsonString(d.id) << ",\"name\":" << jsonString(d.name)
      << ",\"kind\":\"" << inputKinds[static_cast<int>(d.kind)] << "\",\"stereo\":" << (d.stereo ? "true" : "false")
      << "}";
  }
  o << "],\"buses\":[";
  const char* busKinds[] = {"lr", "mix", "grp", "mtx", "fx"};
  for (size_t i = 0; i < busDefs().size(); ++i) {
    const BusDef& d = busDefs()[i];
    o << (i ? "," : "") << "{\"id\":" << jsonString(d.id) << ",\"name\":" << jsonString(d.name)
      << ",\"kind\":\"" << busKinds[static_cast<int>(d.kind)] << "\",\"stereo\":" << (d.stereo ? "true" : "false")
      << "}";
  }
  o << "],\"outputSources\":[";
  for (size_t i = 0; i < outputSources().size(); ++i) o << (i ? "," : "") << jsonString(outputSources()[i].name);
  o << "],\"fxTypes\":[";
  for (int t = 0; t < FxEngine::kNumTypes; ++t) o << (t ? "," : "") << jsonString(FxEngine::typeName(t));
  o << "]}";
  return o.str();
}

std::string Controller::metaMessage() const {
  // "k <path> <min> <max> <default> <kind c|t|h> [unit]" for every parameter.
  std::string out = "k";
  out.reserve(reg_.all().size() * 48);
  for (const auto& p : reg_.all()) {
    const ParamInfo& info = p->info();
    const char kind = info.kind == ParamKind::Toggle ? 't' : info.kind == ParamKind::Choice ? 'h' : 'c';
    out += "\n" + info.path + " " + formatValue(info.min) + " " + formatValue(info.max) + " " +
           formatValue(info.def) + " " + kind;
    if (!info.unit.empty()) out += " " + info.unit;
  }
  return out;
}

std::string Controller::stateMessage() const {
  std::string out;
  out.reserve(reg_.all().size() * 28);
  for (const auto& p : reg_.all()) {
    if (!out.empty()) out += '\n';
    out += "p " + p->info().path + " " + formatValue(p->get());
  }
  for (const auto& [key, text] : labels_.all()) out += "\nl " + key + " " + text;
  return out;
}

std::string Controller::meterMessage() {
  const MeterSnapshot s = mixer_.takeMeters();
  std::string out = "m ";
  out.reserve(1024);
  for (const auto& in : s.inputs) {
    appendDb(&out, in.inDb); out += ',';
    appendDb(&out, in.outDb); out += ',';
    appendDb(&out, in.grDb); out += ',';
    out += in.gateOpen ? "1," : "0,";
  }
  for (const auto& bus : s.buses) {
    appendDb(&out, bus.lDb); out += ',';
    appendDb(&out, bus.rDb); out += ',';
    appendDb(&out, bus.grDb); out += ',';
  }
  appendDb(&out, s.paflLDb); out += ',';
  appendDb(&out, s.paflRDb);
  return out;
}

std::string Controller::rtaMessage() {
  const auto bands = mixer_.takeRta();
  std::string out = "r ";
  for (size_t k = 0; k < bands.size(); ++k) {
    if (k) out += ',';
    appendDb(&out, bands[k]);
  }
  return out;
}

std::string Controller::recorderMessage() const {
  if (!recorder_) return "d none";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "d %s %s %.1f %u ", recorder_->recording() ? "rec" : "stop",
                recorder_->mode() == Recorder::kMultitrack ? "multi" : "stereo", recorder_->seconds(),
                recorder_->dropouts());
  return buf + recorder_->file();
}

std::string Controller::sceneListMessage() const {
  std::string out = "s " + currentScene_;
  for (const auto& name : scenes_.list()) out += "\n" + name;
  return out;
}

}  // namespace qnu
