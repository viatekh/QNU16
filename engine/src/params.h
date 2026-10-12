#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace qnu {

enum class ParamKind { Continuous, Toggle, Choice };  // Choice: integer index min..max

struct ParamInfo {
  std::string path;
  float min = 0.0f;
  float max = 1.0f;
  float def = 0.0f;
  ParamKind kind = ParamKind::Continuous;
  std::string unit;
};

// A single float parameter, written by the control thread and read lock-free by the audio thread.
class Param {
 public:
  explicit Param(ParamInfo info) : info_(std::move(info)), value_(info_.def) {}

  float get() const { return value_.load(std::memory_order_relaxed); }
  bool on() const { return get() >= 0.5f; }
  int index() const { return static_cast<int>(get()); }

  // Clamps (toggles snap to 0/1, choices round to an integer) then stores. Returns the stored value.
  float set(float v);

  const ParamInfo& info() const { return info_; }

 private:
  ParamInfo info_;
  std::atomic<float> value_;
};

// Fixed set of parameters, built once at startup. After construction the set never changes,
// so raw Param* pointers are safe to hold on the audio thread.
class ParamRegistry {
 public:
  Param* add(ParamInfo info);
  Param* find(const std::string& path) const;
  const std::vector<std::unique_ptr<Param>>& all() const { return params_; }
  void resetDefaults();

 private:
  std::vector<std::unique_ptr<Param>> params_;
  std::unordered_map<std::string, Param*> byPath_;
};

// Channel/bus names. Only touched by the control thread, but guarded anyway.
class Labels {
 public:
  explicit Labels(std::vector<std::string> keys) : keys_(std::move(keys)) {}

  bool isValidKey(const std::string& key) const;
  // Returns false for an unknown key. Text is sanitized (no control chars, max 24 chars).
  bool set(const std::string& key, const std::string& text);
  std::string get(const std::string& key) const;
  std::map<std::string, std::string> all() const;
  void clear();
  static std::string sanitize(const std::string& text);

 private:
  std::vector<std::string> keys_;
  mutable std::mutex mu_;
  std::map<std::string, std::string> names_;
};

}  // namespace qnu
