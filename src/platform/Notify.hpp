#pragma once
#include <string>

namespace nightscreen {
// Fire-and-forget desktop notification. Never throws.
void notify(const std::string& title, const std::string& body);
} // namespace nightscreen
