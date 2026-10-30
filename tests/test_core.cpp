// Core self-checks: color math, config round-trip, sun calc. No test framework.

#include <cassert>
#include <cmath>
#include <iostream>

#include "core/BreakTimer.hpp"
#include "core/ColorTemp.hpp"
#include "core/Config.hpp"
#include "core/FilterEngine.hpp"
#include "core/SunCalc.hpp"

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n";              \
      return 1;                                                              \
    }                                                                        \
  } while (0)

int main() {
  using namespace nightscreen;

  // 6500K is ~neutral; 2700K must cut blue hard.
  auto day = kelvinToRgb(6500);
  auto warm = kelvinToRgb(2700);
  CHECK(day.b > 0.9 && day.r > 0.9);
  CHECK(warm.b < 0.6 * day.b);
  CHECK(warm.r >= warm.b);

  // Gamma ramps: full brightness identity-ish, dim scales down monotonically.
  auto full = makeRamps(6500, 1.0);
  CHECK(full.r[255] > 60000 && full.g[0] == 0);
  auto dim = makeRamps(3400, 0.5);
  CHECK(dim.b[255] < full.b[255]);
  for (int i = 1; i < 256; ++i) CHECK(dim.r[i] >= dim.r[i - 1]);
  CHECK(overlayAlphaFor(1.0) == 0.0);
  CHECK(overlayAlphaFor(0.05) > 0.3);

  // Config round-trip.
  Settings s;
  s.temperature = 2700;
  s.brightness = 60;
  s.preset = "Dark Room";
  s.mode = FilterMode::SunAuto;
  s.oledMode = true;
  s.oledMoveSec = 45;
  std::string j = s.toJson();
  Settings back = Settings::fromJson(j);
  CHECK(back.temperature == 2700 && back.brightness == 60);
  CHECK(back.preset == "Dark Room" && back.mode == FilterMode::SunAuto);
  CHECK(back.oledMode && back.oledMoveSec == 45);
  Settings empty = Settings::fromJson("");
  CHECK(empty.temperature == 3400); // defaults survive

  // Sun calc: equator equinox ~= 6:00/18:00 local solar; poles handle extremes.
  SunTimes eq = sunTimesFor(80, 0.0, 0.0, 0.0);
  CHECK(std::abs(eq.sunriseMin - 360.0) < 45.0);
  CHECK(std::abs(eq.sunsetMin - 1080.0) < 45.0);
  SunTimes polar = sunTimesFor(172, 85.0, 0.0, 0.0);
  CHECK(polar.polarDay);

  // Filter engine smoothing converges.
  FilterEngine eng;
  Settings on;
  on.enabled = true;
  on.temperature = 2700;
  on.brightness = 50;
  eng.setTarget(on, false);
  FilterEngine::Applied a{};
  for (int i = 0; i < 50; ++i) a = eng.step(0.1);
  CHECK(a.active && a.temperature < 3000);
  eng.setTarget(on, true); // daytime bypass
  for (int i = 0; i < 50; ++i) a = eng.step(0.1);
  CHECK(!a.active);

  std::cout << "all core tests passed\n";
  return 0;
}
