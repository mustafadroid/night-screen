#include "Config.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <shlobj.h>
#elif defined(__APPLE__)
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace nightscreen {

void Settings::clamp() {
  temperature = std::clamp(temperature, 1000, 6500);
  brightness = std::clamp(brightness, 5, 100);
  gamma = std::clamp(gamma, 0.4, 3.0);
  workMinutes = std::clamp(workMinutes, 1, 180);
  restSeconds = std::clamp(restSeconds, 5, 3600);
  uiPort = std::clamp(uiPort, 1024, 65535);
  oledMoveSec = std::clamp(oledMoveSec, 10, 600);
  if (preset.empty()) preset = "Custom";
}

std::string Settings::toJson() const {
  std::ostringstream o;
  o << "{\n"
    << "  \"enabled\": " << (enabled ? "true" : "false") << ",\n"
    << "  \"temperature\": " << temperature << ",\n"
    << "  \"brightness\": " << brightness << ",\n"
    << "  \"gamma\": " << gamma << ",\n"
    << "  \"preset\": \"" << preset << "\",\n"
    << "  \"mode\": " << static_cast<int>(mode) << ",\n"
    << "  \"scheduleOn\": \"" << scheduleOn << "\",\n"
    << "  \"scheduleOff\": \"" << scheduleOff << "\",\n"
    << "  \"latitude\": " << latitude << ",\n"
    << "  \"longitude\": " << longitude << ",\n"
    << "  \"breakEnabled\": " << (breakEnabled ? "true" : "false") << ",\n"
    << "  \"workMinutes\": " << workMinutes << ",\n"
    << "  \"restSeconds\": " << restSeconds << ",\n"
    << "  \"strictBreak\": " << (strictBreak ? "true" : "false") << ",\n"
    << "  \"startAtLogin\": " << (startAtLogin ? "true" : "false") << ",\n"
    << "  \"uiPort\": " << uiPort << ",\n"
    << "  \"overlayForDeepDim\": " << (overlayForDeepDim ? "true" : "false") << ",\n"
    << "  \"oledMode\": " << (oledMode ? "true" : "false") << ",\n"
    << "  \"oledMoveSec\": " << oledMoveSec << "\n"
    << "}\n";
  return o.str();
}

namespace {
std::string slice(const std::string& s, const std::string& key) {
  auto pos = s.find('\"' + key + '\"');
  if (pos == std::string::npos) return {};
  pos = s.find(':', pos);
  if (pos == std::string::npos) return {};
  auto end = s.find_first_of(",\n}", pos + 1);
  std::string v = s.substr(pos + 1, end - pos - 1);
  // trim
  auto trim = [](std::string t) {
    size_t a = t.find_first_not_of(" \t\r\n\"");
    size_t b = t.find_last_not_of(" \t\r\n\"");
    if (a == std::string::npos) return std::string{};
    return t.substr(a, b - a + 1);
  };
  return trim(v);
}
int toInt(const std::string& v, int dflt) {
  int out = dflt;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}
double toDbl(const std::string& v, double dflt) {
  try {
    if (v.empty()) return dflt;
    return std::stod(v);
  } catch (...) {
    return dflt;
  }
}
bool toBool(const std::string& v, bool dflt) {
  if (v == "true") return true;
  if (v == "false") return false;
  return dflt;
}
} // namespace

Settings Settings::fromJson(const std::string& text) {
  Settings s;
  if (text.empty()) return s;
  s.enabled = toBool(slice(text, "enabled"), s.enabled);
  s.temperature = toInt(slice(text, "temperature"), s.temperature);
  s.brightness = toInt(slice(text, "brightness"), s.brightness);
  s.gamma = toDbl(slice(text, "gamma"), s.gamma);
  {
    std::string p = slice(text, "preset");
    if (!p.empty()) s.preset = p;
  }
  s.mode = static_cast<FilterMode>(toInt(slice(text, "mode"), 0));
  {
    std::string v = slice(text, "scheduleOn");
    if (!v.empty()) s.scheduleOn = v;
    v = slice(text, "scheduleOff");
    if (!v.empty()) s.scheduleOff = v;
  }
  s.latitude = toDbl(slice(text, "latitude"), s.latitude);
  s.longitude = toDbl(slice(text, "longitude"), s.longitude);
  s.breakEnabled = toBool(slice(text, "breakEnabled"), s.breakEnabled);
  s.workMinutes = toInt(slice(text, "workMinutes"), s.workMinutes);
  s.restSeconds = toInt(slice(text, "restSeconds"), s.restSeconds);
  s.strictBreak = toBool(slice(text, "strictBreak"), s.strictBreak);
  s.startAtLogin = toBool(slice(text, "startAtLogin"), s.startAtLogin);
  s.uiPort = toInt(slice(text, "uiPort"), s.uiPort);
  s.overlayForDeepDim = toBool(slice(text, "overlayForDeepDim"), s.overlayForDeepDim);
  s.oledMode = toBool(slice(text, "oledMode"), s.oledMode);
  s.oledMoveSec = toInt(slice(text, "oledMoveSec"), s.oledMoveSec);
  s.clamp();
  return s;
}

std::filesystem::path Settings::defaultPath() {
#if defined(_WIN32)
  char* appdata = nullptr;
  size_t n = 0;
  _dupenv_s(&appdata, &n, "APPDATA");
  std::filesystem::path base = appdata ? appdata : ".";
  if (appdata) free(appdata);
  return base / "night-screen" / "config.json";
#elif defined(__APPLE__)
  const char* home = getenv("HOME");
  std::filesystem::path base = home ? home : ".";
  return base / "Library" / "Application Support" / "night-screen" / "config.json";
#else
  const char* xdg = getenv("XDG_CONFIG_HOME");
  const char* home = getenv("HOME");
  std::filesystem::path base;
  if (xdg && *xdg)
    base = xdg;
  else
    base = std::filesystem::path(home ? home : ".") / ".config";
  return base / "night-screen" / "config.json";
#endif
}

bool Settings::loadFrom(const std::filesystem::path& p) {
  std::ifstream f(p);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *this = fromJson(ss.str());
  return true;
}

bool Settings::saveTo(const std::filesystem::path& p) const {
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::trunc);
  if (!f) return false;
  f << toJson();
  return true;
}

} // namespace nightscreen
