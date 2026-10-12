#pragma once

// Fixed console topology, matching the Qu-16:
//   inputs : 16 mono channels, 3 stereo channels (ST1-3), 4 stereo FX returns
//   buses  : LR, Mix 1-4 (mono), Mix 5-6/7-8/9-10 (stereo), FX sends 1-2 (mono)
//   FX     : 4 engines. FX1/FX2 are fed by their send buses; FX3/FX4 are fed by a mix
//            chosen per engine (fx/N/src), as on the Qu-16.
//   control: 4 DCAs, 4 mute groups
//   I/O    : 22 physical inputs (ch 1-16, ST1-3 L/R), 18 patchable physical outputs
// Groups and matrices (Qu-24/32 features) are supported by the engine but disabled here.

#include <string>
#include <vector>

namespace qnu {

constexpr int kMonoInputs = 16;
constexpr int kStereoInputs = 3;
constexpr int kFxReturns = 4;
constexpr int kInputStrips = kMonoInputs + kStereoInputs + kFxReturns;

constexpr int kMixes = 7;  // Mix 1, 2, 3, 4, 5-6, 7-8, 9-10
constexpr int kMonoMixes = 4;
constexpr int kGroups = 0;    // Qu-16: none
constexpr int kMatrices = 0;  // Qu-16: none
constexpr int kFxUnits = 4;   // FX engines
constexpr int kFxSends = 2;   // FX send buses (FX1, FX2)
constexpr int kDcas = 4;
constexpr int kMuteGroups = 4;
constexpr int kEqBands = 4;
constexpr int kGeqBands = 28;

// Bus index layout.
constexpr int kBusLR = 0;
constexpr int kBusMix0 = 1;
constexpr int kBusGrp0 = kBusMix0 + kMixes;
constexpr int kBusMtx0 = kBusGrp0 + kGroups;
constexpr int kBusFx0 = kBusMtx0 + kMatrices;
constexpr int kBuses = kBusFx0 + kFxSends;

// Matrix sources: LR, every mix, every group.
constexpr int kMatrixSources = 1 + kMixes + kGroups;

constexpr int kPhysInputs = kMonoInputs + 2 * kStereoInputs;
constexpr int kPhysOutputs = 18;
constexpr int kMaxBlock = 256;
constexpr double kMaxSampleRate = 96000.0;

constexpr float kInputDelayMaxMs = 85.0f;
constexpr float kBusDelayMaxMs = 170.0f;

enum class InputKind { Mono, Stereo, FxReturn };
enum class BusKind { LR, Mix, Group, Matrix, FxSend };

struct InputDef {
  InputKind kind;
  int index;          // within its kind, 0-based
  std::string id;     // parameter prefix: "ch/1", "st/2", "fxret/3"
  std::string name;   // default display name: "Ch 1", "ST 2", "FX Ret 3"
  bool stereo;
  int physIn;         // first physical input (L for stereo), -1 for FX returns
};

struct BusDef {
  BusKind kind;
  int index;          // within its kind, 0-based
  std::string id;     // "lr", "mix/5", "grp/2", "mtx/1", "fx/4"
  std::string name;   // "LR", "Mix 5-6", "Grp 3-4", "Mtx 1-2", "FX 4"
  bool stereo;
};

const std::vector<InputDef>& inputDefs();  // ch 1-16, ST 1-3, FX Ret 1-4
const std::vector<BusDef>& busDefs();      // in bus index order

// Output patch sources. Index 0 is "Off".
struct OutputSource {
  std::string name;
  enum Kind { Off, Bus, Pafl, FxReturn, Direct } kind;
  int index;  // bus index / fx unit / physical input
  int side;   // 0 = L (or mono), 1 = R
};
const std::vector<OutputSource>& outputSources();
int defaultOutputSource(int physOut);

// FX patching (the FX back panel), per FX engine.
enum FxPatch { kFxUnassigned = 0, kFxInsert = 1, kFxMixReturn = 2, kFxChReturn = 3 };
// What an FX engine is patched to (fx/N/src): 0 none, then the inputs CH 1-16 and ST 1-3, then
// every bus in bus order (LR, Mix 1..., FX 1-2). Mix>Return takes a bus, Ch>Return an input,
// Insert an input or LR / a mix.
constexpr int kFxTargetInputs = kMonoInputs + kStereoInputs;
constexpr int kFxTargets = 1 + kFxTargetInputs + kBuses;
inline int fxTargetForInput(int input) { return 1 + input; }
inline int fxTargetForBus(int bus) { return 1 + kFxTargetInputs + bus; }
std::vector<std::string> fxTargetNames();

// USB / Qu-Drive recording tracks (usb/T/src). Channel sources take the insert send (post
// preamp) or the direct out, per usb/chsrc.
constexpr int kUsbTracks = 18;
struct UsbSource {
  std::string name;
  enum Kind { Off, Channel, Bus } kind;
  int index;  // physical input / bus index
  int side;
};
const std::vector<UsbSource>& usbSources();
int defaultUsbSource(int track);  // CH 1-16, then LR L/R

// The Qu's mix "pre-fade source" points and the global direct-out source points.
enum MixSource { kSrcPostPreamp = 0, kSrcPreEq, kSrcPostEq, kSrcPostAll };
enum DirectOutSource { kDoPostPreamp = 0, kDoPostInsert, kDoPostPeq, kDoPostComp, kDoPostDelay };

constexpr int kLinkPairs = kMonoInputs / 2;  // CH 1-2 ... 15-16
constexpr int kAmmChannels = kMonoInputs;
constexpr int kSoftKeys = 4;
constexpr int kCustomStrips = 16;

}  // namespace qnu
