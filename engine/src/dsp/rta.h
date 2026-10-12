#pragma once

#include <array>
#include <atomic>

#include "dsp/biquad.h"

namespace qnu {

// Real-time analyser: 31 third-octave bands, 20 Hz - 20 kHz (the ISO centres a graphic EQ uses).
// Each band is two cascaded 2nd-order band-passes (4th order).
// The audio thread feeds blocks; the control thread reads the loudest level per band since its
// last read. Levels are RMS reported +3 dB, so a full-scale sine reads 0 dBFS.
class Rta {
 public:
  static constexpr int kBands = 31;
  static constexpr float kFreqs[kBands] = {20,   25,   31.5f, 40,   50,   63,   80,    100,
                                           125,  160,  200,   250,  315,  400,  500,   630,
                                           800,  1000, 1250,  1600, 2000, 2500, 3150,  4000,
                                           5000, 6300, 8000,  10000, 12500, 16000, 20000};

  void prepare(double fs) {
    for (int k = 0; k < kBands; ++k) {
      // 1/3-octave Q per stage: the cascade is -6 dB at the band edges and about -14 dB at the
      // neighbouring band's centre.
      const double f = std::min<double>(kFreqs[k], 0.45 * fs);
      for (auto& s : stage_[k]) {
        s.set(Biquad::Type::BandPass, fs, f, 0.0, 4.32);
        s.reset();
      }
    }
  }

  void process(const float* x, int n) {
    for (int k = 0; k < kBands; ++k) {
      Biquad& a = stage_[k][0];
      Biquad& b = stage_[k][1];
      double sum = 0.0;
      for (int i = 0; i < n; ++i) {
        const float y = b.process(a.process(x[i]));
        sum += double(y) * y;
      }
      const float ms = static_cast<float>(sum / n);
      float cur = level_[k].load(std::memory_order_relaxed);
      while (ms > cur && !level_[k].compare_exchange_weak(cur, ms, std::memory_order_relaxed)) {
      }
    }
  }

  // Control thread: per-band level in dBFS since the previous call.
  std::array<float, kBands> take() {
    std::array<float, kBands> out{};
    for (int k = 0; k < kBands; ++k) {
      const float ms = level_[k].exchange(0.0f, std::memory_order_relaxed);
      out[k] = ms > 1e-12f ? 10.0f * std::log10(ms * 2.0f) : -120.0f;
    }
    return out;
  }

 private:
  std::array<std::array<Biquad, 2>, kBands> stage_;
  std::array<std::atomic<float>, kBands> level_{};
};

}  // namespace qnu
