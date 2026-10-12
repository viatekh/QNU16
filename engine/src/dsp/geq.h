#pragma once

#include <array>

#include "dsp/biquad.h"

namespace qnu {

// 28-band 1/3-octave graphic EQ (31.5 Hz - 16 kHz), as on Qu mix masters.
// Bands at 0 dB are skipped, so a flat GEQ costs nothing.
class GraphicEq {
 public:
  static constexpr int kBands = 28;
  static constexpr float kFreqs[kBands] = {31.5f, 40,   50,   63,   80,   100,  125,
                                           160,   200,  250,  315,  400,  500,  630,
                                           800,   1000, 1250, 1600, 2000, 2500, 3150,
                                           4000,  5000, 6300, 8000, 10000, 12500, 16000};
  static constexpr double kQ = 4.32;  // 1/3 octave

  void setBand(int k, float gainDb, double fs) {
    if (gainDb == gain_[k] && fs == fs_[k]) return;
    gain_[k] = gainDb;
    fs_[k] = fs;
    band_[k].set(Biquad::Type::Peak, fs, kFreqs[k], gainDb, kQ);
  }

  void reset() {
    for (auto& b : band_) b.reset();
  }

  void process(float* buf, int n) {
    for (int k = 0; k < kBands; ++k)
      if (gain_[k] != 0.0f) band_[k].processBlock(buf, n);
  }

  // Stereo: both sides share band gains, so their filters run interleaved.
  static void processPair(GraphicEq& a, float* l, GraphicEq& b, float* r, int n) {
    for (int k = 0; k < kBands; ++k)
      if (a.gain_[k] != 0.0f) Biquad::processPair(a.band_[k], l, b.band_[k], r, n);
  }

 private:
  std::array<Biquad, kBands> band_;
  std::array<float, kBands> gain_{};
  std::array<double, kBands> fs_{};
};

}  // namespace qnu
