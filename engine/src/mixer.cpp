#include "mixer.h"

#include <cstring>

#include "scene.h"

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

// Copies src * ramped gain into dst.
void copyGain(Ramp& g, const float* src, float* dst, int n) {
  std::memcpy(dst, src, sizeof(float) * n);
  g.apply(dst, n);
}

constexpr float kAmmEnvMs = 20.0f;
constexpr float kAmmEps = 1e-10f;  // -100 dB: with nobody talking the gain is shared equally

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
  auto choice = [&](const std::string& path, int count, int def) {
    return reg_.add({path, 0.0f, float(count - 1), float(def), ParamKind::Choice, ""});
  };
  auto level = [&](const std::string& path, float def) {
    return num(path, kMinusInfDb, 10, def, "dB");
  };
  auto block = [] { return std::vector<float>(kMaxBlock, 0.0f); };

  // The ducker gang: shared settings that any channel's ducker can follow.
  duckGang_.trigger = choice("duck/trigger", 1 + kMonoInputs + kStereoInputs, 0);
  duckGang_.thresh = num("duck/thresh", -72, 18, -30, "dB");
  duckGang_.depth = num("duck/depth", 0, 60, 15, "dB");
  duckGang_.attack = num("duck/attack", 20, 600, 50, "ms");
  duckGang_.hold = num("duck/hold", 40, 10000, 500, "ms");
  duckGang_.release = num("duck/release", 250, 5000, 1000, "ms");

  // Input channels and FX returns. Reset Mix Settings: channel faders off, FX returns at 0 dB.
  inputs_.resize(inputDefs().size());
  for (size_t i = 0; i < inputs_.size(); ++i) {
    Input& ch = inputs_[i];
    ch.def = inputDefs()[i];
    const std::string& id = ch.def.id;
    const bool fxReturn = ch.def.kind == InputKind::FxReturn;
    ch.dsp.init(reg_, id, fxReturn ? ChannelDsp::kPeq : ChannelDsp::kInputSections, ch.def.stereo,
                kInputDelayMaxMs, &duckGang_);
    if (!fxReturn) ch.src = choice(id + "/src", 2, 0);  // Local, USB
    ch.pan = num(id + "/pan", -1, 1, 0, "");
    ch.fader = level(id + "/fader", fxReturn ? 0 : kMinusInfDb);
    ch.mute = tog(id + "/mute", false);
    ch.pafl = tog(id + "/pafl", false);
    ch.lr = tog(id + "/lr", true);
    if (!fxReturn) ch.doTrim = level(id + "/dout/trim", 0);
    for (int m = 0; m < kMixes; ++m) {
      const std::string b = id + "/mix/" + n1(m) + "/";
      ch.mixLevel[m] = level(b + "level", kMinusInfDb);
      ch.mixPre[m] = tog(b + "pre", true);
      ch.mixOn[m] = tog(b + "on", true);  // Assign key
      if (m >= kMonoMixes) ch.mixPan[m] = num(b + "pan", -1, 1, 0, "");
    }
    if (!fxReturn) {
      for (int f = 0; f < kFxSends; ++f) {
        const std::string b = id + "/fx/" + n1(f) + "/";
        ch.fxLevel[f] = level(b + "level", kMinusInfDb);
        ch.fxPre[f] = tog(b + "pre", false);
        ch.fxOn[f] = tog(b + "on", true);
      }
    }
    for (int g = 0; g < kGroups; ++g) ch.grp[g] = tog(id + "/grp/" + n1(g), false);
    for (int d = 0; d < kDcas; ++d) ch.dca[d] = tog(id + "/dca/" + n1(d), false);
    for (int m = 0; m < kMuteGroups; ++m) ch.mg[m] = tog(id + "/mg/" + n1(m), false);
    ch.l = block();
    ch.r = block();
    ch.insL = block();
    ch.insR = block();
    for (int t = 0; t < ChannelDsp::kTaps; ++t) {
      ch.tapL[t] = block();
      ch.tapR[t] = block();
    }
    ch.doL = block();
    ch.doR = block();
    ch.trig = block();
  }

  // Masters. Reset Mix Settings: LR off, mixes and FX sends at -4 dB.
  buses_.resize(busDefs().size());
  for (size_t b = 0; b < buses_.size(); ++b) {
    Bus& bus = buses_[b];
    bus.def = busDefs()[b];
    const std::string& id = bus.def.id;
    const BusKind kind = bus.def.kind;
    const unsigned sections = kind == BusKind::FxSend ? 0u
                              : kind == BusKind::Group ? ChannelDsp::kGroupSections
                                                       : ChannelDsp::kMasterSections;
    bus.dsp.init(reg_, id, sections, bus.def.stereo, kBusDelayMaxMs, &duckGang_);
    bus.fader = level(id + "/fader", kind == BusKind::LR ? kMinusInfDb : -4.0f);
    bus.mute = tog(id + "/mute", false);
    bus.pafl = tog(id + "/pafl", false);
    if (bus.def.stereo) bus.balance = num(id + "/balance", -1, 1, 0, "");
    if (kind == BusKind::Mix || kind == BusKind::FxSend) bus.src = choice(id + "/presrc", 4, kSrcPostEq);
    if (kind == BusKind::Group) bus.lr = tog(id + "/lr", true);
    if (kind != BusKind::Matrix)
      for (int m = 0; m < kMuteGroups; ++m) bus.mg[m] = tog(id + "/mg/" + n1(m), false);
    if (kind == BusKind::Matrix) {
      for (int s = 0; s < kMatrixSources; ++s) {
        const int srcBus = s == 0 ? kBusLR : s <= kMixes ? kBusMix0 + s - 1 : kBusGrp0 + s - 1 - kMixes;
        bus.mtxLevel[s] = level(id + "/" + busDefs()[srcBus].id, kMinusInfDb);
      }
    }
    bus.l = block();
    bus.r = block();
  }

  // FX: Reset Mix Settings loads a plate reverb (EMT 250 style) and a delay into FX1 and FX2,
  // both patched Mix>Return from their send buses.
  const int fxTypes[kFxUnits] = {FxEngine::kReverb, FxEngine::kDelay, FxEngine::kChorus, FxEngine::kReverb};
  const int fxModels[kFxUnits] = {FxEngine::kPlate, FxEngine::kHall, FxEngine::kHall, FxEngine::kRoom};
  const float fxDecay[kFxUnits] = {2.0f, 1.5f, 1.5f, 1.0f};
  for (int f = 0; f < kFxUnits; ++f) {
    Fx& fx = fx_[f];
    const std::string b = "fx/" + n1(f) + "/";
    fx.engine.init(reg_, "fx/" + n1(f), fxTypes[f], fxModels[f], fxDecay[f]);
    fx.patch = choice(b + "patch", 4, kFxMixReturn);
    fx.src = choice(b + "src", kFxTargets, f < kFxSends ? fxTargetForBus(kBusFx0 + f) : 0);
    fx.mix = num(b + "mix", 0, 100, 100, "%");  // insert dry/wet
    fx.retL = block();
    fx.retR = block();
    fx.feed = block();
    fx.insL = block();
    fx.insR = block();
  }

  for (int d = 0; d < kDcas; ++d) {
    dcaFader_[d] = level("dca/" + n1(d) + "/fader", 0);
    dcaMute_[d] = tog("dca/" + n1(d) + "/mute", false);
  }
  for (int m = 0; m < kMuteGroups; ++m) mgMute_[m] = tog("mg/" + n1(m) + "/mute", false);
  for (int o = 0; o < kPhysOutputs; ++o)
    outSrc_[o] = choice("out/" + n1(o) + "/src", static_cast<int>(outputSources().size()), defaultOutputSource(o));
  for (int t = 0; t < kUsbTracks; ++t)
    usbSrc_[t] = choice("usb/" + n1(t) + "/src", static_cast<int>(usbSources().size()), defaultUsbSource(t));
  usbChSrc_ = choice("usb/chsrc", 2, 0);  // Insert Sends, Direct Outs

  // Direct outputs (global, as the Qu's Routing / Main tab).
  doSrc_ = choice("dout/src", 5, kDoPostDelay);
  doFader_ = tog("dout/fader", true);
  doMute_ = tog("dout/mute", true);

  // Stereo linking of CH 1-2 ... 15-16, with the Qu's link options.
  for (int k = 0; k < kLinkPairs; ++k) {
    const std::string b = "link/" + n1(k) + "/";
    linkOn_[k] = tog(b + "on", false);
    tog(b + "preamp", true);  // the controller mirrors these; the engine needs only "sc"
    linkSc_[k] = tog(b + "sc", true);
    tog(b + "fader", true);
    tog(b + "pan", true);
  }

  // PAFL setup.
  tog("pafl/additive", false);
  paflInAfl_ = tog("pafl/inafl", false);
  paflOutAfl_ = tog("pafl/outafl", true);
  tog("pafl/selfollow", true);
  tog("pafl/mixfollow", true);
  paflLr_ = tog("pafl/lr", true);
  paflDelay_ = num("pafl/delay", 0, kInputDelayMaxMs, 0, "ms");
  paflTrim_ = num("pafl/trim", -24, 0, 0, "dB");
  for (auto& d : paflDelayLine_) d.allocate(static_cast<int>(kInputDelayMaxMs * 0.001 * kMaxSampleRate) + 1);

  // AMM.
  ammOn_ = tog("amm/on", true);
  ammHpf_ = num("amm/hpf", 20, 2000, 250, "Hz");
  ammLpf_ = num("amm/lpf", 120, 20000, 5000, "Hz");
  ammFollow_ = tog("amm/follow", false);
  for (int c = 0; c < kAmmChannels; ++c) {
    ammCh_[c] = tog("amm/" + n1(c) + "/on", false);
    ammPrio_[c] = num("amm/" + n1(c) + "/prio", -15, 15, 0, "dB");
  }
  ammGain_.fill(1.0f);
  ammBuf_ = block();

  // Engineer's tools: talkback, signal generator, monitor level.
  auto injector = [&](Injector& inj, const std::string& b) {
    inj.lr = tog(b + "/lr", false);
    for (int m = 0; m < kMixes; ++m) inj.mix[m] = tog(b + "/mix/" + n1(m), false);
  };
  talkOn_ = tog("talk/on", false);
  injector(talk_, "talk");
  talkSrc_ = choice("talk/src", kPhysInputs + 1, 0);  // 0 = none
  talkGain_ = num("talk/gain", -24, 24, 0, "dB");
  talkHpf_ = tog("talk/hpf", true);
  talkHpfFreq_ = num("talk/hpffreq", 20, 2000, 120, "Hz");
  tog("talk/latch", false);  // Talk key: momentary, or latching on a short press
  genMute_ = tog("gen/mute", false);
  injector(gen_, "gen");
  genType_ = choice("gen/type", 4, 2);  // sine, white, pink, band-pass noise
  genFreq_ = num("gen/freq", 20, 20000, 1000, "Hz");
  genLevel_ = num("gen/level", kMinusInfDb, 18, -10, "dB");  // Qu dB scale
  monLevel_ = level("mon/level", 0);

  // Scene recall: the global filter (custom layer and SoftKeys blocked by default) and safes.
  for (const auto& c : sceneCategories()) tog("filter/" + c, c == "custom" || c == "soft");
  for (const auto& d : inputDefs()) tog("safe/" + d.id, false);
  for (const auto& b : busDefs()) tog("safe/" + b.id, false);

  // Surface setup, stored with the desk so every connected tablet shares it.
  // Custom layer strip: 0 unassigned, 1..23 inputs, 24..33 masters (bus order), 34..37 DCA 1-4.
  const int customChoices = 1 + kInputStrips + kBuses + kDcas;
  for (int i = 0; i < kCustomStrips; ++i) choice("custom/" + n1(i) + "/strip", customChoices, 0);
  tog("custom/only", false);
  // SoftKeys: function and argument (see docs/protocol.md). Default: mute groups 1-4.
  for (int i = 0; i < kSoftKeys; ++i) {
    choice("soft/" + n1(i) + "/func", 17, 2);
    choice("soft/" + n1(i) + "/arg", 128, i);
  }
  talkBuf_ = block();
  genBuf_ = block();
  rtaBuf_ = block();
  scratch_ = block();
  for (auto& p : play_) p = block();

  paflL_ = block();
  paflR_ = block();
  silence_ = block();
  direct_.assign(kPhysInputs, block());
  doOf_.assign(kPhysInputs, nullptr);
  for (auto& ch : inputs_) {
    if (ch.def.kind == InputKind::FxReturn) continue;
    doOf_[ch.def.physIn] = ch.doL.data();
    if (ch.def.stereo) doOf_[ch.def.physIn + 1] = ch.doR.data();
  }

  prepare(sampleRate_);
}

void Mixer::prepare(double sampleRate) {
  sampleRate_ = std::min(sampleRate, kMaxSampleRate);
  for (auto& ch : inputs_) ch.dsp.prepare(sampleRate_);
  for (auto& bus : buses_) bus.dsp.prepare(sampleRate_);
  for (auto& f : fx_) f.engine.prepare(sampleRate_);
  talkFreqCache_ = genFreqCache_ = -1.0f;
  talkFilter_.reset();
  genBand_.reset();
  ammCache_[0] = ammCache_[1] = -1.0f;
  for (auto& d : paflDelayLine_) d.reset();
  rta_.prepare(sampleRate_);
}

void Mixer::renderGenerator(int n) {
  float* out = genBuf_.data();
  const int type = genType_->index();
  if (type == 0) {  // sine
    const double inc = 2.0 * kPi * genFreq_->get() / sampleRate_;
    for (int i = 0; i < n; ++i) {
      out[i] = static_cast<float>(std::sin(genPhase_));
      genPhase_ += inc;
    }
    genPhase_ = std::fmod(genPhase_, 2.0 * kPi);
    return;
  }
  // White, or pink (Paul Kellet's filter) from the same white source; band-pass is pink noise
  // through a band-pass filter at gen/freq.
  const bool pink = type >= 2;
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
  if (type == 3) {
    const float f = genFreq_->get();
    if (f != genFreqCache_) {
      genBand_.set(Biquad::Type::BandPass, sampleRate_, f, 0.0, 2.0);
      genFreqCache_ = f;
    }
    genBand_.processBlock(out, n);
    for (int i = 0; i < n; ++i) out[i] *= 3.0f;  // roughly level-match the full-band noise
  }
}

// Adds a mono source into LR and the assigned mixes (unity, no pan).
void Mixer::inject(Injector& inj, bool on, const float* src, float gain, int n) {
  inj.gain.target = on ? gain : 0.0f;
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

void Mixer::paflAdd(const float* l, const float* r, float gain, int n) {
  for (int i = 0; i < n; ++i) {
    paflL_[i] += l[i] * gain;
    paflR_[i] += (r ? r[i] : l[i]) * gain;
  }
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

  std::array<float, kDcas> dcaDb{};
  std::array<bool, kDcas> dcaMuted{};
  for (int d = 0; d < kDcas; ++d) {
    dcaDb[d] = dcaFader_[d]->get();
    dcaMuted[d] = dcaMute_[d]->on();
  }
  std::array<bool, kMuteGroups> mgMuted{};
  for (int m = 0; m < kMuteGroups; ++m) mgMuted[m] = mgMute_[m]->on();

  anyPafl_ = false;
  for (auto& ch : inputs_) anyPafl_ = anyPafl_ || ch.pafl->on();
  for (auto& bus : buses_) anyPafl_ = anyPafl_ || bus.pafl->on();

  // FX patched as inserts, by target.
  insertOf_.fill(-1);
  for (int f = 0; f < kFxUnits; ++f) {
    const int t = fx_[f].src->index();
    if (fx_[f].patch->index() == kFxInsert && t > 0 && t < kFxTargets && insertOf_[t] < 0) insertOf_[t] = f;
  }
  // Linked pairs with linked dynamics are processed as one stereo chain by the odd channel.
  for (int k = 0; k < kLinkPairs; ++k)
    inputs_[2 * k + 1].linkedFollower = linkOn_[k]->on() && linkSc_[k]->on();

  // Qu-Drive playback into the channels whose source is USB.
  playing_ = player_ && player_->playing();
  if (playing_) {
    float* tracks[Recorder::kMultitrackTracks];
    for (int t = 0; t < Recorder::kMultitrackTracks; ++t) tracks[t] = play_[t].data();
    player_->pull(tracks, n);
  }

  // 1. Input channels: source, preamp; then FX insert and processing; then the AMM; then
  //    routing. In stages so that ducker triggers, linked pairs and the AMM see every channel.
  const int firstFxRet = kMonoInputs + kStereoInputs;
  for (int i = 0; i < firstFxRet; ++i) loadInput(inputs_[i], in, n);
  for (int i = 0; i < firstFxRet; ++i) processInputChain(i, n);
  computeAmm(n);
  for (int i = 0; i < firstFxRet; ++i) routeInput(inputs_[i], i, n, dcaDb, dcaMuted, mgMuted);

  // 2. FX sends, then the engines by their patching, then the FX return channels.
  for (int f = 0; f < kFxSends; ++f) processBus(buses_[kBusFx0 + f], kBusFx0 + f, n, mgMuted);
  for (int f = 0; f < kFxUnits; ++f) {
    Fx& fx = fx_[f];
    const int patch = fx.patch->index();
    const int t = fx.src->index();
    const float* src = nullptr;
    if (patch == kFxMixReturn && t > kFxTargetInputs) {
      const int b = t - 1 - kFxTargetInputs;
      // FX send buses are ready; other buses come later, so they feed through last block's copy.
      src = buses_[b].def.kind == BusKind::FxSend ? buses_[b].l.data() : fx.feed.data();
    } else if (patch == kFxChReturn && t > 0 && t <= kFxTargetInputs) {
      const Input& ch = inputs_[t - 1];
      if (ch.def.stereo) {
        for (int i = 0; i < n; ++i) scratch_[i] = 0.5f * (ch.doL[i] + ch.doR[i]);
        src = scratch_.data();
      } else {
        src = ch.doL.data();
      }
    }
    if (src) {
      fx.engine.process(src, fx.retL.data(), fx.retR.data(), n);
    } else {
      std::fill_n(fx.retL.data(), n, 0.0f);
      std::fill_n(fx.retR.data(), n, 0.0f);
    }
  }
  for (size_t i = firstFxRet; i < inputs_.size(); ++i) {
    Input& ch = inputs_[i];
    std::memcpy(ch.l.data(), fx_[ch.def.index].retL.data(), sizeof(float) * n);
    std::memcpy(ch.r.data(), fx_[ch.def.index].retR.data(), sizeof(float) * n);
    atomicMax(inMeters_[i].in, peak2(ch.l.data(), ch.r.data(), n));
    ch.dsp.processChain(ch.l.data(), ch.r.data(), n);
    routeInput(ch, static_cast<int>(i), n, dcaDb, dcaMuted, mgMuted);
  }

  // Engineer's tools: talkback mic and signal generator into LR / mixes. The generator is off
  // while Talk is active.
  const bool talking = talkOn_->on();
  {
    const int src = talkSrc_->index();
    if (src > 0 && in[src - 1]) {
      std::memcpy(talkBuf_.data(), in[src - 1], sizeof(float) * n);
      if (talkHpf_->on()) {
        const float f = talkHpfFreq_->get();
        if (f != talkFreqCache_) {
          talkFilter_.set(Biquad::Type::HighPass, sampleRate_, f, 0.0, 0.7071);
          talkFreqCache_ = f;
        }
        talkFilter_.processBlock(talkBuf_.data(), n);
      }
      inject(talk_, talking, talkBuf_.data(), dbToLin(talkGain_->get()), n);
    }
    const bool genOn = !genMute_->on() && !talking;
    if (genOn || !gen_.gain.silent()) {
      renderGenerator(n);
      const float lvl = genLevel_->get();
      inject(gen_, genOn, genBuf_.data(), lvl <= kMinusInfDb ? 0.0f : dbToLin(lvl + kQuRefDbfs), n);
    }
  }

  // 3. Groups (they feed LR), then LR and the mixes.
  for (int g = 0; g < kGroups; ++g) processBus(buses_[kBusGrp0 + g], kBusGrp0 + g, n, mgMuted);
  processBus(buses_[kBusLR], kBusLR, n, mgMuted);
  for (int m = 0; m < kMixes; ++m) processBus(buses_[kBusMix0 + m], kBusMix0 + m, n, mgMuted);

  // Capture the buses that feed FX engines (Mix>Return) for the next block.
  for (auto& fx : fx_) {
    const int t = fx.src->index();
    if (fx.patch->index() != kFxMixReturn || t <= kFxTargetInputs) continue;
    const Bus& bus = buses_[t - 1 - kFxTargetInputs];
    if (bus.def.kind == BusKind::FxSend) continue;
    float* feed = fx.feed.data();
    if (bus.def.stereo)
      for (int i = 0; i < n; ++i) feed[i] = 0.5f * (bus.l[i] + bus.r[i]);
    else
      std::memcpy(feed, bus.l.data(), sizeof(float) * n);
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
    processBus(mtx, kBusMtx0 + x, n, mgMuted);
  }

  // 5. PAFL: with nothing selected it carries LR ("LR to PAFL"), or silence.
  if (!anyPafl_ && paflLr_->on()) {
    std::memcpy(paflL_.data(), buses_[kBusLR].l.data(), sizeof(float) * n);
    std::memcpy(paflR_.data(), buses_[kBusLR].r.data(), sizeof(float) * n);
  }
  const int paflDelay = static_cast<int>(paflDelay_->get() * 0.001 * sampleRate_ + 0.5);
  paflDelayLine_[0].setDelay(paflDelay);
  paflDelayLine_[1].setDelay(paflDelay);
  paflDelayLine_[0].process(paflL_.data(), n);
  paflDelayLine_[1].process(paflR_.data(), n);
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

  // Qu-Drive recording: the USB track patch (multitrack), or LR (stereo).
  if (recorder_ && recorder_->recording()) {
    const float* tracks[Recorder::kMultitrackTracks];
    if (recorder_->tracks() == Recorder::kMultitrackTracks) {
      const auto& srcs = usbSources();
      const bool directOuts = usbChSrc_->index() == 1;
      for (int t = 0; t < kUsbTracks; ++t) {
        const UsbSource& s = srcs[std::clamp(usbSrc_[t]->index(), 0, static_cast<int>(srcs.size()) - 1)];
        tracks[t] = silence_.data();
        if (s.kind == UsbSource::Channel) {
          tracks[t] = directOuts ? doOf_[s.index] : direct_[s.index].data();
        } else if (s.kind == UsbSource::Bus) {
          const Bus& bus = buses_[s.index];
          tracks[t] = s.side == 1 && bus.def.stereo ? bus.r.data() : bus.l.data();
        }
      }
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
      case OutputSource::FxReturn: src = s.side ? fx_[s.index].retR.data() : fx_[s.index].retL.data(); break;
      case OutputSource::Direct: src = doOf_[s.index]; break;
      case OutputSource::Off: break;
    }
    std::memcpy(out[o], src, sizeof(float) * n);
  }
}

void Mixer::loadInput(Input& ch, const float* const* in, int n) {
  float* l = ch.l.data();
  float* r = ch.def.stereo ? ch.r.data() : nullptr;
  const float* srcL = nullptr;
  const float* srcR = nullptr;
  if (ch.src->index() == 1) {
    // USB: multitrack playback tracks 1-16 to CH 1-16 and 17-18 to ST1; stereo playback to ST3.
    if (playing_ && player_->multitrack()) {
      if (ch.def.kind == InputKind::Mono) srcL = play_[ch.def.index].data();
      else if (ch.def.index == 0) srcL = play_[16].data(), srcR = play_[17].data();
    } else if (playing_ && ch.def.kind == InputKind::Stereo && ch.def.index == kStereoInputs - 1) {
      srcL = play_[0].data();
      srcR = play_[1].data();
    }
  } else {
    srcL = in[ch.def.physIn];
    if (r) srcR = in[ch.def.physIn + 1];
  }
  if (srcL) std::memcpy(l, srcL, sizeof(float) * n);
  else std::fill_n(l, n, 0.0f);
  if (r) {
    if (srcR) std::memcpy(r, srcR, sizeof(float) * n);
    else std::fill_n(r, n, 0.0f);
  }

  ch.dsp.processPreamp(l, r, n);
  std::memcpy(direct_[ch.def.physIn].data(), l, sizeof(float) * n);
  if (r) std::memcpy(direct_[ch.def.physIn + 1].data(), r, sizeof(float) * n);
  float* trig = ch.trig.data();
  if (r)
    for (int i = 0; i < n; ++i) trig[i] = 0.5f * (l[i] + r[i]);
  else
    std::memcpy(trig, l, sizeof(float) * n);
  atomicMax(inMeters_[&ch - inputs_.data()].in, peak2(l, r, n));
}

void Mixer::insertFx(int target, float* l, float* r, int n) {
  const int f = insertOf_[target];
  if (f < 0) return;
  Fx& fx = fx_[f];
  const float* src = l;
  if (r) {
    for (int i = 0; i < n; ++i) scratch_[i] = 0.5f * (l[i] + r[i]);
    src = scratch_.data();
  }
  fx.engine.process(src, fx.insL.data(), fx.insR.data(), n);
  const float wet = fx.mix->get() / 100.0f, dry = 1.0f - wet;
  if (r) {
    for (int i = 0; i < n; ++i) {
      l[i] = l[i] * dry + fx.insL[i] * wet;
      r[i] = r[i] * dry + fx.insR[i] * wet;
    }
  } else {
    for (int i = 0; i < n; ++i) l[i] = l[i] * dry + 0.5f * (fx.insL[i] + fx.insR[i]) * wet;
  }
}

void Mixer::processInputChain(int idx, int n) {
  Input& ch = inputs_[idx];
  if (ch.linkedFollower) return;  // processed with its partner
  float* l = ch.l.data();
  float* r = ch.def.stereo ? ch.r.data() : nullptr;
  Input* partner = nullptr;
  if (ch.def.kind == InputKind::Mono && idx % 2 == 0 && inputs_[idx + 1].linkedFollower) {
    partner = &inputs_[idx + 1];
    r = partner->l.data();
  }

  // FX insert: after the preamp, before the HPF.
  if (ch.dsp.insertOn() && ch.dsp.insertType() == ChannelDsp::kInsertFx) {
    insertFx(fxTargetForInput(idx), l, partner ? nullptr : r, n);
    if (partner && partner->dsp.insertOn() && partner->dsp.insertType() == ChannelDsp::kInsertFx)
      insertFx(fxTargetForInput(idx + 1), r, nullptr, n);
  }
  std::memcpy(ch.insL.data(), l, sizeof(float) * n);
  if (r) std::memcpy((partner ? partner->insL : ch.insR).data(), r, sizeof(float) * n);

  ChannelDsp::Taps taps;
  for (int t = 0; t < ChannelDsp::kTaps; ++t) {
    taps.l[t] = ch.tapL[t].data();
    taps.r[t] = partner ? partner->tapL[t].data() : ch.tapR[t].data();
  }
  const int trigger = ch.dsp.duckTrigger();
  const float* trig = trigger > 0 && trigger <= kFxTargetInputs ? inputs_[trigger - 1].trig.data() : nullptr;
  ch.dsp.processChain(l, r, n, &taps, trig);

  InMeter& meter = inMeters_[idx];
  const float gr = ch.dsp.takeGainReduction();
  atomicMax(meter.gr, gr);
  meter.gate.store(ch.dsp.gateOpen(), std::memory_order_relaxed);
  if (partner) {
    atomicMax(inMeters_[idx + 1].gr, gr);
    inMeters_[idx + 1].gate.store(ch.dsp.gateOpen(), std::memory_order_relaxed);
  }
}

// Automatic mic mixer (gain sharing, as Dugan's): each channel in the AMM gets the share of the
// total gain that its sidechain power has of the total, so the overall gain stays constant.
// Priority offsets a channel's level in that calculation. Detection is post-gate.
void Mixer::computeAmm(int n) {
  if (!ammOn_->on()) {
    ammGain_.fill(1.0f);
    return;
  }
  const float hp = ammHpf_->get(), lp = ammLpf_->get();
  if (hp != ammCache_[0] || lp != ammCache_[1]) {
    for (auto& b : ammHp_) b.set(Biquad::Type::HighPass, sampleRate_, hp, 0.0, 0.7071);
    for (auto& b : ammLp_) b.set(Biquad::Type::LowPass, sampleRate_, lp, 0.0, 0.7071);
    ammCache_[0] = hp;
    ammCache_[1] = lp;
  }
  const float coef = timeToCoef(kAmmEnvMs, static_cast<float>(sampleRate_));
  std::array<float, kAmmChannels> weighted{};
  float total = 0.0f;
  int count = 0;
  for (int c = 0; c < kAmmChannels; ++c) {
    if (!ammCh_[c]->on()) continue;
    std::memcpy(ammBuf_.data(), inputs_[c].tapL[ChannelDsp::kTapPreEq].data(), sizeof(float) * n);
    ammHp_[c].processBlock(ammBuf_.data(), n);
    ammLp_[c].processBlock(ammBuf_.data(), n);
    float p = ammPow_[c];
    for (int i = 0; i < n; ++i) p = ammBuf_[i] * ammBuf_[i] + (p - ammBuf_[i] * ammBuf_[i]) * coef;
    ammPow_[c] = p;
    float w = std::pow(10.0f, ammPrio_[c]->get() / 10.0f);
    if (ammFollow_->on()) w *= std::pow(10.0f, std::max(inputs_[c].fader->get(), -60.0f) / 10.0f);
    weighted[c] = w * p + kAmmEps;
    total += weighted[c];
    ++count;
  }
  for (int c = 0; c < kAmmChannels; ++c) {
    if (!ammCh_[c]->on()) {
      ammGain_[c] = 1.0f;
      continue;
    }
    ammGain_[c] = std::max(std::sqrt(weighted[c] / total), 1e-3f);  // power share, floor -60 dB
    ammMeter_[c].store(ammGain_[c], std::memory_order_relaxed);
  }
  (void)count;
}

void Mixer::routeInput(Input& ch, int idx, int n, const std::array<float, kDcas>& dcaDb,
                       const std::array<bool, kDcas>& dcaMuted,
                       const std::array<bool, kMuteGroups>& mgMuted) {
  float* l = ch.l.data();
  float* r = ch.def.stereo ? ch.r.data() : nullptr;
  const bool fxReturn = ch.def.kind == InputKind::FxReturn;

  // Mute: own mute, any assigned mute group, any assigned DCA that is muted. A DCA's level
  // acts at the fader (post-fade only); its mute acts like a mute group (pre-fade sends too).
  bool muted = ch.mute->on();
  float dcaSum = 0.0f;
  bool dcaOff = false;
  for (int m = 0; m < kMuteGroups; ++m) muted = muted || (ch.mg[m]->on() && mgMuted[m]);
  for (int d = 0; d < kDcas; ++d) {
    if (!ch.dca[d]->on()) continue;
    if (dcaMuted[d]) muted = true;
    if (dcaDb[d] <= kMinusInfDb) dcaOff = true;
    dcaSum += dcaDb[d];
  }
  // A channel's total boost (fader plus DCAs) is limited to +10 dB.
  const float faderDb = ch.fader->get();
  const float level = faderDb <= kMinusInfDb || dcaOff ? 0.0f : dbToLin(std::min(faderDb + dcaSum, 10.0f));
  const float unmuted = muted ? 0.0f : 1.0f;
  const float post = level * unmuted;
  ch.post = post;
  ch.unmuted = unmuted;
  // The AMM acts on the post-fade sends only (LR, post-fade mixes and FX sends).
  const float amm = idx < kAmmChannels ? ammGain_[idx] : 1.0f;
  const float postSend = post * amm;

  // Mono sources pan with constant power; stereo sources use balance.
  float pl, pr;
  if (ch.def.stereo) balanceGains(ch.pan->get(), &pl, &pr);
  else panGains(ch.pan->get(), &pl, &pr);
  const float monoSum = r ? 0.5f : 1.0f;  // stereo into a mono destination: (L+R)/2

  // Signal at a pre-fade tap point (FX returns have only the end of their chain).
  auto tap = [&](int point, int side) -> const float* {
    if (fxReturn || point == kSrcPostAll) return side ? r : l;
    if (point == kSrcPostPreamp) return direct_[ch.def.physIn + side].data();
    const int t = point == kSrcPreEq ? ChannelDsp::kTapPreEq : ChannelDsp::kTapPostEq;
    return side ? ch.tapR[t].data() : ch.tapL[t].data();
  };

  Bus& lr = buses_[kBusLR];
  const float lrGain = ch.lr->on() ? postSend : 0.0f;
  routeInto(ch.toLR.a, ch.toLR.b, l, r, lr.l.data(), lr.r.data(), lrGain * pl, lrGain * pr, n);

  if (!fxReturn) {
    // Direct out: global source point, follow fader, follow mute; per-channel trim.
    const int dsrc = doSrc_->index();
    auto doTap = [&](int side) -> const float* {
      switch (dsrc) {
        case kDoPostPreamp: return direct_[ch.def.physIn + side].data();
        case kDoPostInsert: return side ? ch.insR.data() : ch.insL.data();
        case kDoPostPeq: return side ? ch.tapR[ChannelDsp::kTapPostEq].data() : ch.tapL[ChannelDsp::kTapPostEq].data();
        case kDoPostComp: return side ? ch.tapR[ChannelDsp::kTapPostComp].data() : ch.tapL[ChannelDsp::kTapPostComp].data();
        default: return side ? r : l;
      }
    };
    float g = dbToLin(ch.doTrim->get());
    if (doFader_->on()) g *= level;
    if (doMute_->on()) g *= unmuted;
    ch.doGain.target = g;
    const Ramp start = ch.doGain;
    copyGain(ch.doGain, doTap(0), ch.doL.data(), n);
    if (r) {
      Ramp right = start;
      copyGain(right, doTap(1), ch.doR.data(), n);
    }
  }

  for (int m = 0; m < kMixes; ++m) {
    Bus& mix = buses_[kBusMix0 + m];
    float g = 0.0f;
    const float* sl = l;
    const float* sr = r;
    if (ch.mixOn[m]->on()) {
      g = dbToLin(ch.mixLevel[m]->get());
      if (ch.mixPre[m]->on()) {
        g *= unmuted;
        sl = tap(mix.src->index(), 0);
        sr = r ? tap(mix.src->index(), 1) : nullptr;
      } else {
        g *= postSend;
      }
    }
    if (mix.def.stereo) {
      float ml, mr;
      if (ch.def.stereo) balanceGains(ch.mixPan[m]->get(), &ml, &mr);
      else panGains(ch.mixPan[m]->get(), &ml, &mr);
      routeInto(ch.toMix[m].a, ch.toMix[m].b, sl, sr, mix.l.data(), mix.r.data(), g * ml, g * mr, n);
    } else {
      routeInto(ch.toMix[m].a, ch.toMix[m].b, sl, sr, mix.l.data(), nullptr, g * monoSum, 0, n);
    }
  }
  if (!fxReturn) {
    for (int f = 0; f < kFxSends; ++f) {
      Bus& send = buses_[kBusFx0 + f];
      float g = 0.0f;
      const float* sl = l;
      const float* sr = r;
      if (ch.fxOn[f]->on()) {
        g = dbToLin(ch.fxLevel[f]->get());
        if (ch.fxPre[f]->on()) {
          g *= unmuted;
          sl = tap(send.src->index(), 0);
          sr = r ? tap(send.src->index(), 1) : nullptr;
        } else {
          g *= postSend;
        }
      }
      routeInto(ch.toFx[f].a, ch.toFx[f].b, sl, sr, send.l.data(), nullptr, g * monoSum, 0, n);
    }
  }
  for (int g = 0; g < kGroups; ++g) {
    Bus& grp = buses_[kBusGrp0 + g];
    const float gain = ch.grp[g]->on() ? postSend : 0.0f;
    routeInto(ch.toGrp[g].a, ch.toGrp[g].b, l, r, grp.l.data(), grp.r.data(), gain * pl, gain * pr, n);
  }

  // PAFL: input PFL is pre-fader and ignores mute so a muted source can be checked; input AFL
  // is post-fader and post-pan.
  if (ch.pafl->on()) {
    if (paflInAfl_->on()) {
      for (int i = 0; i < n; ++i) {
        paflL_[i] += l[i] * post * pl;
        paflR_[i] += (r ? r[i] : l[i]) * post * pr;
      }
    } else {
      paflAdd(l, r, dbToLin(paflTrim_->get()), n);
    }
  }
  atomicMax(inMeters_[idx].out, peak2(l, r, n) * post);
}

void Mixer::processBus(Bus& bus, int idx, int n, const std::array<bool, kMuteGroups>& mgMuted) {
  float* l = bus.l.data();
  float* r = bus.def.stereo ? bus.r.data() : nullptr;
  if (bus.dsp.insertOn()) insertFx(fxTargetForBus(idx), l, r, n);  // before the PEQ
  bus.dsp.processChain(l, r, n);
  BusMeter& meter = busMeters_[idx];
  atomicMax(meter.gr, bus.dsp.takeGainReduction());

  bool muted = bus.mute->on();
  for (int m = 0; m < kMuteGroups; ++m) muted = muted || (bus.mg[m] && bus.mg[m]->on() && mgMuted[m]);
  const bool pafl = bus.pafl->on();
  const bool afl = paflOutAfl_->on();
  if (pafl && !afl) paflAdd(l, r, dbToLin(paflTrim_->get()), n);  // output PFL: pre-fader

  const float gain = muted ? 0.0f : dbToLin(bus.fader->get());
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

  if (pafl && afl) paflAdd(l, r, 1.0f, n);  // output AFL: post-fader
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
  const bool amm = ammOn_->on();
  for (int c = 0; c < kAmmChannels; ++c)
    s.ammDb[c] = amm && ammCh_[c]->on() ? linToDb(ammMeter_[c].load(rl)) : -120.0f;
  return s;
}

}  // namespace qnu
