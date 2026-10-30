#include "BreakTimer.hpp"

namespace nightscreen {

void BreakTimer::start(BreakConfig cfg, WarnFn onWarn, BreakFn onBreak, EndFn onEnd) {
  stop();
  if (!cfg.enabled || cfg.workMinutes <= 0) {
    left_ = -1;
    return;
  }
  worker_ = std::jthread([this, cfg, onWarn, onBreak, onEnd](std::stop_token tok) {
    run(tok, cfg, onWarn, onBreak, onEnd);
  });
}

void BreakTimer::stop() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
  left_ = -1;
}

void BreakTimer::snooze(int minutes) {
  const auto now =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  snoozeUntil_ = now + minutes * 60L;
  left_ = static_cast<long>(minutes * 60);
}

long BreakTimer::secondsUntilBreak() const { return left_.load(); }

void BreakTimer::trigger() { fireNow_ = true; }

void BreakTimer::run(std::stop_token tok, BreakConfig cfg, WarnFn onWarn, BreakFn onBreak,
                     EndFn onEnd) {
  using namespace std::chrono;
  auto deadline = steady_clock::now() + minutes(cfg.workMinutes);
  bool warned60 = false;
  while (!tok.stop_requested()) {
    // honor snooze
    const long nowEpoch = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    if (nowEpoch < snoozeUntil_.load()) {
      deadline = steady_clock::now() + seconds(snoozeUntil_.load() - nowEpoch);
      snoozeUntil_ = 0;
      warned60 = false;
    }
    const auto left = duration_cast<seconds>(deadline - steady_clock::now()).count();
    left_ = static_cast<long>(left);
    if (left <= 0 || fireNow_.exchange(false)) break;
    if (left <= 60 && !warned60) {
      warned60 = true;
      if (onWarn) onWarn(static_cast<int>(left));
    }
    std::this_thread::sleep_for(seconds(1));
  }
  if (tok.stop_requested()) return;
  left_ = 0;
  if (onBreak) onBreak(cfg.restSeconds, cfg.strict);
  // rest countdown (interruptible only when !strict)
  auto restEnd = steady_clock::now() + seconds(cfg.restSeconds);
  while (steady_clock::now() < restEnd) {
    if (tok.stop_requested()) return;
    std::this_thread::sleep_for(milliseconds(250));
  }
  if (onEnd) onEnd();
  // restart cycle automatically
  if (!tok.stop_requested() && cfg.enabled) {
    BreakConfig next = cfg;
    start(next, onWarn, onBreak, onEnd);
  }
}

} // namespace nightscreen
