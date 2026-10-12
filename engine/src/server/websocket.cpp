#include "server/websocket.h"

namespace qnu::ws {

namespace {

uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

}  // namespace

std::string sha1(const std::string& data) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::string msg = data;
  const uint64_t bitLen = static_cast<uint64_t>(data.size()) * 8;
  msg.push_back(static_cast<char>(0x80));
  while (msg.size() % 64 != 56) msg.push_back(0);
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xff));

  for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const auto* p = reinterpret_cast<const unsigned char*>(msg.data() + chunk + i * 4);
      w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      const uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  std::string out;
  for (uint32_t v : h)
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
  return out;
}

std::string base64(const std::string& data) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    const uint32_t v = (uint8_t(data[i]) << 16) | (uint8_t(data[i + 1]) << 8) | uint8_t(data[i + 2]);
    out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
  }
  if (i + 1 == data.size()) {
    const uint32_t v = uint8_t(data[i]) << 16;
    out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += "==";
  } else if (i + 2 == data.size()) {
    const uint32_t v = (uint8_t(data[i]) << 16) | (uint8_t(data[i + 1]) << 8);
    out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
  }
  return out;
}

std::string acceptKey(const std::string& clientKey) {
  return base64(sha1(clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

ParseResult parseFrame(const std::string& buf, Frame* frame, size_t* consumed) {
  if (buf.size() < 2) return ParseResult::kNeedMore;
  const auto* p = reinterpret_cast<const unsigned char*>(buf.data());
  const bool fin = p[0] & 0x80;
  const uint8_t opcode = p[0] & 0x0f;
  if (p[0] & 0x70) return ParseResult::kError;  // no extensions negotiated
  const bool masked = p[1] & 0x80;
  if (!masked) return ParseResult::kError;
  uint64_t len = p[1] & 0x7f;
  size_t pos = 2;
  if (len == 126) {
    if (buf.size() < 4) return ParseResult::kNeedMore;
    len = (uint64_t(p[2]) << 8) | p[3];
    pos = 4;
  } else if (len == 127) {
    if (buf.size() < 10) return ParseResult::kNeedMore;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
    pos = 10;
  }
  if (len > kMaxPayload) return ParseResult::kError;
  if (buf.size() < pos + 4 + len) return ParseResult::kNeedMore;
  const unsigned char* mask = p + pos;
  pos += 4;
  frame->fin = fin;
  frame->opcode = opcode;
  frame->payload.resize(len);
  for (uint64_t i = 0; i < len; ++i) frame->payload[i] = static_cast<char>(p[pos + i] ^ mask[i % 4]);
  *consumed = pos + len;
  return ParseResult::kFrame;
}

namespace {

std::string header(uint8_t opcode, size_t len, bool masked) {
  std::string out;
  out.push_back(static_cast<char>(0x80 | opcode));
  const char m = masked ? char(0x80) : 0;
  if (len < 126) {
    out.push_back(static_cast<char>(m | len));
  } else if (len <= 0xffff) {
    out.push_back(static_cast<char>(m | 126));
    out.push_back(static_cast<char>(len >> 8));
    out.push_back(static_cast<char>(len & 0xff));
  } else {
    out.push_back(static_cast<char>(m | 127));
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((uint64_t(len) >> (i * 8)) & 0xff));
  }
  return out;
}

}  // namespace

std::string encodeFrame(uint8_t opcode, const std::string& payload) {
  return header(opcode, payload.size(), false) + payload;
}

std::string encodeMaskedFrame(uint8_t opcode, const std::string& payload, uint32_t mask) {
  std::string out = header(opcode, payload.size(), true);
  const unsigned char m[4] = {uint8_t(mask >> 24), uint8_t(mask >> 16), uint8_t(mask >> 8), uint8_t(mask)};
  for (unsigned char c : m) out.push_back(static_cast<char>(c));
  for (size_t i = 0; i < payload.size(); ++i) out.push_back(static_cast<char>(payload[i] ^ m[i % 4]));
  return out;
}

}  // namespace qnu::ws
