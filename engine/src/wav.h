#pragma once

#include <string>
#include <vector>

namespace qnu {

struct AudioFile {
  double sampleRate = 48000.0;
  std::vector<std::vector<float>> channels;  // deinterleaved
  size_t frames() const { return channels.empty() ? 0 : channels[0].size(); }
};

// Reads PCM 16/24/32-bit and 32-bit float WAV (incl. WAVE_FORMAT_EXTENSIBLE).
bool readWav(const std::string& path, AudioFile* out, std::string* err);
// Writes 32-bit float WAV.
bool writeWav(const std::string& path, const AudioFile& file, std::string* err);

}  // namespace qnu
