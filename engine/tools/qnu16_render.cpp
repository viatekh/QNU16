// qnu16-render: run WAV files through the mixer offline. Good for "virtual soundcheck" on a
// laptop and for testing DSP without any audio hardware.

#include <cstdio>
#include <string>
#include <vector>

#include "mixer.h"
#include "scene.h"
#include "wav.h"

namespace {

void usage() {
  std::printf(
      "usage: qnu16-render [-s scene-file] [--all-outputs] -o out.wav in1.wav [in2.wav ...]\n"
      "  Input channels feed the %d physical inputs in order across all files\n"
      "  (1-16 = Ch 1-16, 17-22 = ST1-3 L/R).\n"
      "  Output is physical outputs 1-2 (LR by default), or all %d with --all-outputs.\n",
      qnu::kPhysInputs, qnu::kPhysOutputs);
}

}  // namespace

int main(int argc, char** argv) {
  std::string sceneFile, outFile;
  std::vector<std::string> inputs;
  bool allOutputs = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if ((a == "-s" || a == "-o") && i + 1 < argc) (a == "-s" ? sceneFile : outFile) = argv[++i];
    else if (a == "--all-outputs") allOutputs = true;
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else if (!a.empty() && a[0] == '-') { usage(); return 2; }
    else inputs.push_back(a);
  }
  if (outFile.empty() || inputs.empty()) {
    usage();
    return 2;
  }

  std::string err;
  std::vector<std::vector<float>> chans;
  double rate = 0;
  for (const auto& path : inputs) {
    qnu::AudioFile f;
    if (!qnu::readWav(path, &f, &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    if (rate == 0) rate = f.sampleRate;
    if (f.sampleRate != rate) {
      std::fprintf(stderr, "%s: sample rate %.0f differs from %.0f\n", path.c_str(), f.sampleRate, rate);
      return 1;
    }
    for (auto& c : f.channels) chans.push_back(std::move(c));
  }
  if (chans.size() > qnu::kPhysInputs) {
    std::fprintf(stderr, "warning: %zu input channels, only the first %d are used\n", chans.size(),
                 qnu::kPhysInputs);
    chans.resize(qnu::kPhysInputs);
  }
  size_t frames = 0;
  for (auto& c : chans) frames = std::max(frames, c.size());
  for (auto& c : chans) c.resize(frames, 0.0f);

  qnu::ParamRegistry reg;
  qnu::Mixer mixer(reg);
  qnu::Labels labels(qnu::Mixer::labelKeys());
  if (!sceneFile.empty()) {
    std::FILE* fp = std::fopen(sceneFile.c_str(), "rb");
    if (!fp) {
      std::fprintf(stderr, "cannot open scene %s\n", sceneFile.c_str());
      return 1;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) text.append(buf, n);
    std::fclose(fp);
    qnu::applyScene(text, reg, labels);
  }
  mixer.prepare(rate);

  qnu::AudioFile out;
  out.sampleRate = rate;
  out.channels.assign(qnu::kPhysOutputs, std::vector<float>(frames));
  const float* in[qnu::kPhysInputs] = {};
  float* outp[qnu::kPhysOutputs];
  for (size_t pos = 0; pos < frames; pos += qnu::kMaxBlock) {
    const int n = static_cast<int>(std::min<size_t>(qnu::kMaxBlock, frames - pos));
    for (size_t c = 0; c < chans.size(); ++c) in[c] = chans[c].data() + pos;
    for (int o = 0; o < qnu::kPhysOutputs; ++o) outp[o] = out.channels[o].data() + pos;
    mixer.process(in, outp, n);
  }
  if (!allOutputs) out.channels.resize(2);
  if (!qnu::writeWav(outFile, out, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  std::printf("rendered %zu frames (%.2f s) from %zu channels -> %s\n", frames, frames / rate,
              chans.size(), outFile.c_str());
  return 0;
}
