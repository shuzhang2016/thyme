#pragma once

// token_bucket.h — write-rate shaping for the settle drain (design 6.3).
// Header-only, clock injected by the caller (unit-testable with a fake
// clock; the engine passes CLOCK_MONOTONIC ns).
//
// Semantics: tokens accumulate at rate_bps up to burst_bytes. TryConsume
// spends `bytes` when available. A single item larger than the burst is
// allowed once the bucket is full (borrowed against future tokens, balance
// may go negative) so one huge frame can never livelock the drain.
// rate_bps == 0 means unlimited (realtime/crash captures).

#include <cstdint>

namespace thyme {
namespace io {

class TokenBucket {
 public:
  TokenBucket() = default;
  TokenBucket(uint64_t rate_bps, uint64_t burst_bytes) {
    SetRate(rate_bps, burst_bytes);
  }

  void SetRate(uint64_t rate_bps, uint64_t burst_bytes) {
    rate_bps_ = rate_bps;
    burst_bytes_ = burst_bytes;
    tokens_ = static_cast<int64_t>(burst_bytes);
    last_ns_ = 0;
    primed_ = false;
  }

  bool unlimited() const { return rate_bps_ == 0; }

  bool TryConsume(uint64_t bytes, uint64_t now_ns) {
    if (rate_bps_ == 0) return true;
    if (!primed_) {
      primed_ = true;
      last_ns_ = now_ns;
    } else if (now_ns > last_ns_) {
      // W7: overflow-safe accrual. rate * delta overflows uint64 for a
      // 20 MB/s rate after ~15 min of pause; the bucket caps at burst anyway,
      // so clamp the delta to the top-up delta first, then divide-first.
      const uint64_t delta_cap =
          (burst_bytes_ / rate_bps_ > 1000000ull ? 1000000ull
                                                 : burst_bytes_ / rate_bps_) *
              1000000000ull +
          2000000000ull;
      uint64_t delta = now_ns - last_ns_;
      last_ns_ = now_ns;
      if (delta > delta_cap) delta = delta_cap;
      const uint64_t add = (delta / 1000000000ull) * rate_bps_ +
                           (delta % 1000000000ull) * rate_bps_ / 1000000000ull;
      tokens_ += static_cast<int64_t>(add);
      if (tokens_ > static_cast<int64_t>(burst_bytes_)) {
        tokens_ = static_cast<int64_t>(burst_bytes_);
      }
    }
    if (tokens_ >= static_cast<int64_t>(bytes)) {
      tokens_ -= static_cast<int64_t>(bytes);
      return true;
    }
    // Oversized single item: allow when the bucket is full, go negative.
    if (bytes > burst_bytes_ && tokens_ >= static_cast<int64_t>(burst_bytes_)) {
      tokens_ -= static_cast<int64_t>(bytes);
      return true;
    }
    return false;
  }

  int64_t tokens() const { return tokens_; }

 private:
  uint64_t rate_bps_ = 0;
  uint64_t burst_bytes_ = 0;
  int64_t tokens_ = 0;
  uint64_t last_ns_ = 0;
  bool primed_ = false;
};

}  // namespace io
}  // namespace thyme
