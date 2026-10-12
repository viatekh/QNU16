#pragma once

#include <memory>
#include <string>

#include "mixer.h"

namespace qnu {

class AudioBackend {
 public:
  virtual ~AudioBackend() = default;
  // Calls mixer.prepare() with the device sample rate, then starts calling mixer.process().
  virtual bool start(Mixer& mixer, std::string* err) = 0;
  virtual void stop() = 0;
  virtual std::string name() const = 0;
};

struct JackOptions {
  std::string clientName = "qnu16";
  bool autoconnect = false;  // physical capture N -> in_N, out_N -> physical playback N
};

// Returns nullptr if the binary was built without JACK.
std::unique_ptr<AudioBackend> makeJackBackend(const JackOptions& options);

// Self-clocked backend with synthetic test signals on every input. For UI development and demos
// without an audio interface; output is discarded.
std::unique_ptr<AudioBackend> makeDummyBackend(double sampleRate = 48000.0);

}  // namespace qnu
