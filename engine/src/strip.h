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

// The ducker "gang": one shared set of ducker settings that any channel can follow.
struct DuckGang {
  Param *trigger = nullptr, *thresh = nullptr, *depth = nullptr, *attack = nullptr,
        *hold = nullptr, *release = nullptr;
};

// A processing chain that owns its parameters under a prefix such as "ch/3" or "mix/5":
//   preamp -> [FX insert, done by the mixer] -> HPF -> ducker -> gate -> PEQ -> GEQ -> comp -> delay
// as on the Qu. Which sections exist depends on where it is used: input channels get everything
// but the GEQ, LR and the mixes get PEQ/GEQ/comp/delay, FX returns only PEQ. Mono or stereo
// (linked dynamics, per-side filters). Thresholds use the Qu dB scale (0 dB = -18 dBFS).
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
    kInsert = 1u << 7,  // insert/on (FX insert, patched on the FX back panel)
    kDucker = 1u << 8,  // insert/type (FX or ducker) and the ducker settings
  };
  static constexpr unsigned kInputSections =
      kPreamp | kHpf | kGate | kPeq | kComp | kDelay | kInsert | kDucker;
  static constexpr unsigned kMasterSections = kPeq | kGeq | kComp | kDelay | kInsert;
  static constexpr unsigned kGroupSections = kPeq | kComp | kInsert | kDucker;

  // Intermediate tap points, for direct outputs and pre-fade mix sources.
  enum Tap { kTapPreEq = 0, kTapPostEq, kTapPostComp, kTaps };
  struct Taps {
    std::array<float*, kTaps> l{}, r{};
  };

  enum InsertType { kInsertFx = 0, kInsertDucker = 1 };

  // gang: shared ducker settings (inputs only).
  void init(ParamRegistry& reg, const std::string& prefix, unsigned sections, bool stereo,
            float maxDelayMs, const DuckGang* gang = nullptr);
  void prepare(double sampleRate);
  void reset();

  // Stage 1: preamp gain/polarity (the direct-out and input-meter tap point).
  void processPreamp(float* l, float* r, int n);
  // Stage 2: everything after the preamp (and after any FX insert). Copies the signal into
  // `taps` on the way. duckTrigger is the ducker's trigger signal (null for none).
  void processChain(float* l, float* r, int n, const Taps* taps = nullptr,
                    const float* duckTrigger = nullptr);

  float takeGainReduction() { return comp_.takeGainReduction(); }
  bool gateOpen() const { return !(sections_ & kGate) || !gateOn_->on() || gate_.isOpen(); }

  bool insertOn() const { return insertOn_ && insertOn_->on(); }
  int insertType() const { return insertType_ ? insertType_->index() : kInsertFx; }
  bool duckerActive() const { return insertOn() && insertType() == kInsertDucker; }
  // Ducker trigger: 0 none, 1..16 CH, 17..19 ST (the gang's when this channel follows it).
  int duckTrigger() const;

 private:
  void updateCoefficients();
  const Param* duck(Param* own, Param* DuckGang::*shared) const {
    return gang_ && duckGang_->on() ? gang_->*shared : own;
  }

  unsigned sections_ = 0;
  bool stereo_ = false;
  double fs_ = 48000.0;
  const DuckGang* gang_ = nullptr;

  Param *gain_ = nullptr, *polarity_ = nullptr;
  Param *hpfOn_ = nullptr, *hpfFreq_ = nullptr;
  Param *insertOn_ = nullptr, *insertType_ = nullptr;
  Param *duckGang_ = nullptr, *duckTrig_ = nullptr, *duckThresh_ = nullptr, *duckDepth_ = nullptr,
        *duckAttack_ = nullptr, *duckHold_ = nullptr, *duckRelease_ = nullptr;
  Param *gateOn_ = nullptr, *gateThresh_ = nullptr, *gateDepth_ = nullptr, *gateAttack_ = nullptr,
        *gateHold_ = nullptr, *gateRelease_ = nullptr;
  Param *eqOn_ = nullptr;
  std::array<Param*, kEqBands> eqFreq_{}, eqGain_{}, eqWidth_{};  // width in octaves
  Param* geqOn_ = nullptr;
  std::array<Param*, kGeqBands> geqGain_{};
  Param *compOn_ = nullptr, *compThresh_ = nullptr, *compRatio_ = nullptr, *compAttack_ = nullptr,
        *compRelease_ = nullptr, *compHard_ = nullptr, *compGain_ = nullptr, *compType_ = nullptr;
  Param *delayOn_ = nullptr, *delayTime_ = nullptr;

  Ramp preampGain_;
  std::array<Biquad, 2> hpf_;
  std::array<std::array<Biquad, kEqBands>, 2> eq_;
  std::array<GraphicEq, 2> geq_;
  Ducker ducker_;
  Gate gate_;
  Compressor comp_;
  std::array<DelayLine, 2> delay_;

  // Cached parameter values, to recompute coefficients only on change.
  float hpfCache_ = -1.0f;
  std::array<float, kEqBands * 3> eqCache_{};
  std::array<float, 5> gateCache_{}, duckCache_{};
  std::array<float, 7> compCache_{};
};

// PEQ width (octaves) at or above which the LF/HF bands become shelves: the widest setting.
constexpr float kEqShelfWidth = 2.0f;

}  // namespace qnu
