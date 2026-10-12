#include "fx.h"

#include "dsp/util.h"

namespace qnu {

namespace {

// Freeverb tunings at 44.1 kHz; the right channel is offset by kStereoSpread for decorrelation.
const int kCombTuning[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
const int kAllpassTuning[4] = {556, 441, 341, 225};
constexpr int kStereoSpread = 23;
constexpr float kRevInputGain = 0.015f;
constexpr float kRevOutputGain = 3.0f;
constexpr float kMaxPredelayMs = 200.0f;
constexpr float kMaxDelayMs = 2000.0f;
constexpr float kMaxModMs = 40.0f;

int scaled(int samples44k, double fs) { return std::max(1, static_cast<int>(samples44k * fs / 44100.0)); }

float readInterp(const std::vector<float>& buf, int writePos, float delaySamples) {
  const int size = static_cast<int>(buf.size());
  float rd = static_cast<float>(writePos) - delaySamples;
  while (rd < 0) rd += static_cast<float>(size);
  const int i0 = static_cast<int>(rd);
  const int i1 = i0 + 1 == size ? 0 : i0 + 1;
  const float frac = rd - static_cast<float>(i0);
  return buf[i0] + (buf[i1] - buf[i0]) * frac;
}

}  // namespace

const char* FxEngine::typeName(int t) {
  static const char* names[kNumTypes] = {"Reverb", "Delay", "Chorus", "Flanger"};
  return t >= 0 && t < kNumTypes ? names[t] : "?";
}

void FxEngine::init(ParamRegistry& reg, const std::string& prefix, int defaultType,
                    float defaultDecay) {
  const std::string b = prefix + "/";
  auto num = [&](const std::string& path, float lo, float hi, float def, const char* unit) {
    return reg.add({b + path, lo, hi, def, ParamKind::Continuous, unit});
  };
  type_ = reg.add({b + "type", 0, kNumTypes - 1, float(defaultType), ParamKind::Choice, ""});
  revDecay_ = num("rev/decay", 0.2f, 10, defaultDecay, "s");
  revPredelay_ = num("rev/predelay", 0, kMaxPredelayMs, 20, "ms");
  revDamp_ = num("rev/damp", 0, 100, 50, "%");
  revSize_ = num("rev/size", 50, 100, 100, "%");
  dlyTime_ = num("dly/time", 1, kMaxDelayMs, 375, "ms");
  dlyFeedback_ = num("dly/feedback", 0, 95, 35, "%");
  dlyHiCut_ = num("dly/hicut", 1000, 20000, 6000, "Hz");
  dlyPingPong_ = reg.add({b + "dly/pingpong", 0, 1, 1, ParamKind::Toggle, ""});
  choRate_ = num("cho/rate", 0.05f, 5, 0.8f, "Hz");
  choDepth_ = num("cho/depth", 0, 10, 3, "ms");
  choDelay_ = num("cho/delay", 5, 30, 12, "ms");
  flgRate_ = num("flg/rate", 0.05f, 5, 0.25f, "Hz");
  flgDepth_ = num("flg/depth", 0, 5, 2, "ms");
  flgDelay_ = num("flg/delay", 0.5f, 10, 1, "ms");
  flgFeedback_ = num("flg/feedback", 0, 90, 60, "%");

  // Allocate for the largest supported sample rate.
  const double fsMax = kMaxSampleRate;
  for (int s = 0; s < 2; ++s) {
    for (int c = 0; c < 8; ++c) combs_[s][c].buf.assign(scaled(kCombTuning[c] + kStereoSpread, fsMax) + 1, 0.0f);
    for (int a = 0; a < 4; ++a)
      allpasses_[s][a].buf.assign(scaled(kAllpassTuning[a] + kStereoSpread, fsMax) + 1, 0.0f);
    dlyBuf_[s].assign(static_cast<size_t>(kMaxDelayMs * 0.001 * fsMax) + 2, 0.0f);
    modBuf_[s].assign(static_cast<size_t>(kMaxModMs * 0.001 * fsMax) + 4, 0.0f);
  }
  predelay_.assign(static_cast<size_t>(kMaxPredelayMs * 0.001 * fsMax) + 2, 0.0f);
}

void FxEngine::prepare(double sampleRate) {
  fs_ = std::min(sampleRate, kMaxSampleRate);
  revCache_.fill(-1.0f);
  reset();
}

void FxEngine::reset() {
  for (int s = 0; s < 2; ++s) {
    for (auto& c : combs_[s]) {
      std::fill(c.buf.begin(), c.buf.end(), 0.0f);
      c.pos = 0;
      c.store = 0.0f;
    }
    for (auto& a : allpasses_[s]) {
      std::fill(a.buf.begin(), a.buf.end(), 0.0f);
      a.pos = 0;
    }
    std::fill(dlyBuf_[s].begin(), dlyBuf_[s].end(), 0.0f);
    std::fill(modBuf_[s].begin(), modBuf_[s].end(), 0.0f);
  }
  std::fill(predelay_.begin(), predelay_.end(), 0.0f);
  predelayPos_ = dlyPos_ = modPos_ = 0;
  dlyLp_.fill(0.0f);
  modFb_.fill(0.0f);
  lfoPhase_ = 0.0;
}

void FxEngine::process(const float* in, float* outL, float* outR, int n) {
  const int t = type_->index();
  if (t != activeType_) {
    reset();  // clear the old effect's tail so switching type never bursts noise
    activeType_ = t;
  }
  switch (t) {
    case kReverb: processReverb(in, outL, outR, n); break;
    case kDelay: processDelay(in, outL, outR, n); break;
    case kChorus: processMod(in, outL, outR, n, false); break;
    case kFlanger: processMod(in, outL, outR, n, true); break;
    default:
      std::fill_n(outL, n, 0.0f);
      std::fill_n(outR, n, 0.0f);
  }
}

void FxEngine::updateReverb() {
  const std::array<float, 3> v = {revDecay_->get(), revDamp_->get(), revSize_->get()};
  if (v == revCache_) return;
  revCache_ = v;
  const float size = v[2] / 100.0f;
  double avgLen = 0;
  for (int s = 0; s < 2; ++s) {
    for (int c = 0; c < 8; ++c) {
      Comb& comb = combs_[s][c];
      comb.len = std::min(scaled(static_cast<int>((kCombTuning[c] + s * kStereoSpread) * size), fs_),
                          static_cast<int>(comb.buf.size()));
      comb.pos %= comb.len;
      avgLen += comb.len;
    }
    for (int a = 0; a < 4; ++a) {
      Allpass& ap = allpasses_[s][a];
      ap.len = std::min(scaled(kAllpassTuning[a] + s * kStereoSpread, fs_), static_cast<int>(ap.buf.size()));
      ap.pos %= ap.len;
    }
  }
  avgLen /= 16.0;
  // Comb feedback for the requested RT60: each pass through a comb of length L loses
  // 60 dB * L / (RT60 * fs).
  revFeedback_ = std::min(0.985f, static_cast<float>(std::pow(10.0, -3.0 * avgLen / (v[0] * fs_))));
  revDampCoef_ = v[1] / 100.0f * 0.6f;
}

void FxEngine::processReverb(const float* in, float* outL, float* outR, int n) {
  updateReverb();
  const int pdSize = static_cast<int>(predelay_.size());
  const int pd = std::min(static_cast<int>(revPredelay_->get() * 0.001 * fs_), pdSize - 1);
  const float damp1 = revDampCoef_, damp2 = 1.0f - revDampCoef_;
  for (int i = 0; i < n; ++i) {
    predelay_[predelayPos_] = in[i];
    int rd = predelayPos_ - pd;
    if (rd < 0) rd += pdSize;
    const float x = predelay_[rd] * kRevInputGain;
    if (++predelayPos_ == pdSize) predelayPos_ = 0;

    float out[2];
    for (int s = 0; s < 2; ++s) {
      float acc = 0.0f;
      for (auto& c : combs_[s]) {
        const float y = c.buf[c.pos];
        c.store = y * damp2 + c.store * damp1;
        c.buf[c.pos] = x + c.store * revFeedback_;
        if (++c.pos >= c.len) c.pos = 0;
        acc += y;
      }
      for (auto& a : allpasses_[s]) {
        const float b = a.buf[a.pos];
        a.buf[a.pos] = acc + b * 0.5f;
        acc = b - acc;
        if (++a.pos >= a.len) a.pos = 0;
      }
      out[s] = acc * kRevOutputGain;
    }
    outL[i] = out[0];
    outR[i] = out[1];
  }
}

void FxEngine::processDelay(const float* in, float* outL, float* outR, int n) {
  const int size = static_cast<int>(dlyBuf_[0].size());
  const int d = std::clamp(static_cast<int>(dlyTime_->get() * 0.001 * fs_), 1, size - 1);
  const float fb = dlyFeedback_->get() / 100.0f;
  const float lp = 1.0f - std::exp(-2.0f * static_cast<float>(kPi) * dlyHiCut_->get() / static_cast<float>(fs_));
  const bool ping = dlyPingPong_->on();
  for (int i = 0; i < n; ++i) {
    int rd = dlyPos_ - d;
    if (rd < 0) rd += size;
    const float yl = dlyBuf_[0][rd], yr = dlyBuf_[1][rd];
    dlyLp_[0] += (yl - dlyLp_[0]) * lp;
    dlyLp_[1] += (yr - dlyLp_[1]) * lp;
    if (ping) {  // input enters left; repeats bounce L -> R -> L
      dlyBuf_[0][dlyPos_] = in[i] + dlyLp_[1] * fb;
      dlyBuf_[1][dlyPos_] = dlyLp_[0] * fb;
    } else {
      dlyBuf_[0][dlyPos_] = in[i] + dlyLp_[0] * fb;
      dlyBuf_[1][dlyPos_] = in[i] + dlyLp_[1] * fb;
    }
    outL[i] = yl;
    outR[i] = yr;
    if (++dlyPos_ == size) dlyPos_ = 0;
  }
}

void FxEngine::processMod(const float* in, float* outL, float* outR, int n, bool flanger) {
  const float rate = (flanger ? flgRate_ : choRate_)->get();
  const float depthMs = (flanger ? flgDepth_ : choDepth_)->get();
  const float baseMs = (flanger ? flgDelay_ : choDelay_)->get();
  const float fb = flanger ? flgFeedback_->get() / 100.0f : 0.0f;
  const int size = static_cast<int>(modBuf_[0].size());
  const double inc = 2.0 * kPi * rate / fs_;
  const float msToSamples = static_cast<float>(fs_ * 0.001);
  for (int i = 0; i < n; ++i) {
    float out[2];
    for (int s = 0; s < 2; ++s) {
      // Left and right LFOs in quadrature for stereo width.
      const double lfo = 0.5 + 0.5 * std::sin(lfoPhase_ + s * kPi * 0.5);
      const float dSamples = std::min((baseMs + depthMs * static_cast<float>(lfo)) * msToSamples,
                                      static_cast<float>(size - 2));
      const float y = readInterp(modBuf_[s], modPos_, std::max(dSamples, 1.0f));
      modFb_[s] = y;
      out[s] = y;
    }
    modBuf_[0][modPos_] = in[i] + modFb_[0] * fb;
    modBuf_[1][modPos_] = in[i] + modFb_[1] * fb;
    if (++modPos_ == size) modPos_ = 0;
    lfoPhase_ += inc;
    if (lfoPhase_ > 2.0 * kPi) lfoPhase_ -= 2.0 * kPi;
    outL[i] = out[0];
    outR[i] = out[1];
  }
}

}  // namespace qnu
