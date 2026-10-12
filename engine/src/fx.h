#pragma once

#include <array>
#include <string>
#include <vector>

#include "params.h"
#include "topology.h"

namespace qnu {

// One internal effects engine: mono send in, stereo 100%-wet return out.
// All buffers are allocated in init() for the highest supported sample rate, so changing the
// effect type or its parameters never allocates on the audio thread.
class FxEngine {
 public:
  enum Type { kReverb = 0, kDelay = 1, kChorus = 2, kFlanger = 3, kNumTypes };
  static const char* typeName(int t);

  void init(ParamRegistry& reg, const std::string& prefix, int defaultType, float defaultDecay);
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
  void processMod(const float* in, float* outL, float* outR, int n, bool flanger);
  void updateReverb();

  double fs_ = 48000.0;
  int activeType_ = -1;

  Param* type_ = nullptr;
  Param *revDecay_ = nullptr, *revPredelay_ = nullptr, *revDamp_ = nullptr, *revSize_ = nullptr;
  Param *dlyTime_ = nullptr, *dlyFeedback_ = nullptr, *dlyHiCut_ = nullptr, *dlyPingPong_ = nullptr;
  Param *choRate_ = nullptr, *choDepth_ = nullptr, *choDelay_ = nullptr;
  Param *flgRate_ = nullptr, *flgDepth_ = nullptr, *flgDelay_ = nullptr, *flgFeedback_ = nullptr;

  // Reverb (Freeverb topology: 8 parallel combs + 4 series allpasses per side).
  std::array<std::array<Comb, 8>, 2> combs_;
  std::array<std::array<Allpass, 4>, 2> allpasses_;
  std::vector<float> predelay_;
  int predelayPos_ = 0;
  float revFeedback_ = 0.0f, revDampCoef_ = 0.0f;
  std::array<float, 3> revCache_{};

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
