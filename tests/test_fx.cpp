#include <vector>

#include "mixer.h"
#include "test.h"

using namespace qnu;

namespace {

constexpr double kFs = 48000.0;

struct FxRig {
  ParamRegistry reg;
  FxEngine fx;
  explicit FxRig(int type) {
    fx.init(reg, "fx/1", type, FxEngine::kHall, 2.0f);
    fx.prepare(kFs);
  }
  void set(const std::string& p, float v) { reg.find("fx/1/" + p)->set(v); }
  // Impulse in, `seconds` of output.
  void impulse(double seconds, std::vector<float>* l, std::vector<float>* r) {
    const int n = static_cast<int>(kFs * seconds);
    std::vector<float> in(n, 0.0f);
    in[0] = 1.0f;
    l->assign(n, 0.0f);
    r->assign(n, 0.0f);
    for (int i = 0; i < n; i += 256) {
      const int k = std::min(256, n - i);
      fx.process(in.data() + i, l->data() + i, r->data() + i, k);
    }
  }
};

double energy(const std::vector<float>& v, size_t from, size_t to) {
  double e = 0;
  for (size_t i = from; i < to && i < v.size(); ++i) e += double(v[i]) * v[i];
  return e;
}

}  // namespace

TEST(fx_reverb_tail_follows_decay_time) {
  FxRig shortRev(FxEngine::kReverb), longRev(FxEngine::kReverb);
  shortRev.set("rev/decay", 0.5f);
  longRev.set("rev/decay", 4.0f);
  std::vector<float> sl, sr, ll, lr;
  shortRev.impulse(2.0, &sl, &sr);
  longRev.impulse(2.0, &ll, &lr);
  const size_t a = static_cast<size_t>(0.8 * kFs), b = static_cast<size_t>(1.2 * kFs);
  CHECK(energy(ll, a, b) > 100.0 * energy(sl, a, b));  // long reverb still ringing
  CHECK(energy(ll, 0, ll.size()) > 0.0);
  // Decorrelated stereo output.
  double diff = 0;
  for (size_t i = 0; i < ll.size(); ++i) diff += std::fabs(ll[i] - lr[i]);
  CHECK(diff > 1.0);
  // Stable: no runaway.
  CHECK(peakOf(ll.data(), static_cast<int>(ll.size())) < 2.0f);
}

TEST(fx_reverb_predelay) {
  FxRig rig(FxEngine::kReverb);
  rig.set("rev/predelay", 50);
  std::vector<float> l, r;
  rig.impulse(0.2, &l, &r);
  // Nothing comes out before predelay + the shortest comb.
  CHECK(peakOf(l.data(), static_cast<int>(0.05 * kFs)) == 0.0f);
  CHECK(peakOf(l.data(), static_cast<int>(l.size())) > 0.0f);
}

TEST(fx_delay_echo_timing_and_pingpong) {
  FxRig rig(FxEngine::kDelay);
  rig.set("dly/time", 100);
  rig.set("dly/rtime", 100);
  rig.set("dly/feedback", 50);
  rig.set("dly/hicut", 20000);
  rig.set("dly/pingpong", 1);
  std::vector<float> l, r;
  rig.impulse(0.5, &l, &r);
  auto peakAt = [](const std::vector<float>& v) {
    size_t best = 0;
    for (size_t i = 0; i < v.size(); ++i)
      if (std::fabs(v[i]) > std::fabs(v[best])) best = i;
    return best;
  };
  CHECK(peakAt(l) == 4800u);           // first echo on the left at 100 ms
  CHECK(std::fabs(r[9600]) > 0.3f);    // second bounces to the right at 200 ms
  CHECK(std::fabs(l[9600]) < 1e-6f);
}

TEST(fx_chorus_and_flanger_produce_modulated_output) {
  for (int type : {FxEngine::kChorus, FxEngine::kFlanger}) {
    FxRig rig(type);
    const int n = static_cast<int>(kFs);
    std::vector<float> in(n), l(n), r(n);
    for (int i = 0; i < n; ++i) in[i] = 0.5f * static_cast<float>(std::sin(2 * kPi * 440 * i / kFs));
    for (int i = 0; i < n; i += 256)
      rig.fx.process(in.data() + i, l.data() + i, r.data() + i, std::min(256, n - i));
    CHECK(peakOf(l.data() + n / 2, n / 2) > 0.1f);
    CHECK(peakOf(l.data(), n) < 3.0f);
    double diff = 0;
    for (int i = n / 2; i < n; ++i) diff += std::fabs(l[i] - r[i]);
    CHECK(diff > 1.0);  // L/R LFOs in quadrature
  }
}

TEST(fx_send_to_return_path_through_mixer) {
  ParamRegistry reg;
  Mixer m(reg);
  m.prepare(kFs);
  reg.find("ch/1/lr")->set(0);           // dry signal off LR
  reg.find("ch/1/fader")->set(0);
  reg.find("fx/2/fader")->set(0);        // FX2 send master
  reg.find("lr/fader")->set(0);
  reg.find("ch/1/fx/2/level")->set(0);   // FX2 send bus -> FX2 engine (Mix>Return)
  reg.find("fx/2/type")->set(FxEngine::kDelay);
  reg.find("fx/2/dly/time")->set(50);
  reg.find("fx/2/dly/rtime")->set(50);
  reg.find("fx/2/dly/pingpong")->set(0);
  const int n = 9600;
  const int t0 = 512;  // after the startup gain ramps have settled
  std::vector<float> in(n, 0.0f), outL(n, 0.0f);
  in[t0] = 1.0f;
  const float* ip[kPhysInputs] = {in.data()};
  float* op[kPhysOutputs] = {outL.data()};
  m.process(ip, op, n);
  // Echo at 50 ms through FX return 2 (unity fader, centre balance), nothing before it.
  CHECK(peakOf(outL.data(), t0 + 2399) == 0.0f);
  CHECK(std::fabs(outL[t0 + 2400]) > 0.5f);
}

TEST(fx_type_switch_clears_state) {
  FxRig rig(FxEngine::kReverb);
  std::vector<float> l, r;
  rig.impulse(0.3, &l, &r);
  rig.set("type", FxEngine::kDelay);
  std::vector<float> silence(256, 0.0f), ol(256), orr(256);
  rig.fx.process(silence.data(), ol.data(), orr.data(), 256);
  CHECK(peakOf(ol.data(), 256) == 0.0f);  // no reverb tail leaking into the delay
}

namespace {

// Steady-state output peak (left) for a sine through an FX rig.
float sineThrough(FxRig& rig, double freq, double seconds = 1.0) {
  const int n = static_cast<int>(kFs * seconds);
  std::vector<float> in(n), l(n), r(n);
  for (int i = 0; i < n; ++i) in[i] = 0.5f * static_cast<float>(std::sin(2 * kPi * freq * i / kFs));
  for (int i = 0; i < n; i += 256) {
    const int k = std::min(256, n - i);
    rig.fx.process(in.data() + i, l.data() + i, r.data() + i, k);
  }
  return peakOf(l.data() + n / 2, n / 2);
}

}  // namespace

TEST(fx_delay_left_and_right_taps) {
  FxRig rig(FxEngine::kDelay);
  rig.set("dly/time", 100);
  rig.set("dly/rtime", 150);
  rig.set("dly/feedback", 0);
  std::vector<float> l, r;
  rig.impulse(0.3, &l, &r);
  CHECK(std::fabs(l[4800]) > 0.9f && std::fabs(r[7200]) > 0.9f);
  CHECK(std::fabs(r[4800]) < 1e-6f && std::fabs(l[7200]) < 1e-6f);
}

TEST(fx_reverb_models_and_cuts) {
  // A Room is smaller than a Hall: its first reflections arrive earlier.
  FxRig hall(FxEngine::kReverb), room(FxEngine::kReverb);
  room.set("rev/model", FxEngine::kRoom);
  hall.set("rev/predelay", 0);
  room.set("rev/predelay", 0);
  std::vector<float> hl, hr, rl, rr;
  hall.impulse(0.2, &hl, &hr);
  room.impulse(0.2, &rl, &rr);
  auto first = [](const std::vector<float>& v) {
    for (size_t i = 0; i < v.size(); ++i)
      if (std::fabs(v[i]) > 1e-4f) return i;
    return v.size();
  };
  CHECK(first(rl) < first(hl));
  // The HF cut takes the top off: 8 kHz through a 1 kHz cut is far quieter than through 20 kHz.
  FxRig bright(FxEngine::kReverb), dark(FxEngine::kReverb);
  bright.set("rev/hfcut", 20000);
  bright.set("rev/damp", 0);
  dark.set("rev/hfcut", 1000);
  dark.set("rev/damp", 0);
  CHECK(linToDb(sineThrough(dark, 8000) / sineThrough(bright, 8000)) < -20.0f);
}

TEST(fx_phaser_notches_and_gated_verb_cuts_off) {
  // A static phaser (no depth): the 4-stage allpass chain plus dry cancels at its notch.
  FxRig ph(FxEngine::kPhaser);
  ph.set("pha/depth", 0);
  ph.set("pha/feedback", 0);
  // 4 first-order stages at 200 Hz: each shifts -2 atan(f / 200 Hz), so the total reaches -180
  // degrees (a notch against the dry signal) at 200 * tan(22.5 deg) = 82.8 Hz.
  ph.set("pha/stages", 0);
  CHECK(sineThrough(ph, 82.84, 2.0) < 0.02f);
  CHECK(sineThrough(ph, 3000) > 0.2f);

  // Gated verb: dense while the input is loud, then cut off after the hold.
  FxRig gv(FxEngine::kGatedVerb);
  gv.set("rev/decay", 5);
  gv.set("rev/predelay", 0);
  gv.set("gvb/hold", 100);
  gv.set("gvb/release", 5);
  const int n = static_cast<int>(kFs * 0.5);
  std::vector<float> in(n, 0.0f), l(n), r(n);
  for (int i = 0; i < 480; ++i) in[i] = 0.5f * static_cast<float>(std::sin(2 * kPi * 500 * i / kFs));
  for (int i = 0; i < n; i += 256) gv.fx.process(in.data() + i, l.data() + i, r.data() + i, std::min(256, n - i));
  CHECK(peakOf(l.data() + 2400, 2400) > 0.01f);                         // 50-100 ms: open
  CHECK(peakOf(l.data() + static_cast<int>(kFs * 0.2), 4800) < 1e-4f);  // 200 ms on: closed
}

TEST(fx_adt_and_symphonic_are_wide_and_moving) {
  for (int type : {FxEngine::kAdt, FxEngine::kSymphonic}) {
    FxRig rig(type);
    const int n = static_cast<int>(kFs);
    std::vector<float> in(n), l(n), r(n);
    for (int i = 0; i < n; ++i) in[i] = 0.5f * static_cast<float>(std::sin(2 * kPi * 440 * i / kFs));
    for (int i = 0; i < n; i += 256) rig.fx.process(in.data() + i, l.data() + i, r.data() + i, std::min(256, n - i));
    CHECK(peakOf(l.data() + n / 2, n / 2) > 0.3f);
    double diff = 0;
    for (int i = n / 2; i < n; ++i) diff += std::fabs(l[i] - r[i]);
    CHECK(diff > 100.0);  // the two sides differ
  }
}
