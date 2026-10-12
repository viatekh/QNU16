#pragma once

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "dsp/rta.h"
#include "fx.h"
#include "params.h"
#include "recorder.h"
#include "strip.h"
#include "topology.h"

namespace qnu {

struct MeterSnapshot {
  struct Input {
    float inDb = -120, outDb = -120, grDb = 0;
    bool gateOpen = true;
  };
  struct Bus {
    float lDb = -120, rDb = -120, grDb = 0;
  };
  std::array<Input, kInputStrips> inputs{};
  std::array<Bus, kBuses> buses{};
  float paflLDb = -120, paflRDb = -120;
  std::array<float, kAmmChannels> ammDb{};  // AMM gain per channel (-120 when not in the AMM)
};

// The console engine. Construct once (registers every parameter), call prepare(), then call
// process() from the audio thread. process() never allocates, locks or blocks.
class Mixer {
 public:
  explicit Mixer(ParamRegistry& registry);

  void prepare(double sampleRate);
  double sampleRate() const { return sampleRate_; }

  // in: kPhysInputs pointers, out: kPhysOutputs pointers. Any pointer may be null.
  void process(const float* const* in, float* const* out, int nframes);

  // Control thread: peaks since the previous call.
  MeterSnapshot takeMeters();
  // Control thread: RTA of the PAFL signal (31 bands, dBFS) since the previous call.
  std::array<float, Rta::kBands> takeRta() { return rta_.take(); }

  // Optional Qu-Drive (set before audio starts). The mixer feeds the recorder and reads the
  // player every block.
  void setRecorder(Recorder* r) { recorder_ = r; }
  void setPlayer(Player* p) { player_ = p; }

  // Label keys: every input and bus id, plus dca/N and mg/N.
  static std::vector<std::string> labelKeys();

 private:
  struct Send {
    Ramp a, b;  // a: left (or mono) leg, b: right leg
  };

  struct Input {
    InputDef def;
    ChannelDsp dsp;
    Param* src = nullptr;  // 0 local, 1 USB (CH and ST only)
    Param *pan = nullptr, *fader = nullptr, *mute = nullptr, *pafl = nullptr, *lr = nullptr;
    Param* doTrim = nullptr;
    std::array<Param*, kMixes> mixLevel{}, mixPre{}, mixOn{}, mixPan{};  // mixPan: stereo mixes
    std::array<Param*, kFxSends> fxLevel{}, fxPre{}, fxOn{};  // null for FX returns
    std::array<Param*, kGroups> grp{};
    std::array<Param*, kDcas> dca{};
    std::array<Param*, kMuteGroups> mg{};

    Send toLR, paflSend;
    std::array<Send, kMixes> toMix;
    std::array<Send, kFxSends> toFx;
    std::array<Send, kGroups> toGrp;
    Ramp doGain;
    // Signal at the end of the chain, plus the tap points (post-insert and the ChannelDsp taps).
    std::vector<float> l, r;
    std::vector<float> insL, insR;
    std::array<std::vector<float>, ChannelDsp::kTaps> tapL, tapR;
    std::vector<float> doL, doR;  // direct out
    std::vector<float> trig;      // ducker trigger (post preamp, mono)
    float post = 0.0f, unmuted = 1.0f;  // this block's gains
    bool linkedFollower = false;        // processed by its odd partner as a linked pair
  };

  struct Bus {
    BusDef def;
    ChannelDsp dsp;
    Param *fader = nullptr, *mute = nullptr, *pafl = nullptr, *balance = nullptr;
    Param* src = nullptr;                                // mixes / FX sends: pre-fade source
    Param* lr = nullptr;                                 // groups: route to LR
    std::array<Param*, kMuteGroups> mg{};
    std::array<Param*, kMatrixSources> mtxLevel{};       // matrices: source send levels
    std::array<Send, kMatrixSources> mtxSend;
    Send toLR;
    Ramp gainL, gainR;
    std::vector<float> l, r;  // summing point, then processed and post-fader in place
  };

  struct Fx {
    FxEngine engine;
    Param *patch = nullptr, *src = nullptr, *mix = nullptr;
    std::vector<float> retL, retR, feed, insL, insR;
  };

  void processBlock(const float* const* in, float* const* out, int n);
  void loadInput(Input& ch, const float* const* in, int n);
  void processInputChain(int idx, int n);
  void routeInput(Input& ch, int idx, int n, const std::array<float, kDcas>& dcaDb,
                  const std::array<bool, kDcas>& dcaMuted, const std::array<bool, kMuteGroups>& mgMuted);
  void processBus(Bus& bus, int idx, int n, const std::array<bool, kMuteGroups>& mgMuted);
  void insertFx(int target, float* l, float* r, int n);
  void computeAmm(int n);
  void paflAdd(const float* l, const float* r, float gain, int n);

  ParamRegistry& reg_;
  double sampleRate_ = 48000.0;
  std::vector<Input> inputs_;
  std::vector<Bus> buses_;
  std::array<Fx, kFxUnits> fx_;
  std::array<int, kFxTargets> insertOf_{};  // FX unit inserted at each target, -1 for none

  std::array<Param*, kDcas> dcaFader_{}, dcaMute_{};
  std::array<Param*, kMuteGroups> mgMute_{};
  std::array<Param*, kPhysOutputs> outSrc_{};
  std::array<Param*, kUsbTracks> usbSrc_{};
  Param* usbChSrc_ = nullptr;  // 0 insert send (post preamp), 1 direct out
  Param *doSrc_ = nullptr, *doFader_ = nullptr, *doMute_ = nullptr;
  DuckGang duckGang_;
  std::array<Param*, kLinkPairs> linkOn_{}, linkSc_{};

  // PAFL setup.
  Param *paflInAfl_ = nullptr, *paflOutAfl_ = nullptr, *paflLr_ = nullptr, *paflTrim_ = nullptr,
        *paflDelay_ = nullptr;
  std::array<DelayLine, 2> paflDelayLine_;

  // AMM (automatic mic mixer) on CH 1-16.
  Param *ammOn_ = nullptr, *ammHpf_ = nullptr, *ammLpf_ = nullptr, *ammFollow_ = nullptr;
  std::array<Param*, kAmmChannels> ammCh_{}, ammPrio_{};
  std::array<Biquad, kAmmChannels> ammHp_, ammLp_;
  std::array<float, kAmmChannels> ammPow_{}, ammGain_{};  // power envelope, linear gain
  std::vector<float> ammBuf_;
  float ammCache_[2] = {-1, -1};

  // Talkback and signal generator, injected into LR and the mixes.
  struct Injector {
    Param* lr = nullptr;
    std::array<Param*, kMixes> mix{};
    Ramp gain;
  };
  Injector talk_, gen_;
  Param *talkOn_ = nullptr, *talkSrc_ = nullptr, *talkGain_ = nullptr, *talkHpf_ = nullptr,
        *talkHpfFreq_ = nullptr;
  Param *genMute_ = nullptr, *genType_ = nullptr, *genFreq_ = nullptr, *genLevel_ = nullptr;
  Biquad talkFilter_, genBand_;
  float talkFreqCache_ = -1.0f, genFreqCache_ = -1.0f;
  double genPhase_ = 0.0;
  uint32_t genSeed_ = 22222;
  std::array<float, 7> pink_{};
  std::vector<float> talkBuf_, genBuf_;
  void renderGenerator(int n);
  void inject(Injector& inj, bool on, const float* src, float gain, int n);

  Param* monLevel_ = nullptr;  // PAFL / headphone level
  Ramp monGain_;
  Rta rta_;
  std::vector<float> rtaBuf_;
  Recorder* recorder_ = nullptr;
  Player* player_ = nullptr;
  std::array<std::vector<float>, Recorder::kMultitrackTracks> play_;
  bool playing_ = false;

  std::vector<float> paflL_, paflR_;
  std::vector<std::vector<float>> direct_;  // post-preamp (insert send) per physical input
  std::vector<float*> doOf_;                // direct out per physical input
  std::vector<float> silence_, scratch_;
  bool anyPafl_ = false;

  // Meters: audio thread accumulates, takeMeters() reads and clears.
  struct InMeter {
    std::atomic<float> in{0}, out{0}, gr{0};
    std::atomic<bool> gate{true};
  };
  struct BusMeter {
    std::atomic<float> l{0}, r{0}, gr{0};
  };
  std::array<InMeter, kInputStrips> inMeters_;
  std::array<BusMeter, kBuses> busMeters_;
  std::atomic<float> paflMeterL_{0}, paflMeterR_{0};
  std::array<std::atomic<float>, kAmmChannels> ammMeter_{};
};

}  // namespace qnu
