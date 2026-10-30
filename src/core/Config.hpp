#pragma once
// App settings: presets, modes, persistence (dependency-free mini-JSON).
// User-facing filter, schedule, and break settings.

#include <filesystem>
#include <string>
#include <vector>

namespace nightscreen {

enum class FilterMode : int { Manual = 0, Scheduled = 1, SunAuto = 2 };

struct Preset {
  std::string name;
  int temperature = 4500;    // Kelvin
  int brightness = 90;       // percent
};

[[nodiscard]] inline std::vector<Preset> builtinPresets() {
  return {
      {"Off", 6500, 100},
      {"Reading", 4500, 90},
      {"Night", 3400, 80},
      {"Dark Room", 2700, 60},
      {"Movie", 5000, 85},
      {"Game", 5500, 95},
      {"Custom", 4500, 90},
  };
}

struct Settings {
  bool enabled = true;
  int temperature = 3400; // K, 1000..6500
  int brightness = 85;    // %, 5..100
  double gamma = 1.0;
  std::string preset = "Night";
  FilterMode mode = FilterMode::Manual;
  std::string scheduleOn = "21:00";
  std::string scheduleOff = "07:00";
  double latitude = 0.0;   // for SunAuto
  double longitude = 0.0;
  bool breakEnabled = true;
  int workMinutes = 20;    // 20-20-20 rule default
  int restSeconds = 20;
  bool strictBreak = false; // lock screen during rest
  bool startAtLogin = false;
  int uiPort = 7788;
  bool overlayForDeepDim = true;
  bool oledMode = false;   // OLED burn-in guard: dark screen + roaming clock
  int oledMoveSec = 60;    // relocate the clock every N seconds (10..600)

  void clamp();
  std::string toJson() const;
  static Settings fromJson(const std::string& text);
  static std::filesystem::path defaultPath();
  bool loadFrom(const std::filesystem::path& p);
  bool saveTo(const std::filesystem::path& p) const;
};

} // namespace nightscreen
