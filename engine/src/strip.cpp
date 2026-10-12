#include "strip.h"

namespace qnu {

namespace {

const float kEqDefaultFreq[kEqBands] = {100.0f, 400.0f, 2500.0f, 8000.0f};
const float kEqDefaultQ[kEqBands] = {0.707f, 1.0f, 1.0f, 0.707f};

std::string n1(int i) { return std::to_string(i + 1); }

}  // namespace

void ChannelDsp::init(ParamRegistry& reg, const std::string& prefix, unsigned sections,
                      bool stereo, float maxDelayMs) {
  sections_ = sections;
  stereo_ = stereo;
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
  if (sections & kHpf) {
    hpfOn_ = tog("hpf/on", false);
    hpfFreq_ = num("hpf/freq", 20, 2000, 80, "Hz");
  }
  if (sections & kGate) {
    gateOn_ = tog("gate/on", false);
    gateThresh_ = num("gate/thresh", -80, 0, -50, "dB");
    gateRange_ = num("gate/range", 0, 80, 40, "dB");
    gateAttack_ = num("gate/attack", 0.05f, 50, 0.5f, "ms");
    gateHold_ = num("gate/hold", 0, 500, 50, "ms");
    gateRelease_ = num("gate/release", 5, 2000, 150, "ms");
  }
  if (sections & kPeq) {
    eqOn_ = tog("eq/on", true);
    for (int k = 0; k < kEqBands; ++k) {
      const std::string e = "eq/" + n1(k) + "/";
      eqFreq_[k] = num(e + "freq", 20, 20000, kEqDefaultFreq[k], "Hz");
      eqGain_[k] = num(e + "gain", -15, 15, 0, "dB");
      eqQ_[k] = num(e + "q", 0.3f, 10, kEqDefaultQ[k], "");
    }
    eqLowType_ = choice("eq/1/type", 2, 0);
    eqHighType_ = choice("eq/4/type", 2, 0);
  }
  if (sections & kGeq) {
    geqOn_ = tog("geq/on", false);
    for (int k = 0; k < kGeqBands; ++k) geqGain_[k] = num("geq/" + n1(k), -12, 12, 0, "dB");
  }
  if (sections & kComp) {
    compOn_ = tog("comp/on", false);
    compThresh_ = num("comp/thresh", -60, 0, -20, "dB");
    compRatio_ = num("comp/ratio", 1, 20, 3, ":1");
    compAttack_ = num("comp/attack", 0.1f, 100, 10, "ms");
    compRelease_ = num("comp/release", 10, 2000, 100, "ms");
    compKnee_ = num("comp/knee", 0, 12, 6, "dB");
    compMakeup_ = num("comp/makeup", 0, 24, 0, "dB");
    compMode_ = choice("comp/mode", 2, 0);
  }
  if (sections & kDelay) {
    delayOn_ = tog("delay/on", false);
    delayTime_ = num("delay/time", 0, maxDelayMs, 0, "ms");
    const int maxSamples = static_cast<int>(maxDelayMs * 0.001 * kMaxSampleRate) + 1;
    for (auto& d : delay_) d.allocate(maxSamples);
  }
}

void ChannelDsp::prepare(double sampleRate) {
  fs_ = sampleRate;
  gate_.setSampleRate(static_cast<float>(fs_));
  comp_.setSampleRate(static_cast<float>(fs_));
  hpfCache_ = -1.0f;
  eqCache_.fill(-1.0f);
  gateCache_.fill(-1.0f);
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
    std::array<float, kEqBands * 3 + 2> v;
    for (int k = 0; k < kEqBands; ++k) {
      v[k * 3] = eqFreq_[k]->get();
      v[k * 3 + 1] = eqGain_[k]->get();
      v[k * 3 + 2] = eqQ_[k]->get();
    }
    v[kEqBands * 3] = eqLowType_->get();
    v[kEqBands * 3 + 1] = eqHighType_->get();
    if (v != eqCache_) {
      for (int k = 0; k < kEqBands; ++k) {
        Biquad::Type t = Biquad::Type::Peak;
        if (k == 0 && v[kEqBands * 3] < 0.5f) t = Biquad::Type::LowShelf;
        if (k == kEqBands - 1 && v[kEqBands * 3 + 1] < 0.5f) t = Biquad::Type::HighShelf;
        for (auto& side : eq_) side[k].set(t, fs_, v[k * 3], v[k * 3 + 1], v[k * 3 + 2]);
      }
      eqCache_ = v;
    }
  }
  if (sections_ & kGeq) {
    for (int k = 0; k < kGeqBands; ++k)
      for (auto& g : geq_) g.setBand(k, geqGain_[k]->get(), fs_);
  }
  if (sections_ & kGate) {
    const std::array<float, 5> g = {gateThresh_->get(), gateRange_->get(), gateAttack_->get(),
                                    gateHold_->get(), gateRelease_->get()};
    if (g != gateCache_) {
      gate_.setParams(g[0], g[1], g[2], g[3], g[4]);
      gateCache_ = g;
    }
  }
  if (sections_ & kComp) {
    const std::array<float, 7> c = {compThresh_->get(), compRatio_->get(), compAttack_->get(),
                                    compRelease_->get(), compKnee_->get(), compMakeup_->get(),
                                    compMode_->get()};
    if (c != compCache_) {
      comp_.setParams(c[0], c[1], c[2], c[3], c[4], c[5], static_cast<int>(c[6]));
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

void ChannelDsp::processChain(float* l, float* r, int n) {
  updateCoefficients();
  if ((sections_ & kHpf) && hpfOn_->on()) {
    if (r) Biquad::processPair(hpf_[0], l, hpf_[1], r, n);
    else hpf_[0].processBlock(l, n);
  }
  if ((sections_ & kGate) && gateOn_->on()) gate_.process(l, r, n);
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
  if ((sections_ & kComp) && compOn_->on()) comp_.process(l, r, n);
  if (sections_ & kDelay) {
    delay_[0].process(l, n);
    if (r) delay_[1].process(r, n);
  }
}

}  // namespace qnu
