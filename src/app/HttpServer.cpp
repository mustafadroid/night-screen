#include "HttpServer.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stop_token>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // keep windows.h min/max macros away from std::min/max
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif
using Sock = SOCKET;
#define CLOSE_SOCK closesocket
#define SOCK_INVALID INVALID_SOCKET
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Sock = int;
#define CLOSE_SOCK ::close
#define SOCK_INVALID -1
#endif

namespace nightscreen {

struct HttpServer::Worker {
  std::jthread thread;
  std::atomic<bool> stop{false};
};

static std::string httpReason(int code) {
  switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    default: return "OK";
  }
}

static void sendAll(Sock fd, const std::string& s) {
#if defined(_WIN32)
  send(fd, s.c_str(), (int)s.size(), 0);
#else
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (n <= 0) break;
    off += (size_t)n;
  }
#endif
}

static std::string recvAll(Sock fd) {
  std::string out;
  char buf[8192];
  // Read headers first, then body per Content-Length.
  std::string head;
  while (head.find("\r\n\r\n") == std::string::npos && head.size() < 65536) {
#if defined(_WIN32)
    int n = recv(fd, buf, sizeof(buf), 0);
#else
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
#endif
    if (n <= 0) break;
    head.append(buf, (size_t)n);
  }
  out = head;
  auto hpos = head.find("\r\n\r\n");
  size_t bodyStart = hpos == std::string::npos ? head.size() : hpos + 4;
  size_t contentLen = 0;
  {
    auto p = head.find("Content-Length:");
    if (p != std::string::npos) {
      contentLen = (size_t)std::stoul(head.substr(p + 15));
    } else {
      p = head.find("content-length:");
      if (p != std::string::npos) contentLen = (size_t)std::stoul(head.substr(p + 15));
    }
  }
  while (out.size() - bodyStart < contentLen && out.size() < 1'000'000) {
#if defined(_WIN32)
    int n = recv(fd, buf, sizeof(buf), 0);
#else
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
#endif
    if (n <= 0) break;
    out.append(buf, (size_t)n);
  }
  return out;
}

bool HttpServer::start(int port, StateFn get, ApplyFn post, std::string& error) {
  if (running_) return true;
#if defined(_WIN32)
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    error = "WSAStartup failed";
    return false;
  }
#endif
  Sock fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == SOCK_INVALID) {
    error = "socket() failed";
    return false;
  }
  int yes = 1;
#if defined(_WIN32)
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
#else
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // loopback only
#if defined(_WIN32)
  if (bind(fd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
    error = "bind 127.0.0.1 failed (port in use?)";
    closesocket(fd);
    return false;
  }
  if (listen(fd, 8) == SOCKET_ERROR) {
    error = "listen() failed";
    closesocket(fd);
    return false;
  }
  // non-blocking accept via timeout
  DWORD tv = 300;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
  if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
    error = "bind 127.0.0.1 failed (port in use?)";
    ::close(fd);
    return false;
  }
  if (listen(fd, 8) != 0) {
    error = "listen() failed";
    ::close(fd);
    return false;
  }
  timeval tv{0, 300000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
  listenFd_ = static_cast<std::intptr_t>(fd);
  port_ = port;
  running_ = true;
  worker_ = new Worker();
  worker_->thread = std::jthread([this, get, post](std::stop_token) { loop(get, post); });
  return true;
}

void HttpServer::loop(StateFn get, ApplyFn post) {
  Sock fd = static_cast<Sock>(listenFd_);
  while (!worker_->stop.load()) {
    sockaddr_in peer{};
#if defined(_WIN32)
    int plen = sizeof(peer);
    Sock c = accept(fd, (sockaddr*)&peer, &plen);
    if (c == INVALID_SOCKET) {
      if (worker_->stop.load()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
#else
    socklen_t plen = sizeof(peer);
    Sock c = accept(fd, (sockaddr*)&peer, &plen);
    if (c < 0) {
      if (worker_->stop.load()) break;
      continue; // timeout -> re-check stop flag
    }
#endif
    std::string req = recvAll(c);
    std::istringstream lines(req);
    std::string requestLine;
    std::getline(lines, requestLine);
    std::string method, path;
    {
      std::istringstream rl(requestLine);
      rl >> method >> path;
    }
    std::string body;
    {
      auto p = req.find("\r\n\r\n");
      if (p != std::string::npos) body = req.substr(p + 4);
    }
    int code = 200;
    std::string ctype = "application/json";
    std::string payload;
    if (method == "GET" && (path == "/" || path == "/index.html")) {
      ctype = "text/html; charset=utf-8";
      payload = embeddedUiHtml();
    } else if (method == "GET" && path == "/api/state") {
      payload = get().toJson();
    } else if ((method == "POST" || method == "PUT") && path == "/api/state") {
      payload = post(body);
      if (payload.rfind("ERROR", 0) == 0) {
        code = 400;
        payload = "{\"error\":\"" + payload + "\"}";
      }
    } else {
      code = 404;
      payload = "{\"error\":\"not found\"}";
    }
    std::ostringstream resp;
    resp << "HTTP/1.1 " << code << " " << httpReason(code) << "\r\n"
         << "Content-Type: " << ctype << "\r\n"
         << "Content-Length: " << payload.size() << "\r\n"
         << "Connection: close\r\n"
         << "Access-Control-Allow-Origin: *\r\n\r\n"
         << payload;
    sendAll(c, resp.str());
    CLOSE_SOCK(c);
  }
}

void HttpServer::stop() {
  if (!running_) return;
  running_ = false;
  if (worker_) {
    worker_->stop = true;
#if defined(_WIN32)
    if (listenFd_ != -1) closesocket(static_cast<Sock>(listenFd_));
    WSACleanup();
#else
    if (listenFd_ != -1) {
      ::shutdown(listenFd_, SHUT_RDWR);
      ::close(listenFd_);
    }
#endif
    listenFd_ = -1;
    if (worker_->thread.joinable()) worker_->thread.join();
    delete worker_;
    worker_ = nullptr;
  }
}

std::string embeddedUiHtml() {
  // Keep in sync with web/ui.html (this copy is served when the file is absent).
  return R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>night-screen</title>
<style>body{font-family:system-ui,sans-serif;background:#111418;color:#e8e8e8;margin:0;padding:24px}main{max-width:640px;margin:auto}.card{background:#1c2127;border-radius:12px;padding:20px;margin-bottom:16px}input[type=range]{width:100%}button{padding:8px 14px;border-radius:8px;border:1px solid #444;background:#2a3138;color:#fff;cursor:pointer}button.primary{background:#4a7db5;border-color:#4a7db5}.row{display:flex;gap:8px;flex-wrap:wrap}.switch{position:relative;display:inline-block;width:46px;height:24px;flex:none}.switch input{display:none}.switch .track{position:absolute;inset:0;background:#444;border-radius:999px;transition:.15s;cursor:pointer}.switch .track:before{content:"";position:absolute;width:18px;height:18px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.15s}.switch input:checked+.track{background:#4a7db5}.switch input:checked+.track:before{transform:translateX(22px)}label{display:block;margin:12px 0 4px;color:#aaa;font-size:13px}output{color:#fff}.presets{display:flex;gap:8px;flex-wrap:wrap}</style>
</head><body><main>
<h1>🌙 night-screen <small style="color:#888">eye-care suite</small></h1>
<div class="card"><div class="row">
<button id="toggle" class="primary">Toggle</button>
<button id="refresh">Refresh</button>
<span id="status" style="align-self:center;color:#9fb"></span></div>
<label>Color temperature <output id="tempOut"></output></label>
<input id="temp" type="range" min="1000" max="6500" step="50">
<label>Brightness <output id="brOut"></output> <small style="color:#666">below 55% adds software dimming</small></label>
<input id="br" type="range" min="5" max="100" step="1">
<label>Preset</label><div class="presets" id="presets"></div>
</div>
<div class="card"><h3>Schedule</h3><div class="row">
<select id="mode"><option value="0">Manual</option><option value="1">Scheduled</option><option value="2">Sunrise/Sunset auto</option></select>
<input id="on" placeholder="21:00" size="6"><input id="off" placeholder="07:00" size="6">
<input id="lat" placeholder="lat" size="7"><input id="lon" placeholder="lon" size="7">
</div></div>
<div class="card"><h3>Eye break (20-20-20)</h3><div class="row">
<label><input id="brk" type="checkbox"> enabled</label>
<input id="work" size="4" title="work minutes"><span>min work /</span>
<input id="rest" size="5" title="rest seconds"><span>sec rest</span>
<label><input id="strict" type="checkbox"> strict lock</label>
<button id="snooze">Snooze 10m</button></div></div>
<div class="card"><h3>OLED burn-in guard</h3><div class="row"><label class="switch" style="margin:0"><input id="oled" type="checkbox"><span class="track"></span></label><span><b id="oledState">off</b> — dark screen + roaming clock</span><input id="oledMove" size="5"><span>sec between moves (10–600)</span></div><p style="color:#777;font-size:12px">The switch applies instantly — no need to press Apply. Any key or mouse activity exits the guard.</p></div>
<div class="card"><button id="save" class="primary">Apply</button> <span style="color:#777;font-size:12px">REST: GET/POST /api/state</span></div>
<script>
const $=id=>document.getElementById(id);
const PRESETS=[["Off",6500,100],["Reading",4500,90],["Night",3400,80],["Dark Room",2700,60],["Movie",5000,85],["Game",5500,95]];
PRESETS.forEach(([n,t,b])=>{const e=document.createElement("button");e.textContent=n;e.onclick=()=>{$("temp").value=t;$("br").value=b;syncOut();};$("presets").appendChild(e);});
function syncOut(){$("tempOut").textContent=$("temp").value+"K";$("brOut").textContent=$("br").value+"%";}
$("temp").oninput=$("br").oninput=syncOut;
async function load(){const r=await fetch("/api/state");const s=await r.json();$("temp").value=s.temperature;$("br").value=s.brightness;$("mode").value=String(s.mode);$("on").value=s.scheduleOn;$("off").value=s.scheduleOff;$("lat").value=s.latitude;$("lon").value=s.longitude;$("brk").checked=s.breakEnabled;$("work").value=s.workMinutes;$("rest").value=s.restSeconds;$("strict").checked=s.strictBreak;$("oled").checked=!!s.oledMode;$("oledState").textContent=s.oledMode?"ON":"off";$("oledMove").value=s.oledMoveSec||60;$("status").textContent=(s.oledMode?"🌙 OLED guard ON · ":"")+(s.enabled?"● filter ON":"○ filter OFF");syncOut();}
const clampMove=()=>Math.min(600,Math.max(10,+$("oledMove").value||60));
$("oled").onchange=async e=>{await send({oledMode:e.target.checked,oledMoveSec:clampMove()});};
async function send(patch){const cur=await (await fetch("/api/state")).json();Object.assign(cur,patch);const r=await fetch("/api/state",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(cur)});await load();}
$("save").onclick=()=>send({temperature:+$("temp").value,brightness:+$("br").value,enabled:true,mode:+$("mode").value,scheduleOn:$("on").value||"21:00",scheduleOff:$("off").value||"07:00",latitude:+$("lat").value||0,longitude:+$("lon").value||0,breakEnabled:$("brk").checked,workMinutes:+$("work").value||20,restSeconds:+$("rest").value||20,strictBreak:$("strict").checked,oledMode:$("oled").checked,oledMoveSec:clampMove()});
$("toggle").onclick=async()=>{const cur=await (await fetch("/api/state")).json();send({enabled:!cur.enabled});};
$("refresh").onclick=load;$("snooze").onclick=()=>send({snoozeMinutes:10});
load();setInterval(load,5000);
</script></main></body></html>)HTML";
}

} // namespace nightscreen
