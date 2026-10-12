#include "scene.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace qnu {

namespace fs = std::filesystem;

std::string formatValue(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6g", v);
  return buf;
}

std::string serializeScene(const ParamRegistry& reg, const Labels& labels) {
  std::string out = "# QNU16 scene v1\n";
  for (const auto& p : reg.all()) out += "p " + p->info().path + " " + formatValue(p->get()) + "\n";
  for (const auto& [key, text] : labels.all()) out += "l " + key + " " + text + "\n";
  return out;
}

int applyScene(const std::string& text, ParamRegistry& reg, Labels& labels) {
  reg.resetDefaults();
  labels.clear();
  int applied = 0;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() < 3 || line[0] == '#' || line[1] != ' ') continue;
    const auto sp = line.find(' ', 2);
    if (sp == std::string::npos) continue;
    const std::string key = line.substr(2, sp - 2);
    const std::string value = line.substr(sp + 1);
    if (line[0] == 'p') {
      Param* p = reg.find(key);
      if (!p) continue;
      try {
        p->set(std::stof(value));
        ++applied;
      } catch (...) {
      }
    } else if (line[0] == 'l') {
      if (labels.set(key, value)) ++applied;
    }
  }
  return applied;
}

bool isValidSceneName(const std::string& name) {
  if (name.empty() || name.size() > 48) return false;
  if (name.front() == ' ' || name.back() == ' ') return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == ' ';
  });
}

bool SceneStore::save(const std::string& name, const std::string& text, std::string* err) const {
  if (!isValidSceneName(name)) {
    *err = "invalid scene name";
    return false;
  }
  std::error_code ec;
  fs::create_directories(dir_, ec);
  // Write to a temp file then rename, so a power cut never leaves a half-written scene.
  const std::string tmp = pathFor(name) + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f || !(f << text) || !f.flush()) {
      *err = "cannot write " + tmp;
      return false;
    }
  }
  fs::rename(tmp, pathFor(name), ec);
  if (ec) {
    *err = ec.message();
    return false;
  }
  return true;
}

bool SceneStore::load(const std::string& name, std::string* text, std::string* err) const {
  if (!isValidSceneName(name)) {
    *err = "invalid scene name";
    return false;
  }
  std::ifstream f(pathFor(name), std::ios::binary);
  if (!f) {
    *err = "no such scene";
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  *text = ss.str();
  return true;
}

std::vector<std::string> SceneStore::list() const {
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir_, ec)) {
    const fs::path& path = entry.path();
    if (path.extension() == ".scene" && isValidSceneName(path.stem().string()))
      names.push_back(path.stem().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace qnu
