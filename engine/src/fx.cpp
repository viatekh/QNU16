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
constexpr float kMaxModMs = 120.0f;  // ADT separation up to 100 ms plus wobble

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
  static const char* names[kNumTypes] = {"Reverb",    "Delay",   "ADT",    "Chorus",
                                         "Symphonic", "Flanger", "Phaser", "Gated Verb"};
  return t >= 0 && t < kNumTypes ? names[t] : "?";
}

const char* FxEngine::reverbModelName(int m) {
  static const char* names[kNumModels] = {"Classic", "Hall", "Room", "Plate"};
  return m >= 0 && m < kNumModels ? names[m] : "?";
}

namespace {
// Per reverb model: comb length scale (room size) and extra damping. Plate is small and bright,
// Hall large and dark.
const float kModelScale[FxEngine::kNumModels] = {0.85f, 1.0f, 0.55f, 0.7f};
const float kModelDamp[FxEngine::kNumModels] = {1.0f, 1.2f, 1.0f, 0.5f};
}  // namespace

void FxEngine::init(ParamRegistry& reg, const std::string& prefix, int defaultType,
                    int defaultModel, float defaultDecay) {
  const std::string b = prefix + "/";
  auto num = [&](const std::string& path, float lo, float hi, float def, const char* unit) {
    return reg.add({b + path, lo, hi, def, ParamKind::Continuous, unit});
  };
  type_ = reg.add({b + "type", 0, kNumTypes - 1, float(defaultType), ParamKind::Choice, ""});
  revModel_ = reg.add({b + "rev/model", 0, kNumModels - 1, float(defaultModel), ParamKind::Choice, ""});
  revDecay_ = num("rev/decay", 0.2f, 10, defaultDecay, "s");
  revPredelay_ = num("rev/predelay", 0, kMaxPredelayMs, 20, "ms");
  revDamp_ = num("rev/damp", 0, 100, 50, "%");  // HF decay
  revSize_ = num("rev/size", 50, 100, 100, "%");
  revLfCut_ = num("rev/lfcut", 20, 1000, 80, "Hz");
  revHfCut_ = num("rev/hfcut", 1000, 20000, 10000, "Hz");
  dlyTime_ = num("dly/time", 1, kMaxDelayMs, 375, "ms");    // left tap
  dlyTimeR_ = num("dly/rtime", 1, kMaxDelayMs, 375, "ms");  // right tap
  dlyFeedback_ = num("dly/feedback", 0, 95, 35, "%");
  dlyHiCut_ = num("dly/hicut", 1000, 20000, 6000, "Hz");
  dlyPingPong_ = reg.add({b + "dly/pingpong", 0, 1, 0, ParamKind::Toggle, ""});
  adtSep_ = num("adt/sep", 5, 100, 25, "ms");
  adtThick_ = num("adt/thick", 0, 100, 50, "%");
  choRate_ = num("cho/rate", 0.05f, 5, 0.8f, "Hz");
  choDepth_ = num("cho/depth", 0, 10, 3, "ms");
  choDelay_ = num("cho/delay", 5, 30, 12, "ms");
  symRate_ = num("sym/rate", 0.05f, 5, 0.5f, "Hz");
  symDepth_ = num("sym/depth", 0, 10, 4, "ms");
  flgRate_ = num("flg/rate", 0.05f, 5, 0.25f, "Hz");
  flgDepth_ = num("flg/depth", 0, 5, 2, "ms");
  flgDelay_ = num("flg/delay", 0.5f, 10, 1, "ms");
  flgFeedback_ = num("flg/feedback", 0, 90, 60, "%");
  phaRate_ = num("pha/rate", 0.05f, 5, 0.4f, "Hz");
  phaDepth_ = num("pha/depth", 0, 100, 70, "%");
  phaFeedback_ = num("pha/feedback", 0, 90, 40, "%");
  phaStages_ = reg.add({b + "pha/stages", 0, 3, 3, ParamKind::Choice, ""});  // 4, 6, 8, 12
  gvbThresh_ = num("gvb/thresh", -72, 18, -20, "dB");
  gvbHold_ = num("gvb/hold", 10, 1000, 250, "ms");
  gvbRelease_ = num("gvb/release", 5, 500, 60, "ms");

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
  cutCache_.fill(-1.0f);
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
  for (auto& b : revLf_) b.reset();
  for (auto& b : revHf_) b.reset();
  for (auto& side : phX_) side.fill(0.0f);
  for (auto& side : phY_) side.fill(0.0f);
  phFb_.fill(0.0f);
  gvbEnv_ = gvbGain_ = 0.0f;
  gvbHoldCount_ = 0;
}

void FxEngine::process(const float* in, float* outL, float* outR, int n) {
  const int t = type_->index();
  if (t != activeType_) {
    reset();  // clear the old effect's tail so switching type never bursts noise
    activeType_ = t;
  }
  switch (t) {
    case kReverb:
      processReverb(in, outL, outR, n);
      reverbCuts(outL, outR, n);
      break;
    case kDelay: processDelay(in, outL, outR, n); break;
    case kAdt:  // double tracking: two slowly wandering short delays, one per side
      processMod(in, outL, outR, n, 0.3f, adtThick_->get() * 0.02f, adtSep_->get(), 0.0f, kPi, 1.0f);
      break;
    case kChorus:
      processMod(in, outL, outR, n, choRate_->get(), choDepth_->get(), choDelay_->get(), 0.0f,
                 kPi * 0.5, 0.0f);
      break;
    case kSymphonic:  // wide: sides in anti-phase, slow wobble on top
      processMod(in, outL, outR, n, symRate_->get(), symDepth_->get(), 8.0f, 0.0f, kPi, 0.3f);
      break;
    case kFlanger:
      processMod(in, outL, outR, n, flgRate_->get(), flgDepth_->get(), flgDelay_->get(),
                 flgFeedback_->get() / 100.0f, kPi * 0.5, 0.0f);
      break;
    case kPhaser: processPhaser(in, outL, outR, n); break;
    case kGatedVerb:
      processGatedVerb(in, outL, outR, n);
      reverbCuts(outL, outR, n);
      break;
    default:
      std::fill_n(outL, n, 0.0f);
      std::fill_n(outR, n, 0.0f);
  }
}

void FxEngine::updateReverb() {
  const std::array<float, 4> v = {revDecay_->get(), revDamp_->get(), revSize_->get(),
                                  revModel_->get()};
  if (v == revCache_) return;
  revCache_ = v;
  const int model = std::clamp(static_cast<int>(v[3]), 0, kNumModels - 1);
  const float size = v[2] / 100.0f * kModelScale[model];
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
  revDampCoef_ = std::min(0.9f, v[1] / 100.0f * 0.6f * kModelDamp[model]);
}

void FxEngine::reverbCuts(float* outL, float* outR, int n) {
  const std::array<float, 2> c = {revLfCut_->get(), revHfCut_->get()};
  if (c != cutCache_) {
    for (auto& b : revLf_) b.set(Biquad::Type::HighPass, fs_, c[0], 0.0, 0.7071);
    for (auto& b : revHf_) b.set(Biquad::Type::LowPass, fs_, c[1], 0.0, 0.7071);
    cutCache_ = c;
  }
  Biquad::processPair(revLf_[0], outL, revLf_[1], outR, n);
  Biquad::processPair(revHf_[0], outL, revHf_[1], outR, n);
}

void FxEngine::processGatedVerb(const float* in, float* outL, float* outR, int n) {
  processReverb(in, outL, outR, n);
  // The gate opens instantly on the input crossing the threshold, holds, then closes quickly,
  // cutting the dense reverb off: the 80s gated snare sound.
  const float thresh = dbToLin(gvbThresh_->get() + kQuRefDbfs);
  const int hold = static_cast<int>(gvbHold_->get() * 0.001 * fs_);
  const float rel = timeToCoef(gvbRelease_->get(), static_cast<float>(fs_));
  const float det = timeToCoef(5.0f, static_cast<float>(fs_));
  for (int i = 0; i < n; ++i) {
    const float a = std::fabs(in[i]);
    gvbEnv_ = a > gvbEnv_ ? a : gvbEnv_ * det;
    if (gvbEnv_ >= thresh) gvbHoldCount_ = hold;
    float target = 0.0f;
    if (gvbHoldCount_ > 0) {
      --gvbHoldCount_;
      target = 1.0f;
    }
    gvbGain_ = target > gvbGain_ ? target : target + (gvbGain_ - target) * rel;
    outL[i] *= gvbGain_;
    outR[i] *= gvbGain_;
  }
}

void FxEngine::processPhaser(const float* in, float* outL, float* outR, int n) {
  static const int kStages[4] = {4, 6, 8, 12};
  const int stages = kStages[std::clamp(phaStages_->index(), 0, 3)];
  const double inc = 2.0 * kPi * phaRate_->get() / fs_;
  const float depth = phaDepth_->get() / 100.0f;
  const float fb = phaFeedback_->get() / 100.0f;
  // The allpass break frequency sweeps from 200 Hz up to 200 Hz * 2^(depth * 5) (6.4 kHz).
  const float fLo = 200.0f;
  for (int i = 0; i < n; ++i) {
    float out[2];
    for (int s = 0; s < 2; ++s) {
      const double lfo = 0.5 + 0.5 * std::sin(lfoPhase_ + s * kPi * 0.5);
      const float f = fLo * std::exp2(depth * 5.0f * static_cast<float>(lfo));
      const float t = std::tan(static_cast<float>(kPi) * f / static_cast<float>(fs_));
      const float g = (t - 1.0f) / (t + 1.0f);
      float x = in[i] + phFb_[s] * fb;
      for (int k = 0; k < stages; ++k) {
        const float y = g * x + phX_[s][k] - g * phY_[s][k];
        phX_[s][k] = x;
        phY_[s][k] = y;
        x = y;
      }
      phFb_[s] = x;
      out[s] = 0.5f * (in[i] + x);  // the wet path includes the dry for the notches
    }
    lfoPhase_ += inc;
    if (lfoPhase_ > 2.0 * kPi) lfoPhase_ -= 2.0 * kPi;
    outL[i] = out[0];
    outR[i] = out[1];
  }
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
  const int dl = std::clamp(static_cast<int>(dlyTime_->get() * 0.001 * fs_), 1, size - 1);
  const int dr = std::clamp(static_cast<int>(dlyTimeR_->get() * 0.001 * fs_), 1, size - 1);
  const float fb = dlyFeedback_->get() / 100.0f;
  const float lp = 1.0f - std::exp(-2.0f * static_cast<float>(kPi) * dlyHiCut_->get() / static_cast<float>(fs_));
  const bool ping = dlyPingPong_->on();
  for (int i = 0; i < n; ++i) {
    int rl = dlyPos_ - dl, rr = dlyPos_ - dr;
    if (rl < 0) rl += size;
    if (rr < 0) rr += size;
    const float yl = dlyBuf_[0][rl], yr = dlyBuf_[1][rr];
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

void FxEngine::processMod(const float* in, float* outL, float* outR, int n, float rate,
                          float depthMs, float baseMs, float fb, double rightPhase, float wobble) {
  const int size = static_cast<int>(modBuf_[0].size());
  const double inc = 2.0 * kPi * rate / fs_;
  const float msToSamples = static_cast<float>(fs_ * 0.001);
  for (int i = 0; i < n; ++i) {
    float out[2];
    for (int s = 0; s < 2; ++s) {
      // A second, slower and incommensurate LFO adds the irregular drift of tape (ADT) or
      // the extra movement of the symphonic chorus.
      const double ph = lfoPhase_ + s * rightPhase;
      double lfo = 0.5 + 0.5 * std::sin(ph);
      if (wobble > 0.0f) lfo = (1.0 - wobble) * lfo + wobble * (0.5 + 0.5 * std::sin(ph * 0.37 + 1.3 * s));
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
    // Wrap after 100 cycles, where both LFOs (1 and 0.37 x rate) complete whole cycles.
    if (lfoPhase_ > 200.0 * kPi) lfoPhase_ -= 200.0 * kPi;
    outL[i] = out[0];
    outR[i] = out[1];
  }
}

}  // namespace qnu
