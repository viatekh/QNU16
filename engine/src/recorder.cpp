#include "recorder.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace qnu {

namespace {

constexpr double kRingSeconds = 4.0;

void put32(std::FILE* f, uint32_t v) {
  const unsigned char b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
  std::fwrite(b, 1, 4, f);
}
void put16(std::FILE* f, uint16_t v) {
  const unsigned char b[2] = {uint8_t(v), uint8_t(v >> 8)};
  std::fwrite(b, 1, 2, f);
}

void writeHeader(std::FILE* f, int channels, uint32_t rate, uint32_t dataBytes) {
  std::fseek(f, 0, SEEK_SET);
  std::fwrite("RIFF", 1, 4, f);
  put32(f, 36 + dataBytes);
  std::fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 3);  // IEEE float
  put16(f, static_cast<uint16_t>(channels));
  put32(f, rate);
  put32(f, rate * channels * 4);
  put16(f, static_cast<uint16_t>(channels * 4));
  put16(f, 32);
  std::fwrite("data", 1, 4, f);
  put32(f, dataBytes);
}

}  // namespace

bool Recorder::start(Mode mode, double sampleRate, std::string* err) {
  if (recording()) {
    *err = "already recording";
    return false;
  }
  stop();  // join any finished writer
  mode_ = mode;
  tracks_ = mode == kMultitrack ? kMultitrackTracks : 2;
  sampleRate_ = sampleRate;

  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  char stamp[32];
  const std::time_t now = std::time(nullptr);
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
  fileName_ = std::string(mode == kMultitrack ? "multitrack-" : "stereo-") + stamp + ".wav";
  fp_ = std::fopen((dir_ + "/" + fileName_).c_str(), "wb");
  if (!fp_) {
    *err = "cannot create " + dir_ + "/" + fileName_;
    return false;
  }
  writeHeader(fp_, tracks_, static_cast<uint32_t>(sampleRate_), 0);
  dataBytes_ = 0;

  ring_.assign(static_cast<size_t>(kRingSeconds * sampleRate_) * tracks_, 0.0f);
  head_.store(0);
  tail_.store(0);
  frames_.store(0);
  dropouts_.store(0);
  writerRun_.store(true);
  writer_ = std::thread([this] { writerLoop(); });
  active_.store(true, std::memory_order_release);
  return true;
}

void Recorder::stop() {
  active_.store(false, std::memory_order_release);
  if (writer_.joinable()) {
    writerRun_.store(false);
    writer_.join();
  }
  if (fp_) {
    drain();
    writeHeader(fp_, tracks_, static_cast<uint32_t>(sampleRate_), static_cast<uint32_t>(dataBytes_));
    std::fclose(fp_);
    fp_ = nullptr;
  }
}

double Recorder::seconds() const { return frames_.load(std::memory_order_relaxed) / sampleRate_; }

void Recorder::push(const float* const* tracks, int n) {
  if (!active_.load(std::memory_order_acquire)) return;
  const size_t size = ring_.size();
  const size_t need = static_cast<size_t>(n) * tracks_;
  const size_t head = head_.load(std::memory_order_relaxed);
  const size_t tail = tail_.load(std::memory_order_acquire);
  const size_t used = head >= tail ? head - tail : size - tail + head;
  if (size - used - 1 < need) {
    dropouts_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  size_t pos = head;
  for (int i = 0; i < n; ++i)
    for (int t = 0; t < tracks_; ++t) {
      ring_[pos] = tracks[t] ? tracks[t][i] : 0.0f;
      if (++pos == size) pos = 0;
    }
  head_.store(pos, std::memory_order_release);
  frames_.fetch_add(static_cast<unsigned long long>(n), std::memory_order_relaxed);
}

void Recorder::drain() {
  const size_t size = ring_.size();
  size_t tail = tail_.load(std::memory_order_relaxed);
  const size_t head = head_.load(std::memory_order_acquire);
  while (tail != head) {
    const size_t end = head > tail ? head : size;
    const size_t count = end - tail;
    std::fwrite(ring_.data() + tail, sizeof(float), count, fp_);
    dataBytes_ += count * sizeof(float);
    tail = end == size ? 0 : end;
  }
  tail_.store(tail, std::memory_order_release);
}

void Recorder::writerLoop() {
  while (writerRun_.load()) {
    drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

}  // namespace qnu
