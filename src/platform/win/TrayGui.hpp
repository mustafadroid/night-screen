#pragma once
// Windows tray icon + main control window.
// Pure Win32 (no extra deps). On other platforms this is an empty stub so the
// Daemon can hold it unconditionally.

#include <functional>
#include <string>

#include "core/Config.hpp"

namespace nightscreen {

#ifdef _WIN32

// Balloon notification posted to the tray icon's thread (thread-safe).
// Returns false when the tray isn't running (caller should fall back).
bool winTryBalloon(const std::string& title, const std::string& body);

class TrayGui {
public:
  using SnapFn = std::function<Settings()>;
  using UpdateFn = std::function<void(std::function<void(Settings&)>, bool save)>;
  using VoidFn = std::function<void()>;
  using SnoozeFn = std::function<void(int minutes)>;

  TrayGui() = default;
  ~TrayGui() { stop(); }
  TrayGui(const TrayGui&) = delete;
  TrayGui& operator=(const TrayGui&) = delete;

  void start(SnapFn get, UpdateFn set, VoidFn quit, VoidFn breakNow, SnoozeFn snooze);
  void stop();

  // Callbacks wired by Daemon::run (read by the UI thread under a mutex).
  SnapFn get_;
  UpdateFn set_;
  VoidFn quit_;
  VoidFn breakNow_;
  SnoozeFn snooze_;
};

#else

inline bool winTryBalloon(const std::string&, const std::string&) { return false; }

class TrayGui {
public:
  using SnapFn = std::function<Settings()>;
  using UpdateFn = std::function<void(std::function<void(Settings&)>, bool)>;
  using VoidFn = std::function<void()>;
  using SnoozeFn = std::function<void(int)>;
  void start(SnapFn, UpdateFn, VoidFn, VoidFn, SnoozeFn) {}
  void stop() {}
};

#endif

} // namespace nightscreen
