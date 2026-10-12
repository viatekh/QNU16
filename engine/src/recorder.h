#pragma once

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace qnu {

// Records interleaved tracks to a 32-bit float WAV file, like the Qu-Drive:
//   multitrack: 18 tracks (Ch 1-16 direct outs + LR)
//   stereo    : LR only
// The audio thread pushes into a lock-free single-producer/single-consumer ring; a writer
// thread drains it to disk, so the audio thread never blocks on I/O.
class Recorder {
 public:
  enum Mode { kStereo = 0, kMultitrack = 1 };
  static constexpr int kMultitrackTracks = 18;

  explicit Recorder(std::string dir) : dir_(std::move(dir)) {}
  ~Recorder() { stop(); }

  // Control thread.
  bool start(Mode mode, double sampleRate, std::string* err);
  void stop();
  bool recording() const { return active_.load(std::memory_order_acquire); }
  Mode mode() const { return mode_; }
  int tracks() const { return tracks_; }
  double seconds() const;
  unsigned dropouts() const { return dropouts_.load(std::memory_order_relaxed); }
  const std::string& file() const { return fileName_; }
  const std::string& dir() const { return dir_; }

  // Audio thread: tracks[t][i] for t < tracks(). Never blocks; drops (and counts) on overflow.
  void push(const float* const* tracks, int n);

 private:
  void writerLoop();
  void drain();

  std::string dir_;
  std::string fileName_;
  std::FILE* fp_ = nullptr;
  Mode mode_ = kStereo;
  int tracks_ = 2;
  double sampleRate_ = 48000.0;
  size_t dataBytes_ = 0;

  std::vector<float> ring_;
  std::atomic<size_t> head_{0}, tail_{0};  // in floats; head written by audio, tail by writer
  std::atomic<bool> active_{false};
  std::atomic<bool> writerRun_{false};
  std::atomic<unsigned> dropouts_{0};
  std::atomic<unsigned long long> frames_{0};
  std::thread writer_;
};

}  // namespace qnu
