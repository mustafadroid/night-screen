#include "Notify.hpp"

#include "win/TrayGui.hpp"

#include <cstdlib>
#include <iostream>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // keep windows.h min/max macros away from std::min/max
#include <windows.h>
#endif

namespace nightscreen {

#if !defined(_WIN32) && !defined(__APPLE__)
static void shellEscapeAppend(std::string& out, const std::string& s) {
  out += '\'';
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += '\'';
}
#endif

void notify(const std::string& title, const std::string& body) {
#if defined(_WIN32)
  // Non-blocking: prefer a tray balloon; fall back to MessageBox when the
  // daemon (and its tray icon) isn't running, e.g. one-shot CLI use.
  if (!winTryBalloon(title, body)) {
    std::thread([title, body] {
      MessageBoxA(nullptr, body.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    }).detach();
  }
#elif defined(__APPLE__)
  std::string cmd = "osascript -e 'display notification ";
  std::string b = body;
  // minimal escape of quotes/backslash
  std::string eb;
  for (char c : b) {
    if (c == '\\' || c == '"') eb += '\\';
    eb += c;
  }
  std::string et;
  for (char c : title) {
    if (c == '\\' || c == '"') et += '\\';
    et += c;
  }
  cmd += "\"" + eb + "\" with title \"" + et + "\"' >/dev/null 2>&1 &";
  (void)std::system(cmd.c_str());
#else
  // Prefer notify-send; fall back to zenity/kdialog/stdout.
  std::string cmd = "which notify-send >/dev/null 2>&1 && notify-send ";
  std::string t = title.empty() ? "night-screen" : title;
  cmd += "--app-name night-screen ";
  shellEscapeAppend(cmd, t);
  cmd += " ";
  shellEscapeAppend(cmd, body);
  cmd += " >/dev/null 2>&1 &";
  int rc = std::system(cmd.c_str());
  if (rc != 0) {
    std::cout << "[notify] " << t << ": " << body << "\n";
  }
#endif
}

} // namespace nightscreen
