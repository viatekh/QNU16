#pragma once

#include <set>
#include <string>
#include <vector>

#include "params.h"

namespace qnu {

// Compact float formatting shared by scenes and the control protocol.
std::string formatValue(float v);

// Scene recall filter categories, as on the Qu's Scene Recall Filter page. The global filter
// is the parameters filter/<id> (1 = blocked); each scene also stores its own filter.
const std::vector<std::string>& sceneCategories();  // ids, in the Qu's page order
int sceneCategoryIndex(const std::string& id);      // -1 if unknown
// Category of a parameter or label, or -1 for global settings that scenes never store
// (PAFL setup, signal generator, monitor level, filters, safes, preferences).
int paramCategory(const std::string& path);
int labelCategory(const std::string& key);
// The input or mix whose "safe" (safe/<id>) protects a parameter or label, or "".
std::string safeOwner(const std::string& pathOrKey);

constexpr int kSceneCount = 100;
constexpr size_t kSceneNameChars = 14;

// Scene text format (one entry per line, '#' comments):
//   n <name>
//   f <category-id>          (blocked by this scene's own recall filter)
//   p <param-path> <value>
//   l <label-key> <text...>
struct SceneInfo {
  int number = 0;
  std::string name;
  std::set<std::string> filter;  // blocked category ids
};
std::string serializeScene(const ParamRegistry& reg, const Labels& labels, const SceneInfo& info);
SceneInfo parseSceneInfo(const std::string& text);

// Recalls a scene: every stored parameter and name is set from the scene (or to its default if
// the scene does not mention it), except global settings, categories blocked by the global or
// the scene's filter, and safe channels. Returns the number of entries applied.
int recallScene(const std::string& text, ParamRegistry& reg, Labels& labels);
// Reset Mix Settings: everything a scene stores goes back to its default, and names are
// cleared. Not affected by filters or safes.
void resetMixSettings(ParamRegistry& reg, Labels& labels);

// 100 numbered scene memories, NNN.scene in a directory.
class SceneStore {
 public:
  explicit SceneStore(std::string dir) : dir_(std::move(dir)) {}

  static bool validNumber(int n) { return n >= 1 && n <= kSceneCount; }
  bool store(int number, const std::string& text, std::string* err) const;
  bool load(int number, std::string* text, std::string* err) const;
  bool clear(int number, std::string* err) const;
  std::vector<SceneInfo> list() const;  // stored scenes, in number order
  const std::string& dir() const { return dir_; }

 private:
  std::string pathFor(int number) const;
  std::string dir_;
};

}  // namespace qnu
