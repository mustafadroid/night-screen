#pragma once
// Tiny loopback-only HTTP server (POSIX + Winsock) for the control panel.
// Serves GET / (embedded UI), GET /api/state, POST /api/state.

#include <cstdint>
#include <functional>
#include <string>

#include "core/Config.hpp"

namespace nightscreen {

class HttpServer {
public:
  using StateFn = std::function<Settings()>;
  using ApplyFn = std::function<std::string(const std::string& jsonBody)>;

  HttpServer() = default;
  ~HttpServer() { stop(); }
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  bool start(int port, StateFn get, ApplyFn post, std::string& error);
  void stop();
  [[nodiscard]] bool running() const { return running_; }
  [[nodiscard]] int port() const { return port_; }

private:
  void loop(StateFn get, ApplyFn post);
  // Native socket handle (SOCKET on Windows is 64-bit; int would truncate it).
  std::intptr_t listenFd_ = -1;
  bool running_ = false;
  int port_ = 0;
  struct Worker;
  Worker* worker_ = nullptr;
};

[[nodiscard]] std::string embeddedUiHtml();

} // namespace nightscreen
