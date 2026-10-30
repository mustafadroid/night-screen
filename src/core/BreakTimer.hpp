#pragma once
// 20-20-20 style eye-break scheduler. Header-only logic + jthread worker in .cpp.

#include <atomic>
#include <chrono>
#include <functional>
#include <stop_token>
#include <thread>

namespace nightscreen {

struct BreakConfig {
  bool enabled = true;
  int workMinutes = 20;
  int restSeconds = 20;
  bool strict = false;
};

class BreakTimer {
public:
  using WarnFn = std::function<void(int secondsLeft)>;
  using BreakFn = std::function<void(int restSeconds, bool strict)>;
  using EndFn = std::function<void()>;

  BreakTimer() = default;
  ~BreakTimer() { stop(); }

  BreakTimer(const BreakTimer&) = delete;
  BreakTimer& operator=(const BreakTimer&) = delete;

  void start(BreakConfig cfg, WarnFn onWarn, BreakFn onBreak, EndFn onEnd);
  void stop();
  void snooze(int minutes); // postpone next break
  void trigger();           // start the break immediately
  [[nodiscard]] long secondsUntilBreak() const;

private:
  void run(std::stop_token tok, BreakConfig cfg, WarnFn onWarn, BreakFn onBreak, EndFn onEnd);

  std::jthread worker_;
  std::atomic<long> left_{0};
  std::atomic<long> snoozeUntil_{0}; // epoch seconds
  std::atomic<bool> fireNow_{false};
};

} // namespace nightscreen
