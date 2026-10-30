#include "Daemon.hpp"

#include <chrono>
#include <iostream>

#include "core/SunCalc.hpp"
#include "platform/Autostart.hpp"
#include "platform/Notify.hpp"

namespace nightscreen {
namespace {
int parseHm(const std::string& s) {
  // "HH:MM" -> minutes since midnight, -1 on error
  if (s.size() < 4) return -1;
  auto colon = s.find(':');
  if (colon == std::string::npos) return -1;
  try {
    int h = std::stoi(s.substr(0, colon));
    int m = std::stoi(s.substr(colon + 1));
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
  } catch (...) {
    return -1;
  }
}
int nowMinutesLocal() {
  using namespace std::chrono;
  const std::time_t t = system_clock::to_time_t(system_clock::now());
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  return local.tm_hour * 60 + local.tm_min;
}
bool inScheduleWindow(const std::string& on, const std::string& off, int nowMin) {
  int a = parseHm(on), b = parseHm(off);
  if (a < 0 || b < 0) return true; // malformed -> always on
  if (a == b) return true;
  if (a < b) return nowMin >= a && nowMin < b;
  return nowMin >= a || nowMin < b; // overnight window
}
} // namespace

Daemon::Daemon(Settings s) : settings_(std::move(s)) {
  settings_.clamp();
  // NOTE: gamma_ is created in run(), after the web UI is listening, so a
  // slow/broken X server can never delay startup.
}

Daemon::~Daemon() { stop(); }

void Daemon::run() {
  // Break timer with desktop notifications.
  restartBreakTimer();

  // Any key/mouse activity while the OLED guard is up exits it.
  overlay_.setOnActivity([this] {
    bool was = false;
    {
      std::lock_guard<std::mutex> lk(m_);
      was = settings_.oledMode;
      settings_.oledMode = false;
      settings_.saveTo(Settings::defaultPath());
    }
    if (was) notify("night-screen", "OLED burn-in guard off — welcome back.");
  });

#if defined(_WIN32)
  // Native tray icon + main control panel.
  tray_.start(
      [this] { return snapshot(); },
      [this](std::function<void(Settings&)> fn, bool save) { updateSettings(fn, save); },
      [this] { stop(); },
      [this] { breakNow(); },
      [this](int m) { snoozeBreak(m); });
  // Headless on purpose (GUI subsystem, no console): one balloon so the user
  // can find the tray moon. Tray start waits for its window, so no race.
  notify("night-screen", "Running in the tray — double-click the moon for settings.");
#endif

  std::string err;
  if (!http_.start(settings_.uiPort,
                   [this] { return snapshot(); },
                   [this](const std::string& b) { return applyJson(b); },
                   err)) {
    std::cerr << "night-screen: web UI unavailable: " << err << "\n";
  } else {
    std::cout << "night-screen: control panel at http://127.0.0.1:" << settings_.uiPort
              << "/\n";
  }
  gamma_ = makeGammaController(); // after HTTP is up: XOpenDisplay may block
  std::cout << "night-screen: gamma backend: " << gamma_->backendName()
            << (gamma_->available() ? " (display found)" : " (NO DISPLAY - running headless)")
            << "\n";

  worker_ = std::jthread([this](std::stop_token tok) { applyLoop(tok); });
  // Block until stop requested: poll the jthread's liveness via condition-free sleep.
  while (!stopping_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
  tray_.stop();
  http_.stop();
  breaks_.stop();
  overlay_.hide();
  gamma_->reset();
}

void Daemon::stop() {
  stopping_ = true;
  if (worker_.joinable()) worker_.request_stop();
}

Settings Daemon::snapshot() const {
  std::lock_guard<std::mutex> lk(m_);
  return settings_;
}

std::string Daemon::applyJson(const std::string& body) {
  Settings next = Settings::fromJson(body);
  // Support extra web-only key snoozeMinutes without persisting it.
  int snoozeMin = 0;
  {
    auto p = body.find("snoozeMinutes");
    if (p != std::string::npos) {
      auto c = body.find(':', p);
      if (c != std::string::npos) {
        try {
          snoozeMin = std::stoi(body.substr(c + 1));
        } catch (...) {}
      }
    }
  }
  updateSettings([next](Settings& s) { s = next; }, true);
  if (snoozeMin > 0) breaks_.snooze(snoozeMin);
  return snapshot().toJson();
}

void Daemon::updateSettings(std::function<void(Settings&)> fn, bool save) {
  bool loginChanged = false;
  bool loginValue = false;
  {
    std::lock_guard<std::mutex> lk(m_);
    bool prevLogin = settings_.startAtLogin;
    int prevPort = settings_.uiPort;
    fn(settings_);
    settings_.uiPort = prevPort; // port changes require restart
    settings_.clamp();
    if (save) settings_.saveTo(Settings::defaultPath());
    loginChanged = (settings_.startAtLogin != prevLogin);
    loginValue = settings_.startAtLogin;
  }
  if (loginChanged) setStartAtLogin(loginValue);
  restartBreakTimer();
}

void Daemon::breakNow() { breaks_.trigger(); }

void Daemon::snoozeBreak(int minutes) { breaks_.snooze(minutes); }

bool Daemon::inEffectBypass() const {
  Settings s = snapshot();
  if (!s.enabled) return true;
  switch (s.mode) {
    case FilterMode::Manual:
      return false;
    case FilterMode::Scheduled:
      return !inScheduleWindow(s.scheduleOn, s.scheduleOff, nowMinutesLocal());
    case FilterMode::SunAuto: {
      bool night = isNightNow(s.latitude, s.longitude);
      return !night;
    }
  }
  return false;
}

void Daemon::restartBreakTimer() {
  Settings s = snapshot();
  BreakConfig cfg{.enabled = s.breakEnabled,
                  .workMinutes = s.workMinutes,
                  .restSeconds = s.restSeconds,
                  .strict = s.strictBreak};
  if (!cfg.enabled) {
    breaks_.stop();
    return;
  }
  breaks_.start(
      cfg,
      [](int left) { notify("night-screen", "Eye break in ~" + std::to_string(left) + "s — look 20ft away."); },
      [this](int rest, bool strict) {
        if (strict) overlay_.setBreakMessage("break");
        notify("night-screen",
               "Time for an eye break! Look 20 feet away for " + std::to_string(rest) +
                   "s." + (strict ? " (screen locked)" : ""));
      },
      [this] {
        if (!snapshot().oledMode) overlay_.hide(); // OLED guard manages its own overlay
        notify("night-screen", "Break over — well done.");
      });
}

void Daemon::applyLoop(std::stop_token tok) {
  using namespace std::chrono;
  auto last = steady_clock::now();
  bool wasActive = false;
  while (!tok.stop_requested() && !stopping_.load()) {
    auto now = steady_clock::now();
    double dt = duration<double>(now - last).count();
    last = now;
    Settings s = snapshot();
    engine_.setTarget(s, inEffectBypass());
    FilterEngine::Applied a = engine_.step(dt > 0.5 ? 0.1 : dt);
    GammaRamps ramps = makeRamps(a.active ? a.temperature : 6500,
                                 a.active ? a.brightnessPct / 100.0 : 1.0, s.gamma);
    if (a.active || wasActive) {
      if (!gamma_->apply(ramps) && !gamma_->available()) {
        // Headless: nothing to do, avoid log spam (throttle by only noting transitions).
      }
      wasActive = a.active;
    }
    // Deep-dim overlay follows smoothed brightness (OLED guard wins outright;
    // strict-break blackout at ~0.85 must not be cleared while a break runs).
    if (s.oledMode) {
      overlay_.setOled(true, s.oledMoveSec);
    } else {
      if (overlay_.oledActive()) overlay_.setOled(false);
      if (a.overlayAlpha > 0.005 && a.active)
        overlay_.setDim(a.overlayAlpha);
      else if (breaks_.secondsUntilBreak() != 0)
        overlay_.hide();
    }
    std::this_thread::sleep_for(milliseconds(100));
  }
}

} // namespace nightscreen
