// qnu16d: the mixer daemon. Runs the audio engine on JACK (or the dummy backend) and serves
// the web control surface.

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "backend/backend.h"
#include "controller.h"
#include "mixer.h"
#include "scene.h"
#include "server/server.h"

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop = true; }

void usage() {
  std::printf(
      "usage: qnu16d [options]\n"
      "  --backend jack|dummy   audio backend (default: jack)\n"
      "  --autoconnect          connect physical JACK ports (capture 1-22, playback 1-18)\n"
      "  --client-name NAME     JACK client name (default: qnu16)\n"
      "  --port N               HTTP/WebSocket port (default: 8016)\n"
      "  --bind ADDR            bind address (default: 0.0.0.0)\n"
      "  --ui DIR               web UI directory (default: ui)\n"
      "  --scenes DIR           scene directory (default: scenes)\n"
      "  --load N               recall scene N (1-100) at startup\n"
      "  --recordings DIR       the Qu-Drive: where recordings are written and played from\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string backendName = "jack";
  std::string scenesDir = "scenes";
  std::string loadScene;
  std::string recordingsDir = "recordings";
  qnu::JackOptions jackOpt;
  qnu::ControlServer::Options srvOpt;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--backend") backendName = next();
    else if (a == "--autoconnect") jackOpt.autoconnect = true;
    else if (a == "--client-name") jackOpt.clientName = next();
    else if (a == "--port") srvOpt.port = std::atoi(next().c_str());
    else if (a == "--bind") srvOpt.bindAddress = next();
    else if (a == "--ui") srvOpt.uiDir = next();
    else if (a == "--scenes") scenesDir = next();
    else if (a == "--load") loadScene = next();
    else if (a == "--recordings") recordingsDir = next();
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
  }

  qnu::ParamRegistry registry;
  qnu::Mixer mixer(registry);
  qnu::Labels labels(qnu::Mixer::labelKeys());
  qnu::SceneStore scenes(scenesDir);
  qnu::Recorder recorder(recordingsDir);
  qnu::Player player(recordingsDir);
  mixer.setRecorder(&recorder);
  mixer.setPlayer(&player);

  if (!loadScene.empty()) {
    std::string text, err;
    if (!scenes.load(std::atoi(loadScene.c_str()), &text, &err)) {
      std::fprintf(stderr, "cannot recall scene %s: %s\n", loadScene.c_str(), err.c_str());
      return 1;
    }
    qnu::recallScene(text, registry, labels);
  }

  std::unique_ptr<qnu::AudioBackend> backend;
  if (backendName == "jack") {
    backend = qnu::makeJackBackend(jackOpt);
    if (!backend) {
      std::fprintf(stderr, "this build has no JACK support; use --backend dummy\n");
      return 1;
    }
  } else if (backendName == "dummy") {
    backend = qnu::makeDummyBackend();
  } else {
    std::fprintf(stderr, "unknown backend %s\n", backendName.c_str());
    return 2;
  }

  std::string err;
  if (!backend->start(mixer, &err)) {
    std::fprintf(stderr, "audio backend failed: %s\n", err.c_str());
    return 1;
  }

  qnu::Controller controller(mixer, registry, labels, scenes, backend->name());
  controller.setDrive(&recorder, &player);
  qnu::ControlServer server(controller, srvOpt);
  if (!server.start(&err)) {
    std::fprintf(stderr, "control server failed: %s\n", err.c_str());
    backend->stop();
    return 1;
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::printf("QNU16 running: backend=%s, %.0f Hz, control surface at http://%s:%d/\n",
              backend->name().c_str(), mixer.sampleRate(),
              srvOpt.bindAddress == "0.0.0.0" ? "<this-host>" : srvOpt.bindAddress.c_str(),
              server.port());
  std::fflush(stdout);

  server.run(g_stop);
  backend->stop();
  recorder.stop();
  std::printf("QNU16 stopped\n");
  return 0;
}
