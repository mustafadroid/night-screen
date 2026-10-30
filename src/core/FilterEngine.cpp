#include "FilterEngine.hpp"

#include <algorithm>

namespace nightscreen {

void FilterEngine::setTarget(const Settings& s, bool nightHold) {
  std::lock_guard<std::mutex> lk(m_);
  tgtActive_ = s.enabled && !nightHold ? true : (s.enabled && nightHold ? true : false);
  // Manual/scheduled/sun modes all resolve to "enabled && inEffect" via nightHold=false meaning on.
  // nightHold=true forces bypass (daytime in auto modes).
  if (!s.enabled || nightHold) {
    tgtActive_ = false;
  } else {
    tgtActive_ = true;
  }
  tgtTemp_ = s.enabled && tgtActive_ ? s.temperature : 6500.0;
  tgtBright_ = s.enabled && tgtActive_ ? (s.brightness / 100.0) : 1.0;
  overlay_ = s.overlayForDeepDim;
}

FilterEngine::Applied FilterEngine::step(double dt) {
  std::lock_guard<std::mutex> lk(m_);
  // ~1.2s smoothing
  const double k = std::clamp(dt / 1.2, 0.0, 1.0);
  curTemp_ += (tgtTemp_ - curTemp_) * k;
  curBright_ += (tgtBright_ - curBright_) * k;
  if (std::abs(tgtTemp_ - curTemp_) < 2.0) curTemp_ = tgtTemp_;
  if (std::abs(tgtBright_ - curBright_) < 0.002) curBright_ = tgtBright_;
  Applied a;
  a.active = tgtActive_ && (curTemp_ < 6490.0 || curBright_ < 0.995);
  a.temperature = static_cast<int>(curTemp_ + 0.5);
  a.brightnessPct = static_cast<int>(curBright_ * 100.0 + 0.5);
  a.overlayAlpha = overlay_ ? overlayAlphaFor(curBright_) : 0.0;
  if (!tgtActive_) a.overlayAlpha = 0.0;
  return a;
}

FilterEngine::Applied FilterEngine::current() const {
  std::lock_guard<std::mutex> lk(m_);
  Applied a;
  a.active = tgtActive_;
  a.temperature = static_cast<int>(curTemp_ + 0.5);
  a.brightnessPct = static_cast<int>(curBright_ * 100.0 + 0.5);
  a.overlayAlpha = overlay_ ? overlayAlphaFor(curBright_) : 0.0;
  return a;
}

} // namespace nightscreen
