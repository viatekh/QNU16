#include "params.h"

#include <algorithm>
#include <cmath>

namespace qnu {

float Param::set(float v) {
  if (!std::isfinite(v)) v = info_.def;
  v = std::clamp(v, info_.min, info_.max);
  if (info_.kind == ParamKind::Toggle) v = v >= 0.5f ? 1.0f : 0.0f;
  value_.store(v, std::memory_order_relaxed);
  return v;
}

Param* ParamRegistry::add(ParamInfo info) {
  auto p = std::make_unique<Param>(std::move(info));
  Param* raw = p.get();
  byPath_[raw->info().path] = raw;
  params_.push_back(std::move(p));
  return raw;
}

Param* ParamRegistry::find(const std::string& path) const {
  auto it = byPath_.find(path);
  return it == byPath_.end() ? nullptr : it->second;
}

void ParamRegistry::resetDefaults() {
  for (auto& p : params_) p->set(p->info().def);
}

bool Labels::isValidKey(const std::string& key) const {
  return std::find(keys_.begin(), keys_.end(), key) != keys_.end();
}

bool Labels::set(const std::string& key, const std::string& text) {
  if (!isValidKey(key)) return false;
  std::lock_guard<std::mutex> lock(mu_);
  const std::string clean = sanitize(text);
  if (clean.empty()) names_.erase(key);
  else names_[key] = clean;
  return true;
}

std::string Labels::get(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = names_.find(key);
  return it == names_.end() ? std::string() : it->second;
}

std::map<std::string, std::string> Labels::all() const {
  std::lock_guard<std::mutex> lock(mu_);
  return names_;
}

void Labels::clear() {
  std::lock_guard<std::mutex> lock(mu_);
  names_.clear();
}

std::string Labels::sanitize(const std::string& text) {
  // Copy whole UTF-8 code points only, so truncation never splits a character.
  std::string out;
  size_t chars = 0;
  for (size_t i = 0; i < text.size() && chars < 24;) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : (c >> 3) == 0x1e ? 4 : 0;
    if (len == 0 || i + len > text.size()) {  // invalid byte: skip it
      ++i;
      continue;
    }
    if (len == 1 && (c < 0x20 || c == 0x7f)) {
      ++i;
      continue;
    }
    out.append(text, i, len);
    i += len;
    ++chars;
  }
  // Trim surrounding spaces.
  const auto first = out.find_first_not_of(' ');
  if (first == std::string::npos) return {};
  const auto last = out.find_last_not_of(' ');
  return out.substr(first, last - first + 1);
}

}  // namespace qnu
