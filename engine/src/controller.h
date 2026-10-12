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
  std::string meterMessage();        // "m v,v,v,..."
  std::string sceneListMessage() const;

 private:
  void handleLine(const std::string& line, Reply* reply);

  Mixer& mixer_;
  ParamRegistry& reg_;
  Labels& labels_;
  SceneStore& scenes_;
  std::string backendName_;
  std::string currentScene_;
};

}  // namespace qnu
