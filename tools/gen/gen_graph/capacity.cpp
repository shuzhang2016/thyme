#include "capacity.h"

#include <limits>

namespace sched_capacity {
namespace {

uint64_t CeilProduct(uint64_t lhs, uint64_t rhs, uint64_t divisor,
                     bool *known) {
  if (lhs == 0 || rhs == 0) {
    return 0;
  }
  if (lhs > std::numeric_limits<uint64_t>::max() / rhs) {
    *known = false;
    return 0;
  }
  const uint64_t product = lhs * rhs;
  return product / divisor + (product % divisor == 0 ? 0 : 1);
}

} // namespace

uint64_t NextPowerOfTwo(uint64_t value) {
  if (value <= 1) {
    return 1;
  }
  constexpr uint64_t kHighest = uint64_t{1} << 63;
  if (value > kHighest) {
    return 0;
  }
  uint64_t result = 1;
  while (result < value) {
    result <<= 1;
  }
  return result;
}

Requirement ForRateAndLag(uint32_t rate_hz, uint64_t lag_ns,
                          uint64_t reserved_pins) {
  Requirement out;
  out.known = rate_hz > 0 && lag_ns > 0;
  if (!out.known) {
    return out;
  }
  const uint64_t rate_slots =
      CeilProduct(rate_hz, lag_ns, kNanosPerSecond, &out.known);
  if (!out.known ||
      rate_slots > std::numeric_limits<uint64_t>::max() - reserved_pins) {
    out.known = false;
    return out;
  }
  out.slots = rate_slots + reserved_pins;
  out.suggested_power_of_two = NextPowerOfTwo(out.slots);
  if (out.suggested_power_of_two == 0) {
    out.known = false;
  }
  return out;
}

Requirement ForPinWindow(uint32_t rate_hz, uint64_t before_ns,
                         uint64_t after_ns, uint64_t per_frame_write_ns,
                         uint64_t pin_budget) {
  Requirement out;
  if (before_ns > std::numeric_limits<uint64_t>::max() - after_ns) {
    return out;
  }
  const uint64_t retention_ns = before_ns + after_ns;
  if (retention_ns >
      std::numeric_limits<uint64_t>::max() - per_frame_write_ns) {
    return out;
  }
  return ForRateAndLag(rate_hz, retention_ns + per_frame_write_ns, pin_budget);
}

} // namespace sched_capacity
