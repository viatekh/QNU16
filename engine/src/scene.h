#pragma once

#include <string>
#include <vector>

#include "params.h"

namespace qnu {

// Compact float formatting shared by scenes and the control protocol.
std::string formatValue(float v);

// Scene text format (one entry per line, '#' comments):
//   p <param-path> <value>
//   l <label-key> <text...>
std::string serializeScene(const ParamRegistry& reg, const Labels& labels);

// Resets everything to defaults, then applies the scene. Unknown entries are skipped.
// Returns the number of entries applied.
int applyScene(const std::string& text, ParamRegistry& reg, Labels& labels);

// Scene names: 1-48 chars of [A-Za-z0-9 _-]. Keeps file access inside the scene directory.
bool isValidSceneName(const std::string& name);

class SceneStore {
 public:
  explicit SceneStore(std::string dir) : dir_(std::move(dir)) {}

  bool save(const std::string& name, const std::string& text, std::string* err) const;
  bool load(const std::string& name, std::string* text, std::string* err) const;
  std::vector<std::string> list() const;
  const std::string& dir() const { return dir_; }

 private:
  std::string pathFor(const std::string& name) const { return dir_ + "/" + name + ".scene"; }
  std::string dir_;
};

}  // namespace qnu
