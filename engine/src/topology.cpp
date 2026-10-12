#include "topology.h"

namespace qnu {

namespace {

std::string num(int i) { return std::to_string(i); }

// "5-6" style names for stereo mixes/groups/matrices (1-based first channel).
std::string pairName(int first) { return num(first) + "-" + num(first + 1); }

}  // namespace

const std::vector<InputDef>& inputDefs() {
  static const std::vector<InputDef> defs = [] {
    std::vector<InputDef> d;
    for (int i = 0; i < kMonoInputs; ++i)
      d.push_back({InputKind::Mono, i, "ch/" + num(i + 1), "Ch " + num(i + 1), false, i});
    for (int i = 0; i < kStereoInputs; ++i)
      d.push_back({InputKind::Stereo, i, "st/" + num(i + 1), "ST " + num(i + 1), true,
                   kMonoInputs + 2 * i});
    for (int i = 0; i < kFxReturns; ++i)
      d.push_back({InputKind::FxReturn, i, "fxret/" + num(i + 1), "FX Ret " + num(i + 1), true, -1});
    return d;
  }();
  return defs;
}

const std::vector<BusDef>& busDefs() {
  static const std::vector<BusDef> defs = [] {
    std::vector<BusDef> d;
    d.push_back({BusKind::LR, 0, "lr", "LR", true});
    for (int i = 0; i < kMixes; ++i) {
      const bool stereo = i >= kMonoMixes;
      const std::string name =
          stereo ? "Mix " + pairName(kMonoMixes + 1 + 2 * (i - kMonoMixes)) : "Mix " + num(i + 1);
      d.push_back({BusKind::Mix, i, "mix/" + num(i + 1), name, stereo});
    }
    for (int i = 0; i < kGroups; ++i)
      d.push_back({BusKind::Group, i, "grp/" + num(i + 1), "Grp " + pairName(1 + 2 * i), true});
    for (int i = 0; i < kMatrices; ++i)
      d.push_back({BusKind::Matrix, i, "mtx/" + num(i + 1), "Mtx " + pairName(1 + 2 * i), true});
    for (int i = 0; i < kFxSends; ++i)
      d.push_back({BusKind::FxSend, i, "fx/" + num(i + 1), "FX " + num(i + 1), false});
    return d;
  }();
  return defs;
}

const std::vector<OutputSource>& outputSources() {
  static const std::vector<OutputSource> srcs = [] {
    std::vector<OutputSource> s;
    s.push_back({"Off", OutputSource::Off, 0, 0});
    const auto& buses = busDefs();
    // Physical-style names: stereo buses expand to their two legs (Mix 5, Mix 6, Grp 1, ...).
    auto addBus = [&](int b, const std::string& base, int firstNumber) {
      if (buses[b].stereo) {
        s.push_back({base + " " + num(firstNumber), OutputSource::Bus, b, 0});
        s.push_back({base + " " + num(firstNumber + 1), OutputSource::Bus, b, 1});
      } else {
        s.push_back({base + " " + num(firstNumber), OutputSource::Bus, b, 0});
      }
    };
    s.push_back({"LR L", OutputSource::Bus, kBusLR, 0});
    s.push_back({"LR R", OutputSource::Bus, kBusLR, 1});
    for (int i = 0; i < kMixes; ++i)
      addBus(kBusMix0 + i, "Mix", i < kMonoMixes ? i + 1 : kMonoMixes + 1 + 2 * (i - kMonoMixes));
    for (int i = 0; i < kGroups; ++i) addBus(kBusGrp0 + i, "Grp", 1 + 2 * i);
    for (int i = 0; i < kMatrices; ++i) addBus(kBusMtx0 + i, "Mtx", 1 + 2 * i);
    s.push_back({"PAFL L", OutputSource::Pafl, 0, 0});
    s.push_back({"PAFL R", OutputSource::Pafl, 0, 1});
    for (int i = 0; i < kFxUnits; ++i) {
      s.push_back({"FX " + num(i + 1) + " L", OutputSource::FxReturn, i, 0});
      s.push_back({"FX " + num(i + 1) + " R", OutputSource::FxReturn, i, 1});
    }
    for (int i = 0; i < kMonoInputs; ++i)
      s.push_back({"Ch " + num(i + 1) + " DO", OutputSource::Direct, i, 0});
    for (int i = 0; i < kStereoInputs; ++i) {
      s.push_back({"ST " + num(i + 1) + " L DO", OutputSource::Direct, kMonoInputs + 2 * i, 0});
      s.push_back({"ST " + num(i + 1) + " R DO", OutputSource::Direct, kMonoInputs + 2 * i + 1, 0});
    }
    return s;
  }();
  return srcs;
}

int defaultOutputSource(int physOut) {
  // As the Qu-16's sockets: out 1-2 LR, 3-12 Mix 1-10, 13-14 Alt out (PAFL), 15-18 spare.
  static const char* names[kPhysOutputs] = {"LR L",  "LR R",  "Mix 1", "Mix 2",  "Mix 3", "Mix 4",
                                            "Mix 5", "Mix 6", "Mix 7", "Mix 8",  "Mix 9", "Mix 10",
                                            "PAFL L", "PAFL R", "Off", "Off", "Off", "Off"};
  if (physOut < 0 || physOut >= kPhysOutputs) return 0;
  const auto& srcs = outputSources();
  for (size_t i = 0; i < srcs.size(); ++i)
    if (srcs[i].name == names[physOut]) return static_cast<int>(i);
  return 0;
}

std::vector<std::string> fxTargetNames() {
  std::vector<std::string> n = {"None"};
  for (int i = 0; i < kFxTargetInputs; ++i) n.push_back(inputDefs()[i].name);
  for (const auto& b : busDefs()) n.push_back(b.name);
  return n;
}

const std::vector<UsbSource>& usbSources() {
  static const std::vector<UsbSource> srcs = [] {
    std::vector<UsbSource> s;
    s.push_back({"Off", UsbSource::Off, 0, 0});
    for (int i = 0; i < kMonoInputs; ++i) s.push_back({"CH " + num(i + 1), UsbSource::Channel, i, 0});
    for (int i = 0; i < kStereoInputs; ++i) {
      s.push_back({"ST" + num(i + 1) + " L", UsbSource::Channel, kMonoInputs + 2 * i, 0});
      s.push_back({"ST" + num(i + 1) + " R", UsbSource::Channel, kMonoInputs + 2 * i + 1, 0});
    }
    for (const auto& o : outputSources())
      if (o.kind == OutputSource::Bus) s.push_back({o.name, UsbSource::Bus, o.index, o.side});
    return s;
  }();
  return srcs;
}

int defaultUsbSource(int track) {
  const std::string want = track < kMonoInputs ? "CH " + num(track + 1) : track == 16 ? "LR L" : "LR R";
  const auto& srcs = usbSources();
  for (size_t i = 0; i < srcs.size(); ++i)
    if (srcs[i].name == want) return static_cast<int>(i);
  return 0;
}

}  // namespace qnu
