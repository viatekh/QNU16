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
  CHECK(l.get("ch/1") == "Kick I");  // Qu names: up to 6 characters
  CHECK(l.set("ch/2", "Vox"));
  CHECK(l.get("ch/2") == "Vox");
  CHECK(!l.set("ch/99", "x"));
  CHECK(l.set("ch/3", "Gt \xc3\xa4\xc3\xa4"));  // UTF-8 kept intact
  CHECK(l.get("ch/3") == "Gt \xc3\xa4\xc3\xa4");
}

TEST(scene_roundtrip_and_categories) {
  ParamRegistry reg;
  Mixer m(reg);
  Labels labels(Mixer::labelKeys());
  reg.find("ch/5/eq/3/gain")->set(-4.5f);
  reg.find("ch/16/mix/2/level")->set(-12.25f);
  reg.find("lr/fader")->set(-3);
  reg.find("pafl/trim")->set(-6);  // PAFL setup is not stored in scenes
  labels.set("ch/5", "Vox");
  SceneInfo info;
  info.name = "Gig";
  const std::string text = serializeScene(reg, labels, info);
  CHECK(text.find("p pafl/trim") == std::string::npos);
  CHECK(text.find("p gen/") == std::string::npos);
  CHECK(parseSceneInfo(text).name == "Gig");

  ParamRegistry reg2;
  Mixer m2(reg2);
  Labels labels2(Mixer::labelKeys());
  reg2.find("ch/1/mute")->set(1);  // reset by the recall
  reg2.find("custom/1/strip")->set(5);  // custom layer: blocked by the default global filter
  recallScene(text, reg2, labels2);
  CHECK(reg2.find("ch/1/mute")->get() == 0.0f);
  CHECK(reg2.find("ch/5/eq/3/gain")->get() == -4.5f);
  CHECK(reg2.find("ch/16/mix/2/level")->get() == -12.25f);
  CHECK(reg2.find("lr/fader")->get() == -3.0f);
  CHECK(reg2.find("custom/1/strip")->get() == 5.0f);
  CHECK(labels2.get("ch/5") == "Vox");

  CHECK(paramCategory("ch/1/gain") == sceneCategoryIndex("preamp"));
  CHECK(paramCategory("ch/1/eq/1/freq") == sceneCategoryIndex("chproc"));
  CHECK(paramCategory("ch/1/mix/3/level") == sceneCategoryIndex("mixsends"));
  CHECK(paramCategory("fx/1/fader") == sceneCategoryIndex("mixfaders"));  // FX send master
  CHECK(paramCategory("fx/1/rev/decay") == sceneCategoryIndex("fxparam"));
  CHECK(paramCategory("fx/1/patch") == sceneCategoryIndex("fxpatch"));
  CHECK(paramCategory("lr/comp/thresh") == sceneCategoryIndex("lrproc"));
  CHECK(paramCategory("mix/5/geq/3") == sceneCategoryIndex("mixproc"));
  CHECK(paramCategory("ch/1/pafl") == -1);
  CHECK(paramCategory("gen/level") == -1);
}

TEST(scene_filters_and_safes) {
  ParamRegistry reg;
  Mixer m(reg);
  Labels labels(Mixer::labelKeys());
  reg.find("ch/1/fader")->set(-5);
  reg.find("ch/2/fader")->set(-5);
  reg.find("ch/1/gain")->set(6);
  SceneInfo info;
  info.name = "A";
  info.filter = {"preamp"};  // this scene never recalls preamps
  const std::string text = serializeScene(reg, labels, info);

  reg.find("ch/1/fader")->set(-30);
  reg.find("ch/2/fader")->set(-30);
  reg.find("ch/1/gain")->set(12);
  reg.find("safe/ch/2")->set(1);
  reg.find("filter/infaders")->set(0);
  recallScene(text, reg, labels);
  CHECK(reg.find("ch/1/fader")->get() == -5.0f);
  CHECK(reg.find("ch/2/fader")->get() == -30.0f);  // safe
  CHECK(reg.find("ch/1/gain")->get() == 12.0f);    // blocked by the scene filter

  reg.find("filter/infaders")->set(1);  // global filter
  reg.find("ch/1/fader")->set(-40);
  recallScene(text, reg, labels);
  CHECK(reg.find("ch/1/fader")->get() == -40.0f);
}

TEST(reset_mix_settings_defaults) {
  ParamRegistry reg;
  Mixer m(reg);
  Labels labels(Mixer::labelKeys());
  reg.find("ch/3/fader")->set(0);
  reg.find("pafl/trim")->set(-6);
  labels.set("ch/3", "Gtr");
  resetMixSettings(reg, labels);
  // The Qu's Reset Mix Settings values.
  CHECK(reg.find("ch/3/fader")->get() == kMinusInfDb);
  CHECK(reg.find("fxret/1/fader")->get() == 0.0f);
  CHECK(reg.find("mix/1/fader")->get() == -4.0f);
  CHECK(reg.find("fx/1/fader")->get() == -4.0f);
  CHECK(reg.find("lr/fader")->get() == kMinusInfDb);
  CHECK(reg.find("ch/3/hpf/freq")->get() == 100.0f && !reg.find("ch/3/hpf/on")->on());
  CHECK(reg.find("ch/3/gate/thresh")->get() == -36.0f && reg.find("ch/3/gate/depth")->get() == 20.0f);
  CHECK(reg.find("ch/3/comp/thresh")->get() == 0.0f);
  CHECK(std::fabs(reg.find("ch/3/comp/ratio")->get() - 3.2f) < 1e-6f);
  CHECK(reg.find("ch/3/mix/1/pre")->on() && !reg.find("ch/3/fx/1/pre")->on());
  CHECK(reg.find("mix/1/presrc")->get() == kSrcPostEq);
  CHECK(reg.find("fx/1/type")->get() == FxEngine::kReverb && reg.find("fx/2/type")->get() == FxEngine::kDelay);
  CHECK(reg.find("fx/1/patch")->get() == kFxMixReturn);
  CHECK(reg.find("soft/1/func")->get() == 2 && reg.find("soft/4/arg")->get() == 3);
  CHECK(reg.find("custom/1/strip")->get() == 0);
  CHECK(reg.find("pafl/trim")->get() == -6.0f);  // not a mix setting
  CHECK(labels.get("ch/3").empty());
}

TEST(controller_set_label_and_errors) {
  Desk d(tempDir("ctl"));
  auto r = d.ctl.handle("set ch/3/fader -10\nset ch/3/pan 9");
  CHECK(r.broadcast.size() == 2);
  CHECK(r.broadcast[0] == "p ch/3/fader -10");
  CHECK(r.broadcast[1] == "p ch/3/pan 1");  // clamped value is echoed
  CHECK(d.reg.find("ch/3/fader")->get() == -10.0f);

  r = d.ctl.handle("label ch/3 Snare");
  CHECK(r.broadcast.size() == 1 && r.broadcast[0] == "l ch/3 Snare");

  r = d.ctl.handle("set nope 1");
  CHECK(r.toSender.size() == 1 && r.toSender[0].rfind("e ", 0) == 0);
  r = d.ctl.handle("set ch/1/fader abc");
  CHECK(r.toSender.size() == 1 && r.toSender[0].rfind("e ", 0) == 0);
  r = d.ctl.handle("bogus");
  CHECK(r.toSender.size() == 1 && r.broadcast.empty());
}

TEST(controller_scenes) {
  Desk d(tempDir("scenes"));
  d.ctl.handle("set ch/1/fader -20\nlabel ch/1 Kick");
  auto r = d.ctl.handle("scene store 15 Polly");
  CHECK(r.toSender.empty());
  CHECK(r.broadcast.size() == 1 && r.broadcast[0] == "s 15\n15 - Polly");

  d.ctl.handle("set ch/1/fader 0\nlabel ch/1 Other");
  r = d.ctl.handle("scene recall 15");
  CHECK(d.reg.find("ch/1/fader")->get() == -20.0f);
  CHECK(d.labels.get("ch/1") == "Kick");
  CHECK(r.broadcast.size() == 2);

  // Per-scene filter, rename, copy, clear.
  r = d.ctl.handle("scene filter 15 infaders 1");
  CHECK(r.broadcast.back() == "s 15\n15 infaders Polly");
  d.ctl.handle("set ch/1/fader -3");
  d.ctl.handle("scene recall 15");
  CHECK(d.reg.find("ch/1/fader")->get() == -3.0f);
  r = d.ctl.handle("scene name 15 A very long scene name");
  CHECK(r.broadcast.back() == "s 15\n15 infaders A very long sc");
  d.ctl.handle("scene copy 15 16");
  r = d.ctl.handle("scene filter 16 infaders 0");
  CHECK(r.broadcast.back() == "s 15\n15 infaders A very long sc\n16 - A very long sc");
  d.ctl.handle("scene recall 16");
  CHECK(d.reg.find("ch/1/fader")->get() == -20.0f);
  r = d.ctl.handle("scene clear 15");
  CHECK(r.broadcast.back() == "s 16\n16 - A very long sc");
  // Naming an empty scene stores the current settings in it.
  r = d.ctl.handle("scene name 3 Start");
  CHECK(r.broadcast.back() == "s 16\n3 - Start\n16 - A very long sc");

  r = d.ctl.handle("scene recall 101");
  CHECK(r.toSender.size() == 1 && r.toSender[0] == "e scene number must be 1-100");
  r = d.ctl.handle("scene recall 50");
  CHECK(r.toSender.size() == 1 && r.toSender[0] == "e scene 50 is empty");

  d.ctl.handle("set ch/1/fader 0");
  r = d.ctl.handle("scene reset");
  CHECK(d.reg.find("ch/1/fader")->get() == kMinusInfDb);
}

TEST(controller_stereo_link) {
  Desk d(tempDir("link"));
  d.ctl.handle("set ch/3/eq/2/gain 6\nset ch/3/pan -1\nset ch/3/gain 5\nset ch/3/polarity 1");
  // Linking copies CH3 to CH4; pan becomes width (CH4 mirrors CH3).
  d.ctl.handle("set link/2/on 1");
  CHECK(d.reg.find("ch/4/eq/2/gain")->get() == 6.0f);
  CHECK(d.reg.find("ch/4/pan")->get() == 1.0f);
  CHECK(d.reg.find("ch/4/gain")->get() == 5.0f);
  CHECK(d.reg.find("ch/4/polarity")->get() == 0.0f);  // polarity is never linked
  auto r = d.ctl.handle("set ch/4/comp/thresh -10");
  CHECK(d.reg.find("ch/3/comp/thresh")->get() == -10.0f);
  CHECK(r.broadcast.size() == 2);
  d.ctl.handle("set link/2/preamp 0\nset ch/3/gain 0");
  CHECK(d.reg.find("ch/4/gain")->get() == 5.0f);
  d.ctl.handle("set link/2/on 0\nset ch/3/eq/2/gain 0");
  CHECK(d.reg.find("ch/4/eq/2/gain")->get() == 6.0f);
}

TEST(controller_pafl_autocancel_and_amm) {
  Desk d(tempDir("pafl"));
  d.ctl.handle("set ch/1/pafl 1\nset mix/2/pafl 1");
  CHECK(!d.reg.find("ch/1/pafl")->on() && d.reg.find("mix/2/pafl")->on());
  d.ctl.handle("set pafl/additive 1\nset ch/5/pafl 1");
  CHECK(d.reg.find("ch/5/pafl")->on() && d.reg.find("mix/2/pafl")->on());
  // Switching a channel into the AMM opens its fader.
  d.ctl.handle("set amm/7/on 1");
  CHECK(d.reg.find("ch/7/fader")->get() == 0.0f);
}

TEST(controller_info_state_meters) {
  Desk d(tempDir("info"));
  const std::string info = d.ctl.infoMessage();
  CHECK(info.find("{\"id\":\"st/1\",\"name\":\"ST 1\",\"kind\":\"stereo\",\"stereo\":true}") != std::string::npos);
  CHECK(info.find("\"name\":\"Mix 9-10\"") != std::string::npos);
  CHECK(info.find("\"fxTypes\":[\"Reverb\",\"Delay\",\"ADT\",\"Chorus\",\"Symphonic\",\"Flanger\",\"Phaser\",\"Gated Verb\"]") != std::string::npos);
  CHECK(info.find("\"reverbModels\":[\"Classic\",\"Hall\",\"Room\",\"Plate\"]") != std::string::npos);
  CHECK(info.find("\"fxTargets\":[\"None\",\"Ch 1\"") != std::string::npos);
  const std::string meta = d.ctl.metaMessage();
  CHECK(meta.find("\nch/1/fader -90 10 -90 c dB\n") != std::string::npos);
  CHECK(meta.find("\nch/1/mute 0 1 0 t\n") != std::string::npos);
  CHECK(meta.find("\nfx/1/type 0 7 0 h\n") != std::string::npos);
  CHECK(meta.find("\nch/1/gate/thresh -72 18 -36 c dB\n") != std::string::npos);
  CHECK(meta.find("\nch/1/comp/ratio 1 50 3.2 c :1\n") != std::string::npos);
  CHECK(meta.find("\nch/1/eq/1/width 0.111111 2 0.75 c oct\n") != std::string::npos);
  const std::string state = d.ctl.stateMessage();
  CHECK(state.find("p ch/16/fader -90") != std::string::npos);
  const std::string meters = d.ctl.meterMessage();
  CHECK(meters.rfind("m ", 0) == 0);
  const size_t commas = std::count(meters.begin(), meters.end(), ',');
  CHECK(commas == static_cast<size_t>(kInputStrips * 4 + kBuses * 3 + 1));
  const std::string amm = d.ctl.ammMessage();
  CHECK(amm.rfind("a ", 0) == 0 && std::count(amm.begin(), amm.end(), ',') == kAmmChannels - 1);
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
  CHECK(resp.find("\nch/1/fader -90 10 -90 c dB") != std::string::npos);

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
