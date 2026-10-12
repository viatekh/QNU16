#pragma once

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "fx.h"
#include "params.h"
#include "strip.h"
#include "topology.h"

namespace qnu {

struct MeterSnapshot {
  struct Input {
    float inDb = -120, outDb = -120, grDb = 0;
    bool gateOpen = true;
  };
  struct Bus {
    float lDb = -120, rDb = -120, grDb = 0;
  };
  std::array<Input, kInputStrips> inputs{};
  std::array<Bus, kBuses> buses{};
  float paflLDb = -120, paflRDb = -120;
};

// The console engine. Construct once (registers every parameter), call prepare(), then call
// process() from the audio thread. process() never allocates, locks or blocks.
class Mixer {
 public:
  explicit Mixer(ParamRegistry& registry);

  void prepare(double sampleRate);
  double sampleRate() const { return sampleRate_; }

  // in: kPhysInputs pointers, out: kPhysOutputs pointers. Any pointer may be null.
  void process(const float* const* in, float* const* out, int nframes);

  // Control thread: peaks since the previous call.
  MeterSnapshot takeMeters();

  // Label keys: every input and bus id, plus dca/N and mg/N.
  static std::vector<std::string> labelKeys();

 private:
  struct Send {
    Ramp a, b;  // a: left (or mono) leg, b: right leg
  };

  struct Input {
    InputDef def;
    ChannelDsp dsp;
    Param *pan = nullptr, *fader = nullptr, *mute = nullptr, *pafl = nullptr, *lr = nullptr;
    std::array<Param*, kMixes> mixLevel{}, mixPre{};
    std::array<Param*, kFxUnits> fxLevel{}, fxPre{};  // null for FX returns
    std::array<Param*, kGroups> grp{};
    std::array<Param*, kDcas> dca{};
    std::array<Param*, kMuteGroups> mg{};

    Send toLR;
    std::array<Send, kMixes> toMix;
    std::array<Send, kFxUnits> toFx;
    std::array<Send, kGroups> toGrp;
    std::vector<float> l, r;
  };

  struct Bus {
    BusDef def;
    ChannelDsp dsp;
    Param *fader = nullptr, *mute = nullptr, *pafl = nullptr, *balance = nullptr;
    Param* lr = nullptr;                                 // groups: route to LR
    std::array<Param*, kMatrixSources> mtxLevel{};       // matrices: source send levels
    std::array<Send, kMatrixSources> mtxSend;
    Send toLR;
    Ramp gainL, gainR;
    std::vector<float> l, r;  // summing point, then processed and post-fader in place
  };

  void processBlock(const float* const* in, float* const* out, int n);
  void processInput(Input& ch, int idx, int n, const std::array<float, kDcas>& dcaGain,
                    const std::array<bool, kMuteGroups>& mgMuted);
  void processBus(Bus& bus, int idx, int n);

  ParamRegistry& reg_;
  double sampleRate_ = 48000.0;
  std::vector<Input> inputs_;
  std::vector<Bus> buses_;
  std::array<FxEngine, kFxUnits> fx_;
  std::array<std::vector<float>, kFxUnits> fxRetL_, fxRetR_;
  std::array<Param*, kDcas> dcaFader_{}, dcaMute_{};
  std::array<Param*, kMuteGroups> mgMute_{};
  std::array<Param*, kPhysOutputs> outSrc_{};

  std::vector<float> paflL_, paflR_;
  std::vector<std::vector<float>> direct_;  // post-preamp copies of each physical input
  std::vector<float> silence_;
  bool anyPafl_ = false;

  // Meters: audio thread accumulates, takeMeters() reads and clears.
  struct InMeter {
    std::atomic<float> in{0}, out{0}, gr{0};
    std::atomic<bool> gate{true};
  };
  struct BusMeter {
    std::atomic<float> l{0}, r{0}, gr{0};
  };
  std::array<InMeter, kInputStrips> inMeters_;
  std::array<BusMeter, kBuses> busMeters_;
  std::atomic<float> paflMeterL_{0}, paflMeterR_{0};
};

}  // namespace qnu
