#pragma once

#include <complex>

#include "dsp/util.h"

namespace qnu {

// RBJ "Audio EQ Cookbook" biquad, transposed direct form II with double state.
class Biquad {
 public:
  enum class Type { Peak, LowShelf, HighShelf, HighPass, LowPass };

  void set(Type type, double fs, double f0, double gainDb, double q) {
    f0 = std::clamp(f0, 10.0, 0.45 * fs);
    q = std::max(q, 0.05);
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cs = std::cos(w0);
    const double sn = std::sin(w0);
    const double alpha = sn / (2.0 * q);
    const double sqA2a = 2.0 * std::sqrt(A) * alpha;
    double b0, b1, b2, a0, a1, a2;
    switch (type) {
      case Type::Peak:
        b0 = 1 + alpha * A; b1 = -2 * cs; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cs; a2 = 1 - alpha / A;
        break;
      case Type::LowShelf:
        b0 = A * ((A + 1) - (A - 1) * cs + sqA2a);
        b1 = 2 * A * ((A - 1) - (A + 1) * cs);
        b2 = A * ((A + 1) - (A - 1) * cs - sqA2a);
        a0 = (A + 1) + (A - 1) * cs + sqA2a;
        a1 = -2 * ((A - 1) + (A + 1) * cs);
        a2 = (A + 1) + (A - 1) * cs - sqA2a;
        break;
      case Type::HighShelf:
        b0 = A * ((A + 1) + (A - 1) * cs + sqA2a);
        b1 = -2 * A * ((A - 1) + (A + 1) * cs);
        b2 = A * ((A + 1) + (A - 1) * cs - sqA2a);
        a0 = (A + 1) - (A - 1) * cs + sqA2a;
        a1 = 2 * ((A - 1) - (A + 1) * cs);
        a2 = (A + 1) - (A - 1) * cs - sqA2a;
        break;
      case Type::HighPass:
        b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2;
        a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
        break;
      case Type::LowPass:
      default:
        b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = (1 - cs) / 2;
        a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
        break;
    }
    b0_ = b0 / a0; b1_ = b1 / a0; b2_ = b2 / a0;
    a1_ = a1 / a0; a2_ = a2 / a0;
  }

  void reset() { z1_ = z2_ = 0.0; }

  inline float process(float x) {
    const double y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    return static_cast<float>(y);
  }

  void processBlock(float* buf, int n) {
    for (int i = 0; i < n; ++i) buf[i] = process(buf[i]);
  }

  double magnitudeDb(double f, double fs) const {
    const std::complex<double> z1 = std::polar(1.0, -2.0 * kPi * f / fs);
    const std::complex<double> z2 = z1 * z1;
    const auto h = (b0_ + b1_ * z1 + b2_ * z2) / (1.0 + a1_ * z1 + a2_ * z2);
    return 20.0 * std::log10(std::abs(h));
  }

 private:
  double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
  double z1_ = 0, z2_ = 0;
};

}  // namespace qnu
