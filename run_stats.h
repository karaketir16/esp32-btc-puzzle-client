#pragma once
#include <cstdint>
namespace btc {
struct RunStats {
  uint64_t attempts = 0;
  uint64_t elapsedMs = 0;
  uint32_t lastTick = 0;
  bool running = false;
  void tick(uint32_t now) {
    if (running)
      elapsedMs += uint32_t(now - lastTick);
    lastTick = now;
  }
  void start(uint32_t now) {
    tick(now);
    running = true;
  }
  void stop(uint32_t now) {
    tick(now);
    running = false;
  }
  void reset(uint32_t now) {
    attempts = elapsedMs = 0;
    lastTick = now;
    running = false;
  }
  double rate() const { return elapsedMs ? double(attempts) * 1000.0 / double(elapsedMs) : 0.0; }
};
} // namespace btc
