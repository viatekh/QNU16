#pragma once

#include <cstdint>
#include <string>

namespace qnu::ws {

std::string sha1(const std::string& data);  // 20 raw bytes
std::string base64(const std::string& data);
// Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key (RFC 6455 §4.2.2).
std::string acceptKey(const std::string& clientKey);

enum Opcode : uint8_t { kContinuation = 0, kText = 1, kBinary = 2, kClose = 8, kPing = 9, kPong = 10 };

struct Frame {
  bool fin = true;
  uint8_t opcode = kText;
  std::string payload;
};

enum class ParseResult { kNeedMore, kFrame, kError };

constexpr size_t kMaxPayload = 64 * 1024;

// Parses one client->server frame from the front of buf. Client frames must be masked.
// On kFrame, *consumed is the number of bytes to drop from buf.
ParseResult parseFrame(const std::string& buf, Frame* frame, size_t* consumed);

// Server->client frame (unmasked).
std::string encodeFrame(uint8_t opcode, const std::string& payload);

// Client->server frame (masked) - used by tests and tools.
std::string encodeMaskedFrame(uint8_t opcode, const std::string& payload, uint32_t mask);

}  // namespace qnu::ws
