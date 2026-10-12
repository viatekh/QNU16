#pragma once

#include <complex>

#include "dsp/util.h"

namespace qnu {

// RBJ "Audio EQ Cookbook" biquad, transposed direct form II with double state.
class Biquad {
 public:
  enum class Type { Peak, LowShelf, HighShelf, HighPass, LowPass, BandPass };

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
      case Type::BandPass:  // constant 0 dB peak gain
        b0 = alpha; b1 = 0; b2 = -alpha;
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
    double z1 = z1_, z2 = z2_;
    for (int i = 0; i < n; ++i) {
      const double x = buf[i];
      const double y = b0_ * x + z1;
      z1 = b1_ * x - a1_ * y + z2;
      z2 = b2_ * x - a2_ * y;
      buf[i] = static_cast<float>(y);
    }
    z1_ = z1;
    z2_ = z2;
  }

  // Process two independent filters (e.g. the L and R of a stereo channel) in one loop. The two
  // recursions are independent, so the CPU overlaps them; nearly 2x faster than two passes.
  static void processPair(Biquad& a, float* x, Biquad& b, float* y, int n) {
    double az1 = a.z1_, az2 = a.z2_, bz1 = b.z1_, bz2 = b.z2_;
    for (int i = 0; i < n; ++i) {
      const double xa = x[i], xb = y[i];
      const double ya = a.b0_ * xa + az1;
      const double yb = b.b0_ * xb + bz1;
      az1 = a.b1_ * xa - a.a1_ * ya + az2;
      bz1 = b.b1_ * xb - b.a1_ * yb + bz2;
      az2 = a.b2_ * xa - a.a2_ * ya;
      bz2 = b.b2_ * xb - b.a2_ * yb;
      x[i] = static_cast<float>(ya);
      y[i] = static_cast<float>(yb);
    }
    a.z1_ = az1; a.z2_ = az2; b.z1_ = bz1; b.z2_ = bz2;
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
