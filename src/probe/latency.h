#pragma once
// Fixed-size latency/jitter collector. Bounded by construction: at most
// kCap samples are retained (and kBuckets histogram counts), so a long probe
// run never grows memory.

#include "thyme/probe/probe.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace thyme {
namespace probe {
namespace detail {

class LatencyRecorder {
 public:
  static constexpr std::size_t kCap = 4096;

  void Add(uint64_t ns);
  uint64_t count() const { return total_; }
  bool empty() const { return total_ == 0; }

  // Percentiles are exact over the retained (last kCap) samples; the
  // histogram always covers every Add since construction.
  LatencyStats Summarize(uint64_t window_ns) const;

 private:
  std::array<uint64_t, kCap> ring_{};
  std::size_t next_ = 0;
  std::size_t live_ = 0;
  uint64_t total_ = 0;
  Histogram hist_{};
};

}  // namespace detail
}  // namespace probe
}  // namespace thyme
