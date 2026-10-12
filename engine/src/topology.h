#pragma once

// Fixed console topology, modelled on a Qu-16:
//   inputs : 16 mono channels, 3 stereo channels (ST1-3), 4 stereo FX returns
//   buses  : LR, Mix 1-4 (mono), Mix 5-6/7-8/9-10 (stereo), Groups 1-2..7-8 (stereo),
//            Matrix 1-2/3-4 (stereo), FX sends 1-4 (mono, feeding the internal FX engines)
//   control: 4 DCAs, 4 mute groups
//   I/O    : 22 physical inputs (ch 1-16, ST1-3 L/R), 18 patchable physical outputs

#include <string>
#include <vector>

namespace qnu {

constexpr int kMonoInputs = 16;
constexpr int kStereoInputs = 3;
constexpr int kFxReturns = 4;
constexpr int kInputStrips = kMonoInputs + kStereoInputs + kFxReturns;

constexpr int kMixes = 7;  // Mix 1, 2, 3, 4, 5-6, 7-8, 9-10
constexpr int kMonoMixes = 4;
constexpr int kGroups = 4;
constexpr int kMatrices = 2;
constexpr int kFxUnits = 4;
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
constexpr int kBuses = kBusFx0 + kFxUnits;

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

}  // namespace qnu
