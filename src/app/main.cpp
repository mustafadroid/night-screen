// night-screen: blue-light filter + eye-break daemon.
// Cross-platform: Linux (X11/Xrandr), Windows (GDI gamma), macOS (CoreGraphics).

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "app/Daemon.hpp"
#include "core/ColorTemp.hpp"
#include "core/Config.hpp"
#include "core/SunCalc.hpp"
#include "platform/Autostart.hpp"
#include "platform/Gamma.hpp"
#include "platform/Overlay.hpp"

namespace {
std::atomic<bool> g_stop{false};
void onSigint(int) { g_stop = true; }
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
static nightscreen::Daemon* g_daemon = nullptr;
static std::atomic<bool> g_done{false};
// Console-less GUI app: one-shot commands borrow the caller's console so their
// output is visible; `daemon` stays headless unless --console is passed.
static void useParentConsole() {
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
#if defined(_MSC_VER)
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
#else
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
#endif
  }
}
static void useOwnConsole() {
  if (AllocConsole()) {
#if defined(_MSC_VER)
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
#else
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
#endif
  }
}
// Closing the console window (or logoff/shutdown/Ctrl+C) must still restore
// the display: stop the daemon and wait for its cleanup (gamma reset).
static BOOL WINAPI onConsoleCtrl(DWORD ev) {
  if (ev == CTRL_C_EVENT || ev == CTRL_BREAK_EVENT || ev == CTRL_CLOSE_EVENT ||
      ev == CTRL_LOGOFF_EVENT || ev == CTRL_SHUTDOWN_EVENT) {
    g_stop = true;
    if (g_daemon) g_daemon->stop();
    for (int i = 0; i < 80 && !g_done.load(); ++i) Sleep(50); // <= ~4s
    return TRUE;
  }
  return FALSE;
}
#endif

void usage() {
  std::cout
      << "night-screen — blue-light filter & eye-break reminders\n"
         "\n"
         "Usage:\n"
         "  nightscreen daemon [--port N] [--console]\n"
         "                                   run filter + breaks + tray icon\n"
         "                                   (no terminal window; --console forces one)\n"
         "  nightscreen on|off|toggle            enable/disable filter (then exit)\n"
         "  nightscreen temp <1000-6500>         set color temperature\n"
         "  nightscreen brightness <5-100>       set brightness\n"
         "  nightscreen preset <name>            apply preset (Off/Reading/Night/Dark Room/Movie/Game)\n"
         "  nightscreen ui                      print control-panel URL (starts daemon if needed)\n"
         "  nightscreen status                   show current settings\n"
         "  nightscreen reset                    clear filter immediately\n"
         "  nightscreen blackout <seconds>       fullscreen dark break (default 60)\n"
         "  nightscreen oled <on|off> [moveSec]  OLED burn-in guard: dark screen +\n"
         "                                     roaming clock (any input exits)\n"
         "  nightscreen autostart <on|off>        run at login\n"
         "\n"
         "Web control panel: http://127.0.0.1:7788/ (while daemon runs)\n"
         "Config file: per-OS standard location (see README).\n";
}

nightscreen::Settings loadOrDefault() {
  nightscreen::Settings s;
  s.loadFrom(nightscreen::Settings::defaultPath());
  s.clamp();
  return s;
}
} // namespace

int main(int argc, char** argv) {
  using namespace nightscreen;
  std::signal(SIGINT, onSigint);
  std::signal(SIGTERM, onSigint);
#if !defined(_WIN32)
  std::signal(SIGHUP, onSigint); // terminal closed: restore gamma, don't strand it
#else
  SetConsoleCtrlHandler(onConsoleCtrl, TRUE); // console X / logoff / shutdown
#endif

  std::vector<std::string> args(argv + 1, argv + argc);
  Settings cfg = loadOrDefault();
  if (!args.empty() && args[0] == "--port") {
    // tolerate `nightscreen --port N daemon` ordering
    args = {"daemon", args[0], args.size() > 1 ? args[1] : ""};
  }

  std::string cmd = args.empty() ? "daemon" : args[0];
#if defined(_WIN32)
  // GUI subsystem = no console window. One-shot commands borrow the caller's
  // console for output; `daemon` stays invisible unless --console is given.
  if (cmd != "daemon") useParentConsole();
#endif

  if (cmd == "daemon" || cmd == "ui") {
    bool wantConsole = false;
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--port" && i + 1 < args.size()) cfg.uiPort = std::stoi(args[++i]);
      if (args[i] == "--console") wantConsole = true; // debug: force a console
    }
#if defined(_WIN32)
    if (wantConsole) useOwnConsole(); // daemon is headless by default (tray only)
#else
    (void)wantConsole;
#endif
    cfg.saveTo(Settings::defaultPath());
    if (cmd == "ui") {
      std::cout << "Open http://127.0.0.1:" << cfg.uiPort
                << "/ after starting: nightscreen daemon\n";
      return 0;
    }
    Daemon d(cfg);
#if defined(_WIN32)
    g_daemon = &d;
#endif
    std::jthread killer([](std::stop_token) {
      while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    });
    // stop daemon when signal arrives
    std::thread waiter([&d] {
      while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      d.stop();
    });
    waiter.detach();
    d.run();
#if defined(_WIN32)
    g_daemon = nullptr;
    g_done = true;
#endif
    return 0;
  }
  if (cmd == "status") {
    std::cout << "enabled=" << cfg.enabled << " temp=" << cfg.temperature << "K brightness="
              << cfg.brightness << "% preset=" << cfg.preset << " mode=" << (int)cfg.mode
              << " breaks=" << cfg.breakEnabled << " work=" << cfg.workMinutes
              << "m rest=" << cfg.restSeconds
              << "s oled=" << (cfg.oledMode ? "on" : "off")
              << " backend=" << makeGammaController()->backendName() << "\n";
    return 0;
  }
  if (cmd == "on") cfg.enabled = true;
  else if (cmd == "off") cfg.enabled = false;
  else if (cmd == "toggle") cfg.enabled = !cfg.enabled;
  else if (cmd == "temp" && args.size() > 1) cfg.temperature = std::stoi(args[1]);
  else if (cmd == "brightness" && args.size() > 1) cfg.brightness = std::stoi(args[1]);
  else if (cmd == "preset" && args.size() > 1) {
    std::string want = args[1];
    for (auto& p : builtinPresets()) {
      std::string a = p.name, b = want;
      for (auto& c : a) c = (char)tolower(c);
      for (auto& c : b) c = (char)tolower(c);
      if (a == b || a.starts_with(b)) {
        cfg.preset = p.name;
        cfg.temperature = p.temperature;
        cfg.brightness = p.brightness;
        cfg.enabled = (p.name != "Off");
        break;
      }
    }
  } else if (cmd == "reset") {
    makeGammaController()->reset();
    std::cout << "filter cleared\n";
    return 0;
  } else if (cmd == "blackout") {
    int secs = 60;
    if (args.size() > 1) secs = std::clamp(std::stoi(args[1]), 1, 3600);
    Overlay ov;
    ov.setBreakMessage("break");
    std::cout << "blackout for " << secs << "s (Ctrl+C to cancel)\n";
    for (int i = 0; i < secs * 10 && !g_stop.load(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ov.hide();
    return 0;
  } else if (cmd == "oled" && args.size() > 1) {
    std::string v = args[1];
    if (v == "toggle")
      cfg.oledMode = !cfg.oledMode;
    else
      cfg.oledMode = (v == "on" || v == "1" || v == "true");
    if (args.size() > 2) cfg.oledMoveSec = std::clamp(std::stoi(args[2]), 10, 600);
    cfg.clamp();
    cfg.saveTo(Settings::defaultPath());
    if (!cfg.oledMode) {
      std::cout << "OLED guard off\n";
      return 0;
    }
    // Hold the guard until Ctrl+C (daemon keeps it up if running instead).
    Overlay ov;
    ov.setOnActivity([] { g_stop = true; });
    ov.setOled(true, cfg.oledMoveSec);
    std::cout << "OLED burn-in guard on (clock moves every " << cfg.oledMoveSec
              << "s; any key/mouse or Ctrl+C exits)\n";
    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ov.hide();
    cfg.oledMode = false;
    cfg.saveTo(Settings::defaultPath());
    return 0;
  } else if (cmd == "autostart" && args.size() > 1) {
    bool en = (args[1] == "on" || args[1] == "1" || args[1] == "true");
    cfg.startAtLogin = en;
    cfg.saveTo(Settings::defaultPath());
    std::cout << setStartAtLogin(en) << "\n";
    return 0;
  } else {
    usage();
    return cmd == "help" || cmd == "--help" ? 0 : 1;
  }
  cfg.clamp();
  cfg.saveTo(Settings::defaultPath());
  // Apply once immediately (so scripting works without the daemon).
  auto gamma = makeGammaController();
  GammaRamps r = makeRamps(cfg.enabled ? cfg.temperature : 6500,
                            cfg.enabled ? cfg.brightness / 100.0 : 1.0, cfg.gamma);
  bool ok = gamma->apply(r);
  std::cout << (cfg.enabled ? "filter on" : "filter off") << " (temp=" << cfg.temperature
            << "K brightness=" << cfg.brightness << "%"
            << (ok ? "" : " [no display — saved for daemon]") << ")\n";
  return 0;
}
