#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "controller.h"

namespace qnu {

// Single-threaded HTTP + WebSocket server (poll-based). Serves the web UI from uiDir and the
// control protocol on /ws. Runs entirely on the control thread, never on the audio thread.
class ControlServer {
 public:
  struct Options {
    std::string bindAddress = "0.0.0.0";
    int port = 8016;  // 0 = pick a free port
    std::string uiDir = "ui";
    int meterIntervalMs = 50;
  };

  ControlServer(Controller& controller, Options options);
  ~ControlServer();

  bool start(std::string* err);
  int port() const { return port_; }
  // Serves until stop becomes true.
  void run(const std::atomic<bool>& stop);

 private:
  struct Client {
    int fd = -1;
    bool websocket = false;
    bool closing = false;  // flush outBuf then close
    std::string inBuf, outBuf;
  };

  void acceptClients();
  void readClient(Client& c);
  void writeClient(Client& c);
  void handleHttp(Client& c);
  void handleWsData(Client& c);
  void sendText(Client& c, const std::string& text);
  void broadcast(const std::string& text);
  std::string serveFile(const std::string& urlPath, const std::string& method);

  Controller& controller_;
  Options opt_;
  int listenFd_ = -1;
  int port_ = 0;
  std::vector<std::unique_ptr<Client>> clients_;
};

}  // namespace qnu
