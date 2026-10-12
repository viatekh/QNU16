#include "strip.h"

#include <cstring>

namespace qnu {

namespace {

// Reset defaults, as the Qu's FX return / channel PEQ after Reset Mix Settings: flat, 3/4 octave.
const float kEqDefaultFreq[kEqBands] = {63.0f, 200.0f, 2500.0f, 10000.0f};
constexpr float kEqDefaultWidth = 0.75f;
constexpr float kEqWidestBell = 1.5f;  // LF/HF widths beyond this are shelves

// Qu-style bell width in octaves (1.5 wide ... 1/9 narrow) to biquad Q.
double widthToQ(double octaves) {
  const double p = std::pow(2.0, octaves);
  return std::sqrt(p) / (p - 1.0);
}
constexpr double kShelfQ = 0.7071;

std::string n1(int i) { return std::to_string(i + 1); }

void copyTap(const ChannelDsp::Taps* taps, int t, const float* l, const float* r, int n) {
  if (!taps) return;
  if (taps->l[t]) std::memcpy(taps->l[t], l, sizeof(float) * n);
  if (taps->r[t] && r) std::memcpy(taps->r[t], r, sizeof(float) * n);
}

}  // namespace

void ChannelDsp::init(ParamRegistry& reg, const std::string& prefix, unsigned sections,
                      bool stereo, float maxDelayMs, const DuckGang* gang) {
  sections_ = sections;
  stereo_ = stereo;
  gang_ = gang;
  const std::string b = prefix + "/";
  auto num = [&](const std::string& path, float lo, float hi, float def, const char* unit) {
    return reg.add({b + path, lo, hi, def, ParamKind::Continuous, unit});
  };
  auto tog = [&](const std::string& path, bool def) {
    return reg.add({b + path, 0.0f, 1.0f, def ? 1.0f : 0.0f, ParamKind::Toggle, ""});
  };
  auto choice = [&](const std::string& path, int count, int def) {
    return reg.add({b + path, 0.0f, float(count - 1), float(def), ParamKind::Choice, ""});
  };

  if (sections & kPreamp) {
    gain_ = num("gain", -24, 24, 0, "dB");
    polarity_ = tog("polarity", false);
  }
  if (sections & kInsert) insertOn_ = tog("insert/on", false);
  if (sections & kDucker) {
    insertType_ = choice("insert/type", 2, kInsertFx);
    duckGang_ = tog("duck/gang", false);
    duckTrig_ = choice("duck/trigger", 1 + kMonoInputs + kStereoInputs, 0);
    duckThresh_ = num("duck/thresh", -72, 18, -30, "dB");
    duckDepth_ = num("duck/depth", 0, 60, 15, "dB");
    duckAttack_ = num("duck/attack", 20, 600, 50, "ms");
    duckHold_ = num("duck/hold", 40, 10000, 500, "ms");
    duckRelease_ = num("duck/release", 250, 5000, 1000, "ms");
  }
  if (sections & kHpf) {
    hpfOn_ = tog("hpf/on", false);
    hpfFreq_ = num("hpf/freq", 20, 2000, 100, "Hz");
  }
  if (sections & kGate) {
    gateOn_ = tog("gate/on", false);
    gateThresh_ = num("gate/thresh", -72, 18, -36, "dB");
    gateDepth_ = num("gate/depth", 0, 60, 20, "dB");
    gateAttack_ = num("gate/attack", 0.05f, 300, 0.05f, "ms");
    gateHold_ = num("gate/hold", 10, 5000, 100, "ms");
    gateRelease_ = num("gate/release", 10, 1000, 300, "ms");
  }
  if (sections & kPeq) {
    eqOn_ = tog("eq/on", true);
    for (int k = 0; k < kEqBands; ++k) {
      const std::string e = "eq/" + n1(k) + "/";
      const bool outer = k == 0 || k == kEqBands - 1;  // LF/HF: widest setting is a shelf
      eqFreq_[k] = num(e + "freq", 20, 20000, kEqDefaultFreq[k], "Hz");
      eqGain_[k] = num(e + "gain", -15, 15, 0, "dB");
      eqWidth_[k] = num(e + "width", 1.0f / 9.0f, outer ? kEqShelfWidth : kEqWidestBell,
                        kEqDefaultWidth, "oct");
    }
  }
  if (sections & kGeq) {
    geqOn_ = tog("geq/on", true);
    for (int k = 0; k < kGeqBands; ++k) geqGain_[k] = num("geq/" + n1(k), -12, 12, 0, "dB");
  }
  if (sections & kComp) {
    compOn_ = tog("comp/on", false);
    compThresh_ = num("comp/thresh", -46, 18, 0, "dB");
    compRatio_ = num("comp/ratio", 1, Compressor::kRatioInfinity, 3.2f, ":1");
    compAttack_ = num("comp/attack", 0.3f, 300, 10, "ms");
    compRelease_ = num("comp/release", 100, 2000, 300, "ms");
    compHard_ = tog("comp/hard", false);  // Hard Knee button; off = soft knee
    compGain_ = num("comp/gain", 0, 18, 0, "dB");
    // 0 Manual Peak, 1 Manual RMS, 2 Auto Slow Opto, 3 Auto Punchbag.
    compType_ = choice("comp/type", 4, Compressor::kPeak);
  }
  if (sections & kDelay) {
    delayOn_ = tog("delay/on", false);
    delayTime_ = num("delay/time", 0, maxDelayMs, 0, "ms");
    const int maxSamples = static_cast<int>(maxDelayMs * 0.001 * kMaxSampleRate) + 1;
    for (auto& d : delay_) d.allocate(maxSamples);
  }
}

int ChannelDsp::duckTrigger() const {
  if (!(sections_ & kDucker)) return 0;
  return duck(duckTrig_, &DuckGang::trigger)->index();
}

void ChannelDsp::prepare(double sampleRate) {
  fs_ = sampleRate;
  gate_.setSampleRate(static_cast<float>(fs_));
  comp_.setSampleRate(static_cast<float>(fs_));
  ducker_.setSampleRate(static_cast<float>(fs_));
  hpfCache_ = -1.0f;
  eqCache_.fill(-1.0f);
  gateCache_.fill(-1.0f);
  duckCache_.fill(-1.0f);
  compCache_.fill(-1.0f);
  reset();
}

void ChannelDsp::reset() {
  for (int s = 0; s < 2; ++s) {
    hpf_[s].reset();
    for (auto& e : eq_[s]) e.reset();
    geq_[s].reset();
    delay_[s].reset();
  }
  gate_.reset();
  comp_.reset();
  ducker_.reset();
}

void ChannelDsp::updateCoefficients() {
  if (sections_ & kHpf) {
    const float f = hpfFreq_->get();
    if (f != hpfCache_) {
      for (auto& h : hpf_) h.set(Biquad::Type::HighPass, fs_, f, 0.0, 0.7071);
      hpfCache_ = f;
    }
  }
  if (sections_ & kPeq) {
    std::array<float, kEqBands * 3> v;
    for (int k = 0; k < kEqBands; ++k) {
      v[k * 3] = eqFreq_[k]->get();
      v[k * 3 + 1] = eqGain_[k]->get();
      v[k * 3 + 2] = eqWidth_[k]->get();
    }
    if (v != eqCache_) {
      for (int k = 0; k < kEqBands; ++k) {
        Biquad::Type t = Biquad::Type::Peak;
        if (v[k * 3 + 2] > kEqWidestBell)
          t = k == 0 ? Biquad::Type::LowShelf : Biquad::Type::HighShelf;
        const double q = t == Biquad::Type::Peak ? widthToQ(v[k * 3 + 2]) : kShelfQ;
        for (auto& side : eq_) side[k].set(t, fs_, v[k * 3], v[k * 3 + 1], q);
      }
      eqCache_ = v;
    }
  }
  if (sections_ & kGeq) {
    for (int k = 0; k < kGeqBands; ++k)
      for (auto& g : geq_) g.setBand(k, geqGain_[k]->get(), fs_);
  }
  if (sections_ & kDucker) {
    const std::array<float, 5> d = {
        duck(duckThresh_, &DuckGang::thresh)->get(), duck(duckDepth_, &DuckGang::depth)->get(),
        duck(duckAttack_, &DuckGang::attack)->get(), duck(duckHold_, &DuckGang::hold)->get(),
        duck(duckRelease_, &DuckGang::release)->get()};
    if (d != duckCache_) {
      ducker_.setParams(d[0] + kQuRefDbfs, d[1], d[2], d[3], d[4]);
      duckCache_ = d;
    }
  }
  if (sections_ & kGate) {
    const std::array<float, 5> g = {gateThresh_->get(), gateDepth_->get(), gateAttack_->get(),
                                    gateHold_->get(), gateRelease_->get()};
    if (g != gateCache_) {
      gate_.setParams(g[0] + kQuRefDbfs, g[1], g[2], g[3], g[4]);
      gateCache_ = g;
    }
  }
  if (sections_ & kComp) {
    const std::array<float, 7> c = {compThresh_->get(), compRatio_->get(), compAttack_->get(),
                                    compRelease_->get(), compHard_->get(), compGain_->get(),
                                    compType_->get()};
    if (c != compCache_) {
      const float knee = c[4] >= 0.5f ? 0.0f : 6.0f;
      comp_.setParams(c[0] + kQuRefDbfs, c[1], c[2], c[3], knee, c[5], static_cast<int>(c[6]));
      compCache_ = c;
    }
  }
  if (sections_ & kDelay) {
    const int samples = delayOn_->on() ? static_cast<int>(delayTime_->get() * 0.001 * fs_ + 0.5) : 0;
    for (auto& d : delay_) d.setDelay(samples);
  }
}

void ChannelDsp::processPreamp(float* l, float* r, int n) {
  if (!(sections_ & kPreamp)) return;
  preampGain_.target = dbToLin(gain_->get()) * (polarity_->on() ? -1.0f : 1.0f);
  if (r) {
    // Same ramp for both sides.
    const Ramp start = preampGain_;
    preampGain_.apply(l, n);
    Ramp right = start;
    right.apply(r, n);
  } else {
    preampGain_.apply(l, n);
  }
}

void ChannelDsp::processChain(float* l, float* r, int n, const Taps* taps,
                              const float* duckTrigger) {
  updateCoefficients();
  if ((sections_ & kHpf) && hpfOn_->on()) {
    if (r) Biquad::processPair(hpf_[0], l, hpf_[1], r, n);
    else hpf_[0].processBlock(l, n);
  }
  if (sections_ & kDucker) {
    if (duckerActive()) ducker_.process(duckTrigger, l, r, n);
    else ducker_.reset();
  }
  if ((sections_ & kGate) && gateOn_->on()) gate_.process(l, r, n);
  copyTap(taps, kTapPreEq, l, r, n);
  if ((sections_ & kPeq) && eqOn_->on()) {
    for (int k = 0; k < kEqBands; ++k) {
      if (r) Biquad::processPair(eq_[0][k], l, eq_[1][k], r, n);
      else eq_[0][k].processBlock(l, n);
    }
  }
  if ((sections_ & kGeq) && geqOn_->on()) {
    if (r) GraphicEq::processPair(geq_[0], l, geq_[1], r, n);
    else geq_[0].process(l, n);
  }
  copyTap(taps, kTapPostEq, l, r, n);
  if ((sections_ & kComp) && compOn_->on()) comp_.process(l, r, n);
  copyTap(taps, kTapPostComp, l, r, n);
  if (sections_ & kDelay) {
    delay_[0].process(l, n);
    if (r) delay_[1].process(r, n);
  }
}

}  // namespace qnu
