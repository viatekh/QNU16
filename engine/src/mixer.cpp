#include "mixer.h"

#include <cstring>

namespace qnu {

namespace {

std::string n1(int i) { return std::to_string(i + 1); }

// Routes a mono (r == null) or stereo source into a mono (dr == null) or stereo destination.
// Stereo->stereo: L->L with gl, R->R with gr. Mono->stereo: src->L with gl, src->R with gr.
// Any->mono: (L [+ R]) with gl.
void routeInto(Ramp& a, Ramp& b, const float* l, const float* r, float* dl, float* dr, float gl,
               float gr, int n) {
  a.target = gl;
  b.target = dr ? gr : 0.0f;
  if (dr) {
    if (!a.silent()) a.mixInto(l, dl, n);
    if (!b.silent()) b.mixInto(r ? r : l, dr, n);
  } else if (!a.silent()) {
    if (r) a.mixInto2(l, r, dl, n);
    else a.mixInto(l, dl, n);
  }
}

// Constant-power pan for mono sources (-3 dB at centre).
void panGains(float pan, float* gl, float* gr) {
  const float angle = static_cast<float>((pan + 1.0f) * kPi / 4.0);
  *gl = std::cos(angle);
  *gr = std::sin(angle);
}

// Balance for stereo sources: unity at centre, turning one side down.
void balanceGains(float bal, float* gl, float* gr) {
  *gl = bal > 0.0f ? 1.0f - bal : 1.0f;
  *gr = bal < 0.0f ? 1.0f + bal : 1.0f;
}

float peak2(const float* l, const float* r, int n) {
  return r ? std::max(peakOf(l, n), peakOf(r, n)) : peakOf(l, n);
}

}  // namespace

std::vector<std::string> Mixer::labelKeys() {
  std::vector<std::string> keys;
  for (const auto& d : inputDefs()) keys.push_back(d.id);
  for (const auto& b : busDefs()) keys.push_back(b.id);
  for (int d = 0; d < kDcas; ++d) keys.push_back("dca/" + n1(d));
  for (int m = 0; m < kMuteGroups; ++m) keys.push_back("mg/" + n1(m));
  return keys;
}

Mixer::Mixer(ParamRegistry& registry) : reg_(registry) {
  auto num = [&](const std::string& path, float lo, float hi, float def, const char* unit) {
    return reg_.add({path, lo, hi, def, ParamKind::Continuous, unit});
  };
  auto tog = [&](const std::string& path, bool def) {
    return reg_.add({path, 0.0f, 1.0f, def ? 1.0f : 0.0f, ParamKind::Toggle, ""});
  };
  auto level = [&](const std::string& path, float def) {
    return num(path, kMinusInfDb, 10, def, "dB");
  };

  // Input channels and FX returns.
  inputs_.resize(inputDefs().size());
  for (size_t i = 0; i < inputs_.size(); ++i) {
    Input& ch = inputs_[i];
    ch.def = inputDefs()[i];
    const std::string& id = ch.def.id;
    const bool fxReturn = ch.def.kind == InputKind::FxReturn;
    ch.dsp.init(reg_, id, fxReturn ? ChannelDsp::kPeq : ChannelDsp::kInputSections, ch.def.stereo,
                kInputDelayMaxMs);
    ch.pan = num(id + "/pan", -1, 1, 0, "");
    ch.fader = level(id + "/fader", 0);
    ch.mute = tog(id + "/mute", false);
    ch.pafl = tog(id + "/pafl", false);
    ch.lr = tog(id + "/lr", true);
    for (int m = 0; m < kMixes; ++m) {
      ch.mixLevel[m] = level(id + "/mix/" + n1(m) + "/level", kMinusInfDb);
      ch.mixPre[m] = tog(id + "/mix/" + n1(m) + "/pre", true);
      ch.mixOn[m] = tog(id + "/mix/" + n1(m) + "/on", true);  // Assign key
    }
    if (!fxReturn) {
      for (int f = 0; f < kFxSends; ++f) {
        ch.fxLevel[f] = level(id + "/fx/" + n1(f) + "/level", kMinusInfDb);
        ch.fxPre[f] = tog(id + "/fx/" + n1(f) + "/pre", false);
        ch.fxOn[f] = tog(id + "/fx/" + n1(f) + "/on", true);
      }
    }
    for (int g = 0; g < kGroups; ++g) ch.grp[g] = tog(id + "/grp/" + n1(g), false);
    for (int d = 0; d < kDcas; ++d) ch.dca[d] = tog(id + "/dca/" + n1(d), false);
    for (int m = 0; m < kMuteGroups; ++m) ch.mg[m] = tog(id + "/mg/" + n1(m), false);
    ch.l.assign(kMaxBlock, 0.0f);
    ch.r.assign(kMaxBlock, 0.0f);
  }

  // Masters.
  buses_.resize(busDefs().size());
  for (size_t b = 0; b < buses_.size(); ++b) {
    Bus& bus = buses_[b];
    bus.def = busDefs()[b];
    const std::string& id = bus.def.id;
    const bool fxSend = bus.def.kind == BusKind::FxSend;
    const unsigned sections = fxSend ? 0u
                              : bus.def.kind == BusKind::Group ? ChannelDsp::kGroupSections
                                                               : ChannelDsp::kMasterSections;
    bus.dsp.init(reg_, id, sections, bus.def.stereo, kBusDelayMaxMs);
    bus.fader = level(id + "/fader", 0);
    bus.mute = tog(id + "/mute", false);
    bus.pafl = tog(id + "/pafl", false);
    if (bus.def.stereo) bus.balance = num(id + "/balance", -1, 1, 0, "");
    if (bus.def.kind == BusKind::Group) bus.lr = tog(id + "/lr", true);
    if (bus.def.kind == BusKind::Matrix) {
      for (int s = 0; s < kMatrixSources; ++s) {
        const int srcBus = s == 0 ? kBusLR : s <= kMixes ? kBusMix0 + s - 1 : kBusGrp0 + s - 1 - kMixes;
        bus.mtxLevel[s] = level(id + "/" + busDefs()[srcBus].id, kMinusInfDb);
      }
    }
    bus.l.assign(kMaxBlock, 0.0f);
    bus.r.assign(kMaxBlock, 0.0f);
  }

  // FX engines: two reverbs (long and short), a delay and a chorus out of the box.
  const int fxTypes[kFxUnits] = {FxEngine::kReverb, FxEngine::kReverb, FxEngine::kDelay, FxEngine::kChorus};
  const float fxDecay[kFxUnits] = {2.4f, 1.1f, 1.5f, 1.5f};
  for (int f = 0; f < kFxUnits; ++f) {
    fx_[f].init(reg_, "fx/" + n1(f), fxTypes[f], fxDecay[f]);
    fxRetL_[f].assign(kMaxBlock, 0.0f);
    fxRetR_[f].assign(kMaxBlock, 0.0f);
    fxMixFeed_[f].assign(kMaxBlock, 0.0f);
    // 0 = off, M = Mix M (1..7). FX1/FX2 always use their own send bus.
    if (f >= kFxSends)
      fxSrc_[f] = reg_.add({"fx/" + n1(f) + "/src", 0, float(kMixes), 0, ParamKind::Choice, ""});
  }

  for (int d = 0; d < kDcas; ++d) {
    dcaFader_[d] = level("dca/" + n1(d) + "/fader", 0);
    dcaMute_[d] = tog("dca/" + n1(d) + "/mute", false);
  }
  for (int m = 0; m < kMuteGroups; ++m) mgMute_[m] = tog("mg/" + n1(m) + "/mute", false);
  const float lastSource = static_cast<float>(outputSources().size() - 1);
  for (int o = 0; o < kPhysOutputs; ++o)
    outSrc_[o] = reg_.add({"out/" + n1(o) + "/src", 0, lastSource,
                           static_cast<float>(defaultOutputSource(o)), ParamKind::Choice, ""});

  // Engineer's tools: talkback, signal generator, monitor level.
  auto injector = [&](Injector& inj, const std::string& b) {
    inj.on = tog(b + "/on", false);
    inj.lr = tog(b + "/lr", false);
    for (int m = 0; m < kMixes; ++m) inj.mix[m] = tog(b + "/mix/" + n1(m), false);
  };
  injector(talk_, "talk");
  talkSrc_ = reg_.add({"talk/src", 0, float(kPhysInputs), 0, ParamKind::Choice, ""});  // 0 = none
  talkGain_ = num("talk/gain", -24, 24, 0, "dB");
  talkHpf_ = tog("talk/hpf", true);  // 12 dB/oct at 120 Hz
  injector(gen_, "gen");
  genType_ = reg_.add({"gen/type", 0, 2, 0, ParamKind::Choice, ""});  // sine, white, pink
  genFreq_ = num("gen/freq", 20, 20000, 1000, "Hz");
  genLevel_ = num("gen/level", kMinusInfDb, 0, -30, "dB");
  monLevel_ = level("mon/level", 0);

  // Surface setup, stored with the desk so every connected tablet shares it.
  // Custom layer slot -> strip: 0 none, 1..23 inputs, 24..33 masters (bus order), 34..37 DCA 1-4.
  const int customStrips = kInputStrips + kBuses + kDcas;
  for (int i = 0; i < 16; ++i) {
    const float def = i < kDcas ? float(kInputStrips + kBuses + 1 + i) : 0.0f;
    reg_.add({"custom/" + n1(i) + "/strip", 0, float(customStrips), def, ParamKind::Choice, ""});
  }
  // SoftKey functions: 0 none, 1-4 mute group 1-4, 5-8 DCA 1-4 mute, 9 talkback,
  // 10-13 tap tempo FX1-4, 14 next scene, 15 previous scene. Default: mute groups 1-4.
  for (int i = 0; i < 4; ++i)
    reg_.add({"soft/" + n1(i) + "/func", 0, 15, float(i + 1), ParamKind::Choice, ""});
  talkBuf_.assign(kMaxBlock, 0.0f);
  genBuf_.assign(kMaxBlock, 0.0f);
  rtaBuf_.assign(kMaxBlock, 0.0f);

  paflL_.assign(kMaxBlock, 0.0f);
  paflR_.assign(kMaxBlock, 0.0f);
  silence_.assign(kMaxBlock, 0.0f);
  direct_.assign(kPhysInputs, std::vector<float>(kMaxBlock, 0.0f));

  prepare(sampleRate_);
}

void Mixer::prepare(double sampleRate) {
  sampleRate_ = std::min(sampleRate, kMaxSampleRate);
  for (auto& ch : inputs_) ch.dsp.prepare(sampleRate_);
  for (auto& bus : buses_) bus.dsp.prepare(sampleRate_);
  for (auto& f : fx_) f.prepare(sampleRate_);
  talkFilter_.set(Biquad::Type::HighPass, sampleRate_, 120.0, 0.0, 0.7071);
  talkFilter_.reset();
  rta_.prepare(sampleRate_);
}

void Mixer::renderGenerator(int n) {
  float* out = genBuf_.data();
  switch (genType_->index()) {
    case 0: {  // sine
      const double inc = 2.0 * kPi * genFreq_->get() / sampleRate_;
      for (int i = 0; i < n; ++i) {
        out[i] = static_cast<float>(std::sin(genPhase_));
        genPhase_ += inc;
      }
      genPhase_ = std::fmod(genPhase_, 2.0 * kPi);
      break;
    }
    default: {  // white, or pink (Paul Kellet's filter), from the same white source
      const bool pink = genType_->index() == 2;
      auto& b = pink_;
      for (int i = 0; i < n; ++i) {
        genSeed_ = genSeed_ * 1664525u + 1013904223u;
        const float w = static_cast<float>(genSeed_ >> 8) / float(1 << 24) * 2.0f - 1.0f;
        if (!pink) {
          out[i] = w;
          continue;
        }
        b[0] = 0.99886f * b[0] + w * 0.0555179f;
        b[1] = 0.99332f * b[1] + w * 0.0750759f;
        b[2] = 0.96900f * b[2] + w * 0.1538520f;
        b[3] = 0.86650f * b[3] + w * 0.3104856f;
        b[4] = 0.55000f * b[4] + w * 0.5329522f;
        b[5] = -0.7616f * b[5] - w * 0.0168980f;
        out[i] = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f) * 0.11f;
        b[6] = w * 0.115926f;
      }
    }
  }
}

// Adds a mono source into LR and the assigned mixes (unity, no pan).
void Mixer::inject(Injector& inj, const float* src, float gain, int n) {
  inj.gain.target = inj.on->on() ? gain : 0.0f;
  if (inj.gain.silent()) return;
  float g = inj.gain.cur;
  const float step = (inj.gain.target - inj.gain.cur) / static_cast<float>(n);
  const bool toLr = inj.lr->on();
  for (int i = 0; i < n; ++i) {
    const float x = src[i] * g;
    g += step;
    if (toLr) {
      buses_[kBusLR].l[i] += x;
      buses_[kBusLR].r[i] += x;
    }
    for (int m = 0; m < kMixes; ++m) {
      if (!inj.mix[m]->on()) continue;
      Bus& mix = buses_[kBusMix0 + m];
      mix.l[i] += x;
      if (mix.def.stereo) mix.r[i] += x;
    }
  }
  inj.gain.cur = inj.gain.target;
}

void Mixer::process(const float* const* in, float* const* out, int nframes) {
  enableFlushToZero();
  int offset = 0;
  while (offset < nframes) {
    const int n = std::min(kMaxBlock, nframes - offset);
    const float* inChunk[kPhysInputs];
    float* outChunk[kPhysOutputs];
    for (int c = 0; c < kPhysInputs; ++c) inChunk[c] = in && in[c] ? in[c] + offset : nullptr;
    for (int o = 0; o < kPhysOutputs; ++o) outChunk[o] = out && out[o] ? out[o] + offset : nullptr;
    processBlock(inChunk, outChunk, n);
    offset += n;
  }
}

void Mixer::processBlock(const float* const* in, float* const* out, int n) {
  for (auto& bus : buses_) {
    std::fill_n(bus.l.data(), n, 0.0f);
    std::fill_n(bus.r.data(), n, 0.0f);
  }
  std::fill_n(paflL_.data(), n, 0.0f);
  std::fill_n(paflR_.data(), n, 0.0f);

  std::array<float, kDcas> dcaGain{};
  for (int d = 0; d < kDcas; ++d) dcaGain[d] = dcaMute_[d]->on() ? 0.0f : dbToLin(dcaFader_[d]->get());
  std::array<bool, kMuteGroups> mgMuted{};
  for (int m = 0; m < kMuteGroups; ++m) mgMuted[m] = mgMute_[m]->on();

  anyPafl_ = false;
  for (auto& ch : inputs_) anyPafl_ = anyPafl_ || ch.pafl->on();
  for (auto& bus : buses_) anyPafl_ = anyPafl_ || bus.pafl->on();

  // 1. Input channels (load from the physical inputs).
  for (size_t i = 0; i < inputs_.size(); ++i) {
    Input& ch = inputs_[i];
    if (ch.def.kind == InputKind::FxReturn) continue;
    const float* srcL = in[ch.def.physIn];
    if (srcL) std::memcpy(ch.l.data(), srcL, sizeof(float) * n);
    else std::fill_n(ch.l.data(), n, 0.0f);
    if (ch.def.stereo) {
      const float* srcR = in[ch.def.physIn + 1];
      if (srcR) std::memcpy(ch.r.data(), srcR, sizeof(float) * n);
      else std::fill_n(ch.r.data(), n, 0.0f);
    }
    processInput(ch, static_cast<int>(i), n, dcaGain, mgMuted);
  }

  // 2. FX sends -> engines -> FX return channels. Engines past the send buses read the mix
  //    captured at the end of the previous block.
  for (int f = 0; f < kFxUnits; ++f) {
    if (f < kFxSends) {
      Bus& send = buses_[kBusFx0 + f];
      processBus(send, kBusFx0 + f, n);
      fx_[f].process(send.l.data(), fxRetL_[f].data(), fxRetR_[f].data(), n);
    } else {
      fx_[f].process(fxMixFeed_[f].data(), fxRetL_[f].data(), fxRetR_[f].data(), n);
    }
  }
  for (size_t i = 0; i < inputs_.size(); ++i) {
    Input& ch = inputs_[i];
    if (ch.def.kind != InputKind::FxReturn) continue;
    std::memcpy(ch.l.data(), fxRetL_[ch.def.index].data(), sizeof(float) * n);
    std::memcpy(ch.r.data(), fxRetR_[ch.def.index].data(), sizeof(float) * n);
    processInput(ch, static_cast<int>(i), n, dcaGain, mgMuted);
  }

  // Engineer's tools: talkback mic and signal generator into LR / mixes.
  {
    const int src = talkSrc_->index();
    if (src > 0 && in[src - 1]) {
      std::memcpy(talkBuf_.data(), in[src - 1], sizeof(float) * n);
      if (talkHpf_->on()) talkFilter_.processBlock(talkBuf_.data(), n);
      inject(talk_, talkBuf_.data(), dbToLin(talkGain_->get()), n);
    }
    if (gen_.on->on() || !gen_.gain.silent()) {
      renderGenerator(n);
      inject(gen_, genBuf_.data(), dbToLin(genLevel_->get()), n);
    }
  }

  // 3. Groups (they feed LR), then LR and the mixes.
  for (int g = 0; g < kGroups; ++g) processBus(buses_[kBusGrp0 + g], kBusGrp0 + g, n);
  processBus(buses_[kBusLR], kBusLR, n);
  for (int m = 0; m < kMixes; ++m) processBus(buses_[kBusMix0 + m], kBusMix0 + m, n);

  // Capture the mixes that feed FX3/FX4 for the next block.
  for (int f = kFxSends; f < kFxUnits; ++f) {
    const int m = fxSrc_[f]->index();
    float* feed = fxMixFeed_[f].data();
    if (m <= 0 || m > kMixes) {
      std::fill_n(feed, n, 0.0f);
      continue;
    }
    const Bus& mix = buses_[kBusMix0 + m - 1];
    if (mix.def.stereo)
      for (int i = 0; i < n; ++i) feed[i] = 0.5f * (mix.l[i] + mix.r[i]);
    else
      std::memcpy(feed, mix.l.data(), sizeof(float) * n);
  }

  // 4. Matrices, fed post-fader from LR, mixes and groups.
  for (int x = 0; x < kMatrices; ++x) {
    Bus& mtx = buses_[kBusMtx0 + x];
    for (int s = 0; s < kMatrixSources; ++s) {
      const int srcIdx = s == 0 ? kBusLR : s <= kMixes ? kBusMix0 + s - 1 : kBusGrp0 + s - 1 - kMixes;
      const Bus& src = buses_[srcIdx];
      const float g = dbToLin(mtx.mtxLevel[s]->get());
      routeInto(mtx.mtxSend[s].a, mtx.mtxSend[s].b, src.l.data(), src.def.stereo ? src.r.data() : nullptr,
                mtx.l.data(), mtx.r.data(), g, g, n);
    }
    processBus(mtx, kBusMtx0 + x, n);
  }

  // 5. PAFL follows LR when nothing is selected, so the engineer's headphones are never dead.
  if (!anyPafl_) {
    std::memcpy(paflL_.data(), buses_[kBusLR].l.data(), sizeof(float) * n);
    std::memcpy(paflR_.data(), buses_[kBusLR].r.data(), sizeof(float) * n);
  }
  // The RTA follows PAFL (so it shows LR when nothing is selected), before the monitor level.
  for (int i = 0; i < n; ++i) rtaBuf_[i] = 0.5f * (paflL_[i] + paflR_[i]);
  rta_.process(rtaBuf_.data(), n);
  monGain_.target = dbToLin(monLevel_->get());
  {
    const Ramp start = monGain_;
    monGain_.apply(paflL_.data(), n);
    Ramp right = start;
    right.apply(paflR_.data(), n);
  }
  atomicMax(paflMeterL_, peakOf(paflL_.data(), n));
  atomicMax(paflMeterR_, peakOf(paflR_.data(), n));

  // Recorder: Ch 1-16 direct outs + LR (multitrack), or LR (stereo).
  if (recorder_ && recorder_->recording()) {
    const float* tracks[Recorder::kMultitrackTracks];
    if (recorder_->tracks() == Recorder::kMultitrackTracks) {
      for (int c = 0; c < kMonoInputs; ++c) tracks[c] = direct_[c].data();
      tracks[16] = buses_[kBusLR].l.data();
      tracks[17] = buses_[kBusLR].r.data();
    } else {
      tracks[0] = buses_[kBusLR].l.data();
      tracks[1] = buses_[kBusLR].r.data();
    }
    recorder_->push(tracks, n);
  }

  // 6. Output patch.
  const auto& sources = outputSources();
  for (int o = 0; o < kPhysOutputs; ++o) {
    if (!out[o]) continue;
    const int idx = std::clamp(outSrc_[o]->index(), 0, static_cast<int>(sources.size()) - 1);
    const OutputSource& s = sources[idx];
    const float* src = silence_.data();
    switch (s.kind) {
      case OutputSource::Bus: {
        const Bus& bus = buses_[s.index];
        src = s.side == 1 && bus.def.stereo ? bus.r.data() : bus.l.data();
        break;
      }
      case OutputSource::Pafl: src = s.side ? paflR_.data() : paflL_.data(); break;
      case OutputSource::FxReturn: src = s.side ? fxRetR_[s.index].data() : fxRetL_[s.index].data(); break;
      case OutputSource::Direct: src = direct_[s.index].data(); break;
      case OutputSource::Off: break;
    }
    std::memcpy(out[o], src, sizeof(float) * n);
  }
}

void Mixer::processInput(Input& ch, int idx, int n, const std::array<float, kDcas>& dcaGain,
                         const std::array<bool, kMuteGroups>& mgMuted) {
  float* l = ch.l.data();
  float* r = ch.def.stereo ? ch.r.data() : nullptr;
  const bool fxReturn = ch.def.kind == InputKind::FxReturn;

  ch.dsp.processPreamp(l, r, n);
  if (!fxReturn) {
    std::memcpy(direct_[ch.def.physIn].data(), l, sizeof(float) * n);
    if (r) std::memcpy(direct_[ch.def.physIn + 1].data(), r, sizeof(float) * n);
  }
  InMeter& meter = inMeters_[idx];
  atomicMax(meter.in, peak2(l, r, n));

  ch.dsp.processChain(l, r, n);
  atomicMax(meter.gr, ch.dsp.takeGainReduction());
  meter.gate.store(ch.dsp.gateOpen(), std::memory_order_relaxed);

  // Mute: own mute, any assigned mute group, any assigned DCA that is muted.
  bool muted = ch.mute->on();
  float dca = 1.0f;
  for (int m = 0; m < kMuteGroups; ++m) muted = muted || (ch.mg[m]->on() && mgMuted[m]);
  for (int d = 0; d < kDcas; ++d) {
    if (!ch.dca[d]->on()) continue;
    if (dcaGain[d] == 0.0f) muted = true;
    else dca *= dcaGain[d];
  }
  const float unmuted = muted ? 0.0f : 1.0f;
  const float post = dbToLin(ch.fader->get()) * unmuted * dca;

  // Mono sources pan with constant power; stereo sources use balance.
  float pl, pr;
  if (ch.def.stereo) balanceGains(ch.pan->get(), &pl, &pr);
  else panGains(ch.pan->get(), &pl, &pr);
  const float monoSum = r ? 0.5f : 1.0f;  // stereo into a mono destination: (L+R)/2

  Bus& lr = buses_[kBusLR];
  const float lrGain = ch.lr->on() ? post : 0.0f;
  routeInto(ch.toLR.a, ch.toLR.b, l, r, lr.l.data(), lr.r.data(), lrGain * pl, lrGain * pr, n);

  for (int m = 0; m < kMixes; ++m) {
    Bus& mix = buses_[kBusMix0 + m];
    const float g = ch.mixOn[m]->on() ? dbToLin(ch.mixLevel[m]->get()) * (ch.mixPre[m]->on() ? unmuted : post) : 0.0f;
    if (mix.def.stereo)
      routeInto(ch.toMix[m].a, ch.toMix[m].b, l, r, mix.l.data(), mix.r.data(), g * pl, g * pr, n);
    else
      routeInto(ch.toMix[m].a, ch.toMix[m].b, l, r, mix.l.data(), nullptr, g * monoSum, 0, n);
  }
  if (!fxReturn) {
    for (int f = 0; f < kFxSends; ++f) {
      Bus& send = buses_[kBusFx0 + f];
      const float g = ch.fxOn[f]->on() ? dbToLin(ch.fxLevel[f]->get()) * (ch.fxPre[f]->on() ? unmuted : post) : 0.0f;
      routeInto(ch.toFx[f].a, ch.toFx[f].b, l, r, send.l.data(), nullptr, g * monoSum, 0, n);
    }
  }
  for (int g = 0; g < kGroups; ++g) {
    Bus& grp = buses_[kBusGrp0 + g];
    const float gain = ch.grp[g]->on() ? post : 0.0f;
    routeInto(ch.toGrp[g].a, ch.toGrp[g].b, l, r, grp.l.data(), grp.r.data(), gain * pl, gain * pr, n);
  }

  // PFL: pre-fader, and deliberately ignores mute so a muted source can be checked.
  if (ch.pafl->on()) {
    for (int i = 0; i < n; ++i) {
      paflL_[i] += l[i];
      paflR_[i] += r ? r[i] : l[i];
    }
  }
  atomicMax(meter.out, peak2(l, r, n) * post);
}

void Mixer::processBus(Bus& bus, int idx, int n) {
  float* l = bus.l.data();
  float* r = bus.def.stereo ? bus.r.data() : nullptr;
  bus.dsp.processChain(l, r, n);
  BusMeter& meter = busMeters_[idx];
  atomicMax(meter.gr, bus.dsp.takeGainReduction());

  const float gain = bus.mute->on() ? 0.0f : dbToLin(bus.fader->get());
  float bl = 1.0f, br = 1.0f;
  if (bus.balance) balanceGains(bus.balance->get(), &bl, &br);
  bus.gainL.target = gain * bl;
  bus.gainL.apply(l, n);
  if (r) {
    bus.gainR.target = gain * br;
    bus.gainR.apply(r, n);
  }

  atomicMax(meter.l, peakOf(l, n));
  atomicMax(meter.r, peakOf(r ? r : l, n));

  // AFL: post-fader.
  if (bus.pafl->on()) {
    for (int i = 0; i < n; ++i) {
      paflL_[i] += l[i];
      paflR_[i] += r ? r[i] : l[i];
    }
  }
  if (bus.lr) {
    Bus& lr = buses_[kBusLR];
    const float g = bus.lr->on() ? 1.0f : 0.0f;
    routeInto(bus.toLR.a, bus.toLR.b, l, r, lr.l.data(), lr.r.data(), g, g, n);
  }
}

MeterSnapshot Mixer::takeMeters() {
  MeterSnapshot s;
  constexpr auto rl = std::memory_order_relaxed;
  for (int i = 0; i < kInputStrips; ++i) {
    s.inputs[i].inDb = linToDb(inMeters_[i].in.exchange(0.0f, rl));
    s.inputs[i].outDb = linToDb(inMeters_[i].out.exchange(0.0f, rl));
    s.inputs[i].grDb = inMeters_[i].gr.exchange(0.0f, rl);
    s.inputs[i].gateOpen = inMeters_[i].gate.load(rl);
  }
  for (int b = 0; b < kBuses; ++b) {
    s.buses[b].lDb = linToDb(busMeters_[b].l.exchange(0.0f, rl));
    s.buses[b].rDb = linToDb(busMeters_[b].r.exchange(0.0f, rl));
    s.buses[b].grDb = busMeters_[b].gr.exchange(0.0f, rl);
  }
  s.paflLDb = linToDb(paflMeterL_.exchange(0.0f, rl));
  s.paflRDb = linToDb(paflMeterR_.exchange(0.0f, rl));
  return s;
}

}  // namespace qnu
