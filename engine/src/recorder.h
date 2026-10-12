#pragma once

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace qnu {

// Qu-Drive style recordings in a directory (the "drive"):
//   stereo    : QU-STnnn.WAV, 2 channels, 48 kHz 24-bit
//   multitrack: folder QU-MTnnn holding TRK01.WAV ... TRK18.WAV, one mono 24-bit file per track
// Numbers count up from 001. drive.cpp lists them.
std::string driveStereoName(int number);      // "QU-ST001.WAV"
std::string driveMultitrackName(int number);  // "QU-MT001"
std::string driveTrackName(int track);        // "TRK01.WAV" (track 0-based)
struct DriveEntry {
  bool multitrack;
  std::string name;
};
std::vector<DriveEntry> listDrive(const std::string& dir);

// Records to the drive. The audio thread pushes into a lock-free single-producer /
// single-consumer ring; a writer thread drains it to disk, so the audio thread never blocks on
// I/O.
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
  const std::string& file() const { return fileName_; }  // QU-STnnn.WAV or QU-MTnnn
  const std::string& dir() const { return dir_; }

  // Audio thread: tracks[t][i] for t < tracks(). Never blocks; drops (and counts) on overflow.
  void push(const float* const* tracks, int n);

 private:
  void writerLoop();
  void drain();

  std::string dir_;
  std::string fileName_;
  std::vector<std::FILE*> files_;  // one stereo file, or one per track
  Mode mode_ = kStereo;
  int tracks_ = 2;
  double sampleRate_ = 48000.0;
  size_t frames_written_ = 0;
  std::vector<unsigned char> scratch_;

  std::vector<float> ring_;
  std::atomic<size_t> head_{0}, tail_{0};  // in floats; head written by audio, tail by writer
  std::atomic<bool> active_{false};
  std::atomic<bool> writerRun_{false};
  std::atomic<unsigned> dropouts_{0};
  std::atomic<unsigned long long> frames_{0};
  std::thread writer_;
};

// Plays a drive recording back. A reader thread streams the file(s) into a lock-free ring; the
// audio thread pulls from it. Stereo playback feeds ST3, multitrack feeds the channels whose
// source is USB (tracks 1-16 to CH 1-16, 17-18 to ST1), as on the Qu.
class Player {
 public:
  explicit Player(std::string dir) : dir_(std::move(dir)) {}
  ~Player() { stop(); }

  // Control thread. name: a QU-STnnn.WAV file or a QU-MTnnn folder.
  bool start(const std::string& name, double sampleRate, std::string* err);
  void stop();
  bool playing() const { return active_.load(std::memory_order_acquire); }
  bool multitrack() const { return multitrack_; }
  int tracks() const { return tracks_; }
  double seconds() const;
  double length() const { return lengthFrames_ / sampleRate_; }
  const std::string& file() const { return name_; }

  // Audio thread: out[t][i] for t < tracks() (null entries are skipped). Silence once the
  // recording ends, and playing() turns false.
  void pull(float* const* out, int n);

 private:
  struct Source {
    std::FILE* fp = nullptr;
    int channels = 1, bits = 24;
    bool isFloat = false;
    size_t frames = 0;
  };
  void readerLoop();
  bool fill();  // false at the end of the recording

  std::string dir_, name_;
  std::vector<Source> sources_;
  bool multitrack_ = false;
  int tracks_ = 2;
  double sampleRate_ = 48000.0;
  size_t lengthFrames_ = 0, readFrames_ = 0;
  std::vector<unsigned char> scratch_;
  std::vector<float> chunk_;

  std::vector<float> ring_;
  std::atomic<size_t> head_{0}, tail_{0};  // head written by the reader, tail by audio
  std::atomic<bool> active_{false}, readerRun_{false}, eof_{false};
  std::atomic<unsigned long long> frames_{0};
  std::thread reader_;
};

}  // namespace qnu
