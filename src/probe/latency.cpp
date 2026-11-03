#include "latency.h"

#include <algorithm>
#include <vector>

namespace thyme {
namespace probe {

namespace {

// 16 upper bounds (ns) + one overflow bucket. Chosen so sub-ms scheduling
// noise, queueing in the tens of ms, and multi-second stalls all land in
// distinct buckets.
constexpr uint64_t kUpper[16] = {
    10000ULL,        // 10 us
    25000ULL,        // 25 us
    50000ULL,        // 50 us
    100000ULL,       // 100 us
    250000ULL,       // 250 us
    500000ULL,       // 500 us
    1000000ULL,      // 1 ms
    2500000ULL,      // 2.5 ms
    5000000ULL,      // 5 ms
    10000000ULL,     // 10 ms
    25000000ULL,     // 25 ms
    50000000ULL,     // 50 ms
    100000000ULL,    // 100 ms
    250000000ULL,    // 250 ms
    500000000ULL,    // 500 ms
    1000000000ULL,   // 1 s
};

std::size_t BucketOf(uint64_t ns) {
  for (std::size_t i = 0; i < 16; ++i) {
    if (ns < kUpper[i]) return i;
  }
  return 16;
}

uint64_t Percentile(const std::vector<uint64_t>& sorted, int percent) {
  if (sorted.empty()) return 0;
  // Nearest-rank: index ceil(p/100 * N) - 1.
  const std::size_t n = sorted.size();
  std::size_t idx =
      (static_cast<std::size_t>(percent) * n + 99u) / 100u;
  if (idx == 0) idx = 1;
  if (idx > n) idx = n;
  return sorted[idx - 1];
}

}  // namespace

uint64_t Histogram::UpperNs(std::size_t index) {
  if (index < 16) return kUpper[index];
  return kNone;
}

namespace detail {

void LatencyRecorder::Add(uint64_t ns) {
  ring_[next_] = ns;
  next_ = (next_ + 1) % kCap;
  if (live_ < kCap) ++live_;
  ++total_;
  ++hist_.count[BucketOf(ns)];
}

LatencyStats LatencyRecorder::Summarize(uint64_t window_ns) const {
  LatencyStats out;
  out.window_ns = window_ns;
  out.count = total_;
  out.histogram = hist_;
  if (live_ == 0) return out;
  std::vector<uint64_t> sorted(ring_.begin(), ring_.begin() +
                                                  static_cast<std::ptrdiff_t>(
                                                      live_));
  std::sort(sorted.begin(), sorted.end());
  out.min_ns = sorted.front();
  out.max_ns = sorted.back();
  out.p50_ns = Percentile(sorted, 50);
  out.p90_ns = Percentile(sorted, 90);
  out.p99_ns = Percentile(sorted, 99);
  return out;
}

}  // namespace detail
}  // namespace probe
}  // namespace thyme
