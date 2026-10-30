#pragma once
// Approximate sunrise/sunset (NOAA-style, low precision).
// Good enough for auto day/night switching.

#include <chrono>
#include <cmath>

namespace nightscreen {

// minutes since midnight (local clock approx, ignoring DST nuances;
// caller should treat result as rough +/-15min).
struct SunTimes {
  double sunriseMin = 6.5 * 60.0;
  double sunsetMin = 18.5 * 60.0;
  bool polarDay = false;
  bool polarNight = false;
};

[[nodiscard]] SunTimes sunTimesFor(int dayOfYear, double latitudeDeg, double longitudeDeg,
                                   double utcOffsetHours = 0.0) noexcept;

// Current local "should filter be on" for SunAuto mode: on when night.
[[nodiscard]] inline bool isNightNow(double latitude, double longitude) noexcept {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const std::time_t t = system_clock::to_time_t(now);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  const int doy = local.tm_yday + 1;
  // Rough UTC offset from tm_gmtoff when available.
  double off = 0.0;
#if defined(__linux__) || defined(__APPLE__)
  off = static_cast<double>(local.tm_gmtoff) / 3600.0;
#else
  // Windows: derive from difference between local and UTC.
  std::tm utc{};
  gmtime_s(&utc, &t);
  off = static_cast<double>(std::mktime(&local) - std::mktime(&utc)) / 3600.0;
#endif
  SunTimes st = sunTimesFor(doy, latitude, longitude, off);
  if (st.polarDay) return false;
  if (st.polarNight) return true;
  const double nowMin = local.tm_hour * 60.0 + local.tm_min;
  return (nowMin < st.sunriseMin || nowMin > st.sunsetMin);
}

} // namespace nightscreen
