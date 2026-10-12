#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "controller.h"
#include "scene.h"
#include "server/server.h"
#include "server/websocket.h"
#include "test.h"

using namespace qnu;

namespace {

std::string hex(const std::string& s) {
  static const char* d = "0123456789abcdef";
  std::string out;
  for (unsigned char c : s) {
    out += d[c >> 4];
    out += d[c & 15];
  }
  return out;
}

std::string tempDir(const char* tag) {
  auto p = std::filesystem::temp_directory_path() / ("qnu16-test-" + std::string(tag) + "-" +
                                                    std::to_string(::getpid()));
  std::filesystem::remove_all(p);
  std::filesystem::create_directories(p);
  return p.string();
}

struct Desk {
  ParamRegistry reg;
  Mixer mixer{reg};
  Labels labels{Mixer::labelKeys()};
  SceneStore scenes;
  Controller ctl;
  explicit Desk(const std::string& dir) : scenes(dir), ctl(mixer, reg, labels, scenes, "test") {}
};

}  // namespace

TEST(ws_sha1_and_base64_vectors) {
  CHECK(hex(ws::sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(hex(ws::sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  CHECK(hex(ws::sha1(std::string(1000, 'a'))) == "291e9a6c66994949b57ba5e650361e98fc36b1ba");
  CHECK(ws::base64("") == "");
  CHECK(ws::base64("f") == "Zg==");
  CHECK(ws::base64("fo") == "Zm8=");
  CHECK(ws::base64("foobar") == "Zm9vYmFy");
}

TEST(ws_accept_key_rfc6455_example) {
  CHECK(ws::acceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(ws_frame_roundtrip) {
  for (size_t len : {size_t(0), size_t(5), size_t(125), size_t(126), size_t(70000 / 2)}) {
    const std::string payload(len, 'x');
    const std::string wire = ws::encodeMaskedFrame(ws::kText, payload, 0xA1B2C3D4);
    ws::Frame f;
    size_t used = 0;
    CHECK(ws::parseFrame(wire.substr(0, wire.size() - 1), &f, &used) == ws::ParseResult::kNeedMore ||
          len == 0);
    CHECK(ws::parseFrame(wire, &f, &used) == ws::ParseResult::kFrame);
    CHECK(used == wire.size());
    CHECK(f.payload == payload && f.opcode == ws::kText && f.fin);
  }
  // Unmasked client frames and oversized payloads are rejected.
  ws::Frame f;
  size_t used;
  CHECK(ws::parseFrame(ws::encodeFrame(ws::kText, "hi"), &f, &used) == ws::ParseResult::kError);
  CHECK(ws::parseFrame(ws::encodeMaskedFrame(ws::kText, std::string(ws::kMaxPayload + 1, 'a'), 1),
                       &f, &used) == ws::ParseResult::kError);
}

TEST(param_clamping_and_toggles) {
  ParamRegistry reg;
  Mixer m(reg);
  CHECK(reg.find("ch/1/fader")->set(50) == 10.0f);
  CHECK(reg.find("ch/1/fader")->set(-500) == kMinusInfDb);
  CHECK(reg.find("ch/1/mute")->set(0.7f) == 1.0f);
  CHECK(reg.find("ch/1/mute")->set(0.2f) == 0.0f);
  CHECK(reg.find("ch/1/pan")->set(NAN) == 0.0f);  // non-finite -> default
}

TEST(labels_sanitize) {
  Labels l(Mixer::labelKeys());
  CHECK(l.set("ch/1", "  Kick\n In  "));
  CHECK(l.get("ch/1") == "Kick In");
  CHECK(!l.set("ch/99", "x"));
  CHECK(l.set("ch/2", std::string(40, 'a')));
  CHECK(l.get("ch/2").size() == 24);
  CHECK(l.set("ch/3", "Gitarre \xc3\xa4\xc3\xa4"));  // UTF-8 kept intact
  CHECK(l.get("ch/3") == "Gitarre \xc3\xa4\xc3\xa4");
}

TEST(scene_roundtrip) {
  ParamRegistry reg;
  Mixer m(reg);
  Labels labels(Mixer::labelKeys());
  reg.find("ch/5/eq/3/gain")->set(-4.5f);
  reg.find("ch/16/mix/2/level")->set(-12.25f);
  reg.find("lr/fader")->set(-3);
  labels.set("ch/5", "Vox");
  const std::string text = serializeScene(reg, labels);

  ParamRegistry reg2;
  Mixer m2(reg2);
  Labels labels2(Mixer::labelKeys());
  reg2.find("ch/1/mute")->set(1);  // must be reset by loading
  CHECK(applyScene(text, reg2, labels2) == static_cast<int>(reg.all().size()) + 1);
  for (const auto& p : reg.all()) CHECK(reg2.find(p->info().path)->get() == p->get());
  CHECK(labels2.get("ch/5") == "Vox");
}

TEST(scene_names_are_validated) {
  CHECK(isValidSceneName("Sunday Service"));
  CHECK(isValidSceneName("band-2_final"));
  CHECK(!isValidSceneName(""));
  CHECK(!isValidSceneName("../etc/passwd"));
  CHECK(!isValidSceneName("a/b"));
  CHECK(!isValidSceneName(" padded"));
}

TEST(controller_set_label_and_errors) {
  Desk d(tempDir("ctl"));
  auto r = d.ctl.handle("set ch/3/fader -10\nset ch/3/pan 9");
  CHECK(r.broadcast.size() == 2);
  CHECK(r.broadcast[0] == "p ch/3/fader -10");
  CHECK(r.broadcast[1] == "p ch/3/pan 1");  // clamped value is echoed
  CHECK(d.reg.find("ch/3/fader")->get() == -10.0f);

  r = d.ctl.handle("label ch/3 Snare Top");
  CHECK(r.broadcast.size() == 1 && r.broadcast[0] == "l ch/3 Snare Top");

  r = d.ctl.handle("set nope 1");
  CHECK(r.toSender.size() == 1 && r.toSender[0].rfind("e ", 0) == 0);
  r = d.ctl.handle("set ch/1/fader abc");
  CHECK(r.toSender.size() == 1 && r.toSender[0].rfind("e ", 0) == 0);
  r = d.ctl.handle("bogus");
  CHECK(r.toSender.size() == 1 && r.broadcast.empty());
}

TEST(controller_scene_save_load) {
  Desk d(tempDir("scenes"));
  d.ctl.handle("set ch/1/fader -20\nlabel ch/1 Kick");
  auto r = d.ctl.handle("scene save Gig 1");
  CHECK(r.toSender.empty());
  CHECK(r.broadcast.size() == 1 && r.broadcast[0] == "s Gig 1\nGig 1");

  d.ctl.handle("set ch/1/fader 0\nlabel ch/1 Other");
  r = d.ctl.handle("scene load Gig 1");
  CHECK(d.reg.find("ch/1/fader")->get() == -20.0f);
  CHECK(d.labels.get("ch/1") == "Kick");
  CHECK(r.broadcast.size() == 2);

  r = d.ctl.handle("scene load ../../etc/passwd");
  CHECK(r.toSender.size() == 1 && r.toSender[0] == "e invalid scene name");
  r = d.ctl.handle("scene load Missing");
  CHECK(r.toSender.size() == 1 && r.toSender[0] == "e no such scene");
}

TEST(controller_info_state_meters) {
  Desk d(tempDir("info"));
  const std::string info = d.ctl.infoMessage();
  CHECK(info.find("{\"id\":\"st/1\",\"name\":\"ST 1\",\"kind\":\"stereo\",\"stereo\":true}") != std::string::npos);
  CHECK(info.find("\"name\":\"Mix 9-10\"") != std::string::npos);
  CHECK(info.find("\"fxTypes\":[\"Reverb\",\"Delay\",\"Chorus\",\"Flanger\"]") != std::string::npos);
  const std::string meta = d.ctl.metaMessage();
  CHECK(meta.find("\nch/1/fader -90 10 0 c dB\n") != std::string::npos);
  CHECK(meta.find("\nch/1/mute 0 1 0 t\n") != std::string::npos);
  CHECK(meta.find("\nfx/1/type 0 3 0 h\n") != std::string::npos);
  const std::string state = d.ctl.stateMessage();
  CHECK(state.find("p ch/16/fader 0") != std::string::npos);
  const std::string meters = d.ctl.meterMessage();
  CHECK(meters.rfind("m ", 0) == 0);
  const size_t commas = std::count(meters.begin(), meters.end(), ',');
  CHECK(commas == static_cast<size_t>(kInputStrips * 4 + kBuses * 3 + 1));
}

namespace {

int connectTo(int port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    close(fd);
    return -1;
  }
  timeval tv{2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

std::string readUntil(int fd, const std::string& marker) {
  std::string data;
  char buf[4096];
  while (data.find(marker) == std::string::npos) {
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    data.append(buf, static_cast<size_t>(n));
  }
  return data;
}

}  // namespace

TEST(server_http_and_websocket_end_to_end) {
  const std::string ui = tempDir("ui");
  {
    std::FILE* f = std::fopen((ui + "/index.html").c_str(), "w");
    std::fputs("<h1>QNU16</h1>", f);
    std::fclose(f);
  }
  Desk d(tempDir("srv"));
  ControlServer::Options opt;
  opt.bindAddress = "127.0.0.1";
  opt.port = 0;
  opt.uiDir = ui;
  ControlServer server(d.ctl, opt);
  std::string err;
  CHECK(server.start(&err));
  std::atomic<bool> stop{false};
  std::thread t([&] { server.run(stop); });

  // Static file.
  int fd = connectTo(server.port());
  CHECK(fd >= 0);
  std::string req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
  send(fd, req.data(), req.size(), 0);
  std::string resp = readUntil(fd, "</h1>");
  CHECK(resp.rfind("HTTP/1.1 200", 0) == 0);
  CHECK(resp.find("<h1>QNU16</h1>") != std::string::npos);
  close(fd);

  // Path traversal is refused.
  fd = connectTo(server.port());
  req = "GET /../../etc/passwd HTTP/1.1\r\n\r\n";
  send(fd, req.data(), req.size(), 0);
  resp = readUntil(fd, "\r\n\r\n");
  CHECK(resp.rfind("HTTP/1.1 400", 0) == 0);
  close(fd);

  // WebSocket: handshake, initial state, then a parameter change is applied and echoed.
  fd = connectTo(server.port());
  req = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
  send(fd, req.data(), req.size(), 0);
  resp = readUntil(fd, "p out/18/src");
  CHECK(resp.rfind("HTTP/1.1 101", 0) == 0);
  CHECK(resp.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
  CHECK(resp.find("\"inputs\":[") != std::string::npos);
  CHECK(resp.find("\nch/1/fader -90 10 0 c dB") != std::string::npos);

  const std::string frame = ws::encodeMaskedFrame(ws::kText, "set ch/7/fader -12.5", 0x12345678);
  send(fd, frame.data(), frame.size(), 0);
  resp = readUntil(fd, "p ch/7/fader -12.5");
  CHECK(resp.find("p ch/7/fader -12.5") != std::string::npos);
  CHECK(d.reg.find("ch/7/fader")->get() == -12.5f);

  // Meter frames are pushed periodically.
  resp = readUntil(fd, "m ");
  CHECK(resp.find("m ") != std::string::npos);
  close(fd);

  stop = true;
  t.join();
}
