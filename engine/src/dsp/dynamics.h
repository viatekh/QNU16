#pragma once

#include "dsp/util.h"

namespace qnu {

// Noise gate: peak detector, hysteresis, hold, and separate open/close gain smoothing.
// Stereo use is linked: the louder side drives the detector and both sides get the same gain.
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

  // r may be null (mono).
  void process(float* l, float* r, int n) {
    for (int i = 0; i < n; ++i) {
      const float a = r ? std::max(std::fabs(l[i]), std::fabs(r[i])) : std::fabs(l[i]);
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
      l[i] *= gain_;
      if (r) r[i] *= gain_;
    }
  }
  void process(float* buf, int n) { process(buf, nullptr, n); }

  bool isOpen() const { return open_; }

 private:
  float sr_ = 48000.0f;
  float openThresh_ = 0.0f, closeThresh_ = 0.0f, floor_ = 0.0f;
  float atkCoef_ = 0.0f, relCoef_ = 0.0f, detCoef_ = 0.0f;
  int holdSamples_ = 0, holdCounter_ = 0;
  float env_ = 0.0f, gain_ = 1.0f;
  bool open_ = true;
};

// Feed-forward compressor: peak or RMS detector, attack/release envelope, soft-knee gain
// computer, makeup gain. Stereo use is linked.
class Compressor {
 public:
  enum Mode { kPeak = 0, kRms = 1 };

  void setSampleRate(float sr) {
    sr_ = sr;
    rmsCoef_ = timeToCoef(10.0f, sr_);
  }

  void setParams(float threshDb, float ratio, float attackMs, float releaseMs, float kneeDb,
                 float makeupDb, int mode = kPeak) {
    thresh_ = threshDb;
    slope_ = 1.0f / std::max(ratio, 1.0f) - 1.0f;
    knee_ = std::max(kneeDb, 0.0f);
    makeup_ = makeupDb;
    atkCoef_ = timeToCoef(attackMs, sr_);
    relCoef_ = timeToCoef(releaseMs, sr_);
    mode_ = mode;
  }

  void reset() {
    env_ = 0.0f;
    rms_ = 0.0f;
    maxGr_ = 0.0f;
    gain_ = targetGain_ = dbToLin(makeup_);
    gainStep_ = 0.0f;
    countdown_ = 0;
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

  // r may be null (mono). The detector runs every sample; the (log/exp) gain computer runs
  // every kControlInterval samples and the gain is interpolated linearly in between.
  void process(float* l, float* r, int n) {
    for (int i = 0; i < n; ++i) {
      float a = r ? std::max(std::fabs(l[i]), std::fabs(r[i])) : std::fabs(l[i]);
      if (mode_ == kRms) {
        rms_ = a * a + (rms_ - a * a) * rmsCoef_;
        a = std::sqrt(rms_);
      }
      const float c = a > env_ ? atkCoef_ : relCoef_;
      env_ = a + (env_ - a) * c;
      if (--countdown_ <= 0) {
        countdown_ = kControlInterval;
        const float gr = computeGainDb(linToDb(env_));
        maxGr_ = std::max(maxGr_, -gr);
        targetGain_ = dbToLin(gr + makeup_);
        gainStep_ = (targetGain_ - gain_) / kControlInterval;
      }
      gain_ += gainStep_;
      l[i] *= gain_;
      if (r) r[i] *= gain_;
    }
  }
  void process(float* buf, int n) { process(buf, nullptr, n); }

  // Largest gain reduction (positive dB) since the last call.
  float takeGainReduction() {
    const float g = maxGr_;
    maxGr_ = 0.0f;
    return g;
  }

 private:
  float sr_ = 48000.0f;
  float thresh_ = 0.0f, slope_ = 0.0f, knee_ = 0.0f, makeup_ = 0.0f;
  float atkCoef_ = 0.0f, relCoef_ = 0.0f, rmsCoef_ = 0.0f;
  static constexpr int kControlInterval = 8;
  int mode_ = kPeak;
  float env_ = 0.0f, rms_ = 0.0f, maxGr_ = 0.0f;
  float gain_ = 1.0f, targetGain_ = 1.0f, gainStep_ = 0.0f;
  int countdown_ = 0;
};

}  // namespace qnu
