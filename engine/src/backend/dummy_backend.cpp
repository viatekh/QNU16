#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "backend/backend.h"

namespace qnu {

namespace {

// Each input gets a distinct signal: tones, pulsing "drums" (to exercise gates) and noise.
class SignalGenerator {
 public:
  explicit SignalGenerator(double sr) : sr_(sr) { phase_.fill(0.0); }

  void render(std::vector<std::vector<float>>& bufs, int n) {
    for (int c = 0; c < kPhysInputs; ++c) {
      float* out = bufs[c].data();
      const double freq = 55.0 * std::pow(2.0, c / 3.0);
      const double inc = 2.0 * kPi * freq / sr_;
      for (int i = 0; i < n; ++i) {
        const double t = (frame_ + i) / sr_;
        double v;
        if (c % 4 == 0) {  // drum-like: decaying burst every 0.5 s
          const double tb = std::fmod(t + c * 0.07, 0.5);
          v = std::sin(phase_[c]) * std::exp(-tb * 12.0);
        } else if (c % 4 == 3) {  // noise with slow swell
          v = noise() * (0.5 + 0.5 * std::sin(t * 0.7 + c));
        } else {  // tone with slow tremolo
          v = std::sin(phase_[c]) * (0.6 + 0.4 * std::sin(t * 1.3 + c));
        }
        phase_[c] += inc;
        out[i] = static_cast<float>(v * 0.25);  // about -12 dBFS peak
      }
      phase_[c] = std::fmod(phase_[c], 2.0 * kPi);
    }
    frame_ += n;
  }

 private:
  double noise() {
    seed_ = seed_ * 1664525u + 1013904223u;
    return (seed_ >> 8) / double(1 << 24) * 2.0 - 1.0;
  }

  double sr_;
  std::array<double, kPhysInputs> phase_;
  uint64_t frame_ = 0;
  uint32_t seed_ = 12345;
};

class DummyBackend : public AudioBackend {
 public:
  explicit DummyBackend(double sr) : sr_(sr) {}
  ~DummyBackend() override { stop(); }

  bool start(Mixer& mixer, std::string*) override {
    mixer.prepare(sr_);
    running_ = true;
    thread_ = std::thread([this, &mixer] { loop(mixer); });
    return true;
  }

  void stop() override {
    running_ = false;
    if (thread_.joinable()) thread_.join();
  }

  std::string name() const override { return "dummy"; }

 private:
  void loop(Mixer& mixer) {
    constexpr int kBlock = 256;
    std::vector<std::vector<float>> in(kPhysInputs, std::vector<float>(kBlock));
    std::vector<std::vector<float>> out(kPhysOutputs, std::vector<float>(kBlock));
    const float* inPtr[kPhysInputs];
    float* outPtr[kPhysOutputs];
    for (int c = 0; c < kPhysInputs; ++c) inPtr[c] = in[c].data();
    for (int o = 0; o < kPhysOutputs; ++o) outPtr[o] = out[o].data();
    SignalGenerator gen(sr_);
    const auto period = std::chrono::duration<double>(kBlock / sr_);
    auto next = std::chrono::steady_clock::now();
    while (running_) {
      gen.render(in, kBlock);
      mixer.process(inPtr, outPtr, kBlock);
      next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
      std::this_thread::sleep_until(next);
    }
  }

  double sr_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace

std::unique_ptr<AudioBackend> makeDummyBackend(double sampleRate) {
  return std::make_unique<DummyBackend>(sampleRate);
}

}  // namespace qnu
