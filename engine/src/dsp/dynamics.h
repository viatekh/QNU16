#pragma once

#include "dsp/util.h"

namespace qnu {

// Noise gate: peak detector, hysteresis, hold, and separate open/close gain smoothing.
class Gate {
 public:
  void setSampleRate(float sr) {
    sr_ = sr;
    detCoef_ = timeToCoef(10.0f, sr_);
  }

  void setParams(float threshDb, float rangeDb, float attackMs, float holdMs, float releaseMs) {
    openThresh_ = dbToLin(threshDb);
    closeThresh_ = openThresh_ * 0.7079f;  // 3 dB hysteresis
    floor_ = dbToLin(-rangeDb);
    atkCoef_ = timeToCoef(attackMs, sr_);
    relCoef_ = timeToCoef(releaseMs, sr_);
    holdSamples_ = static_cast<int>(holdMs * 0.001f * sr_);
  }

  void reset() {
    env_ = 0.0f;
    gain_ = 1.0f;
    open_ = true;
    holdCounter_ = 0;
  }

  void process(float* buf, int n) {
    for (int i = 0; i < n; ++i) {
      const float a = std::fabs(buf[i]);
      env_ = a > env_ ? a : env_ * detCoef_;
      if (env_ >= openThresh_) {
        open_ = true;
        holdCounter_ = holdSamples_;
      } else if (env_ < closeThresh_) {
        if (holdCounter_ > 0) --holdCounter_;
        else open_ = false;
      }
      const float target = open_ ? 1.0f : floor_;
      const float c = target > gain_ ? atkCoef_ : relCoef_;
      gain_ = target + (gain_ - target) * c;
      buf[i] *= gain_;
    }
  }

  bool isOpen() const { return open_; }

 private:
  float sr_ = 48000.0f;
  float openThresh_ = 0.0f, closeThresh_ = 0.0f, floor_ = 0.0f;
  float atkCoef_ = 0.0f, relCoef_ = 0.0f, detCoef_ = 0.0f;
  int holdSamples_ = 0, holdCounter_ = 0;
  float env_ = 0.0f, gain_ = 1.0f;
  bool open_ = true;
};

// Feed-forward compressor: attack/release peak detector, soft-knee gain computer, makeup.
class Compressor {
 public:
  void setSampleRate(float sr) { sr_ = sr; }

  void setParams(float threshDb, float ratio, float attackMs, float releaseMs, float kneeDb,
                 float makeupDb) {
    thresh_ = threshDb;
    slope_ = 1.0f / std::max(ratio, 1.0f) - 1.0f;
    knee_ = std::max(kneeDb, 0.0f);
    makeup_ = makeupDb;
    atkCoef_ = timeToCoef(attackMs, sr_);
    relCoef_ = timeToCoef(releaseMs, sr_);
  }

  void reset() {
    env_ = 0.0f;
    maxGr_ = 0.0f;
  }

  // Gain change in dB (<= 0) for a detector level in dB.
  float computeGainDb(float levelDb) const {
    const float over = levelDb - thresh_;
    if (knee_ > 0.0f && 2.0f * std::fabs(over) <= knee_) {
      const float x = over + knee_ * 0.5f;
      return slope_ * x * x / (2.0f * knee_);
    }
    return over > 0.0f ? slope_ * over : 0.0f;
  }

  void process(float* buf, int n) {
    for (int i = 0; i < n; ++i) {
      const float a = std::fabs(buf[i]);
      const float c = a > env_ ? atkCoef_ : relCoef_;
      env_ = a + (env_ - a) * c;
      const float gr = computeGainDb(linToDb(env_));
      maxGr_ = std::max(maxGr_, -gr);
      buf[i] *= dbToLin(gr + makeup_);
    }
  }

  // Largest gain reduction (positive dB) since the last call.
  float takeGainReduction() {
    const float g = maxGr_;
    maxGr_ = 0.0f;
    return g;
  }

 private:
  float sr_ = 48000.0f;
  float thresh_ = 0.0f, slope_ = 0.0f, knee_ = 0.0f, makeup_ = 0.0f;
  float atkCoef_ = 0.0f, relCoef_ = 0.0f;
  float env_ = 0.0f, maxGr_ = 0.0f;
};

}  // namespace qnu
