#include <array>
#include <vector>

#include "mixer.h"
#include "test.h"

using namespace qnu;

namespace {

constexpr double kFs = 48000.0;

struct Rig {
  ParamRegistry reg;
  Mixer mixer{reg};
  std::array<std::vector<float>, kChannels> in;
  std::array<std::vector<float>, kOutputs> out;

  Rig() { mixer.prepare(kFs); }

  void set(const std::string& path, float v) {
    Param* p = reg.find(path);
    CHECK(p != nullptr);
    if (p) p->set(v);
  }

  // Feed a 1 kHz sine into the given channels for `seconds`, return steady-state output peaks.
  std::array<float, kOutputs> run(std::initializer_list<int> channels, float amp = 0.5f,
                                  double seconds = 0.5) {
    const int n = static_cast<int>(kFs * seconds);
    for (auto& v : in) v.assign(n, 0.0f);
    for (auto& v : out) v.assign(n, 0.0f);
    for (int c : channels)
      for (int i = 0; i < n; ++i) in[c][i] = amp * static_cast<float>(std::sin(2 * kPi * 1000 * i / kFs));
    const float* ip[kChannels];
    float* op[kOutputs];
    for (int c = 0; c < kChannels; ++c) ip[c] = in[c].data();
    for (int o = 0; o < kOutputs; ++o) op[o] = out[o].data();
    mixer.process(ip, op, n);
    std::array<float, kOutputs> peaks{};
    for (int o = 0; o < kOutputs; ++o) peaks[o] = peakOf(out[o].data() + n / 2, n / 2);
    return peaks;
  }
};

}  // namespace

TEST(mixer_registers_expected_parameters) {
  ParamRegistry reg;
  Mixer m(reg);
  CHECK(reg.find("ch/1/fader") && reg.find("ch/16/eq/4/q") && reg.find("ch/16/aux/4/pre"));
  CHECK(reg.find("aux/4/fader") && reg.find("dca/4/mute") && reg.find("main/fader"));
  CHECK(reg.find("ch/17/fader") == nullptr);
  CHECK(reg.find("ch/0/fader") == nullptr);
}

TEST(mixer_unity_centre_pan_is_minus_3db) {
  Rig r;
  const auto p = r.run({0});
  CHECK_NEAR(p[kOutMainL], 0.5 * 0.70711, 0.002);
  CHECK_NEAR(p[kOutMainR], 0.5 * 0.70711, 0.002);
  CHECK_NEAR(p[kOutAux1], 0.0, 1e-9);  // sends default to -inf
}

TEST(mixer_hard_pan_and_fader) {
  Rig r;
  r.set("ch/2/pan", -1);
  r.set("ch/2/fader", -6.0206f);
  const auto p = r.run({1});
  CHECK_NEAR(p[kOutMainL], 0.25, 0.002);
  CHECK_NEAR(p[kOutMainR], 0.0, 1e-6);
}

TEST(mixer_sums_channels) {
  Rig r;
  r.set("ch/1/pan", -1);
  r.set("ch/2/pan", -1);
  const auto p = r.run({0, 1}, 0.25f);  // identical in-phase signals sum
  CHECK_NEAR(p[kOutMainL], 0.5, 0.002);
}

TEST(mixer_mute_and_main_mute) {
  Rig r;
  r.set("ch/1/mute", 1);
  CHECK_NEAR(r.run({0})[kOutMainL], 0.0, 1e-6);
  r.set("ch/1/mute", 0);
  r.set("main/mute", 1);
  CHECK_NEAR(r.run({0})[kOutMainL], 0.0, 1e-6);
}

TEST(mixer_main_assign_off) {
  Rig r;
  r.set("ch/1/main", 0);
  r.set("ch/1/aux/1/send", 0);
  const auto p = r.run({0});
  CHECK_NEAR(p[kOutMainL], 0.0, 1e-6);
  CHECK_NEAR(p[kOutAux1], 0.5, 0.002);
}

TEST(mixer_pre_fader_aux_ignores_fader_but_obeys_mute) {
  Rig r;
  r.set("ch/1/fader", kMinusInfDb);
  r.set("ch/1/aux/2/send", 0);
  r.set("ch/1/aux/2/pre", 1);
  CHECK_NEAR(r.run({0})[kOutAux1 + 1], 0.5, 0.002);
  r.set("ch/1/mute", 1);
  CHECK_NEAR(r.run({0})[kOutAux1 + 1], 0.0, 1e-6);
}

TEST(mixer_post_fader_aux_follows_fader) {
  Rig r;
  r.set("ch/1/fader", -6.0206f);
  r.set("ch/1/aux/3/send", 0);
  r.set("ch/1/aux/3/pre", 0);
  r.set("aux/3/fader", -6.0206f);
  CHECK_NEAR(r.run({0})[kOutAux1 + 2], 0.125, 0.002);
}

TEST(mixer_dca_scales_and_mutes_assigned_channels) {
  Rig r;
  r.set("ch/1/pan", -1);
  r.set("ch/1/dca/2", 1);
  r.set("dca/2/fader", -6.0206f);
  CHECK_NEAR(r.run({0})[kOutMainL], 0.25, 0.002);
  r.set("dca/2/mute", 1);
  CHECK_NEAR(r.run({0})[kOutMainL], 0.0, 1e-6);
  r.set("ch/1/dca/2", 0);  // unassigned: DCA no longer applies
  CHECK_NEAR(r.run({0})[kOutMainL], 0.5, 0.002);
}

TEST(mixer_pfl_follows_main_unless_soloed) {
  Rig r;
  r.set("ch/1/pan", -1);
  auto p = r.run({0, 1});
  CHECK_NEAR(p[kOutPflL], p[kOutMainL], 1e-6);
  CHECK_NEAR(p[kOutPflR], p[kOutMainR], 1e-6);

  r.set("ch/2/solo", 1);
  r.set("ch/2/fader", kMinusInfDb);  // PFL is pre-fader
  r.set("ch/2/mute", 1);              // and ignores mute
  p = r.run({1});
  CHECK_NEAR(p[kOutPflL], 0.5, 0.002);
  CHECK_NEAR(p[kOutPflR], 0.5, 0.002);
  CHECK_NEAR(p[kOutMainL], 0.0, 1e-6);
}

TEST(mixer_trim_and_polarity) {
  Rig r;
  r.set("ch/1/pan", -1);
  r.set("ch/1/trim", 6.0206f);
  r.set("ch/1/polarity", 1);
  r.set("ch/2/pan", -1);
  // Channel 2 gets +6 dB of the same signal through its fader; inverted ch1 cancels it.
  r.set("ch/2/fader", 6.0206f);
  CHECK_NEAR(r.run({0, 1}, 0.25f)[kOutMainL], 0.0, 0.002);
}

TEST(mixer_eq_on_channel) {
  Rig r;
  r.set("ch/1/pan", -1);
  r.set("ch/1/eq/2/freq", 1000);
  r.set("ch/1/eq/2/gain", 6);
  CHECK_NEAR(linToDb(r.run({0})[kOutMainL] / 0.5f), 6.0, 0.1);
  r.set("ch/1/eq/on", 0);
  CHECK_NEAR(linToDb(r.run({0})[kOutMainL] / 0.5f), 0.0, 0.05);
}

TEST(mixer_null_buffers_are_silence) {
  ParamRegistry reg;
  Mixer m(reg);
  m.prepare(kFs);
  const float* in[kChannels] = {};
  std::vector<float> l(1000, 1.0f);
  float* out[kOutputs] = {l.data()};
  m.process(in, out, 1000);  // larger than kMaxBlock: exercises chunking
  CHECK(peakOf(l.data(), 1000) == 0.0f);
}

TEST(mixer_meters) {
  Rig r;
  r.set("ch/3/comp/on", 1);
  r.set("ch/3/comp/thresh", -30);
  r.mixer.takeMeters();
  r.run({2});
  const MeterSnapshot s = r.mixer.takeMeters();
  CHECK_NEAR(s.chInDb[2], -6.02, 0.1);
  CHECK(s.chGrDb[2] > 5.0f);
  CHECK(s.chInDb[0] <= -120.0f);
  CHECK(s.outDb[kOutMainL] > -30.0f);
  // Meters reset after reading.
  CHECK(r.mixer.takeMeters().chInDb[2] <= -120.0f);
}
