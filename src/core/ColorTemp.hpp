#pragma once
// Color temperature -> RGB balance + gamma ramp generation.
// Pure C++26, no OS calls. Used by all platforms.

#include <algorithm>
#include <array>
#include <cmath>

namespace nightscreen {

struct RgbBalance {
  double r = 1.0; // 0..1 multipliers, 6500K ~= 1,1,1
  double g = 1.0;
  double b = 1.0;
};

// Tanner Helland approximation, clamped to 1000..12000K.
[[nodiscard]] inline RgbBalance kelvinToRgb(double kelvin) noexcept {
  kelvin = std::clamp(kelvin, 1000.0, 12000.0);
  const double t = kelvin / 100.0;
  RgbBalance out;
  // Red
  if (t <= 66.0) {
    out.r = 1.0;
  } else {
    double x = t - 60.0;
    out.r = 1.2927564 * std::pow(x, -0.1332047592);
  }
  // Green
  if (t <= 66.0) {
    out.g = 0.3900815788 * std::log(std::max(t, 1.0)) - 0.6318414437;
  } else {
    double x = t - 60.0;
    out.g = 1.129890861 * std::pow(x, -0.0755148492);
  }
  // Blue
  if (t >= 66.0) {
    out.b = 1.0;
  } else if (t <= 19.0) {
    out.b = 0.0;
  } else {
    double x = t - 10.0;
    out.b = 0.5432067891 * std::log(x) - 1.1962540891;
  }
  out.r = std::clamp(out.r, 0.0, 1.0);
  out.g = std::clamp(out.g, 0.0, 1.0);
  out.b = std::clamp(out.b, 0.0, 1.0);
  return out;
}

constexpr int kRampSize = 256;
using Ramp = std::array<unsigned short, kRampSize>; // 0..65535 like OS APIs

struct GammaRamps {
  Ramp r{};
  Ramp g{};
  Ramp b{};
};

// Build OS-style gamma ramps from temperature + brightness.
// brightness: 0.05..1.0, gamma: display gamma exponent (default 1.0 = linear scale).
[[nodiscard]] inline GammaRamps makeRamps(double kelvin, double brightness,
                                         double gamma = 1.0) noexcept {
  brightness = std::clamp(brightness, 0.05, 1.0);
  gamma = std::clamp(gamma, 0.4, 3.0);
  const RgbBalance bal = kelvinToRgb(kelvin);
  GammaRamps out;
  for (int i = 0; i < kRampSize; ++i) {
    const double norm = static_cast<double>(i) / (kRampSize - 1);
    const double shaped = std::pow(norm, gamma);
    const double rv = std::clamp(shaped * bal.r * brightness, 0.0, 1.0);
    const double gv = std::clamp(shaped * bal.g * brightness, 0.0, 1.0);
    const double bv = std::clamp(shaped * bal.b * brightness, 0.0, 1.0);
    out.r[i] = static_cast<unsigned short>(std::lround(rv * 65535.0));
    out.g[i] = static_cast<unsigned short>(std::lround(gv * 65535.0));
    out.b[i] = static_cast<unsigned short>(std::lround(bv * 65535.0));
  }
  return out;
}

// Extra software-dim overlay opacity (0 = none) for very low brightness,
// software dimming that goes below the hardware minimum.
[[nodiscard]] constexpr double overlayAlphaFor(double brightness) noexcept {
  if (brightness >= 0.55) return 0.0;
  double a = (0.55 - brightness) * 1.1; // 0.55->0, 0.05->~0.55
  return std::clamp(a, 0.0, 0.6);
}

} // namespace nightscreen
