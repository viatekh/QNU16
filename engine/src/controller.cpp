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
  } else {
    reply->toSender.push_back("e unknown command " + cmd);
  }
}

std::string Controller::infoMessage() const {
  std::ostringstream o;
  o << "i {\"name\":\"QNU16\",\"version\":\"0.1.0\",\"channels\":" << kChannels
    << ",\"auxes\":" << kAuxes << ",\"dcas\":" << kDcas << ",\"eqBands\":" << kEqBands
    << ",\"sampleRate\":" << mixer_.sampleRate() << ",\"backend\":" << jsonString(backendName_)
    << ",\"outputs\":[";
  for (int i = 0; i < kOutputs; ++i) o << (i ? "," : "") << jsonString(outputName(i));
  o << "]}";
  return o.str();
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
  out.reserve(512);
  for (int c = 0; c < kChannels; ++c) {
    appendDb(&out, s.chInDb[c]); out += ',';
    appendDb(&out, s.chOutDb[c]); out += ',';
    appendDb(&out, s.chGrDb[c]); out += ',';
    out += s.chGateOpen[c] ? "1," : "0,";
  }
  for (int o = 0; o < kOutputs; ++o) {
    appendDb(&out, s.outDb[o]);
    if (o + 1 < kOutputs) out += ',';
  }
  return out;
}

std::string Controller::sceneListMessage() const {
  std::string out = "s " + currentScene_;
  for (const auto& name : scenes_.list()) out += "\n" + name;
  return out;
}

}  // namespace qnu
