// qnu16-bench: worst-case DSP load. Every input runs HPF, gate, PEQ, compressor and delay with
// every mix and FX send active; every master runs PEQ, a full GEQ, compressor and delay; all four
// FX engines run, and the AMM runs on all 16 channels. Run it on the target (e.g. a Raspberry Pi 5)
// to check headroom.

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
  auto set = [&](const std::string& path, float v) {
    if (qnu::Param* p = reg.find(path)) p->set(v);
    else std::fprintf(stderr, "bench: no parameter %s\n", path.c_str());
  };
  for (const auto& in : qnu::inputDefs()) {
    const std::string& b = in.id;
    if (in.kind != qnu::InputKind::FxReturn) {
      for (const char* t : {"/hpf/on", "/gate/on", "/comp/on", "/delay/on"}) set(b + t, 1);
      set(b + "/delay/time", 10);
      for (int f = 1; f <= qnu::kFxSends; ++f) set(b + "/fx/" + std::to_string(f) + "/level", -10);
    }
    for (int k = 1; k <= qnu::kEqBands; ++k) set(b + "/eq/" + std::to_string(k) + "/gain", 3);
    for (int m = 1; m <= qnu::kMixes; ++m) set(b + "/mix/" + std::to_string(m) + "/level", -6);
    if (qnu::kGroups > 0) set(b + "/grp/1", 1);
  }
  for (const auto& bus : qnu::busDefs()) {
    if (bus.kind == qnu::BusKind::FxSend || bus.kind == qnu::BusKind::Group) continue;
    set(bus.id + "/geq/on", 1);
    for (int k = 1; k <= qnu::kGeqBands; ++k) set(bus.id + "/geq/" + std::to_string(k), k % 2 ? 2 : -2);
    set(bus.id + "/comp/on", 1);
    set(bus.id + "/delay/on", 1);
    set(bus.id + "/delay/time", 5);
  }
  for (int f = qnu::kFxSends + 1; f <= qnu::kFxUnits; ++f)
    set("fx/" + std::to_string(f) + "/src", qnu::fxTargetForBus(qnu::kBusMix0 + f - 1));
  for (int c = 1; c <= qnu::kAmmChannels; ++c) set("amm/" + std::to_string(c) + "/on", 1);
  set("gen/type", 2);
  set("gen/mix/1", 1);
  mixer.prepare(rate);

  std::vector<std::vector<float>> in(qnu::kPhysInputs, std::vector<float>(block));
  std::vector<std::vector<float>> out(qnu::kPhysOutputs, std::vector<float>(block));
  const float* inp[qnu::kPhysInputs];
  float* outp[qnu::kPhysOutputs];
  uint32_t seed = 1;
  for (int c = 0; c < qnu::kPhysInputs; ++c) {
    for (auto& v : in[c]) {
      seed = seed * 1664525u + 1013904223u;
      v = ((seed >> 8) / float(1 << 24) - 0.5f) * 0.5f;
    }
    inp[c] = in[c].data();
  }
  for (int o = 0; o < qnu::kPhysOutputs; ++o) outp[o] = out[o].data();

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
  std::printf("processed %.1f s of audio (%d inputs, %d masters, %d FX) in %.3f s\n", audio,
              qnu::kInputStrips, qnu::kBuses, qnu::kFxUnits, elapsed);
  std::printf("realtime factor: %.1fx  (one core load: %.1f%%)\n", audio / elapsed, 100.0 * elapsed / audio);
  std::printf("block %d: budget %.0f us, worst %.0f us\n", block, budgetUs, worstUs);
  return 0;
}
