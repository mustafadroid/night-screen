#include "SunCalc.hpp"

#include <algorithm>

namespace nightscreen {

SunTimes sunTimesFor(int dayOfYear, double latitudeDeg, double longitudeDeg,
                     double utcOffsetHours) noexcept {
  constexpr double kPi = 3.14159265358979323846;
  constexpr double kZenith = 90.833; // official sunrise/sunset zenith
  SunTimes out;
  const double lat = latitudeDeg * kPi / 180.0;
  const double lon = longitudeDeg;

  // Solar declination (Cooper) + equation of time (approx).
  const double decl =
      -23.44 * kPi / 180.0 * std::cos(2.0 * kPi * (dayOfYear + 10) / 365.0);
  const double b = 2.0 * kPi * (dayOfYear - 81) / 364.0;
  const double eot = 9.87 * std::sin(2 * b) - 7.53 * std::cos(b) - 1.5 * std::sin(b); // minutes

  const double cosH =
      (std::cos(kZenith * kPi / 180.0) - std::sin(lat) * std::sin(decl)) /
      (std::cos(lat) * std::cos(decl));

  if (cosH < -1.0) {
    out.polarDay = true;
    return out;
  }
  if (cosH > 1.0) {
    out.polarNight = true;
    return out;
  }
  const double h = std::acos(std::clamp(cosH, -1.0, 1.0)) * 180.0 / kPi; // degrees
  const double solarNoonMin = 720.0 - 4.0 * lon - eot + utcOffsetHours * 60.0;
  const double delta = h * 4.0;
  out.sunriseMin = solarNoonMin - delta;
  out.sunsetMin = solarNoonMin + delta;
  return out;
}

} // namespace nightscreen
