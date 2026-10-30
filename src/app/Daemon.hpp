#pragma once
// Long-running daemon: applies gamma at ~10Hz, handles schedules,
// break reminders, overlay dimming, and the web control panel.

#include <atomic>
#include <mutex>
#include <string>

#include "HttpServer.hpp"
#include "core/BreakTimer.hpp"
#include "core/Config.hpp"
#include "core/FilterEngine.hpp"
#include "platform/Gamma.hpp"
#include "platform/Overlay.hpp"
#include "platform/win/TrayGui.hpp"

#include <functional>
#include "platform/win/TrayGui.hpp"

namespace nightscreen {

class Daemon {
public:
  explicit Daemon(Settings s);
  ~Daemon();

  Daemon(const Daemon&) = delete;
  Daemon& operator=(const Daemon&) = delete;

  // Blocks until stop() is called (e.g. SIGINT).
  void run();
  void stop();

  Settings snapshot() const;
  // Returns JSON of new state, or "ERROR..." string.
  std::string applyJson(const std::string& body);
  // Direct in-process update from the tray GUI / web UI.
  void updateSettings(std::function<void(Settings&)> fn, bool save = true);
  void breakNow();
  void snoozeBreak(int minutes);

private:
  bool inEffectBypass() const; // true => daytime, filter should be off
  void applyLoop(std::stop_token tok);
  void restartBreakTimer();

  mutable std::mutex m_;
  Settings settings_;
  FilterEngine engine_;
  std::unique_ptr<GammaController> gamma_;
  Overlay overlay_;
  BreakTimer breaks_;
  HttpServer http_;
  TrayGui tray_;
  std::jthread worker_;
  std::atomic<bool> stopping_{false};
};

} // namespace nightscreen
