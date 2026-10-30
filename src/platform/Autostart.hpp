#pragma once
#include <string>

namespace nightscreen {
// Install/remove OS login autostart for the current executable.
// Returns human-readable status; never throws.
std::string setStartAtLogin(bool enable);
} // namespace nightscreen
