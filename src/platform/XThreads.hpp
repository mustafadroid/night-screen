#pragma once
// Xlib is not thread-safe unless XInitThreads() runs before the first
// XOpenDisplay. Gamma (main/worker thread) and Overlay (worker thread) each
// open their own Display, so every entry point must ensure this exactly once.

#if !defined(_WIN32) && !defined(__APPLE__)
#include <X11/Xlib.h>

namespace nightscreen {
inline void ensureXThreads() {
  static bool inited = [] {
    XInitThreads(); // process-wide, idempotent for our purposes
    return true;
  }();
  (void)inited;
}
} // namespace nightscreen

#endif
