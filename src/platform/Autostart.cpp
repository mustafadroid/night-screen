#include "Autostart.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // keep windows.h min/max macros away from std::min/max
#include <windows.h>
#elif defined(__APPLE__)
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace nightscreen {

static std::string exePath() {
#if defined(_WIN32)
  char buf[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  return n ? std::string(buf, n) : "nightscreen";
#elif defined(__APPLE__)
  char buf[4096];
  uint32_t n = sizeof(buf);
  // _NSGetExecutablePath would need mach-o/dyld.h; fall back to argv-agnostic name
  (void)n;
  (void)buf;
  return "nightscreen";
#else
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = 0;
    return buf;
  }
  return "nightscreen";
#endif
}

std::string setStartAtLogin(bool enable) {
#if defined(_WIN32)
  HKEY h;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                    KEY_SET_VALUE, &h) != ERROR_SUCCESS)
    return "cannot open Run key";
  if (enable) {
    std::string v = "\"" + exePath() + "\" daemon";
    LONG rc = RegSetValueExA(h, "night-screen", 0, REG_SZ, (const BYTE*)v.c_str(),
                             (DWORD)(v.size() + 1));
    RegCloseKey(h);
    return rc == ERROR_SUCCESS ? "autostart enabled" : "failed to write Run key";
  }
  RegDeleteValueA(h, "night-screen");
  RegCloseKey(h);
  return "autostart disabled";
#elif defined(__APPLE__)
  const char* home = getenv("HOME");
  std::filesystem::path dir =
      std::filesystem::path(home ? home : ".") / "Library" / "LaunchAgents";
  std::filesystem::path plist = dir / "com.nightscreen.app.plist";
  if (!enable) {
    std::error_code ec;
    std::filesystem::remove(plist, ec);
    return "autostart disabled";
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream f(plist);
  if (!f) return "cannot write LaunchAgent";
  f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
       "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
       "<plist version=\"1.0\"><dict>\n"
       "<key>Label</key><string>com.nightscreen.app</string>\n"
       "<key>ProgramArguments</key><array><string>"
    << exePath()
    << "</string><string>daemon</string></array>\n"
       "<key>RunAtLoad</key><true/>\n</dict></plist>\n";
  return "autostart enabled (re-login to take effect)";
#else
  std::filesystem::path dir;
  const char* xdg = getenv("XDG_CONFIG_HOME");
  const char* home = getenv("HOME");
  if (xdg && *xdg)
    dir = std::filesystem::path(xdg) / "autostart";
  else
    dir = std::filesystem::path(home ? home : ".") / ".config" / "autostart";
  std::filesystem::path desktop = dir / "night-screen.desktop";
  if (!enable) {
    std::error_code ec;
    std::filesystem::remove(desktop, ec);
    return "autostart disabled";
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream f(desktop);
  if (!f) return "cannot write autostart entry";
  f << "[Desktop Entry]\nType=Application\nName=night-screen\nComment=Blue-light filter\n"
    << "Exec=" << exePath() << " daemon\n"
    << "X-GNOME-Autostart-enabled=true\nNoDisplay=false\n";
  return "autostart enabled";
#endif
}

} // namespace nightscreen
