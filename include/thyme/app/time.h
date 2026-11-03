#pragma once
#include <cstdint>
#include <ctime>

namespace thyme {
namespace app {
// Monotonic clock in nanoseconds (CLOCK_MONOTONIC).
inline uint64_t NowMonoNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}
}  // namespace app
}  // namespace thyme
