#include "server/server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

#include "server/websocket.h"

namespace qnu {

namespace {

constexpr size_t kMaxRequest = 16 * 1024;
constexpr size_t kMaxOutBuf = 4 * 1024 * 1024;  // slow client: drop it rather than grow forever

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string trim(const std::string& s) {
  const auto a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return {};
  const auto b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

std::string httpResponse(int code, const std::string& status, const std::string& type,
                         const std::string& body, bool headOnly = false) {
  std::ostringstream o;
  o << "HTTP/1.1 " << code << " " << status << "\r\n"
    << "Content-Type: " << type << "\r\n"
    << "Content-Length: " << body.size() << "\r\n"
    << "Cache-Control: no-cache\r\n"
    << "X-Content-Type-Options: nosniff\r\n"
    << "Connection: close\r\n\r\n";
  if (!headOnly) o << body;
  return o.str();
}

std::string mimeType(const std::string& path) {
  auto ends = [&](const char* ext) {
    const size_t n = std::strlen(ext);
    return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
  };
  if (ends(".html")) return "text/html; charset=utf-8";
  if (ends(".js")) return "text/javascript; charset=utf-8";
  if (ends(".css")) return "text/css; charset=utf-8";
  if (ends(".svg")) return "image/svg+xml";
  if (ends(".png")) return "image/png";
  if (ends(".json") || ends(".webmanifest")) return "application/json";
  if (ends(".ico")) return "image/x-icon";
  return "application/octet-stream";
}

void setNonBlocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

}  // namespace

ControlServer::ControlServer(Controller& controller, Options options)
    : controller_(controller), opt_(std::move(options)) {}

ControlServer::~ControlServer() {
  for (auto& c : clients_) close(c->fd);
  if (listenFd_ >= 0) close(listenFd_);
}

bool ControlServer::start(std::string* err) {
  listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd_ < 0) {
    *err = std::strerror(errno);
    return false;
  }
  int one = 1;
  setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(opt_.port));
  if (inet_pton(AF_INET, opt_.bindAddress.c_str(), &addr.sin_addr) != 1) {
    *err = "bad bind address " + opt_.bindAddress;
    return false;
  }
  if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      listen(listenFd_, 16) < 0) {
    *err = std::string("cannot listen: ") + std::strerror(errno);
    return false;
  }
  socklen_t len = sizeof(addr);
  getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);
  setNonBlocking(listenFd_);
  return true;
}

void ControlServer::run(const std::atomic<bool>& stop) {
  using Clock = std::chrono::steady_clock;
  auto nextMeter = Clock::now();
  std::vector<pollfd> fds;

  while (!stop.load()) {
    fds.clear();
    fds.push_back({listenFd_, POLLIN, 0});
    for (auto& c : clients_) {
      short ev = POLLIN;
      if (!c->outBuf.empty()) ev |= POLLOUT;
      fds.push_back({c->fd, ev, 0});
    }
    const auto now = Clock::now();
    const int waitMs = static_cast<int>(std::clamp<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(nextMeter - now).count(), 0, 100));
    poll(fds.data(), fds.size(), waitMs);

    if (fds[0].revents & POLLIN) acceptClients();
    // Clients accepted just now are not in fds; they are polled next round.
    const size_t polled = fds.size() - 1;
    for (size_t i = 0; i < polled && i < clients_.size(); ++i) {
      Client& c = *clients_[i];
      const short re = fds[i + 1].revents;
      if (re & (POLLERR | POLLHUP | POLLNVAL)) c.closing = true, c.outBuf.clear();
      if (!c.closing && (re & POLLIN)) readClient(c);
      if (re & POLLOUT) writeClient(c);
    }

    if (Clock::now() >= nextMeter) {
      nextMeter = Clock::now() + std::chrono::milliseconds(opt_.meterIntervalMs);
      bool anyWs = std::any_of(clients_.begin(), clients_.end(),
                               [](const auto& c) { return c->websocket && !c->closing; });
      // Always drain meters so peaks don't accumulate while nobody is watching.
      const std::string meters = controller_.meterMessage();
      const std::string rta = controller_.rtaMessage();
      if (anyWs) {
        broadcast(meters);
        broadcast(rta);
        broadcast(controller_.ammMessage());
        // Qu-Drive status: every tick while recording or playing, and once more when it stops.
        const std::string drive = controller_.driveMessage();
        if (drive != lastRecorder_) broadcast(drive);
        const bool running = drive.rfind("d rec", 0) == 0 || drive.rfind("d play", 0) == 0;
        lastRecorder_ = running ? std::string() : drive;
      }
    }

    // Try to flush immediately, then drop finished/broken clients.
    for (auto& c : clients_)
      if (!c->outBuf.empty()) writeClient(*c);
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                  [](const auto& c) {
                                    const bool done = c->closing && c->outBuf.empty();
                                    if (done) close(c->fd);
                                    return done;
                                  }),
                   clients_.end());
  }
}

void ControlServer::acceptClients() {
  for (;;) {
    const int fd = accept(listenFd_, nullptr, nullptr);
    if (fd < 0) return;
    if (clients_.size() >= 32) {
      close(fd);
      continue;
    }
    setNonBlocking(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    auto c = std::make_unique<Client>();
    c->fd = fd;
    clients_.push_back(std::move(c));
  }
}

void ControlServer::readClient(Client& c) {
  char buf[8192];
  for (;;) {
    const ssize_t n = recv(c.fd, buf, sizeof(buf), 0);
    if (n > 0) {
      c.inBuf.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      c.closing = true;
      c.outBuf.clear();
      return;
    }
    break;
  }
  if (c.websocket) handleWsData(c);
  else handleHttp(c);
}

void ControlServer::writeClient(Client& c) {
  while (!c.outBuf.empty()) {
    const ssize_t n = send(c.fd, c.outBuf.data(), c.outBuf.size(), MSG_NOSIGNAL);
    if (n > 0) {
      c.outBuf.erase(0, static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
    c.closing = true;
    c.outBuf.clear();
    return;
  }
}

void ControlServer::handleHttp(Client& c) {
  const auto end = c.inBuf.find("\r\n\r\n");
  if (end == std::string::npos) {
    if (c.inBuf.size() > kMaxRequest) {
      c.outBuf = httpResponse(431, "Request Header Fields Too Large", "text/plain", "too large\n");
      c.closing = true;
    }
    return;
  }
  std::istringstream in(c.inBuf.substr(0, end));
  c.inBuf.erase(0, end + 4);
  std::string requestLine;
  std::getline(in, requestLine);
  std::istringstream rl(requestLine);
  std::string method, target;
  rl >> method >> target;

  std::string line, upgrade, wsKey;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string name = lower(trim(line.substr(0, colon)));
    const std::string value = trim(line.substr(colon + 1));
    if (name == "upgrade") upgrade = lower(value);
    else if (name == "sec-websocket-key") wsKey = value;
  }

  const std::string path = target.substr(0, target.find('?'));
  if (path == "/ws" && method == "GET" && upgrade == "websocket" && !wsKey.empty()) {
    c.outBuf =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + ws::acceptKey(wsKey) + "\r\n\r\n";
    c.websocket = true;
    sendText(c, controller_.infoMessage());
    sendText(c, controller_.metaMessage());
    sendText(c, controller_.stateMessage());
    sendText(c, controller_.sceneListMessage());
    sendText(c, controller_.driveMessage());
    sendText(c, controller_.driveListMessage());
    sendText(c, controller_.libraryListMessage());
    if (!c.inBuf.empty()) handleWsData(c);
    return;
  }
  c.outBuf = serveFile(path, method);
  c.closing = true;
}

std::string ControlServer::serveFile(const std::string& urlPath, const std::string& method) {
  if (method != "GET" && method != "HEAD")
    return httpResponse(405, "Method Not Allowed", "text/plain", "method not allowed\n");
  std::string rel = urlPath == "/" ? "/index.html" : urlPath;
  // Only plain relative paths inside uiDir: no traversal, no hidden files, no encoded bytes.
  const bool safe = !rel.empty() && rel[0] == '/' && rel.find("..") == std::string::npos &&
                    rel.find("/.") == std::string::npos && rel.find('%') == std::string::npos &&
                    rel.find('\\') == std::string::npos;
  if (!safe) return httpResponse(400, "Bad Request", "text/plain", "bad path\n");
  std::ifstream f(opt_.uiDir + rel, std::ios::binary);
  if (!f) return httpResponse(404, "Not Found", "text/plain", "not found\n");
  std::ostringstream body;
  body << f.rdbuf();
  return httpResponse(200, "OK", mimeType(rel), body.str(), method == "HEAD");
}

void ControlServer::handleWsData(Client& c) {
  for (;;) {
    ws::Frame frame;
    size_t consumed = 0;
    const auto r = ws::parseFrame(c.inBuf, &frame, &consumed);
    if (r == ws::ParseResult::kNeedMore) return;
    if (r == ws::ParseResult::kError || !frame.fin) {  // fragmented messages aren't needed
      c.outBuf += ws::encodeFrame(ws::kClose, std::string("\x03\xea", 2));  // 1002
      c.closing = true;
      return;
    }
    c.inBuf.erase(0, consumed);
    switch (frame.opcode) {
      case ws::kText: {
        const Controller::Reply reply = controller_.handle(frame.payload);
        for (const auto& m : reply.toSender) sendText(c, m);
        for (const auto& m : reply.broadcast) broadcast(m);
        break;
      }
      case ws::kPing:
        c.outBuf += ws::encodeFrame(ws::kPong, frame.payload);
        break;
      case ws::kClose:
        c.outBuf += ws::encodeFrame(ws::kClose, "");
        c.closing = true;
        return;
      default:
        break;
    }
  }
}

void ControlServer::sendText(Client& c, const std::string& text) {
  if (c.closing) return;
  if (c.outBuf.size() > kMaxOutBuf) {
    c.closing = true;
    c.outBuf.clear();
    return;
  }
  c.outBuf += ws::encodeFrame(ws::kText, text);
}

void ControlServer::broadcast(const std::string& text) {
  for (auto& c : clients_)
    if (c->websocket) sendText(*c, text);
}

}  // namespace qnu
