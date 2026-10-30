#pragma once
// OS gamma control: sets color temperature + brightness for all displays.
// Falls back gracefully (headless / Wayland) so CLI/tests still run.

#include <memory>
#include <string>

#include "core/ColorTemp.hpp"

namespace nightscreen {

class GammaController {
public:
  virtual ~GammaController() = default;
  virtual bool apply(const GammaRamps& ramps) = 0;
  virtual void reset() = 0;
  [[nodiscard]] virtual bool available() const = 0;
  [[nodiscard]] virtual std::string backendName() const = 0;
};

[[nodiscard]] std::unique_ptr<GammaController> makeGammaController();

} // namespace nightscreen
