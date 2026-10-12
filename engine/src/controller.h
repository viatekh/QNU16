#pragma once

#include <string>
#include <vector>

#include "mixer.h"
#include "params.h"
#include "scene.h"

namespace qnu {

// Transport-independent control protocol (see docs/protocol.md). The network server feeds it
// client messages and delivers the replies; tests drive it directly.
class Controller {
 public:
  struct Reply {
    std::vector<std::string> toSender;
    std::vector<std::string> broadcast;  // to every client, including the sender
  };

  Controller(Mixer& mixer, ParamRegistry& reg, Labels& labels, SceneStore& scenes,
             std::string backendName);

  // One message may hold several newline-separated commands.
  Reply handle(const std::string& message);

  std::string infoMessage() const;   // "i {json}": topology
  std::string metaMessage() const;   // "k\n<path> <min> <max> <def> <kind> [unit]..." for every param
  std::string stateMessage() const;  // "p ..." and "l ..." lines for every param/label
  std::string meterMessage();        // "m v,v,v,..."; also latches the AMM gains for ammMessage()
  std::string ammMessage() const;    // "a v,...": AMM gain per channel (dB)
  std::string rtaMessage();          // "r v,...": 31 RTA bands (dBFS), 20 Hz - 20 kHz
  std::string driveMessage() const;  // "d <state> <mode> <seconds> <length> <dropouts> <file>"
  std::string driveListMessage() const;  // "q\n<name>..." recordings on the drive
  void setDrive(Recorder* r, Player* p) { recorder_ = r; player_ = p; }
  std::string sceneListMessage() const;

 private:
  void handleLine(const std::string& line, Reply* reply);
  void handleScene(std::istringstream& in, Reply* reply);
  // Sets a parameter and everything that follows from it on the Qu: linked channels, PAFL
  // auto-cancel, the AMM opening its faders. Broadcasts every change.
  void setParam(Param* p, float v, Reply* reply);
  void put(Param* p, float v, Reply* reply);  // set and broadcast only
  void mirrorLinked(const std::string& path, float v, Reply* reply);
  void copyToPartner(int pair, Reply* reply);

  Mixer& mixer_;
  ParamRegistry& reg_;
  Labels& labels_;
  SceneStore& scenes_;
  std::string backendName_;
  Recorder* recorder_ = nullptr;
  Player* player_ = nullptr;
  int currentScene_ = 0;
  std::array<float, kAmmChannels> ammDb_{};
};

}  // namespace qnu
