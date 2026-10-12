#include <vector>

#include "dsp/biquad.h"
#include "dsp/dynamics.h"
#include "test.h"

using namespace qnu;

namespace {

constexpr double kFs = 48000.0;

// Steady-state peak of a sine after passing through fn (first 0.5 s discarded).
template <typename Fn>
float sinePeakThrough(double freq, float amp, Fn fn) {
  const int n = static_cast<int>(kFs);
  std::vector<float> buf(n);
  for (int i = 0; i < n; ++i) buf[i] = amp * static_cast<float>(std::sin(2 * kPi * freq * i / kFs));
  for (int i = 0; i < n; i += 256) fn(buf.data() + i, std::min(256, n - i));
  return peakOf(buf.data() + n / 2, n / 2);
}

}  // namespace

TEST(biquad_peak_boosts_at_centre) {
  Biquad b;
  b.set(Biquad::Type::Peak, kFs, 1000, 6.0, 1.0);
  CHECK_NEAR(b.magnitudeDb(1000, kFs), 6.0, 0.01);
  CHECK_NEAR(b.magnitudeDb(20, kFs), 0.0, 0.1);
  const float peak = sinePeakThrough(1000, 0.5f, [&](float* p, int n) { b.processBlock(p, n); });
  CHECK_NEAR(linToDb(peak / 0.5f), 6.0, 0.05);
}

TEST(biquad_shelves) {
  Biquad lo, hi;
  lo.set(Biquad::Type::LowShelf, kFs, 200, -9.0, 0.707);
  hi.set(Biquad::Type::HighShelf, kFs, 4000, 4.0, 0.707);
  CHECK_NEAR(lo.magnitudeDb(30, kFs), -9.0, 0.3);
  CHECK_NEAR(lo.magnitudeDb(10000, kFs), 0.0, 0.1);
  CHECK_NEAR(hi.magnitudeDb(18000, kFs), 4.0, 0.3);
  CHECK_NEAR(hi.magnitudeDb(100, kFs), 0.0, 0.1);
}

TEST(biquad_highpass_is_butterworth) {
  Biquad b;
  b.set(Biquad::Type::HighPass, kFs, 100, 0, 0.7071);
  CHECK_NEAR(b.magnitudeDb(100, kFs), -3.01, 0.05);
  CHECK(b.magnitudeDb(25, kFs) < -23.0);  // 12 dB/octave: two octaves down = ~-24 dB
  CHECK_NEAR(b.magnitudeDb(1000, kFs), 0.0, 0.01);
}

TEST(compressor_static_curve) {
  Compressor c;
  c.setSampleRate(kFs);
  c.setParams(-20, 4, 10, 100, 0, 0);
  CHECK_NEAR(c.computeGainDb(-30), 0.0, 1e-6);
  CHECK_NEAR(c.computeGainDb(-20), 0.0, 1e-6);
  CHECK_NEAR(c.computeGainDb(0), -15.0, 1e-4);  // 20 dB over at 4:1 -> 5 dB over -> -15 dB
  c.setParams(-20, 4, 10, 100, 10, 0);
  CHECK(c.computeGainDb(-20) < 0.0);              // inside the soft knee
  CHECK_NEAR(c.computeGainDb(-26), 0.0, 1e-6);    // below knee
  CHECK_NEAR(c.computeGainDb(0), -15.0, 1e-4);    // above knee: same as hard
}

TEST(compressor_reduces_loud_sine) {
  Compressor c;
  c.setSampleRate(kFs);
  c.setParams(-20, 4, 5, 200, 0, 0);
  const float peak = sinePeakThrough(1000, 1.0f, [&](float* p, int n) { c.process(p, n); });
  CHECK_NEAR(linToDb(peak), -15.0, 1.0);  // 20 dB over at 4:1 -> -20 + 5
  CHECK_NEAR(c.takeGainReduction(), 15.0, 1.0);
}

TEST(compressor_makeup_and_bypass_below_threshold) {
  Compressor c;
  c.setSampleRate(kFs);
  c.setParams(-10, 4, 5, 100, 0, 6);
  const float peak = sinePeakThrough(1000, 0.1f, [&](float* p, int n) { c.process(p, n); });  // -20 dB
  CHECK_NEAR(linToDb(peak), -14.0, 0.1);
}

TEST(gate_closes_below_threshold_and_opens_above) {
  Gate g;
  g.setSampleRate(kFs);
  g.setParams(-40, 30, 0.5f, 10, 20);
  const float quiet = sinePeakThrough(500, 0.003f, [&](float* p, int n) { g.process(p, n); });  // ~-50 dB
  CHECK(!g.isOpen());
  CHECK_NEAR(linToDb(quiet / 0.003f), -30.0, 0.5);
  const float loud = sinePeakThrough(500, 0.1f, [&](float* p, int n) { g.process(p, n); });
  CHECK(g.isOpen());
  CHECK_NEAR(loud, 0.1, 0.001);
}

TEST(db_conversions) {
  CHECK(dbToLin(kMinusInfDb) == 0.0f);
  CHECK_NEAR(dbToLin(-6.0206f), 0.5, 1e-4);
  CHECK_NEAR(linToDb(0.5f), -6.0206, 1e-3);
}
