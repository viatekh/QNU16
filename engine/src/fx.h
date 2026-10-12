#pragma once

#include <array>
#include <string>
#include <vector>

#include "dsp/biquad.h"
#include "params.h"
#include "topology.h"

namespace qnu {

// One internal effects engine: mono send in, stereo 100%-wet return out. The types follow the
// Qu's FX rack: Reverb (Classic, Hall, Room, Plate models), Delay (separate left/right taps),
// ADT, Chorus, Symphonic Chorus, Flanger, Phaser and Gated Verb.
// All buffers are allocated in init() for the highest supported sample rate, so changing the
// effect type or its parameters never allocates on the audio thread.
class FxEngine {
 public:
  enum Type {
    kReverb = 0, kDelay, kAdt, kChorus, kSymphonic, kFlanger, kPhaser, kGatedVerb, kNumTypes
  };
  enum ReverbModel { kClassic = 0, kHall, kRoom, kPlate, kNumModels };
  static const char* typeName(int t);
  static const char* reverbModelName(int m);

  void init(ParamRegistry& reg, const std::string& prefix, int defaultType, int defaultModel,
            float defaultDecay);
  void prepare(double sampleRate);
  void reset();
  void process(const float* in, float* outL, float* outR, int n);

 private:
  struct Comb {
    std::vector<float> buf;
    int len = 1, pos = 0;
    float store = 0.0f;
  };
  struct Allpass {
    std::vector<float> buf;
    int len = 1, pos = 0;
  };

  void processReverb(const float* in, float* outL, float* outR, int n);
  void processDelay(const float* in, float* outL, float* outR, int n);
  // Modulated delay: chorus family (chorus, symphonic, flanger, ADT).
  void processMod(const float* in, float* outL, float* outR, int n, float rate, float depthMs,
                  float baseMs, float feedback, double rightPhase, float wobble);
  void processPhaser(const float* in, float* outL, float* outR, int n);
  void processGatedVerb(const float* in, float* outL, float* outR, int n);
  void updateReverb();
  void reverbCuts(float* outL, float* outR, int n);

  double fs_ = 48000.0;
  int activeType_ = -1;

  Param* type_ = nullptr;
  Param *revModel_ = nullptr, *revDecay_ = nullptr, *revPredelay_ = nullptr, *revDamp_ = nullptr,
        *revSize_ = nullptr, *revLfCut_ = nullptr, *revHfCut_ = nullptr;
  Param *dlyTime_ = nullptr, *dlyTimeR_ = nullptr, *dlyFeedback_ = nullptr, *dlyHiCut_ = nullptr,
        *dlyPingPong_ = nullptr;
  Param *adtSep_ = nullptr, *adtThick_ = nullptr;
  Param *choRate_ = nullptr, *choDepth_ = nullptr, *choDelay_ = nullptr;
  Param *symRate_ = nullptr, *symDepth_ = nullptr;
  Param *flgRate_ = nullptr, *flgDepth_ = nullptr, *flgDelay_ = nullptr, *flgFeedback_ = nullptr;
  Param *phaRate_ = nullptr, *phaDepth_ = nullptr, *phaFeedback_ = nullptr, *phaStages_ = nullptr;
  Param *gvbThresh_ = nullptr, *gvbHold_ = nullptr, *gvbRelease_ = nullptr;

  // Reverb (Freeverb topology: 8 parallel combs + 4 series allpasses per side).
  std::array<std::array<Comb, 8>, 2> combs_;
  std::array<std::array<Allpass, 4>, 2> allpasses_;
  std::vector<float> predelay_;
  int predelayPos_ = 0;
  float revFeedback_ = 0.0f, revDampCoef_ = 0.0f;
  std::array<float, 4> revCache_{};
  std::array<float, 2> cutCache_{};
  std::array<Biquad, 2> revLf_, revHf_;

  // Gated verb envelope.
  float gvbEnv_ = 0.0f, gvbGain_ = 0.0f;
  int gvbHoldCount_ = 0;

  // Phaser: up to 12 first-order allpass stages per side.
  static constexpr int kMaxPhaserStages = 12;
  std::array<std::array<float, kMaxPhaserStages>, 2> phX_{}, phY_{};
  std::array<float, 2> phFb_{};

  // Delay.
  std::array<std::vector<float>, 2> dlyBuf_;
  int dlyPos_ = 0;
  std::array<float, 2> dlyLp_{};

  // Chorus / flanger.
  std::array<std::vector<float>, 2> modBuf_;
  int modPos_ = 0;
  double lfoPhase_ = 0.0;
  std::array<float, 2> modFb_{};
};

}  // namespace qnu
