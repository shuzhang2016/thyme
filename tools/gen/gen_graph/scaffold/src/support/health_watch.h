#pragma once
#include <cstdint>

// Per-worker stall-detection state machine for HealthReporterThread. Tracks
// whether a worker has been seen alive at least once and, once seen, whether
// its alive_counter has stopped advancing for more than 3 worker periods
// (3 * 1e9 / hz ns) after its last finish. Pure (no clock, no atomics) so
// tests can drive it directly.
struct HealthWatch {
  // Returns true when the reporter must SetError(1) on its own slot.
  // last_finish_ns is the caller's freshly-loaded worker last_finish value.
  bool Observe(uint32_t alive, uint64_t last_finish_ns, uint32_t worker_hz,
               uint64_t now_ns) {
    if (!seen_) {
      if (alive != 0) {
        seen_ = true;
        last_alive_ = alive;
      }
      return false;
    }
    if (alive != last_alive_) {
      last_alive_ = alive;
      return false;
    }
    if (worker_hz == 0) return false;
    const uint64_t threshold_ns = (3ULL * 1000000000ULL) / worker_hz;
    return now_ns > last_finish_ns &&
           (now_ns - last_finish_ns) > threshold_ns;
  }

  bool seen() const { return seen_; }

 private:
  bool seen_ = false;
  uint32_t last_alive_ = 0;
};
