#pragma once
// Holds current vs target filter state and smooths transitions.

#include <mutex>

#include "ColorTemp.hpp"
#include "Config.hpp"

namespace nightscreen {

class FilterEngine {
public:
  struct Applied {
    bool active = false; // gamma actually modified
    int temperature = 6500;
    int brightnessPct = 100;
    double overlayAlpha = 0.0;
  };

  void setTarget(const Settings& s, bool nightHold);
  Applied step(double dtSeconds); // call ~10Hz; lerps current->target
  Applied current() const;

private:
  mutable std::mutex m_;
  double curTemp_ = 6500.0;
  double curBright_ = 1.0; // 0..1
  double tgtTemp_ = 6500.0;
  double tgtBright_ = 1.0;
  bool tgtActive_ = false;
  bool overlay_ = true;
};

} // namespace nightscreen
