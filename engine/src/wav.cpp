#include "wav.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace qnu {

namespace {

uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t le16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xff)); }
void put16(std::string& s, uint16_t v) { s.push_back(char(v & 0xff)); s.push_back(char(v >> 8)); }

}  // namespace

bool readWav(const std::string& path, AudioFile* out, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    *err = "cannot open " + path;
    return false;
  }
  const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const auto* d = reinterpret_cast<const unsigned char*>(data.data());
  if (data.size() < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) {
    *err = path + ": not a RIFF/WAVE file";
    return false;
  }
  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  const unsigned char* samples = nullptr;
  size_t sampleBytes = 0;
  for (size_t pos = 12; pos + 8 <= data.size();) {
    const uint32_t size = le32(d + pos + 4);
    const size_t body = pos + 8;
    const size_t avail = std::min<size_t>(size, data.size() - body);
    if (std::memcmp(d + pos, "fmt ", 4) == 0 && avail >= 16) {
      format = le16(d + body);
      channels = le16(d + body + 2);
      rate = le32(d + body + 4);
      bits = le16(d + body + 14);
      if (format == 0xFFFE && avail >= 26) format = le16(d + body + 24);  // extensible subformat
    } else if (std::memcmp(d + pos, "data", 4) == 0) {
      samples = d + body;
      sampleBytes = avail;
    }
    pos = body + size + (size & 1);
  }
  const bool pcm = format == 1 && (bits == 16 || bits == 24 || bits == 32);
  const bool flt = format == 3 && bits == 32;
  if (!samples || channels == 0 || rate == 0 || (!pcm && !flt)) {
    *err = path + ": unsupported WAV format (need PCM 16/24/32 or float32)";
    return false;
  }
  const size_t bytesPer = bits / 8;
  const size_t frames = sampleBytes / (bytesPer * channels);
  out->sampleRate = rate;
  out->channels.assign(channels, std::vector<float>(frames));
  for (size_t i = 0; i < frames; ++i) {
    for (size_t c = 0; c < channels; ++c) {
      const unsigned char* p = samples + (i * channels + c) * bytesPer;
      float v;
      if (flt) {
        uint32_t u = le32(p);
        std::memcpy(&v, &u, 4);
      } else if (bits == 16) {
        v = static_cast<int16_t>(le16(p)) / 32768.0f;
      } else if (bits == 24) {
        int32_t s = (p[0] << 8) | (p[1] << 16) | (int32_t(p[2]) << 24);
        v = static_cast<float>((s >> 8) / 8388608.0);
      } else {
        v = static_cast<float>(static_cast<int32_t>(le32(p)) / 2147483648.0);
      }
      out->channels[c][i] = v;
    }
  }
  return true;
}

bool writeWav(const std::string& path, const AudioFile& file, std::string* err) {
  const uint16_t channels = static_cast<uint16_t>(file.channels.size());
  const size_t frames = file.frames();
  const uint32_t dataBytes = static_cast<uint32_t>(frames * channels * 4);
  const uint32_t rate = static_cast<uint32_t>(file.sampleRate);
  std::string s;
  s.reserve(44 + dataBytes);
  s += "RIFF"; put32(s, 36 + dataBytes); s += "WAVE";
  s += "fmt "; put32(s, 16); put16(s, 3); put16(s, channels); put32(s, rate);
  put32(s, rate * channels * 4); put16(s, static_cast<uint16_t>(channels * 4)); put16(s, 32);
  s += "data"; put32(s, dataBytes);
  for (size_t i = 0; i < frames; ++i)
    for (const auto& ch : file.channels) {
      uint32_t u;
      std::memcpy(&u, &ch[i], 4);
      put32(s, u);
    }
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f || !f.write(s.data(), static_cast<std::streamsize>(s.size()))) {
    *err = "cannot write " + path;
    return false;
  }
  return true;
}

}  // namespace qnu
