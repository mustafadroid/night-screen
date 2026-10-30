#pragma once
// Fullscreen dim/blackout overlay for deep-dimming, strict breaks, and the
// OLED burn-in guard (dark screen + clock that relocates to random spots).

#include <functional>

namespace nightscreen {

class Overlay {
public:
  Overlay();
  ~Overlay();
  Overlay(const Overlay&) = delete;
  Overlay& operator=(const Overlay&) = delete;

  // alpha 0..0.9 black fullscreen overlay. alpha<=0 hides it (unless OLED on).
  void setDim(double alpha);
  // opaque break screen; empty text ends break (caller re-applies dim after).
  void setBreakMessage(const char* text);
  // OLED guard: opaque black + live clock at random positions every moveSeconds.
  // Any key/mouse activity invokes the onActivity callback (e.g. exit guard).
  void setOled(bool on, int moveSeconds = 60);
  [[nodiscard]] bool oledActive() const;
  void setOnActivity(std::function<void()> cb);
  void hide(); // hides dim + break + oled

private:
  struct Impl;
  Impl* impl_ = nullptr;
};

} // namespace nightscreen
