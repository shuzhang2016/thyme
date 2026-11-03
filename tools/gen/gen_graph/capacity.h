#pragma once

#include <cstdint>

namespace sched_capacity {

constexpr uint64_t kNanosPerSecond = 1000000000ULL;
constexpr uint64_t kNanosPerMillisecond = 1000000ULL;

struct Requirement {
  bool known = false;
  uint64_t slots = 0;
  uint64_t suggested_power_of_two = 0;
};

Requirement ForRateAndLag(uint32_t rate_hz, uint64_t lag_ns,
                          uint64_t reserved_pins);
Requirement ForPinWindow(uint32_t rate_hz, uint64_t before_ns,
                         uint64_t after_ns, uint64_t per_frame_write_ns,
                         uint64_t pin_budget);
uint64_t NextPowerOfTwo(uint64_t value);

} // namespace sched_capacity
