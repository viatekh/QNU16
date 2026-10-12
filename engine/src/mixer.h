#pragma once

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "dsp/biquad.h"
#include "dsp/dynamics.h"
#include "params.h"

namespace qnu {

constexpr int kChannels = 16;
constexpr int kAuxes = 4;
constexpr int kDcas = 4;
constexpr int kEqBands = 4;
constexpr int kMaxBlock = 256;

// Physical output order. With a UMC1820 these map 1:1 onto its first 8 line outputs.
enum Output : int {
  kOutMainL = 0,
  kOutMainR = 1,
  kOutAux1 = 2,
  kOutPflL = kOutAux1 + kAuxes,
  kOutPflR,
  kOutputs
};

const char* outputName(int index);  // "main_L", "aux_1", "pfl_R", ...

struct MeterSnapshot {
  std::array<float, kChannels> chInDb{};    // after trim (preamp) stage
  std::array<float, kChannels> chOutDb{};   // post-fader
  std::array<float, kChannels> chGrDb{};    // compressor gain reduction, positive dB
  std::array<bool, kChannels> chGateOpen{};
  std::array<float, kOutputs> outDb{};
};

// The 16-channel mixing engine. Construct once (registers all parameters), call prepare(),
// then process() from the audio thread. Everything in process() is allocation- and lock-free.
class Mixer {
 public:
  explicit Mixer(ParamRegistry& registry);

  void prepare(double sampleRate);
  double sampleRate() const { return sampleRate_; }

  // in: kChannels pointers (any may be null = silence). out: kOutputs pointers (any may be null).
  void process(const float* const* in, float* const* out, int nframes);

  // Control thread: peak values since the previous call.
  MeterSnapshot takeMeters();

  // Keys accepted by Labels: ch/1..16, aux/1..4, dca/1..4, main.
  static std::vector<std::string> labelKeys();

 private:
  struct ChannelParams {
    Param *trim, *polarity;
    Param *hpfOn, *hpfFreq;
    Param *gateOn, *gateThresh, *gateRange, *gateAttack, *gateHold, *gateRelease;
    Param *eqOn;
    std::array<Param*, kEqBands> eqFreq, eqGain, eqQ;
    Param *compOn, *compThresh, *compRatio, *compAttack, *compRelease, *compKnee, *compMakeup;
    Param *pan, *fader, *mute, *solo, *mainAssign;
    std::array<Param*, kAuxes> auxSend, auxPre;
    std::array<Param*, kDcas> dca;
  };

  struct Channel {
    ChannelParams p;
    Biquad hpf;
    std::array<Biquad, kEqBands> eq;
    Gate gate;
    Compressor comp;
    // Cached parameter values, to recompute coefficients only on change.
    float hpfFreqCache = -1.0f;
    std::array<float, kEqBands> eqFreqCache{}, eqGainCache{}, eqQCache{};
    std::array<float, 5> gateCache{};
    std::array<float, 6> compCache{};
    Ramp trimGain, mainL, mainR;
    std::array<Ramp, kAuxes> aux;
    float postGain = 0.0f;  // fader*mute*dca (for metering)
  };

  struct BusParams {
    Param *fader, *mute;
  };

  void processBlock(const float* const* in, float* const* out, int n);
  void updateChannelDsp(Channel& ch);
  void resetDspState();

  ParamRegistry& reg_;
  double sampleRate_ = 48000.0;
  std::array<Channel, kChannels> ch_;
  BusParams main_{};
  std::array<BusParams, kAuxes> auxBus_{};
  std::array<BusParams, kDcas> dcaBus_{};
  Ramp mainBusL_, mainBusR_;
  std::array<Ramp, kAuxes> auxBusGain_;

  // Scratch buffers, sized at construction (never reallocated).
  std::vector<float> chBuf_, mixL_, mixR_, pfl_;
  std::vector<std::vector<float>> auxMix_;

  // Meters: audio thread accumulates, takeMeters() reads-and-clears.
  std::array<std::atomic<float>, kChannels> mChIn_{}, mChOut_{}, mChGr_{};
  std::array<std::atomic<bool>, kChannels> mChGate_{};
  std::array<std::atomic<float>, kOutputs> mOut_{};
};

}  // namespace qnu
