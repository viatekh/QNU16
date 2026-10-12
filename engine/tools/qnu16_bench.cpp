// qnu16-bench: worst-case DSP load. Every channel runs HPF, gate, 4-band EQ and compressor with
// all aux sends active. Run it on the target (e.g. a Raspberry Pi 5) to check headroom.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "mixer.h"

int main(int argc, char** argv) {
  const double seconds = argc > 1 ? std::atof(argv[1]) : 20.0;
  const int block = argc > 2 ? std::atoi(argv[2]) : 64;
  const double rate = 48000.0;

  qnu::ParamRegistry reg;
  qnu::Mixer mixer(reg);
  for (int c = 1; c <= qnu::kChannels; ++c) {
    const std::string b = "ch/" + std::to_string(c) + "/";
    for (const char* t : {"hpf/on", "gate/on", "eq/on", "comp/on"}) reg.find(b + t)->set(1);
    for (int k = 1; k <= qnu::kEqBands; ++k) reg.find(b + "eq/" + std::to_string(k) + "/gain")->set(3);
    for (int a = 1; a <= qnu::kAuxes; ++a) reg.find(b + "aux/" + std::to_string(a) + "/send")->set(-6);
  }
  mixer.prepare(rate);

  std::vector<std::vector<float>> in(qnu::kChannels, std::vector<float>(block));
  std::vector<std::vector<float>> out(qnu::kOutputs, std::vector<float>(block));
  const float* inp[qnu::kChannels];
  float* outp[qnu::kOutputs];
  uint32_t seed = 1;
  for (int c = 0; c < qnu::kChannels; ++c) {
    for (auto& v : in[c]) {
      seed = seed * 1664525u + 1013904223u;
      v = ((seed >> 8) / float(1 << 24) - 0.5f) * 0.5f;
    }
    inp[c] = in[c].data();
  }
  for (int o = 0; o < qnu::kOutputs; ++o) outp[o] = out[o].data();

  const long blocks = static_cast<long>(seconds * rate / block);
  double worstUs = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (long i = 0; i < blocks; ++i) {
    const auto b0 = std::chrono::steady_clock::now();
    mixer.process(inp, outp, block);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - b0).count();
    if (us > worstUs) worstUs = us;
  }
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double audio = blocks * double(block) / rate;
  const double budgetUs = block / rate * 1e6;
  std::printf("processed %.1f s of 16-channel audio in %.3f s\n", audio, elapsed);
  std::printf("realtime factor: %.1fx  (one core load: %.1f%%)\n", audio / elapsed, 100.0 * elapsed / audio);
  std::printf("block %d: budget %.0f us, worst %.0f us\n", block, budgetUs, worstUs);
  return 0;
}
