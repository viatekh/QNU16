// Qu reference-guide behaviour: Qu dB thresholds, ducker, FX patching (insert, Ch>Return),
// tap points (direct outs, pre-fade mix sources), PAFL options, linking, the AMM, the
// generator's band-pass noise and Qu-Drive playback. Each test measures real signal.

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
constexpr int kOutL = 0, kOutR = 1;
int outMix(int n) { return 1 + n; }
constexpr int kOutPaflL = 12, kOutPaflR = 13;

struct Desk {
  ParamRegistry reg;
  Mixer mixer{reg};
  std::array<std::vector<float>, kPhysInputs> in;
  std::array<std::vector<float>, kPhysOutputs> out;

  Desk() {
    mixer.prepare(kFs);
    for (const auto& d : inputDefs()) reg.find(d.id + "/fader")->set(0);
    for (const auto& b : busDefs()) reg.find(b.id + "/fader")->set(0);
  }
  void set(const std::string& p, float v) {
    Param* q = reg.find(p);
    if (!q) std::printf("    no parameter %s\n", p.c_str());
    CHECK(q != nullptr);
    if (q) q->set(v);
  }
  void clear(int n) {
    for (auto& v : in) v.assign(n, 0.0f);
    for (auto& v : out) v.assign(n, 0.0f);
  }
  void sine(int c, float amp, double freq = 1000) {
    for (size_t i = 0; i < in[c].size(); ++i) in[c][i] = amp * static_cast<float>(std::sin(2 * kPi * freq * i / kFs));
  }
  void process() {
    const float* ip[kPhysInputs];
    float* op[kPhysOutputs];
    for (int c = 0; c < kPhysInputs; ++c) ip[c] = in[c].data();
    for (int o = 0; o < kPhysOutputs; ++o) op[o] = out[o].data();
    mixer.process(ip, op, static_cast<int>(in[0].size()));
  }
  // Steady-state peak of an output (second half of the run).
  float tail(int o) const {
    const int n = static_cast<int>(out[o].size());
    return peakOf(out[o].data() + n / 2, n / 2);
  }
  // Sine amplitudes on inputs, then steady-state output peaks.
  std::array<float, kPhysOutputs> run(std::initializer_list<std::pair<int, float>> sines,
                                      double seconds = 0.5, double freq = 1000) {
    clear(static_cast<int>(kFs * seconds));
    for (auto [c, a] : sines) sine(c, a, freq);
    process();
    std::array<float, kPhysOutputs> p{};
    for (int o = 0; o < kPhysOutputs; ++o) p[o] = tail(o);
    return p;
  }
  // Index of the first sample above 0.5 on an output, or -1.
  int firstAbove(int o, float level = 0.5f) const {
    for (size_t i = 0; i < out[o].size(); ++i)
      if (std::fabs(out[o][i]) > level) return static_cast<int>(i);
    return -1;
  }
};

float db(float lin) { return linToDb(lin); }
constexpr float kM18 = 0.125893f;  // -18 dBFS = 0 dB on the Qu scale

}  // namespace

TEST(qu_gate_threshold_on_the_qu_scale) {
  Desk d;
  d.set("ch/1/pan", -1);
  d.set("ch/1/gate/on", 1);
  d.set("ch/1/gate/thresh", 0);   // 0 dB = -18 dBFS
  d.set("ch/1/gate/depth", 20);
  d.set("ch/1/gate/release", 10);
  // -10 dBFS (above): open. -30 dBFS (below): 20 dB down.
  CHECK_NEAR(db(d.run({{0, 0.316f}})[kOutL] / 0.316f), 0.0, 0.1);
  CHECK_NEAR(db(d.run({{0, 0.0316f}})[kOutL] / 0.0316f), -20.0, 0.5);
}

TEST(qu_compressor_infinite_ratio_limits_at_threshold) {
  Desk d;
  d.set("ch/1/pan", -1);
  d.set("ch/1/comp/on", 1);
  d.set("ch/1/comp/type", Compressor::kPeak);
  d.set("ch/1/comp/hard", 1);
  d.set("ch/1/comp/thresh", 0);  // -18 dBFS
  d.set("ch/1/comp/ratio", Compressor::kRatioInfinity);
  d.set("ch/1/comp/attack", 0.3f);
  // 12 and 6 dB over the threshold both come out at the threshold.
  CHECK_NEAR(db(d.run({{0, 0.5f}})[kOutL]), -18.0, 0.5);
  CHECK_NEAR(db(d.run({{0, 0.25f}})[kOutL]), -18.0, 0.5);
  d.set("ch/1/comp/ratio", 2);  // 2:1: 12 dB over comes out 6 dB over
  CHECK_NEAR(db(d.run({{0, 0.5f}})[kOutL]), -12.0, 0.5);
}

TEST(qu_ducker_turns_a_channel_down_under_its_trigger) {
  Desk d;
  d.set("ch/1/pan", -1);
  d.set("ch/2/lr", 0);
  d.set("ch/1/insert/type", ChannelDsp::kInsertDucker);
  d.set("ch/1/insert/on", 1);
  d.set("ch/1/duck/trigger", 2);  // CH 2 (the announcer)
  d.set("ch/1/duck/thresh", -20);
  d.set("ch/1/duck/depth", 15);
  // Trigger silent: unity. Trigger talking: 15 dB down.
  CHECK_NEAR(db(d.run({{0, 0.5f}})[kOutL] / 0.5f), 0.0, 0.1);
  CHECK_NEAR(db(d.run({{0, 0.5f}, {1, 0.5f}})[kOutL] / 0.5f), -15.0, 0.3);
  // Following the gang: the gang's trigger is CH 3, which is silent.
  d.set("ch/1/duck/gang", 1);
  d.set("duck/trigger", 3);
  d.set("duck/release", 250);
  d.run({{0, 0.5f}, {1, 0.5f}}, 3.0);
  CHECK_NEAR(db(d.tail(kOutL) / 0.5f), 0.0, 0.1);
  // Insert switched out: no ducking.
  d.set("ch/1/duck/gang", 0);
  d.set("ch/1/insert/on", 0);
  CHECK_NEAR(db(d.run({{0, 0.5f}, {1, 0.5f}})[kOutL] / 0.5f), 0.0, 0.1);
}

TEST(qu_fx_insert_and_ch_to_return_patching) {
  Desk d;
  d.set("ch/1/pan", -1);
  // FX1 as an insert on CH 1: a 10 ms delay, 100% wet.
  d.set("fx/1/patch", kFxInsert);
  d.set("fx/1/src", fxTargetForInput(0));
  d.set("fx/1/type", FxEngine::kDelay);
  d.set("fx/1/dly/time", 10);
  d.set("fx/1/dly/rtime", 10);
  d.set("fx/1/dly/feedback", 0);
  d.set("ch/1/insert/on", 1);
  d.clear(4800);
  d.in[0][1000] = 1.0f;
  d.process();
  CHECK(d.firstAbove(kOutL, 0.3f) == 1000 + 480);  // mono FX: (L+R)/2 of the wet signal
  d.set("fx/1/mix", 0);                            // dry
  d.clear(4800);
  d.in[0][1000] = 1.0f;
  d.process();
  CHECK(d.firstAbove(kOutL, 0.3f) == 1000);

  // FX2 Ch>Return from CH 3's direct out; the return comes back on FX Ret 2.
  Desk e;
  e.set("ch/3/lr", 0);
  e.set("fx/2/patch", kFxChReturn);
  e.set("fx/2/src", fxTargetForInput(2));
  e.set("fx/2/dly/time", 5);
  e.set("fx/2/dly/rtime", 5);
  e.set("fx/2/dly/feedback", 0);
  e.clear(4800);
  e.in[2][1000] = 1.0f;
  e.process();
  CHECK(e.firstAbove(kOutL, 0.3f) == 1000 + 240);
}

TEST(qu_direct_out_tap_follow_fader_mute_and_trim) {
  Desk d;
  int direct = 0;
  for (size_t i = 0; i < outputSources().size(); ++i)
    if (outputSources()[i].name == "Ch 1 DO") direct = static_cast<int>(i);
  d.set("out/16/src", direct);
  d.set("ch/1/eq/2/freq", 1000);
  d.set("ch/1/eq/2/gain", 6);
  d.set("ch/1/fader", -6.0206f);
  // Default: post delay (so post-EQ), following the fader.
  CHECK_NEAR(db(d.run({{0, 0.25f}})[15] / 0.25f), 0.0, 0.15);
  d.set("dout/fader", 0);
  CHECK_NEAR(db(d.run({{0, 0.25f}})[15] / 0.25f), 6.0, 0.15);
  d.set("dout/src", kDoPostPreamp);
  CHECK_NEAR(db(d.run({{0, 0.25f}})[15] / 0.25f), 0.0, 0.15);
  d.set("ch/1/dout/trim", -10);
  CHECK_NEAR(db(d.run({{0, 0.25f}})[15] / 0.25f), -10.0, 0.15);
  d.set("ch/1/mute", 1);
  CHECK_NEAR(d.run({{0, 0.25f}})[15], 0.0, 1e-6);
  d.set("dout/mute", 0);
  CHECK(d.run({{0, 0.25f}})[15] > 0.05f);
}

TEST(qu_pre_fade_mix_source_points) {
  Desk d;
  d.set("ch/1/gain", -6.0206f);
  d.set("ch/1/eq/2/freq", 1000);
  d.set("ch/1/eq/2/gain", 6);
  d.set("ch/1/mix/1/level", 0);  // pre-fade
  d.set("ch/1/fader", kMinusInfDb);
  d.set("mix/1/presrc", kSrcPostEq);
  CHECK_NEAR(db(d.run({{0, 0.5f}})[outMix(1)] / 0.5f), 0.0, 0.15);
  d.set("mix/1/presrc", kSrcPreEq);
  CHECK_NEAR(db(d.run({{0, 0.5f}})[outMix(1)] / 0.5f), -6.0, 0.15);
  d.set("ch/1/gain", 0);
  d.set("ch/1/delay/on", 1);
  d.set("ch/1/delay/time", 1);
  d.set("mix/1/presrc", kSrcPostAll);  // through the delay
  d.clear(4800);
  d.in[0][1000] = 0.5f;
  d.set("ch/1/eq/2/gain", 0);
  d.process();
  CHECK(d.firstAbove(outMix(1), 0.3f) == 1048);
  d.set("mix/1/presrc", kSrcPostPreamp);
  d.clear(4800);
  d.in[0][1000] = 0.5f;
  d.process();
  CHECK(d.firstAbove(outMix(1), 0.3f) == 1000);
}

TEST(qu_stereo_mix_send_pan_and_master_mute_groups) {
  Desk d;
  d.set("ch/1/mix/7/level", 0);  // Mix 9-10
  d.set("ch/1/mix/7/pan", -1);
  auto p = d.run({{0, 0.5f}});
  CHECK_NEAR(p[outMix(9)], 0.5, 0.002);
  CHECK_NEAR(p[outMix(10)], 0.0, 1e-6);
  // Mixes, FX sends and LR can be in mute groups.
  d.set("mix/7/mg/2", 1);
  d.set("mg/2/mute", 1);
  CHECK_NEAR(d.run({{0, 0.5f}})[outMix(9)], 0.0, 1e-6);
  d.set("lr/mg/2", 1);
  CHECK_NEAR(d.run({{0, 0.5f}})[kOutL], 0.0, 1e-6);
}

TEST(qu_dca_total_boost_is_capped_at_plus_10) {
  Desk d;
  d.set("ch/1/pan", -1);
  d.set("ch/1/dca/1", 1);
  d.set("ch/1/fader", 10);
  d.set("dca/1/fader", 10);
  CHECK_NEAR(db(d.run({{0, 0.05f}})[kOutL] / 0.05f), 10.0, 0.1);
  // A DCA at -inf turns the channel off post-fade, but pre-fade sends carry on.
  d.set("dca/1/fader", kMinusInfDb);
  d.set("ch/1/mix/1/level", 0);
  const auto p = d.run({{0, 0.05f}});
  CHECK_NEAR(p[kOutL], 0.0, 1e-6);
  CHECK_NEAR(p[outMix(1)], 0.05, 0.001);
}

TEST(qu_pafl_options) {
  Desk d;
  d.set("ch/1/pan", -1);
  d.set("ch/1/fader", -6.0206f);
  d.set("ch/1/pafl", 1);
  // Input PFL: pre-fader, both sides, with the PFL trim.
  auto p = d.run({{0, 0.5f}});
  CHECK_NEAR(p[kOutPaflL], 0.5, 0.002);
  CHECK_NEAR(p[kOutPaflR], 0.5, 0.002);
  d.set("pafl/trim", -6.0206f);
  CHECK_NEAR(d.run({{0, 0.5f}})[kOutPaflL], 0.25, 0.002);
  // Input AFL: post-fader and post-pan.
  d.set("pafl/inafl", 1);
  p = d.run({{0, 0.5f}});
  CHECK_NEAR(p[kOutPaflL], 0.25, 0.002);
  CHECK_NEAR(p[kOutPaflR], 0.0, 1e-6);
  // Output PFL (Output AFL off): mix master pre-fader, with the trim.
  d.set("ch/1/pafl", 0);
  d.set("pafl/trim", 0);
  d.set("pafl/outafl", 0);
  d.set("ch/1/mix/2/level", 0);
  d.set("mix/2/fader", kMinusInfDb);
  d.set("mix/2/pafl", 1);
  CHECK_NEAR(d.run({{0, 0.5f}})[kOutPaflL], 0.5, 0.002);
  // LR to PAFL off: silence when nothing is selected.
  d.set("mix/2/pafl", 0);
  d.set("pafl/lr", 0);
  CHECK_NEAR(d.run({{0, 0.5f}})[kOutPaflL], 0.0, 1e-6);
  d.set("pafl/lr", 1);
  // PAFL delay, sample exact.
  d.set("pafl/delay", 2);  // 96 samples
  d.set("ch/1/fader", 0);
  d.clear(4800);
  d.in[0][1000] = 1.0f;
  d.process();
  CHECK(d.firstAbove(kOutL) == 1000);
  CHECK(d.firstAbove(kOutPaflL) == 1096);
}

TEST(qu_linked_pair_shares_dynamics) {
  // CH 1 loud, CH 2 quiet, both gated at -36 dB (Qu): linked, CH 2 opens with CH 1.
  auto ch2 = [](bool linked) {
    Desk d;
    d.set("ch/1/lr", 0);
    d.set("ch/2/pan", 1);
    for (const char* ch : {"ch/1", "ch/2"}) {
      d.set(std::string(ch) + "/gate/on", 1);
      d.set(std::string(ch) + "/gate/thresh", -10);  // -28 dBFS
      d.set(std::string(ch) + "/gate/depth", 40);
      d.set(std::string(ch) + "/gate/release", 10);
    }
    d.set("link/1/on", linked);
    return db(d.run({{0, 0.5f}, {1, 0.01f}})[kOutR] / 0.01f);
  };
  CHECK_NEAR(ch2(false), -40.0, 0.5);
  CHECK_NEAR(ch2(true), 0.0, 0.1);
}

TEST(qu_amm_shares_gain) {
  Desk d;
  d.set("ch/1/pan", -1);
  d.set("ch/2/pan", 1);
  d.set("amm/1/on", 1);
  d.set("amm/2/on", 1);
  // One talker: full gain to that mic, the silent one turned right down.
  d.clear(static_cast<int>(kFs));
  d.sine(0, 0.3f, 1000);
  d.process();
  CHECK_NEAR(db(d.tail(kOutL) / 0.3f), 0.0, 0.2);
  MeterSnapshot s = d.mixer.takeMeters();
  CHECK(s.ammDb[1] < -40.0f);
  // Two equal talkers (uncorrelated tones): -3 dB each, constant total gain.
  d.clear(static_cast<int>(kFs));
  d.sine(0, 0.3f, 1000);
  d.sine(1, 0.3f, 1250);
  d.process();
  CHECK_NEAR(db(d.tail(kOutL) / 0.3f), -3.0, 0.4);
  CHECK_NEAR(db(d.tail(kOutR) / 0.3f), -3.0, 0.4);
  // Priority +10 dB on CH 1: it takes most of the gain.
  d.set("amm/1/prio", 10);
  d.process();
  CHECK(db(d.tail(kOutL) / 0.3f) > -0.6f);
  CHECK(db(d.tail(kOutR) / 0.3f) < -9.0f);
  // Pre-fade sends are not affected.
  d.set("ch/2/mix/1/level", 0);
  d.process();
  CHECK_NEAR(d.tail(outMix(1)), 0.3, 0.003);
  // AMM out: no gain sharing.
  d.set("amm/on", 0);
  d.process();
  CHECK_NEAR(db(d.tail(kOutR) / 0.3f), 0.0, 0.1);
}

TEST(qu_generator_band_pass_noise_and_talk_hpf) {
  Desk d;
  d.set("gen/type", 3);
  d.set("gen/freq", 2000);
  d.set("gen/lr", 1);
  d.mixer.takeRta();
  d.run({}, 1.0);
  const auto rta = d.mixer.takeRta();
  int best = 0;
  for (int k = 0; k < Rta::kBands; ++k)
    if (rta[k] > rta[best]) best = k;
  CHECK(Rta::kFreqs[best] == 2000.0f);
  CHECK(rta[best] - rta[best - 6] > 15.0f);  // 500 Hz, two octaves down
  d.set("gen/mute", 1);
  d.run({});
  CHECK_NEAR(d.run({})[kOutL], 0.0, 1e-6);

  // Talkback HPF frequency: a 200 Hz voice through a 1 kHz HPF (12 dB/oct) loses ~28 dB.
  d.set("talk/src", 1);
  d.set("talk/on", 1);
  d.set("talk/lr", 1);
  d.set("ch/1/lr", 0);
  d.set("talk/hpffreq", 1000);
  CHECK(db(d.run({{0, 0.5f}}, 0.5, 200)[kOutL] / 0.5f) < -25.0f);
  d.set("talk/hpf", 0);
  CHECK_NEAR(db(d.run({{0, 0.5f}}, 0.5, 200)[kOutL] / 0.5f), 0.0, 0.1);
}

TEST(qu_drive_multitrack_playback_into_usb_channels) {
  const auto dir = std::filesystem::temp_directory_path() / ("qnu16-drive-" + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  Recorder rec(dir.string());
  Player player(dir.string());
  std::string err;
  {
    Desk d;
    d.mixer.setRecorder(&rec);
    CHECK(rec.start(Recorder::kMultitrack, kFs, &err));
    d.run({{2, 0.5f}}, 1.0);  // CH 3 plays a tone
    rec.stop();
  }
  Desk d;
  d.mixer.setPlayer(&player);
  d.set("ch/3/pan", -1);
  d.set("ch/3/src", 1);  // USB
  CHECK(player.start("QU-MT001", kFs, &err));
  CHECK(player.playing() && player.tracks() == 18);
  CHECK_NEAR(player.length(), 1.0, 1e-6);
  d.run({}, 0.5);  // local inputs silent; CH 3 plays back from the drive
  CHECK_NEAR(d.tail(kOutL), 0.5, 0.001);
  d.run({}, 1.2);  // past the end: silence, and the player stops
  CHECK_NEAR(d.tail(kOutL), 0.0, 1e-6);
  CHECK(!player.playing());
  player.stop();
  // Back to local: the physical input again.
  d.set("ch/3/src", 0);
  CHECK_NEAR(d.run({{2, 0.25f}})[kOutL], 0.25, 0.001);
  std::filesystem::remove_all(dir);
}

TEST(qu_drive_stereo_playback_into_st3) {
  const auto dir = std::filesystem::temp_directory_path() / ("qnu16-st-" + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  Recorder rec(dir.string());
  Player player(dir.string());
  std::string err;
  {
    Desk d;
    d.mixer.setRecorder(&rec);
    d.set("ch/1/pan", -1);
    CHECK(rec.start(Recorder::kStereo, kFs, &err));
    d.run({{0, 0.5f}}, 0.5);
    rec.stop();
  }
  Desk d;
  d.mixer.setPlayer(&player);
  d.set("st/3/src", 1);
  CHECK(player.start("QU-ST001.WAV", kFs, &err));
  const auto p = d.run({}, 0.4);
  CHECK_NEAR(p[kOutL], 0.5, 0.001);
  CHECK_NEAR(p[kOutR], 0.0, 1e-6);
  CHECK(!player.start("../etc/passwd", kFs, &err));
  std::filesystem::remove_all(dir);
}
