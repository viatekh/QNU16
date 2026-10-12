// Qu-16 features: assign keys, FX3/FX4 mix feeds, PEQ width, compressor types, RTA,
// talkback, signal generator, monitor level and the recorder. Each test measures real signal.

#include <unistd.h>

#include <array>
#include <filesystem>
#include <vector>

#include "mixer.h"
#include "test.h"
#include "wav.h"

using namespace qnu;

namespace {

constexpr double kFs = 48000.0;
constexpr int kOutL = 0;
int outMix(int n) { return 1 + n; }
constexpr int kOutPaflL = 12;

struct Desk {
  ParamRegistry reg;
  Mixer mixer{reg};
  std::array<std::vector<float>, kPhysInputs> in;
  std::array<std::vector<float>, kPhysOutputs> out;

  Desk() { mixer.prepare(kFs); }
  void set(const std::string& p, float v) {
    Param* q = reg.find(p);
    if (!q) std::printf("    no parameter %s\n", p.c_str());
    CHECK(q != nullptr);
    if (q) q->set(v);
  }
  // Sine (freq, amp) into the given inputs; returns steady-state output peaks.
  std::array<float, kPhysOutputs> run(std::initializer_list<int> ins, double freq = 1000,
                                      float amp = 0.5f, double seconds = 0.5) {
    const int n = static_cast<int>(kFs * seconds);
    for (auto& v : in) v.assign(n, 0.0f);
    for (auto& v : out) v.assign(n, 0.0f);
    for (int c : ins)
      for (int i = 0; i < n; ++i) in[c][i] = amp * static_cast<float>(std::sin(2 * kPi * freq * i / kFs));
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

}  // namespace

TEST(qu_assign_key_switches_a_send_off) {
  Desk d;
  d.set("ch/1/mix/2/level", 0);
  CHECK_NEAR(d.run({0})[outMix(2)], 0.5, 0.002);
  d.set("ch/1/mix/2/on", 0);
  CHECK_NEAR(d.run({0})[outMix(2)], 0.0, 1e-6);
}

TEST(qu_fx3_is_fed_from_a_mix) {
  Desk d;
  d.set("ch/1/lr", 0);
  d.set("ch/1/mix/4/level", 0);          // Mix 4 carries the channel
  d.set("fx/3/src", 4);                  // FX3 <- Mix 4
  d.set("fx/3/type", FxEngine::kDelay);
  d.set("fx/3/dly/time", 20);
  d.set("fx/3/dly/feedback", 0);
  d.set("fx/3/dly/pingpong", 0);
  CHECK(d.run({0})[kOutL] > 0.2f);       // FX return 3 reaches LR
  d.set("fx/3/src", 0);
  d.run({0});
  CHECK_NEAR(d.run({0})[kOutL], 0.0, 1e-6);
}

TEST(qu_peq_width_in_octaves) {
  // A narrower bell boosts less half an octave away from its centre.
  auto boostAt = [](float width) {
    Desk d;
    d.set("ch/1/pan", -1);
    d.set("ch/1/eq/2/freq", 1000);
    d.set("ch/1/eq/2/gain", 12);
    d.set("ch/1/eq/2/width", width);
    return linToDb(d.run({0}, 1414.0)[kOutL] / 0.5f);
  };
  const float wide = boostAt(1.5f), narrow = boostAt(1.0f / 9.0f);
  CHECK(wide > 6.0f);
  CHECK(narrow < 1.0f);
  // At the centre every width gives the full boost.
  Desk d;
  d.set("ch/1/pan", -1);
  d.set("ch/1/eq/2/freq", 1000);
  d.set("ch/1/eq/2/gain", 12);
  d.set("ch/1/eq/2/width", 1.0f / 9.0f);
  CHECK_NEAR(linToDb(d.run({0})[kOutL] / 0.5f), 12.0, 0.2);
}

TEST(qu_compressor_types) {
  // Same threshold/ratio; drive with a loud burst then a quiet tail and look at how fast each
  // type lets go. Punchbag recovers fastest, Slow Opto slowest.
  auto recovery = [](int mode) {
    Compressor c;
    c.setSampleRate(kFs);
    c.setParams(-30, 4, 10, 100, 0, 0, mode);
    std::vector<float> buf(static_cast<size_t>(kFs * 1.2));
    for (size_t i = 0; i < buf.size(); ++i) {
      const float amp = i < kFs * 0.5 ? 1.0f : 0.01f;  // 0 dB burst, then -40 dB (below threshold)
      buf[i] = amp * static_cast<float>(std::sin(2 * kPi * 500 * i / kFs));
    }
    for (size_t i = 0; i < buf.size(); i += 256) c.process(buf.data() + i, nullptr, 256);
    // Gain 400 ms after the burst ends, relative to the quiet input.
    const size_t at = static_cast<size_t>(kFs * 0.9);
    return linToDb(peakOf(buf.data() + at, 480) / 0.01f);
  };
  const float punch = recovery(Compressor::kPunch);
  const float peak = recovery(Compressor::kPeak);
  const float opto = recovery(Compressor::kOpto);
  CHECK(punch > -1.0f);   // fully recovered
  CHECK(opto < -3.0f);    // still holding gain down
  CHECK(punch > opto);
  CHECK(peak > opto);
}

TEST(qu_rta_finds_the_tone) {
  Desk d;
  d.mixer.takeRta();
  d.run({0}, 1000, 0.5f);
  const auto rta = d.mixer.takeRta();
  int best = 0;
  for (int k = 0; k < Rta::kBands; ++k)
    if (rta[k] > rta[best]) best = k;
  CHECK(Rta::kFreqs[best] == 1000.0f);
  // LR at centre pan: 0.5 * 0.707 per side; the RTA reads the mono PAFL (= LR) sum.
  CHECK_NEAR(rta[best], linToDb(0.5f * 0.70711f), 1.0);
  CHECK(rta[best] - rta[best - 1] > 10.0f);
  CHECK(rta[best] - rta[best + 1] > 10.0f);
  CHECK(rta[0] < -60.0f);
}

TEST(qu_rta_follows_pafl) {
  Desk d;
  d.set("ch/2/pafl", 1);
  d.set("ch/2/fader", kMinusInfDb);  // PFL: still analysed
  d.mixer.takeRta();
  d.run({0, 1}, 125, 0.5f);          // both channels play 125 Hz; only ch 2 is soloed
  const auto rta = d.mixer.takeRta();
  CHECK_NEAR(rta[8], linToDb(0.5f), 1.0);  // 125 Hz band = ch 2 alone, pre-fader
}

TEST(qu_signal_generator_and_talkback) {
  Desk d;
  d.set("gen/on", 1);
  d.set("gen/type", 0);
  d.set("gen/freq", 1000);
  d.set("gen/level", -20);
  d.set("gen/mix/3", 1);
  auto p = d.run({});
  CHECK_NEAR(linToDb(p[outMix(3)]), -20.0, 0.2);
  CHECK_NEAR(p[kOutL], 0.0, 1e-6);  // not assigned to LR
  d.set("gen/on", 0);
  d.set("talk/src", 5);              // talkback mic on input 5
  d.set("talk/on", 1);
  d.set("talk/mix/1", 1);
  p = d.run({4});
  CHECK_NEAR(p[outMix(1)], 0.5, 0.01);  // 1 kHz passes the 120 Hz HPF
  d.set("talk/on", 0);
  d.run({4});
  CHECK_NEAR(d.run({4})[outMix(1)], 0.0, 1e-6);
}

TEST(qu_monitor_level_scales_pafl_only) {
  Desk d;
  d.set("ch/1/pan", -1);
  d.set("mon/level", -6.0206f);
  const auto p = d.run({0});
  CHECK_NEAR(p[kOutL], 0.5, 0.002);
  CHECK_NEAR(p[kOutPaflL], 0.25, 0.002);
}

TEST(qu_recorder_writes_multitrack_wav) {
  const auto dir = std::filesystem::temp_directory_path() / ("qnu16-rec-" + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  Desk d;
  Recorder rec(dir.string());
  d.mixer.setRecorder(&rec);
  std::string err;
  CHECK(rec.start(Recorder::kMultitrack, kFs, &err));
  d.run({2}, 1000, 0.5f, 0.5);   // ch 3 plays
  rec.stop();
  CHECK(rec.dropouts() == 0u);
  AudioFile f;
  CHECK(readWav((dir / rec.file()).string(), &f, &err));
  CHECK(f.channels.size() == 18u);
  CHECK(f.frames() == static_cast<size_t>(kFs * 0.5));
  if (f.channels.size() == 18u) {
    CHECK_NEAR(peakOf(f.channels[2].data(), static_cast<int>(f.frames())), 0.5, 0.01);  // ch 3 track
    CHECK(peakOf(f.channels[0].data(), static_cast<int>(f.frames())) == 0.0f);        // ch 1 silent
    CHECK(peakOf(f.channels[16].data(), static_cast<int>(f.frames())) > 0.3f);        // LR L
  }
  std::filesystem::remove_all(dir);
}
