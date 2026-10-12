#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace qnu {

// Flush denormals to zero on the calling thread. Decaying filter/envelope state otherwise
// produces denormals in silence, which can be 100x slower on some CPUs and cause xruns.
inline void enableFlushToZero() {
#if defined(__SSE__) || defined(__x86_64__)
  _mm_setcsr(_mm_getcsr() | 0x8040);  // FTZ | DAZ
#elif defined(__aarch64__)
  uint64_t fpcr;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
  __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));  // FZ
#endif
}

// Fader values at or below this are treated as -inf (fully off).
constexpr float kMinusInfDb = -90.0f;
constexpr double kPi = 3.14159265358979323846;

inline float dbToLin(float db) {
  return db <= kMinusInfDb + 0.01f ? 0.0f : std::pow(10.0f, db / 20.0f);
}

inline float linToDb(float lin) {
  return lin <= 1e-6f ? -120.0f : 20.0f * std::log10(lin);
}

// One-pole smoothing coefficient for a time constant in milliseconds.
inline float timeToCoef(float ms, float sampleRate) {
  if (ms <= 0.0f) return 0.0f;
  return std::exp(-1.0f / (ms * 0.001f * sampleRate));
}

// Linear per-block gain ramp: click-free gain changes without per-sample param reads.
struct Ramp {
  float cur = 0.0f;
  float target = 0.0f;

  // Multiply-accumulate src*gain into dst while ramping cur -> target.
  inline void mixInto(const float* src, float* dst, int n) {
    float g = cur;
    const float step = (target - cur) / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      dst[i] += src[i] * g;
      g += step;
    }
    cur = target;
  }

  // Multiply-accumulate (src1 + src2) * gain into dst while ramping.
  inline void mixInto2(const float* src1, const float* src2, float* dst, int n) {
    float g = cur;
    const float step = (target - cur) / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      dst[i] += (src1[i] + src2[i]) * g;
      g += step;
    }
    cur = target;
  }

  // Apply gain in place while ramping.
  inline void apply(float* buf, int n) {
    float g = cur;
    const float step = (target - cur) / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      buf[i] *= g;
      g += step;
    }
    cur = target;
  }

  bool silent() const { return cur == 0.0f && target == 0.0f; }
};

inline float peakOf(const float* buf, int n) {
  float p = 0.0f;
  for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(buf[i]));
  return p;
}

// Lock-free "max since last read" accumulator for meters (audio thread writes, UI thread reads).
inline void atomicMax(std::atomic<float>& a, float v) {
  float cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

}  // namespace qnu
