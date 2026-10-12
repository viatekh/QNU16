#include "backend/backend.h"

#ifdef QNU_HAVE_JACK

#include <jack/jack.h>

#include <atomic>
#include <cstdio>

namespace qnu {

namespace {

class JackBackend : public AudioBackend {
 public:
  explicit JackBackend(JackOptions opt) : opt_(std::move(opt)) {}
  ~JackBackend() override { stop(); }

  bool start(Mixer& mixer, std::string* err) override {
    mixer_ = &mixer;
    jack_status_t status;
    client_ = jack_client_open(opt_.clientName.c_str(), JackNoStartServer, &status);
    if (!client_) {
      *err = "cannot connect to JACK server (is jackd running?)";
      return false;
    }
    for (int c = 0; c < kChannels; ++c) {
      const std::string n = "in_" + std::to_string(c + 1);
      in_[c] = jack_port_register(client_, n.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    }
    for (int o = 0; o < kOutputs; ++o)
      out_[o] = jack_port_register(client_, outputName(o), JACK_DEFAULT_AUDIO_TYPE,
                                   JackPortIsOutput, 0);

    mixer.prepare(jack_get_sample_rate(client_));
    jack_set_process_callback(client_, &JackBackend::processCb, this);
    jack_set_xrun_callback(client_, &JackBackend::xrunCb, this);
    if (jack_activate(client_) != 0) {
      *err = "cannot activate JACK client";
      return false;
    }
    if (opt_.autoconnect) autoconnect();
    return true;
  }

  void stop() override {
    if (client_) {
      jack_deactivate(client_);
      jack_client_close(client_);
      client_ = nullptr;
    }
  }

  std::string name() const override { return "jack"; }

 private:
  static int processCb(jack_nframes_t n, void* arg) {
    auto* self = static_cast<JackBackend*>(arg);
    const float* in[kChannels];
    float* out[kOutputs];
    for (int c = 0; c < kChannels; ++c)
      in[c] = static_cast<const float*>(jack_port_get_buffer(self->in_[c], n));
    for (int o = 0; o < kOutputs; ++o)
      out[o] = static_cast<float*>(jack_port_get_buffer(self->out_[o], n));
    self->mixer_->process(in, out, static_cast<int>(n));
    return 0;
  }

  static int xrunCb(void* arg) {
    static_cast<JackBackend*>(arg)->xruns_.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }

  void autoconnect() {
    const char** capture = jack_get_ports(client_, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                          JackPortIsPhysical | JackPortIsOutput);
    const char** playback = jack_get_ports(client_, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                           JackPortIsPhysical | JackPortIsInput);
    for (int c = 0; capture && capture[c] && c < kChannels; ++c)
      jack_connect(client_, capture[c], jack_port_name(in_[c]));
    for (int o = 0; playback && playback[o] && o < kOutputs; ++o)
      jack_connect(client_, jack_port_name(out_[o]), playback[o]);
    if (capture) jack_free(capture);
    if (playback) jack_free(playback);
  }

  JackOptions opt_;
  Mixer* mixer_ = nullptr;
  jack_client_t* client_ = nullptr;
  jack_port_t* in_[kChannels] = {};
  jack_port_t* out_[kOutputs] = {};
  std::atomic<unsigned> xruns_{0};
};

}  // namespace

std::unique_ptr<AudioBackend> makeJackBackend(const JackOptions& options) {
  return std::make_unique<JackBackend>(options);
}

}  // namespace qnu

#else

namespace qnu {
std::unique_ptr<AudioBackend> makeJackBackend(const JackOptions&) { return nullptr; }
}  // namespace qnu

#endif
