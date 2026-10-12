#pragma once

#include <array>
#include <string>

#include "dsp/biquad.h"
#include "dsp/delay.h"
#include "dsp/dynamics.h"
#include "dsp/geq.h"
#include "params.h"
#include "topology.h"

namespace qnu {

// A processing chain (preamp -> HPF -> gate -> PEQ -> GEQ -> compressor -> delay) that owns
// its parameters under a prefix such as "ch/3" or "mix/5". Which sections exist depends on
// where it is used: input channels get everything but the GEQ, masters get PEQ/GEQ/comp/delay,
// FX returns only PEQ. Mono or stereo (linked dynamics, per-side filters).
class ChannelDsp {
 public:
  enum Section : unsigned {
    kPreamp = 1u << 0,  // gain (digital trim) + polarity
    kHpf = 1u << 1,
    kGate = 1u << 2,
    kPeq = 1u << 3,
    kGeq = 1u << 4,
    kComp = 1u << 5,
    kDelay = 1u << 6,
  };
  static constexpr unsigned kInputSections = kPreamp | kHpf | kGate | kPeq | kComp | kDelay;
  static constexpr unsigned kMasterSections = kPeq | kGeq | kComp | kDelay;  // LR, mixes, matrix
  static constexpr unsigned kGroupSections = kPeq | kComp;

  void init(ParamRegistry& reg, const std::string& prefix, unsigned sections, bool stereo,
            float maxDelayMs);
  void prepare(double sampleRate);
  void reset();

  // Stage 1: preamp gain/polarity (the direct-out and input-meter tap point).
  void processPreamp(float* l, float* r, int n);
  // Stage 2: everything after the preamp.
  void processChain(float* l, float* r, int n);

  float takeGainReduction() { return comp_.takeGainReduction(); }
  bool gateOpen() const { return !(sections_ & kGate) || !gateOn_->on() || gate_.isOpen(); }

 private:
  void updateCoefficients();

  unsigned sections_ = 0;
  bool stereo_ = false;
  double fs_ = 48000.0;

  Param *gain_ = nullptr, *polarity_ = nullptr;
  Param *hpfOn_ = nullptr, *hpfFreq_ = nullptr;
  Param *gateOn_ = nullptr, *gateThresh_ = nullptr, *gateRange_ = nullptr, *gateAttack_ = nullptr,
        *gateHold_ = nullptr, *gateRelease_ = nullptr;
  Param *eqOn_ = nullptr;
  std::array<Param*, kEqBands> eqFreq_{}, eqGain_{}, eqQ_{};  // eqQ_: width in octaves
  Param *eqLowType_ = nullptr, *eqHighType_ = nullptr;  // 0 = shelf, 1 = bell
  Param* geqOn_ = nullptr;
  std::array<Param*, kGeqBands> geqGain_{};
  Param *compOn_ = nullptr, *compThresh_ = nullptr, *compRatio_ = nullptr, *compAttack_ = nullptr,
        *compRelease_ = nullptr, *compKnee_ = nullptr, *compMakeup_ = nullptr, *compMode_ = nullptr;
  Param *delayOn_ = nullptr, *delayTime_ = nullptr;

  Ramp preampGain_;
  std::array<Biquad, 2> hpf_;
  std::array<std::array<Biquad, kEqBands>, 2> eq_;
  std::array<GraphicEq, 2> geq_;
  Gate gate_;
  Compressor comp_;
  std::array<DelayLine, 2> delay_;

  // Cached parameter values, to recompute coefficients only on change.
  float hpfCache_ = -1.0f;
  std::array<float, kEqBands * 3 + 2> eqCache_{};
  std::array<float, 5> gateCache_{};
  std::array<float, 7> compCache_{};
};

}  // namespace qnu
