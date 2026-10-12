#include "mixer.h"

#include <cstring>

namespace qnu {

namespace {

const float kEqDefaultFreq[kEqBands] = {100.0f, 400.0f, 2500.0f, 8000.0f};
const float kEqDefaultQ[kEqBands] = {0.707f, 1.0f, 1.0f, 0.707f};
const Biquad::Type kEqType[kEqBands] = {Biquad::Type::LowShelf, Biquad::Type::Peak,
                                        Biquad::Type::Peak, Biquad::Type::HighShelf};

std::string idx(int i) { return std::to_string(i + 1); }

}  // namespace

const char* outputName(int index) {
  static const char* names[kOutputs] = {"main_L", "main_R", "aux_1", "aux_2",
                                        "aux_3",  "aux_4",  "pfl_L", "pfl_R"};
  static_assert(kOutputs == 8, "update outputName table");
  return index >= 0 && index < kOutputs ? names[index] : "?";
}

std::vector<std::string> Mixer::labelKeys() {
  std::vector<std::string> keys;
  for (int c = 0; c < kChannels; ++c) keys.push_back("ch/" + idx(c));
  for (int a = 0; a < kAuxes; ++a) keys.push_back("aux/" + idx(a));
  for (int d = 0; d < kDcas; ++d) keys.push_back("dca/" + idx(d));
  keys.push_back("main");
  return keys;
}

Mixer::Mixer(ParamRegistry& registry) : reg_(registry) {
  auto num = [&](const std::string& path, float lo, float hi, float def, const char* unit) {
    return reg_.add({path, lo, hi, def, ParamKind::Continuous, unit});
  };
  auto tog = [&](const std::string& path, bool def) {
    return reg_.add({path, 0.0f, 1.0f, def ? 1.0f : 0.0f, ParamKind::Toggle, ""});
  };

  for (int c = 0; c < kChannels; ++c) {
    const std::string b = "ch/" + idx(c) + "/";
    ChannelParams& p = ch_[c].p;
    p.trim = num(b + "trim", -24, 24, 0, "dB");
    p.polarity = tog(b + "polarity", false);
    p.hpfOn = tog(b + "hpf/on", false);
    p.hpfFreq = num(b + "hpf/freq", 20, 400, 80, "Hz");
    p.gateOn = tog(b + "gate/on", false);
    p.gateThresh = num(b + "gate/thresh", -80, 0, -50, "dB");
    p.gateRange = num(b + "gate/range", 0, 80, 40, "dB");
    p.gateAttack = num(b + "gate/attack", 0.05f, 50, 0.5f, "ms");
    p.gateHold = num(b + "gate/hold", 0, 500, 50, "ms");
    p.gateRelease = num(b + "gate/release", 5, 2000, 150, "ms");
    p.eqOn = tog(b + "eq/on", true);
    for (int k = 0; k < kEqBands; ++k) {
      const std::string e = b + "eq/" + idx(k) + "/";
      p.eqFreq[k] = num(e + "freq", 20, 20000, kEqDefaultFreq[k], "Hz");
      p.eqGain[k] = num(e + "gain", -15, 15, 0, "dB");
      p.eqQ[k] = num(e + "q", 0.3f, 10, kEqDefaultQ[k], "");
    }
    p.compOn = tog(b + "comp/on", false);
    p.compThresh = num(b + "comp/thresh", -60, 0, -20, "dB");
    p.compRatio = num(b + "comp/ratio", 1, 20, 3, ":1");
    p.compAttack = num(b + "comp/attack", 0.1f, 100, 10, "ms");
    p.compRelease = num(b + "comp/release", 10, 2000, 100, "ms");
    p.compKnee = num(b + "comp/knee", 0, 12, 6, "dB");
    p.compMakeup = num(b + "comp/makeup", 0, 24, 0, "dB");
    p.pan = num(b + "pan", -1, 1, 0, "");
    p.fader = num(b + "fader", kMinusInfDb, 10, 0, "dB");
    p.mute = tog(b + "mute", false);
    p.solo = tog(b + "solo", false);
    p.mainAssign = tog(b + "main", true);
    for (int a = 0; a < kAuxes; ++a) {
      p.auxSend[a] = num(b + "aux/" + idx(a) + "/send", kMinusInfDb, 10, kMinusInfDb, "dB");
      p.auxPre[a] = tog(b + "aux/" + idx(a) + "/pre", true);
    }
    for (int d = 0; d < kDcas; ++d) p.dca[d] = tog(b + "dca/" + idx(d), false);
  }
  for (int a = 0; a < kAuxes; ++a) {
    auxBus_[a].fader = num("aux/" + idx(a) + "/fader", kMinusInfDb, 10, 0, "dB");
    auxBus_[a].mute = tog("aux/" + idx(a) + "/mute", false);
  }
  for (int d = 0; d < kDcas; ++d) {
    dcaBus_[d].fader = num("dca/" + idx(d) + "/fader", kMinusInfDb, 10, 0, "dB");
    dcaBus_[d].mute = tog("dca/" + idx(d) + "/mute", false);
  }
  main_.fader = num("main/fader", kMinusInfDb, 10, 0, "dB");
  main_.mute = tog("main/mute", false);

  chBuf_.assign(kMaxBlock, 0.0f);
  mixL_.assign(kMaxBlock, 0.0f);
  mixR_.assign(kMaxBlock, 0.0f);
  pfl_.assign(kMaxBlock, 0.0f);
  auxMix_.assign(kAuxes, std::vector<float>(kMaxBlock, 0.0f));

  prepare(sampleRate_);
}

void Mixer::prepare(double sampleRate) {
  sampleRate_ = sampleRate;
  for (auto& ch : ch_) {
    ch.gate.setSampleRate(static_cast<float>(sampleRate));
    ch.comp.setSampleRate(static_cast<float>(sampleRate));
    // Force coefficient recomputation on the next block.
    ch.hpfFreqCache = -1.0f;
    ch.eqFreqCache.fill(-1.0f);
    ch.gateCache.fill(-1.0f);
    ch.compCache.fill(-1.0f);
  }
  resetDspState();
}

void Mixer::resetDspState() {
  for (auto& ch : ch_) {
    ch.hpf.reset();
    for (auto& e : ch.eq) e.reset();
    ch.gate.reset();
    ch.comp.reset();
  }
}

void Mixer::updateChannelDsp(Channel& ch) {
  const ChannelParams& p = ch.p;
  const double fs = sampleRate_;

  const float hf = p.hpfFreq->get();
  if (hf != ch.hpfFreqCache) {
    ch.hpf.set(Biquad::Type::HighPass, fs, hf, 0.0, 0.7071);
    ch.hpfFreqCache = hf;
  }
  for (int k = 0; k < kEqBands; ++k) {
    const float f = p.eqFreq[k]->get(), g = p.eqGain[k]->get(), q = p.eqQ[k]->get();
    if (f != ch.eqFreqCache[k] || g != ch.eqGainCache[k] || q != ch.eqQCache[k]) {
      ch.eq[k].set(kEqType[k], fs, f, g, q);
      ch.eqFreqCache[k] = f;
      ch.eqGainCache[k] = g;
      ch.eqQCache[k] = q;
    }
  }
  const std::array<float, 5> gate = {p.gateThresh->get(), p.gateRange->get(),
                                     p.gateAttack->get(), p.gateHold->get(),
                                     p.gateRelease->get()};
  if (gate != ch.gateCache) {
    ch.gate.setParams(gate[0], gate[1], gate[2], gate[3], gate[4]);
    ch.gateCache = gate;
  }
  const std::array<float, 6> comp = {p.compThresh->get(),  p.compRatio->get(),
                                     p.compAttack->get(),  p.compRelease->get(),
                                     p.compKnee->get(),    p.compMakeup->get()};
  if (comp != ch.compCache) {
    ch.comp.setParams(comp[0], comp[1], comp[2], comp[3], comp[4], comp[5]);
    ch.compCache = comp;
  }
}

void Mixer::process(const float* const* in, float* const* out, int nframes) {
  enableFlushToZero();
  int offset = 0;
  while (offset < nframes) {
    const int n = std::min(kMaxBlock, nframes - offset);
    const float* inChunk[kChannels];
    float* outChunk[kOutputs];
    for (int c = 0; c < kChannels; ++c) inChunk[c] = in && in[c] ? in[c] + offset : nullptr;
    for (int o = 0; o < kOutputs; ++o) outChunk[o] = out && out[o] ? out[o] + offset : nullptr;
    processBlock(inChunk, outChunk, n);
    offset += n;
  }
}

void Mixer::processBlock(const float* const* in, float* const* out, int n) {
  float* buf = chBuf_.data();
  std::fill_n(mixL_.data(), n, 0.0f);
  std::fill_n(mixR_.data(), n, 0.0f);
  std::fill_n(pfl_.data(), n, 0.0f);
  for (auto& a : auxMix_) std::fill_n(a.data(), n, 0.0f);

  // DCA gains for this block.
  std::array<float, kDcas> dcaGain{};
  for (int d = 0; d < kDcas; ++d)
    dcaGain[d] = dcaBus_[d].mute->on() ? 0.0f : dbToLin(dcaBus_[d].fader->get());

  bool anySolo = false;
  for (auto& ch : ch_) anySolo = anySolo || ch.p.solo->on();

  for (int c = 0; c < kChannels; ++c) {
    Channel& ch = ch_[c];
    const ChannelParams& p = ch.p;
    updateChannelDsp(ch);

    // Input stage: trim + polarity.
    if (in[c]) std::memcpy(buf, in[c], sizeof(float) * n);
    else std::fill_n(buf, n, 0.0f);
    ch.trimGain.target = dbToLin(p.trim->get()) * (p.polarity->on() ? -1.0f : 1.0f);
    ch.trimGain.apply(buf, n);
    atomicMax(mChIn_[c], peakOf(buf, n));

    // Processing: HPF -> gate -> EQ -> compressor (Qu-style order).
    if (p.hpfOn->on()) ch.hpf.processBlock(buf, n);
    if (p.gateOn->on()) {
      ch.gate.process(buf, n);
      mChGate_[c].store(ch.gate.isOpen(), std::memory_order_relaxed);
    } else {
      mChGate_[c].store(true, std::memory_order_relaxed);
    }
    if (p.eqOn->on())
      for (auto& band : ch.eq) band.processBlock(buf, n);
    if (p.compOn->on()) {
      ch.comp.process(buf, n);
      atomicMax(mChGr_[c], ch.comp.takeGainReduction());
    }

    // PFL taps the pre-fader signal and ignores mute (so you can check a muted source).
    if (p.solo->on())
      for (int i = 0; i < n; ++i) pfl_[i] += buf[i];

    float dca = 1.0f;
    for (int d = 0; d < kDcas; ++d)
      if (p.dca[d]->on()) dca *= dcaGain[d];
    const float muteGain = p.mute->on() ? 0.0f : 1.0f;
    const float post = dbToLin(p.fader->get()) * muteGain * dca;
    ch.postGain = post;

    for (int a = 0; a < kAuxes; ++a) {
      const float send = dbToLin(p.auxSend[a]->get());
      ch.aux[a].target = send * (p.auxPre[a]->on() ? muteGain : post);
      if (!ch.aux[a].silent()) ch.aux[a].mixInto(buf, auxMix_[a].data(), n);
    }

    // Constant-power pan (-3 dB at centre).
    const float angle = static_cast<float>((p.pan->get() + 1.0f) * kPi / 4.0);
    const float assign = p.mainAssign->on() ? post : 0.0f;
    ch.mainL.target = std::cos(angle) * assign;
    ch.mainR.target = std::sin(angle) * assign;
    if (!ch.mainL.silent()) ch.mainL.mixInto(buf, mixL_.data(), n);
    if (!ch.mainR.silent()) ch.mainR.mixInto(buf, mixR_.data(), n);

    atomicMax(mChOut_[c], peakOf(buf, n) * post);
  }

  // Bus masters.
  const float mainGain = main_.mute->on() ? 0.0f : dbToLin(main_.fader->get());
  mainBusL_.target = mainGain;
  mainBusR_.target = mainGain;
  mainBusL_.apply(mixL_.data(), n);
  mainBusR_.apply(mixR_.data(), n);
  for (int a = 0; a < kAuxes; ++a) {
    auxBusGain_[a].target = auxBus_[a].mute->on() ? 0.0f : dbToLin(auxBus_[a].fader->get());
    auxBusGain_[a].apply(auxMix_[a].data(), n);
  }

  // Outputs. PFL follows main when nothing is soloed.
  const float* sources[kOutputs];
  sources[kOutMainL] = mixL_.data();
  sources[kOutMainR] = mixR_.data();
  for (int a = 0; a < kAuxes; ++a) sources[kOutAux1 + a] = auxMix_[a].data();
  sources[kOutPflL] = anySolo ? pfl_.data() : mixL_.data();
  sources[kOutPflR] = anySolo ? pfl_.data() : mixR_.data();
  for (int o = 0; o < kOutputs; ++o) {
    if (out[o]) std::memcpy(out[o], sources[o], sizeof(float) * n);
    atomicMax(mOut_[o], peakOf(sources[o], n));
  }
}

MeterSnapshot Mixer::takeMeters() {
  MeterSnapshot s;
  for (int c = 0; c < kChannels; ++c) {
    s.chInDb[c] = linToDb(mChIn_[c].exchange(0.0f, std::memory_order_relaxed));
    s.chOutDb[c] = linToDb(mChOut_[c].exchange(0.0f, std::memory_order_relaxed));
    s.chGrDb[c] = mChGr_[c].exchange(0.0f, std::memory_order_relaxed);
    s.chGateOpen[c] = mChGate_[c].load(std::memory_order_relaxed);
  }
  for (int o = 0; o < kOutputs; ++o)
    s.outDb[o] = linToDb(mOut_[o].exchange(0.0f, std::memory_order_relaxed));
  return s;
}

}  // namespace qnu
