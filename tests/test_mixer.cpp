#include <array>
#include <vector>

#include "mixer.h"
#include "test.h"

using namespace qnu;

namespace {

constexpr double kFs = 48000.0;

// Physical output indices with the default patch.
constexpr int kOutL = 0, kOutR = 1;
int outMix(int n) { return 1 + n; }  // Mix 1..10 -> out 3..12 (0-based 2..11)
constexpr int kOutMtx1 = 12, kOutPaflL = 16, kOutPaflR = 17;

struct Rig {
  ParamRegistry reg;
  Mixer mixer{reg};
  std::array<std::vector<float>, kPhysInputs> in;
  std::array<std::vector<float>, kPhysOutputs> out;

  Rig() { mixer.prepare(kFs); }

  void set(const std::string& path, float v) {
    Param* p = reg.find(path);
    if (!p) std::printf("    no parameter %s\n", path.c_str());
    CHECK(p != nullptr);
    if (p) p->set(v);
  }

  // 1 kHz sine into the given physical inputs; returns steady-state peaks of every output.
  std::array<float, kPhysOutputs> run(std::initializer_list<int> physIns, float amp = 0.5f,
                                      double seconds = 0.5) {
    const int n = static_cast<int>(kFs * seconds);
    for (auto& v : in) v.assign(n, 0.0f);
    for (auto& v : out) v.assign(n, 0.0f);
    for (int c : physIns)
      for (int i = 0; i < n; ++i) in[c][i] = amp * static_cast<float>(std::sin(2 * kPi * 1000 * i / kFs));
    return process(n);
  }

  std::array<float, kPhysOutputs> process(int n) {
    const float* ip[kPhysInputs];
    float* op[kPhysOutputs];
    for (int c = 0; c < kPhysInputs; ++c) ip[c] = in[c].data();
    for (int o = 0; o < kPhysOutputs; ++o) op[o] = out[o].data();
    mixer.process(ip, op, n);
    std::array<float, kPhysOutputs> peaks{};
    for (int o = 0; o < kPhysOutputs; ++o) peaks[o] = peakOf(out[o].data() + n / 2, n / 2);
    return peaks;
  }
};

constexpr float k6dB = 6.0206f;
constexpr float kCentre = 0.70711f;  // constant-power pan at centre

}  // namespace

TEST(topology_matches_qu16) {
  CHECK(inputDefs().size() == 23u);
  CHECK(busDefs().size() == 18u);
  CHECK(busDefs()[kBusMix0 + 4].name == "Mix 5-6" && busDefs()[kBusMix0 + 4].stereo);
  CHECK(busDefs()[kBusMix0 + 3].name == "Mix 4" && !busDefs()[kBusMix0 + 3].stereo);
  CHECK(busDefs()[kBusGrp0 + 3].name == "Grp 7-8");
  CHECK(busDefs()[kBusMtx0 + 1].name == "Mtx 3-4");
  CHECK(outputSources()[defaultOutputSource(0)].name == "LR L");
  CHECK(outputSources()[defaultOutputSource(11)].name == "Mix 10");
  CHECK(outputSources()[defaultOutputSource(12)].name == "Mtx 1");
  CHECK(outputSources()[defaultOutputSource(17)].name == "PAFL R");
}

TEST(mixer_registers_expected_parameters) {
  ParamRegistry reg;
  Mixer m(reg);
  for (const char* p : {"ch/1/fader", "ch/16/eq/4/q", "ch/16/mix/7/pre", "ch/3/fx/4/level",
                        "ch/1/delay/time", "ch/1/comp/mode", "st/3/gain", "st/1/pan",
                        "fxret/4/eq/2/gain", "lr/geq/28", "mix/5/balance", "grp/4/lr",
                        "mtx/2/grp/4", "mtx/1/lr", "fx/1/type", "fx/3/dly/time", "dca/4/mute",
                        "mg/4/mute", "out/18/src", "ch/1/mg/2", "ch/1/grp/4"}) {
    if (!reg.find(p)) std::printf("    missing %s\n", p);
    CHECK(reg.find(p) != nullptr);
  }
  CHECK(reg.find("ch/1/fader") && reg.find("st/3/gain") && reg.find("fxret/4/eq/2/gain"));
  CHECK(reg.find("lr/geq/28") && reg.find("mtx/2/grp/4") && reg.find("out/18/src"));
  CHECK(reg.find("grp/1/geq/1") == nullptr);   // groups have no GEQ
  CHECK(reg.find("fxret/1/fx/1/level") == nullptr);  // no FX-return -> FX feedback path
  CHECK(reg.find("mix/1/balance") == nullptr);  // mono mix
  CHECK(reg.find("ch/17/fader") == nullptr);
}

TEST(mixer_unity_centre_pan_is_minus_3db) {
  Rig r;
  const auto p = r.run({0});
  CHECK_NEAR(p[kOutL], 0.5 * kCentre, 0.002);
  CHECK_NEAR(p[kOutR], 0.5 * kCentre, 0.002);
  CHECK_NEAR(p[outMix(1)], 0.0, 1e-9);  // sends default to -inf
}

TEST(mixer_hard_pan_fader_and_sum) {
  Rig r;
  r.set("ch/2/pan", -1);
  r.set("ch/2/fader", -k6dB);
  r.set("ch/3/pan", -1);
  r.set("ch/3/fader", -k6dB);
  const auto p = r.run({1, 2});
  CHECK_NEAR(p[kOutL], 0.5, 0.002);
  CHECK_NEAR(p[kOutR], 0.0, 1e-6);
}

TEST(mixer_stereo_input_and_balance) {
  Rig r;
  // ST1 = physical inputs 17/18 (0-based 16/17). Signal on the left leg only.
  auto p = r.run({16});
  CHECK_NEAR(p[kOutL], 0.5, 0.002);  // stereo channels pass L->L at unity
  CHECK_NEAR(p[kOutR], 0.0, 1e-6);
  r.set("st/1/pan", 0.5f);  // balance right: left side down 6 dB
  p = r.run({16});
  CHECK_NEAR(p[kOutL], 0.25, 0.002);
}

TEST(mixer_stereo_input_to_mono_mix_sums) {
  Rig r;
  r.set("st/2/mix/1/level", 0);
  const auto p = r.run({18, 19}, 0.5f);  // identical L and R
  CHECK_NEAR(p[outMix(1)], 0.5, 0.002);  // (L+R)/2
}

TEST(mixer_mono_input_to_stereo_mix_follows_pan) {
  Rig r;
  r.set("ch/1/mix/5/level", 0);  // Mix 5-6
  r.set("ch/1/pan", 1);
  const auto p = r.run({0});
  CHECK_NEAR(p[outMix(5)], 0.0, 1e-6);
  CHECK_NEAR(p[outMix(6)], 0.5, 0.002);
}

TEST(mixer_pre_fader_mix_ignores_fader_but_obeys_mute) {
  Rig r;
  r.set("ch/1/fader", kMinusInfDb);
  r.set("ch/1/mix/2/level", 0);
  CHECK_NEAR(r.run({0})[outMix(2)], 0.5, 0.002);
  r.set("ch/1/mute", 1);
  CHECK_NEAR(r.run({0})[outMix(2)], 0.0, 1e-6);
}

TEST(mixer_post_fader_mix_and_master) {
  Rig r;
  r.set("ch/1/fader", -k6dB);
  r.set("ch/1/mix/3/level", 0);
  r.set("ch/1/mix/3/pre", 0);
  r.set("mix/3/fader", -k6dB);
  CHECK_NEAR(r.run({0})[outMix(3)], 0.125, 0.002);
  r.set("mix/3/mute", 1);
  CHECK_NEAR(r.run({0})[outMix(3)], 0.0, 1e-6);
}

TEST(mixer_groups_feed_lr) {
  Rig r;
  r.set("ch/1/lr", 0);
  r.set("ch/1/grp/2", 1);
  r.set("ch/1/pan", -1);
  auto p = r.run({0});
  CHECK_NEAR(p[kOutL], 0.5, 0.002);  // via the group
  r.set("grp/2/fader", -k6dB);
  CHECK_NEAR(r.run({0})[kOutL], 0.25, 0.002);
  r.set("grp/2/lr", 0);
  CHECK_NEAR(r.run({0})[kOutL], 0.0, 1e-6);
}

TEST(mixer_matrix_takes_post_fader_masters) {
  Rig r;
  r.set("ch/1/pan", -1);
  r.set("lr/fader", -k6dB);
  r.set("mtx/1/lr", 0);
  auto p = r.run({0});
  CHECK_NEAR(p[kOutMtx1], 0.25, 0.002);
  r.set("ch/1/mix/1/level", 0);
  r.set("mtx/1/mix/1", -k6dB);
  r.set("mtx/1/lr", kMinusInfDb);
  p = r.run({0});
  CHECK_NEAR(p[kOutMtx1], 0.25, 0.002);      // mono mix feeds both matrix legs
  CHECK_NEAR(p[kOutMtx1 + 1], 0.25, 0.002);
}

TEST(mixer_dca_scales_and_mutes) {
  Rig r;
  r.set("ch/1/pan", -1);
  r.set("ch/1/dca/2", 1);
  r.set("dca/2/fader", -k6dB);
  CHECK_NEAR(r.run({0})[kOutL], 0.25, 0.002);
  r.set("dca/2/mute", 1);
  CHECK_NEAR(r.run({0})[kOutL], 0.0, 1e-6);
  r.set("ch/1/dca/2", 0);
  CHECK_NEAR(r.run({0})[kOutL], 0.5, 0.002);
}

TEST(mixer_mute_groups) {
  Rig r;
  r.set("ch/1/pan", -1);
  r.set("ch/1/mg/3", 1);
  r.set("mg/3/mute", 1);
  CHECK_NEAR(r.run({0})[kOutL], 0.0, 1e-6);
  r.set("mg/3/mute", 0);
  CHECK_NEAR(r.run({0})[kOutL], 0.5, 0.002);
}

TEST(mixer_pafl_pfl_inputs_afl_masters) {
  Rig r;
  r.set("ch/1/pan", -1);
  auto p = r.run({0, 1});
  CHECK_NEAR(p[kOutPaflL], p[kOutL], 1e-6);  // follows LR when nothing is selected

  r.set("ch/2/pafl", 1);
  r.set("ch/2/fader", kMinusInfDb);  // PFL is pre-fader
  r.set("ch/2/mute", 1);             // and ignores mute
  p = r.run({1});
  CHECK_NEAR(p[kOutPaflL], 0.5, 0.002);
  CHECK_NEAR(p[kOutPaflR], 0.5, 0.002);

  r.set("ch/2/pafl", 0);
  r.set("ch/1/mix/4/level", -k6dB);
  r.set("mix/4/fader", -k6dB);
  r.set("mix/4/pafl", 1);  // AFL is post-fader
  p = r.run({0});
  CHECK_NEAR(p[kOutPaflL], 0.125, 0.002);
}

TEST(mixer_output_patch) {
  Rig r;
  r.set("ch/1/pan", -1);
  r.set("out/5/src", 1);  // LR L
  int direct = 0;
  for (size_t i = 0; i < outputSources().size(); ++i)
    if (outputSources()[i].name == "Ch 3 DO") direct = static_cast<int>(i);
  r.set("out/6/src", direct);
  r.set("ch/3/gain", -k6dB);  // direct out is post-preamp, pre-fader
  r.set("ch/3/fader", kMinusInfDb);
  const auto p = r.run({0, 2});
  CHECK_NEAR(p[4], p[kOutL], 1e-6);
  CHECK_NEAR(p[5], 0.25, 0.002);
  r.set("out/5/src", 0);
  CHECK_NEAR(r.run({0})[4], 0.0, 1e-9);
}

TEST(mixer_channel_delay_is_sample_exact) {
  Rig r;
  r.set("ch/1/pan", -1);
  r.set("ch/1/delay/on", 1);
  r.set("ch/1/delay/time", 1.0f);  // 48 samples
  const int n = 1024;
  for (auto& v : r.in) v.assign(n, 0.0f);
  for (auto& v : r.out) v.assign(n, 0.0f);
  r.in[0][600] = 1.0f;  // after the startup gain ramps have settled
  r.process(n);
  int at = -1;
  for (int i = 0; i < n; ++i)
    if (std::fabs(r.out[kOutL][i]) > 0.5f) at = i;
  CHECK(at == 648);
}

TEST(mixer_peq_and_geq) {
  Rig r;
  r.set("ch/1/pan", -1);
  r.set("ch/1/eq/2/freq", 1000);
  r.set("ch/1/eq/2/gain", 6);
  CHECK_NEAR(linToDb(r.run({0})[kOutL] / 0.5f), 6.0, 0.1);
  r.set("ch/1/eq/on", 0);
  r.set("lr/geq/on", 1);
  r.set("lr/geq/16", -9);  // 1 kHz band
  CHECK_NEAR(linToDb(r.run({0})[kOutL] / 0.5f), -9.0, 0.2);
  r.set("lr/geq/16", 0);
  r.set("lr/geq/6", 12);  // 100 Hz band barely touches 1 kHz
  CHECK_NEAR(linToDb(r.run({0})[kOutL] / 0.5f), 0.0, 0.1);
}

TEST(mixer_peq_low_band_bell_vs_shelf) {
  Rig r;
  r.set("ch/1/pan", -1);
  r.set("ch/1/eq/1/freq", 1000);
  r.set("ch/1/eq/1/gain", 6);
  r.set("ch/1/eq/1/q", 4);
  r.set("ch/1/eq/1/type", 1);  // bell: full boost at its centre
  CHECK_NEAR(linToDb(r.run({0})[kOutL] / 0.5f), 6.0, 0.1);
  r.set("ch/1/eq/1/type", 0);  // shelf: half the boost at its corner
  CHECK_NEAR(linToDb(r.run({0})[kOutL] / 0.5f), 3.0, 0.5);
}

TEST(mixer_null_buffers_are_silence) {
  ParamRegistry reg;
  Mixer m(reg);
  m.prepare(kFs);
  const float* in[kPhysInputs] = {};
  std::vector<float> l(1000, 1.0f);
  float* out[kPhysOutputs] = {l.data()};
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
  CHECK_NEAR(s.inputs[2].inDb, -6.02, 0.1);
  CHECK(s.inputs[2].grDb > 5.0f);
  CHECK(s.inputs[0].inDb <= -120.0f);
  CHECK(s.buses[kBusLR].lDb > -30.0f);
  CHECK(r.mixer.takeMeters().inputs[2].inDb <= -120.0f);
}
