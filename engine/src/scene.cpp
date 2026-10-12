#include "scene.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace qnu {

namespace fs = std::filesystem;

namespace {

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

std::vector<std::string> split(const std::string& path) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    const size_t slash = path.find('/', start);
    out.push_back(path.substr(start, slash - start));
    if (slash == std::string::npos) return out;
    start = slash + 1;
  }
}

bool isInputKind(const std::string& s) { return s == "ch" || s == "st" || s == "fxret"; }
bool isBusKind(const std::string& s) { return s == "mix" || s == "grp" || s == "mtx"; }
// Parameters of an FX send master (fx/N/...), as opposed to its FX engine.
bool isFxSendParam(const std::string& rest) {
  return rest == "fader" || rest == "mute" || rest == "pafl" || rest == "presrc" || startsWith(rest, "mg/");
}

int cat(const char* id) { return sceneCategoryIndex(id); }

}  // namespace

std::string formatValue(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6g", v);
  return buf;
}

const std::vector<std::string>& sceneCategories() {
  static const std::vector<std::string> ids = {
      "inpatch", "preamp",  "fxpatch",   "chproc",   "fxparam", "innames",  "infaders", "inmutes",
      "outpatch", "mixproc", "lrproc",   "mixnames", "mixfaders", "mixmutes", "amm",     "custom",
      "soft",     "talk",    "mixsends", "fxsends",  "mutedca",  "masters"};
  return ids;
}

int sceneCategoryIndex(const std::string& id) {
  const auto& ids = sceneCategories();
  const auto it = std::find(ids.begin(), ids.end(), id);
  return it == ids.end() ? -1 : static_cast<int>(it - ids.begin());
}

int paramCategory(const std::string& path) {
  const auto seg = split(path);
  if (seg.size() < 2) return -1;
  const std::string& kind = seg[0];
  // Everything after "<kind>/<n>/".
  const std::string rest = seg.size() > 2 ? path.substr(seg[0].size() + seg[1].size() + 2) : "";
  if (isInputKind(kind)) {
    if (rest == "src") return cat("inpatch");
    if (rest == "gain" || rest == "polarity") return cat("preamp");
    if (rest == "fader" || rest == "pan") return cat("infaders");
    if (rest == "mute") return cat("inmutes");
    if (rest == "pafl") return -1;
    if (rest == "lr" || startsWith(rest, "mix/")) return cat("mixsends");
    if (startsWith(rest, "fx/")) return cat("fxsends");
    if (startsWith(rest, "dca/") || startsWith(rest, "mg/") || startsWith(rest, "grp/")) return cat("mutedca");
    if (startsWith(rest, "dout/")) return cat("outpatch");
    return cat("chproc");
  }
  if (kind == "lr" || isBusKind(kind) || (kind == "fx" && isFxSendParam(rest))) {
    const std::string r = kind == "lr" ? path.substr(3) : rest;
    if (r == "fader" || r == "balance") return cat("mixfaders");
    if (r == "mute") return cat("mixmutes");
    if (r == "pafl") return -1;
    if (startsWith(r, "mg/")) return cat("mutedca");
    if (r == "presrc" || r == "lr" || kind == "mtx") return cat("mixsends");
    return kind == "lr" ? cat("lrproc") : cat("mixproc");
  }
  if (kind == "fx") return rest == "patch" || rest == "src" || rest == "mix" ? cat("fxpatch") : cat("fxparam");
  if (kind == "dca" || kind == "mg") return cat("masters");
  if (kind == "out" || kind == "usb" || kind == "dout") return cat("outpatch");
  if (kind == "link" || kind == "duck") return cat("chproc");
  if (kind == "amm") return cat("amm");
  if (kind == "custom") return seg[1] == "only" ? -1 : cat("custom");
  if (kind == "soft") return cat("soft");
  if (kind == "talk") return path == "talk/latch" ? -1 : cat("talk");
  return -1;  // pafl/, gen/, mon/, filter/, safe/
}

int labelCategory(const std::string& key) {
  const std::string kind = key.substr(0, key.find('/'));
  if (isInputKind(kind)) return cat("innames");
  if (kind == "dca" || kind == "mg") return cat("masters");
  return cat("mixnames");
}

std::string safeOwner(const std::string& path) {
  const auto seg = split(path);
  if (seg[0] == "lr") return "lr";
  if (seg.size() < 2) return "";
  if (isInputKind(seg[0]) || seg[0] == "mix") return seg[0] + "/" + seg[1];
  if (seg[0] == "fx" && (seg.size() == 2 || isFxSendParam(path.substr(seg[0].size() + seg[1].size() + 2))))
    return seg[0] + "/" + seg[1];
  return "";
}

std::string serializeScene(const ParamRegistry& reg, const Labels& labels, const SceneInfo& info) {
  std::string out = "# QNU16 scene v2\nn " + info.name + "\n";
  for (const auto& f : info.filter) out += "f " + f + "\n";
  for (const auto& p : reg.all())
    if (paramCategory(p->info().path) >= 0) out += "p " + p->info().path + " " + formatValue(p->get()) + "\n";
  for (const auto& [key, text] : labels.all()) out += "l " + key + " " + text + "\n";
  return out;
}

namespace {

// Calls fn(kind, key, value) for each "<kind> <key> <value>" entry ("n <name>" has key only).
template <typename Fn>
void forEachEntry(const std::string& text, Fn fn) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() < 3 || line[0] == '#' || line[1] != ' ') continue;
    const std::string body = line.substr(2);
    if (line[0] == 'n' || line[0] == 'f') {
      fn(line[0], body, std::string());
      continue;
    }
    const auto sp = body.find(' ');
    if (sp == std::string::npos) continue;
    fn(line[0], body.substr(0, sp), body.substr(sp + 1));
  }
}

}  // namespace

SceneInfo parseSceneInfo(const std::string& text) {
  SceneInfo info;
  forEachEntry(text, [&](char kind, const std::string& key, const std::string&) {
    if (kind == 'n') info.name = key;
    if (kind == 'f' && sceneCategoryIndex(key) >= 0) info.filter.insert(key);
  });
  return info;
}

int recallScene(const std::string& text, ParamRegistry& reg, Labels& labels) {
  std::map<std::string, float> values;
  std::map<std::string, std::string> names;
  std::vector<bool> blocked(sceneCategories().size(), false);
  forEachEntry(text, [&](char kind, const std::string& key, const std::string& value) {
    if (kind == 'p') {
      try {
        values[key] = std::stof(value);
      } catch (...) {
      }
    } else if (kind == 'l') {
      names[key] = value;
    } else if (kind == 'f') {
      const int c = sceneCategoryIndex(key);
      if (c >= 0) blocked[c] = true;
    }
  });
  for (size_t c = 0; c < blocked.size(); ++c) {
    const Param* g = reg.find("filter/" + sceneCategories()[c]);
    if (g && g->on()) blocked[c] = true;
  }
  auto isSafe = [&](const std::string& key) {
    const std::string owner = safeOwner(key);
    const Param* s = owner.empty() ? nullptr : reg.find("safe/" + owner);
    return s && s->on();
  };

  int applied = 0;
  for (const auto& p : reg.all()) {
    const std::string& path = p->info().path;
    const int c = paramCategory(path);
    if (c < 0 || blocked[c] || isSafe(path)) continue;
    const auto it = values.find(path);
    p->set(it == values.end() ? p->info().def : it->second);
    if (it != values.end()) ++applied;
  }
  for (const auto& key : labels.keys()) {
    const int c = labelCategory(key);
    if (blocked[c] || isSafe(key)) continue;
    const auto it = names.find(key);
    labels.set(key, it == names.end() ? std::string() : it->second);
    if (it != names.end()) ++applied;
  }
  return applied;
}

void resetMixSettings(ParamRegistry& reg, Labels& labels) {
  for (const auto& p : reg.all())
    if (paramCategory(p->info().path) >= 0) p->set(p->info().def);
  labels.clear();
}

std::string SceneStore::pathFor(int number) const {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "/%03d.scene", number);
  return dir_ + buf;
}

bool SceneStore::store(int number, const std::string& text, std::string* err) const {
  if (!validNumber(number)) {
    *err = "scene number must be 1-100";
    return false;
  }
  std::error_code ec;
  fs::create_directories(dir_, ec);
  // Write to a temp file then rename, so a power cut never leaves a half-written scene.
  const std::string tmp = pathFor(number) + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f || !(f << text) || !f.flush()) {
      *err = "cannot write " + tmp;
      return false;
    }
  }
  fs::rename(tmp, pathFor(number), ec);
  if (ec) {
    *err = ec.message();
    return false;
  }
  return true;
}

bool SceneStore::load(int number, std::string* text, std::string* err) const {
  if (!validNumber(number)) {
    *err = "scene number must be 1-100";
    return false;
  }
  std::ifstream f(pathFor(number), std::ios::binary);
  if (!f) {
    *err = "scene " + std::to_string(number) + " is empty";
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  *text = ss.str();
  return true;
}

bool SceneStore::clear(int number, std::string* err) const {
  if (!validNumber(number)) {
    *err = "scene number must be 1-100";
    return false;
  }
  std::error_code ec;
  fs::remove(pathFor(number), ec);
  return true;
}

std::vector<SceneInfo> SceneStore::list() const {
  std::vector<SceneInfo> out;
  for (int n = 1; n <= kSceneCount; ++n) {
    std::ifstream f(pathFor(n), std::ios::binary);
    if (!f) continue;
    // The name and filter lines come first; stop at the first parameter.
    std::string text, line;
    while (std::getline(f, line) && line.rfind("p ", 0) != 0) text += line + "\n";
    SceneInfo info = parseSceneInfo(text);
    info.number = n;
    out.push_back(std::move(info));
  }
  return out;
}

// ---- User libraries ----

bool LibraryStore::validType(const std::string& t) {
  return t == "peq" || t == "geq" || t == "gate" || t == "comp" || t == "channel" || t == "fx";
}

bool LibraryStore::validValues(const std::string& v) {
  return std::all_of(v.begin(), v.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '=' || c == ',' ||
           c == '.' || c == '-' || c == 'e' || c == '+';
  });
}

std::string LibraryStore::pathFor(int number) const {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "/%03d.lib", number);
  return dir_ + buf;
}

int LibraryStore::store(int number, const LibraryEntry& e, std::string* err) const {
  if (!validType(e.type) || !validValues(e.values) || e.name.empty()) {
    *err = "invalid library";
    return 0;
  }
  if (number == 0) {
    for (int n = 1; n <= kLibraryCount && !number; ++n)
      if (!fs::exists(pathFor(n))) number = n;
    if (!number) {
      *err = "the library is full (128)";
      return 0;
    }
  }
  if (number < 1 || number > kLibraryCount) {
    *err = "library number must be 1-128";
    return 0;
  }
  std::error_code ec;
  fs::create_directories(dir_, ec);
  const std::string tmp = pathFor(number) + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f || !(f << "t " << e.type << "\nn " << e.name << "\nv " << e.values << "\n") || !f.flush()) {
      *err = "cannot write " + tmp;
      return 0;
    }
  }
  fs::rename(tmp, pathFor(number), ec);
  if (ec) {
    *err = ec.message();
    return 0;
  }
  return number;
}

bool LibraryStore::load(int number, LibraryEntry* e) const {
  if (number < 1 || number > kLibraryCount) return false;
  std::ifstream f(pathFor(number), std::ios::binary);
  if (!f) return false;
  e->number = number;
  std::string line;
  while (std::getline(f, line)) {
    if (line.size() < 2 || line[1] != ' ') continue;
    if (line[0] == 't') e->type = line.substr(2);
    if (line[0] == 'n') e->name = line.substr(2);
    if (line[0] == 'v') e->values = line.substr(2);
  }
  return validType(e->type);
}

bool LibraryStore::remove(int number) const {
  std::error_code ec;
  return number >= 1 && number <= kLibraryCount && fs::remove(pathFor(number), ec);
}

std::vector<LibraryEntry> LibraryStore::list() const {
  std::vector<LibraryEntry> out;
  for (int n = 1; n <= kLibraryCount; ++n) {
    LibraryEntry e;
    if (!load(n, &e)) continue;
    e.values.clear();
    out.push_back(std::move(e));
  }
  return out;
}

}  // namespace qnu
