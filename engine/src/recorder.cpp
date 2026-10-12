#include "recorder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>

namespace qnu {

namespace fs = std::filesystem;

namespace {

constexpr double kRingSeconds = 4.0;
constexpr size_t kReadChunkFrames = 4096;

void put32(std::FILE* f, uint32_t v) {
  const unsigned char b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
  std::fwrite(b, 1, 4, f);
}
void put16(std::FILE* f, uint16_t v) {
  const unsigned char b[2] = {uint8_t(v), uint8_t(v >> 8)};
  std::fwrite(b, 1, 2, f);
}

// 24-bit PCM WAV header.
void writeHeader(std::FILE* f, int channels, uint32_t rate, uint32_t dataBytes) {
  std::fseek(f, 0, SEEK_SET);
  std::fwrite("RIFF", 1, 4, f);
  put32(f, 36 + dataBytes);
  std::fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);  // PCM
  put16(f, static_cast<uint16_t>(channels));
  put32(f, rate);
  put32(f, rate * channels * 3);
  put16(f, static_cast<uint16_t>(channels * 3));
  put16(f, 24);
  std::fwrite("data", 1, 4, f);
  put32(f, dataBytes);
}

void encode24(float v, unsigned char* p) {
  const double c = std::clamp(static_cast<double>(v), -1.0, 1.0 - 1.0 / 8388608.0);
  const int32_t s = static_cast<int32_t>(std::lround(c * 8388608.0));
  p[0] = uint8_t(s);
  p[1] = uint8_t(s >> 8);
  p[2] = uint8_t(s >> 16);
}

std::string number3(int n) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%03d", n);
  return buf;
}

// Lowest unused recording number on the drive.
int nextNumber(const std::string& dir) {
  int best = 0;
  for (const auto& e : listDrive(dir)) {
    const int n = std::atoi(e.name.c_str() + 5);
    best = std::max(best, n);
  }
  return best + 1;
}

uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t le16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// Opens a WAV file and seeks to its sample data.
bool openWav(const std::string& path, std::FILE** out, int* channels, int* bits, bool* isFloat,
             size_t* frames, double* rate) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  unsigned char hdr[12];
  if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4)) {
    std::fclose(f);
    return false;
  }
  int format = 0;
  for (;;) {
    unsigned char ch[8];
    if (std::fread(ch, 1, 8, f) != 8) break;
    const uint32_t size = le32(ch + 4);
    if (!std::memcmp(ch, "fmt ", 4)) {
      unsigned char fmt[40] = {};
      const size_t got = std::fread(fmt, 1, std::min<uint32_t>(size, sizeof(fmt)), f);
      if (got < 16) break;
      format = le16(fmt);
      *channels = le16(fmt + 2);
      *rate = le32(fmt + 4);
      *bits = le16(fmt + 14);
      if (format == 0xFFFE && got >= 26) format = le16(fmt + 24);
      if (size > got) std::fseek(f, static_cast<long>(size - got), SEEK_CUR);
      if (size & 1) std::fseek(f, 1, SEEK_CUR);
    } else if (!std::memcmp(ch, "data", 4)) {
      *isFloat = format == 3;
      const bool ok = (format == 1 && (*bits == 16 || *bits == 24 || *bits == 32)) ||
                      (format == 3 && *bits == 32);
      if (!ok || *channels <= 0) break;
      *frames = size / (*channels * (*bits / 8));
      *out = f;
      return true;
    } else {
      std::fseek(f, static_cast<long>(size + (size & 1)), SEEK_CUR);
    }
  }
  std::fclose(f);
  return false;
}

float decode(const unsigned char* p, int bits, bool isFloat) {
  if (isFloat) {
    const uint32_t u = le32(p);
    float v;
    std::memcpy(&v, &u, 4);
    return v;
  }
  if (bits == 16) return static_cast<int16_t>(le16(p)) / 32768.0f;
  if (bits == 24) {
    const int32_t s = (p[0] << 8) | (p[1] << 16) | (int32_t(p[2]) << 24);
    return static_cast<float>((s >> 8) / 8388608.0);
  }
  return static_cast<float>(static_cast<int32_t>(le32(p)) / 2147483648.0);
}

}  // namespace

std::string driveStereoName(int number) { return "QU-ST" + number3(number) + ".WAV"; }
std::string driveMultitrackName(int number) { return "QU-MT" + number3(number); }
std::string driveTrackName(int track) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "TRK%02d.WAV", track + 1);
  return buf;
}

std::vector<DriveEntry> listDrive(const std::string& dir) {
  std::vector<DriveEntry> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    const std::string name = e.path().filename().string();
    auto digits = [&](size_t from) {
      return name.size() >= from + 3 && std::all_of(name.begin() + from, name.begin() + from + 3, ::isdigit);
    };
    if (name.size() == 12 && name.rfind("QU-ST", 0) == 0 && digits(5) && name.substr(8) == ".WAV")
      out.push_back({false, name});
    else if (name.size() == 8 && name.rfind("QU-MT", 0) == 0 && digits(5) && e.is_directory())
      out.push_back({true, name});
  }
  std::sort(out.begin(), out.end(), [](const DriveEntry& a, const DriveEntry& b) { return a.name < b.name; });
  return out;
}

// ---- Recorder ----

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
  fs::create_directories(dir_, ec);
  const int number = nextNumber(dir_);
  const auto rate = static_cast<uint32_t>(sampleRate_);
  if (mode == kMultitrack) {
    fileName_ = driveMultitrackName(number);
    fs::create_directories(dir_ + "/" + fileName_, ec);
    for (int t = 0; t < tracks_; ++t) {
      const std::string path = dir_ + "/" + fileName_ + "/" + driveTrackName(t);
      std::FILE* f = std::fopen(path.c_str(), "wb");
      if (!f) {
        *err = "cannot create " + path;
        for (auto* g : files_) std::fclose(g);
        files_.clear();
        return false;
      }
      writeHeader(f, 1, rate, 0);
      files_.push_back(f);
    }
  } else {
    fileName_ = driveStereoName(number);
    const std::string path = dir_ + "/" + fileName_;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
      *err = "cannot create " + path;
      return false;
    }
    writeHeader(f, 2, rate, 0);
    files_.push_back(f);
  }
  frames_written_ = 0;

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
  if (!files_.empty()) {
    drain();
    const auto rate = static_cast<uint32_t>(sampleRate_);
    const int perFile = files_.size() == 1 ? tracks_ : 1;
    for (auto* f : files_) {
      writeHeader(f, perFile, rate, static_cast<uint32_t>(frames_written_ * perFile * 3));
      std::fclose(f);
    }
    files_.clear();
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
  const size_t avail = (head >= tail ? head - tail : size - tail + head) / tracks_;  // frames
  if (avail == 0) return;
  const bool split = files_.size() > 1;
  scratch_.resize(avail * tracks_ * 3);
  if (split) {
    // Deinterleave into one run of 24-bit samples per track.
    for (size_t i = 0; i < avail; ++i)
      for (int t = 0; t < tracks_; ++t) {
        encode24(ring_[tail], &scratch_[(t * avail + i) * 3]);
        if (++tail == size) tail = 0;
      }
    for (int t = 0; t < tracks_; ++t) std::fwrite(&scratch_[t * avail * 3], 3, avail, files_[t]);
  } else {
    for (size_t k = 0; k < avail * tracks_; ++k) {
      encode24(ring_[tail], &scratch_[k * 3]);
      if (++tail == size) tail = 0;
    }
    std::fwrite(scratch_.data(), 3, avail * tracks_, files_[0]);
  }
  frames_written_ += avail;
  tail_.store(tail, std::memory_order_release);
}

void Recorder::writerLoop() {
  while (writerRun_.load()) {
    drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

// ---- Player ----

bool Player::start(const std::string& name, double sampleRate, std::string* err) {
  stop();
  const bool multi = name.size() == 8 && name.rfind("QU-MT", 0) == 0;
  const bool stereo = name.size() == 12 && name.rfind("QU-ST", 0) == 0;
  if ((!multi && !stereo) || name.find('/') != std::string::npos) {
    *err = "not a Qu-Drive recording: " + name;
    return false;
  }
  multitrack_ = multi;
  sampleRate_ = sampleRate;
  std::vector<std::string> paths;
  if (multi)
    for (int t = 0; t < Recorder::kMultitrackTracks; ++t) paths.push_back(dir_ + "/" + name + "/" + driveTrackName(t));
  else
    paths.push_back(dir_ + "/" + name);
  tracks_ = 0;
  lengthFrames_ = 0;
  for (const auto& path : paths) {
    Source s;
    double rate = 0;
    if (!openWav(path, &s.fp, &s.channels, &s.bits, &s.isFloat, &s.frames, &rate)) {
      if (multi && !sources_.empty()) break;  // fewer tracks than 18 is fine
      *err = "cannot play " + path;
      stop();
      return false;
    }
    tracks_ += s.channels;
    lengthFrames_ = std::max(lengthFrames_, s.frames);
    sources_.push_back(s);
  }
  tracks_ = std::min(tracks_, Recorder::kMultitrackTracks);
  name_ = name;
  readFrames_ = 0;
  ring_.assign(static_cast<size_t>(kRingSeconds * sampleRate_) * tracks_, 0.0f);
  head_.store(0);
  tail_.store(0);
  frames_.store(0);
  eof_.store(false);
  fill();  // prime the ring before the audio thread starts pulling
  readerRun_.store(true);
  reader_ = std::thread([this] { readerLoop(); });
  active_.store(true, std::memory_order_release);
  return true;
}

void Player::stop() {
  active_.store(false, std::memory_order_release);
  if (reader_.joinable()) {
    readerRun_.store(false);
    reader_.join();
  }
  for (auto& s : sources_) std::fclose(s.fp);
  sources_.clear();
}

double Player::seconds() const { return frames_.load(std::memory_order_relaxed) / sampleRate_; }

bool Player::fill() {
  const size_t size = ring_.size();
  for (;;) {
    if (readFrames_ >= lengthFrames_) {
      eof_.store(true, std::memory_order_release);
      return false;
    }
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    const size_t used = head >= tail ? head - tail : size - tail + head;
    const size_t freeFrames = (size - used - 1) / tracks_;
    const size_t frames = std::min({freeFrames, kReadChunkFrames, lengthFrames_ - readFrames_});
    if (frames == 0) return true;
    chunk_.assign(frames * tracks_, 0.0f);
    int track = 0;
    for (auto& s : sources_) {
      const int bytes = s.bits / 8;
      scratch_.resize(frames * s.channels * bytes);
      const size_t got = s.frames > readFrames_ ? std::fread(scratch_.data(), s.channels * bytes, std::min(frames, s.frames - readFrames_), s.fp) : 0;
      for (size_t i = 0; i < got; ++i)
        for (int c = 0; c < s.channels && track + c < tracks_; ++c)
          chunk_[i * tracks_ + track + c] = decode(&scratch_[(i * s.channels + c) * bytes], s.bits, s.isFloat);
      track += s.channels;
    }
    size_t pos = head;
    for (float v : chunk_) {
      ring_[pos] = v;
      if (++pos == size) pos = 0;
    }
    head_.store(pos, std::memory_order_release);
    readFrames_ += frames;
  }
}

void Player::readerLoop() {
  while (readerRun_.load()) {
    if (!fill() && eof_.load()) {
      // Keep the thread until stop(); the audio thread drains what is left.
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

void Player::pull(float* const* out, int n) {
  if (!active_.load(std::memory_order_acquire)) return;
  const size_t size = ring_.size();
  size_t tail = tail_.load(std::memory_order_relaxed);
  const size_t head = head_.load(std::memory_order_acquire);
  const size_t avail = (head >= tail ? head - tail : size - tail + head) / tracks_;
  const int frames = static_cast<int>(std::min<size_t>(avail, static_cast<size_t>(n)));
  for (int i = 0; i < frames; ++i)
    for (int t = 0; t < tracks_; ++t) {
      if (out[t]) out[t][i] = ring_[tail];
      if (++tail == size) tail = 0;
    }
  for (int t = 0; t < tracks_; ++t)
    if (out[t]) std::fill(out[t] + frames, out[t] + n, 0.0f);
  tail_.store(tail, std::memory_order_release);
  frames_.fetch_add(static_cast<unsigned long long>(frames), std::memory_order_relaxed);
  if (frames < n && eof_.load(std::memory_order_acquire)) active_.store(false, std::memory_order_release);
}

}  // namespace qnu
