#include "controller.h"

#include <cstdio>
#include <sstream>

#include "recorder.h"

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

template <typename List, typename Fn>
std::string jsonArray(const List& list, Fn name) {
  std::string out = "[";
  bool first = true;
  for (const auto& item : list) {
    if (!first) out += ",";
    out += jsonString(name(item));
    first = false;
  }
  return out + "]";
}

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// For "ch/N/<rest>" returns N (1-based) and sets *rest; 0 otherwise.
int channelOf(const std::string& path, std::string* rest) {
  if (path.rfind("ch/", 0) != 0) return 0;
  const size_t slash = path.find('/', 3);
  if (slash == std::string::npos) return 0;
  const int n = std::atoi(path.c_str() + 3);
  if (n < 1 || n > kMonoInputs) return 0;
  *rest = path.substr(slash + 1);
  return n;
}

}  // namespace

Controller::Controller(Mixer& mixer, ParamRegistry& reg, Labels& labels, SceneStore& scenes,
                       std::string backendName)
    : mixer_(mixer), reg_(reg), labels_(labels), scenes_(scenes),
      backendName_(std::move(backendName)) {
  ammDb_.fill(-120.0f);
}

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

void Controller::put(Param* p, float v, Reply* reply) {
  reply->broadcast.push_back("p " + p->info().path + " " + formatValue(p->set(v)));
}

// What a link between CH n and n+1 carries for a parameter, given the link options: 0 nothing,
// 1 the same value, -1 the mirrored value (pan becomes width).
static int linkMode(const std::string& rest, const ParamRegistry& reg, int pair) {
  auto opt = [&](const char* name) {
    const Param* p = reg.find("link/" + std::to_string(pair) + "/" + name);
    return p && p->on();
  };
  if (rest == "polarity") return 0;
  if (rest == "gain" || rest == "src") return opt("preamp") ? 1 : 0;
  if (rest == "fader" || rest == "mute") return opt("fader") ? 1 : 0;
  if (rest == "pan" || (rest.rfind("mix/", 0) == 0 && endsWith(rest, "/pan"))) return opt("pan") ? -1 : 0;
  return 1;  // processing, sends, assignments, PAFL, direct out trim
}

void Controller::mirrorLinked(const std::string& path, float v, Reply* reply) {
  std::string rest;
  const int n = channelOf(path, &rest);
  if (!n) return;
  const int pair = (n + 1) / 2;
  const Param* on = reg_.find("link/" + std::to_string(pair) + "/on");
  if (!on || !on->on()) return;
  const int mode = linkMode(rest, reg_, pair);
  if (!mode) return;
  const int partner = n % 2 ? n + 1 : n - 1;
  Param* p = reg_.find("ch/" + std::to_string(partner) + "/" + rest);
  if (p) put(p, mode < 0 ? -v : v, reply);
}

// Linking copies the odd channel's settings to the even one.
void Controller::copyToPartner(int pair, Reply* reply) {
  const std::string odd = "ch/" + std::to_string(2 * pair - 1) + "/";
  const std::string even = "ch/" + std::to_string(2 * pair) + "/";
  for (const auto& p : reg_.all()) {
    const std::string& path = p->info().path;
    if (path.rfind(odd, 0) != 0) continue;
    const std::string rest = path.substr(odd.size());
    const int mode = linkMode(rest, reg_, pair);
    Param* q = reg_.find(even + rest);
    if (mode && q) put(q, mode < 0 ? -p->get() : p->get(), reply);
  }
}

void Controller::setParam(Param* p, float v, Reply* reply) {
  const std::string& path = p->info().path;
  put(p, v, reply);
  mirrorLinked(path, p->get(), reply);
  if (path.rfind("link/", 0) == 0 && endsWith(path, "/on") && p->on()) {
    copyToPartner(std::atoi(path.c_str() + 5), reply);
  }
  // PAFL auto-cancel: unless additive, selecting a PAFL clears the others (a linked partner
  // stays selected).
  if (endsWith(path, "/pafl") && p->on()) {
    const Param* additive = reg_.find("pafl/additive");
    if (!additive || !additive->on()) {
      std::string rest, partner;
      const int n = channelOf(path, &rest);
      if (n) {
        const Param* on = reg_.find("link/" + std::to_string((n + 1) / 2) + "/on");
        if (on && on->on()) partner = "ch/" + std::to_string(n % 2 ? n + 1 : n - 1) + "/pafl";
      }
      for (const auto& q : reg_.all()) {
        const std::string& other = q->info().path;
        if (other != path && other != partner && endsWith(other, "/pafl") && q->on()) put(q.get(), 0, reply);
      }
    }
  }
  // Switching a channel into the AMM opens its fader to 0 dB; the AMM then controls its gain.
  if (path.rfind("amm/", 0) == 0 && endsWith(path, "/on") && path != "amm/on" && p->on()) {
    Param* fader = reg_.find("ch/" + std::to_string(std::atoi(path.c_str() + 4)) + "/fader");
    if (fader) put(fader, 0, reply);
  }
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
    setParam(p, v, reply);
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
    handleScene(in, reply);
  } else if (cmd == "lib") {
    handleLibrary(in, reply);
  } else if (cmd == "rec" || cmd == "play" || cmd == "drive") {
    std::string sub;
    in >> sub;
    if (!recorder_ || !player_) {
      reply->toSender.push_back("e the Qu-Drive is not available");
      return;
    }
    std::string err;
    if (cmd == "drive") {
      reply->toSender.push_back(driveListMessage());
      return;
    }
    if (cmd == "rec" && (sub == "multi" || sub == "stereo")) {
      player_->stop();
      if (!recorder_->start(sub == "multi" ? Recorder::kMultitrack : Recorder::kStereo,
                            mixer_.sampleRate(), &err)) {
        reply->toSender.push_back("e " + err);
        return;
      }
    } else if (cmd == "play" && sub != "stop" && !sub.empty()) {
      recorder_->stop();
      if (!player_->start(sub, mixer_.sampleRate(), &err)) {
        reply->toSender.push_back("e " + err);
        return;
      }
    } else if (sub == "stop") {
      const bool wasRecording = recorder_->recording();
      recorder_->stop();
      player_->stop();
      if (wasRecording) reply->broadcast.push_back(driveListMessage());
    } else {
      reply->toSender.push_back("e unknown " + cmd + " command");
      return;
    }
    reply->broadcast.push_back(driveMessage());
  } else {
    reply->toSender.push_back("e unknown command " + cmd);
  }
}

void Controller::handleScene(std::istringstream& in, Reply* reply) {
  std::string sub;
  int number = 0;
  in >> sub;
  if (sub != "list" && sub != "reset") {
    in >> number;
    if (!SceneStore::validNumber(number)) {
      reply->toSender.push_back("e scene number must be 1-100");
      return;
    }
  }
  std::string rest, err, text;
  std::getline(in >> std::ws, rest);
  auto existing = [&](SceneInfo* info) {
    std::string t, e;
    if (!scenes_.load(number, &t, &e)) return false;
    *info = parseSceneInfo(t);
    return true;
  };
  auto save = [&](const std::string& body) {
    if (!scenes_.store(number, body, &err)) {
      reply->toSender.push_back("e " + err);
      return false;
    }
    reply->broadcast.push_back(sceneListMessage());
    return true;
  };

  if (sub == "list") {
    reply->toSender.push_back(sceneListMessage());
  } else if (sub == "store" || sub == "name") {
    // store: current settings into the scene (keeping its name and filter unless renamed).
    // name: renames a stored scene; naming an empty scene stores the current settings in it.
    SceneInfo info;
    const bool had = existing(&info);
    if (!rest.empty()) info.name = Labels::sanitize(rest, kSceneNameChars);
    if (sub == "name" && had) {
      if (!scenes_.load(number, &text, &err)) return;
      // Replace the name line, keep the contents.
      const size_t nl = text.find("\nn ");
      if (nl != std::string::npos) text.replace(nl + 3, text.find('\n', nl + 1) - nl - 3, info.name);
      save(text);
      return;
    }
    const int previous = currentScene_;
    if (sub == "store") currentScene_ = number;  // before the list goes out
    if (!save(serializeScene(reg_, labels_, info))) currentScene_ = previous;
  } else if (sub == "recall") {
    if (!scenes_.load(number, &text, &err)) {
      reply->toSender.push_back("e " + err);
      return;
    }
    recallScene(text, reg_, labels_);
    currentScene_ = number;
    reply->broadcast.push_back(stateMessage());
    reply->broadcast.push_back(sceneListMessage());
  } else if (sub == "clear") {
    scenes_.clear(number, &err);
    if (currentScene_ == number) currentScene_ = 0;
    reply->broadcast.push_back(sceneListMessage());
  } else if (sub == "copy") {
    // copy <from> <to>: name, contents and filter.
    const int to = std::atoi(rest.c_str());
    if (!SceneStore::validNumber(to) || !scenes_.load(number, &text, &err) || !scenes_.store(to, text, &err)) {
      reply->toSender.push_back("e cannot copy scene");
      return;
    }
    reply->broadcast.push_back(sceneListMessage());
  } else if (sub == "filter") {
    // filter <n> <category> <0|1>
    std::istringstream r(rest);
    std::string category;
    int block = 0;
    r >> category >> block;
    if (sceneCategoryIndex(category) < 0 || !scenes_.load(number, &text, &err)) {
      reply->toSender.push_back("e cannot set the filter of scene " + std::to_string(number));
      return;
    }
    const std::string line = "\nf " + category + "\n";
    const size_t at = text.find(line);
    if (block && at == std::string::npos) {
      const size_t nameEnd = text.find('\n', text.find("\nn ") + 1);
      text.insert(nameEnd, line.substr(0, line.size() - 1));
    } else if (!block && at != std::string::npos) {
      text.erase(at, line.size() - 1);
    }
    save(text);
  } else if (sub == "reset") {
    resetMixSettings(reg_, labels_);
    reply->broadcast.push_back(stateMessage());
  } else {
    reply->toSender.push_back("e unknown scene command");
  }
}

// lib list | lib get <n> | lib store <n|0> <type> <name>|<values> | lib name <n> <name> |
// lib delete <n>
void Controller::handleLibrary(std::istringstream& in, Reply* reply) {
  if (!libraries_) {
    reply->toSender.push_back("e libraries are not available");
    return;
  }
  std::string sub, err;
  int number = 0;
  in >> sub;
  if (sub == "list") {
    reply->toSender.push_back(libraryListMessage());
    return;
  }
  in >> number;
  LibraryEntry e;
  if (sub == "get") {
    if (!libraries_->load(number, &e)) {
      reply->toSender.push_back("e no library " + std::to_string(number));
      return;
    }
    reply->toSender.push_back("B " + std::to_string(number) + " " + e.type + " " + e.name + "|" + e.values);
    return;
  }
  if (sub == "store") {
    std::string rest;
    in >> e.type;
    std::getline(in >> std::ws, rest);
    const size_t bar = rest.find('|');
    if (bar == std::string::npos) {
      reply->toSender.push_back("e bad library");
      return;
    }
    e.name = Labels::sanitize(rest.substr(0, bar), kSceneNameChars);
    e.values = rest.substr(bar + 1);
    if (!libraries_->store(number, e, &err)) {
      reply->toSender.push_back("e " + err);
      return;
    }
  } else if (sub == "name") {
    std::string name;
    std::getline(in >> std::ws, name);
    if (!libraries_->load(number, &e)) {
      reply->toSender.push_back("e no library " + std::to_string(number));
      return;
    }
    e.name = Labels::sanitize(name, kSceneNameChars);
    if (!libraries_->store(number, e, &err)) {
      reply->toSender.push_back("e " + err);
      return;
    }
  } else if (sub == "delete") {
    libraries_->remove(number);
  } else {
    reply->toSender.push_back("e unknown lib command");
    return;
  }
  reply->broadcast.push_back(libraryListMessage());
}

std::string Controller::libraryListMessage() const {
  std::string out = "b";
  if (libraries_)
    for (const auto& e : libraries_->list()) out += "\n" + std::to_string(e.number) + " " + e.type + " " + e.name;
  return out;
}

std::string Controller::infoMessage() const {
  std::ostringstream o;
  o << "i {\"name\":\"QNU16\",\"version\":\"0.3.0\",\"sampleRate\":" << mixer_.sampleRate()
    << ",\"backend\":" << jsonString(backendName_) << ",\"physInputs\":" << kPhysInputs
    << ",\"physOutputs\":" << kPhysOutputs << ",\"dcas\":" << kDcas
    << ",\"muteGroups\":" << kMuteGroups << ",\"geqBands\":" << kGeqBands
    << ",\"linkPairs\":" << kLinkPairs << ",\"softKeys\":" << kSoftKeys
    << ",\"customStrips\":" << kCustomStrips << ",\"scenes\":" << kSceneCount
    << ",\"nameChars\":" << Labels::kMaxNameChars << ",\"sceneNameChars\":" << kSceneNameChars
    << ",\"inputs\":[";
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
  o << "],\"outputSources\":" << jsonArray(outputSources(), [](const OutputSource& s) { return s.name; });
  o << ",\"usbSources\":" << jsonArray(usbSources(), [](const UsbSource& s) { return s.name; });
  o << ",\"fxTargets\":" << jsonArray(fxTargetNames(), [](const std::string& s) { return s; });
  std::vector<int> types(FxEngine::kNumTypes), models(FxEngine::kNumModels);
  for (int t = 0; t < FxEngine::kNumTypes; ++t) types[t] = t;
  for (int m = 0; m < FxEngine::kNumModels; ++m) models[m] = m;
  o << ",\"fxTypes\":" << jsonArray(types, [](int t) { return std::string(FxEngine::typeName(t)); });
  o << ",\"reverbModels\":" << jsonArray(models, [](int m) { return std::string(FxEngine::reverbModelName(m)); });
  o << ",\"sceneCategories\":" << jsonArray(sceneCategories(), [](const std::string& s) { return s; });
  o << "}";
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
  ammDb_ = s.ammDb;
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

std::string Controller::ammMessage() const {
  std::string out = "a ";
  for (size_t c = 0; c < ammDb_.size(); ++c) {
    if (c) out += ',';
    appendDb(&out, ammDb_[c]);
  }
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

std::string Controller::driveMessage() const {
  if (!recorder_ || !player_) return "d none";
  char buf[96];
  const bool rec = recorder_->recording(), play = player_->playing();
  const bool multi = rec ? recorder_->mode() == Recorder::kMultitrack : player_->multitrack();
  std::snprintf(buf, sizeof(buf), "d %s %s %.1f %.1f %u ", rec ? "rec" : play ? "play" : "stop",
                multi ? "multi" : "stereo", rec ? recorder_->seconds() : player_->seconds(),
                play ? player_->length() : 0.0, recorder_->dropouts());
  return buf + (rec ? recorder_->file() : player_->file());
}

std::string Controller::driveListMessage() const {
  std::string out = "q";
  if (recorder_)
    for (const auto& e : listDrive(recorder_->dir())) out += "\n" + e.name;
  return out;
}

std::string Controller::sceneListMessage() const {
  // "s <current>" then "<n> <blocked categories, comma-separated, or -> <name>" per stored scene.
  std::string out = "s " + std::to_string(currentScene_);
  for (const auto& sc : scenes_.list()) {
    std::string filter;
    for (const auto& f : sc.filter) filter += (filter.empty() ? "" : ",") + f;
    out += "\n" + std::to_string(sc.number) + " " + (filter.empty() ? "-" : filter) + " " + sc.name;
  }
  return out;
}

}  // namespace qnu
