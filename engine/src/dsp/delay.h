#pragma once

#include <vector>

#include "dsp/util.h"

namespace qnu {

// Integer-sample delay line. Memory is allocated once (allocate()), never on the audio thread.
class DelayLine {
 public:
  void allocate(int maxSamples) {
    buf_.assign(static_cast<size_t>(maxSamples) + 1, 0.0f);
    pos_ = 0;
    delay_ = std::min(delay_, maxSamples);
  }

  void reset() {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    pos_ = 0;
  }

  void setDelay(int samples) { delay_ = std::clamp(samples, 0, maxDelay()); }
  int delay() const { return delay_; }
  int maxDelay() const { return buf_.empty() ? 0 : static_cast<int>(buf_.size()) - 1; }

  void process(float* x, int n) {
    if (delay_ == 0 || buf_.empty()) return;
    const int size = static_cast<int>(buf_.size());
    for (int i = 0; i < n; ++i) {
      buf_[pos_] = x[i];
      int rd = pos_ - delay_;
      if (rd < 0) rd += size;
      x[i] = buf_[rd];
      if (++pos_ == size) pos_ = 0;
    }
  }

 private:
  std::vector<float> buf_;
  int pos_ = 0;
  int delay_ = 0;
};

}  // namespace qnu
