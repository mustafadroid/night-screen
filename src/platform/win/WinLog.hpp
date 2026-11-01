#pragma once
// Startup/shutdown diary for the console-less Windows build.
// With no terminal window, this log is the only witness when something fails:
// %LOCALAPPDATA%\night-screen\night-screen.log (falls back to the temp dir).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace nightscreen {

inline std::filesystem::path winLogPath() {
  std::filesystem::path dir;
#if defined(_WIN32)
  char* local = nullptr;
  size_t n = 0;
  _dupenv_s(&local, &n, "LOCALAPPDATA");
  if (local && *local)
    dir = std::filesystem::path(local) / "night-screen";
  else
    dir = std::filesystem::temp_directory_path() / "night-screen";
  if (local) free(local);
#else
  dir = std::filesystem::temp_directory_path() / "night-screen";
#endif
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir / "night-screen.log";
}

inline void winLog(const std::string& msg) {
  static std::mutex mu;
  std::lock_guard<std::mutex> lk(mu);
  std::ofstream f(winLogPath(), std::ios::app);
  if (!f) return;
  using namespace std::chrono;
  std::time_t t = system_clock::to_time_t(system_clock::now());
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char ts[24];
  snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
           tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  f << "[" << ts << "] " << msg << "\n";
}

} // namespace nightscreen
