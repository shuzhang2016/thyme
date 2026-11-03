// test_rate_limit.cpp — token-bucket drain shaping math (design 6.3) with an
// injected clock: initial burst, refill rate, burst cap, oversized-item
// borrow, and rate 0 = unlimited (realtime/crash lifts the rate limit).
#include "thyme/check.h"
#include "thyme/record/token_bucket.h"

#include <cstdint>

using namespace thyme;

int main() {
  constexpr uint64_t kSec = 1000000000ULL;

  // Initial burst is full; refill accrues at rate; cap at burst.
  {
    io::TokenBucket b(1000, 2000);  // 1000 B/s, 2000 B burst
    CHECK(b.TryConsume(2000, 0));   // full burst available at once
    CHECK(!b.TryConsume(1, 0));     // empty now
    // after 0.5 s: 500 tokens — 600 is too much, 500 exactly fits
    CHECK(!b.TryConsume(600, kSec / 2));
    CHECK(b.TryConsume(500, kSec / 2));
    CHECK(b.tokens() == 0);
    // Cap: 10 s of accrual must not exceed burst (2001 would be an oversized
    // borrow — allowed by design when full — so assert the cap via balance).
    CHECK(b.TryConsume(2000, 11 * kSec));
    CHECK(b.tokens() == 0);
    CHECK(!b.TryConsume(1, 11 * kSec));
  }

  // Oversized single item (larger than burst): allowed once the bucket is
  // full, borrowing into a negative balance that recovers over time.
  {
    io::TokenBucket b(1000, 2000);
    CHECK(b.TryConsume(5000, 0));    // burst full -> borrow, tokens = -3000
    CHECK(b.tokens() == -3000);
    CHECK(!b.TryConsume(1, 2 * kSec));   // -1000: still negative
    CHECK(!b.TryConsume(1, 3 * kSec));   // exactly 0: not enough for 1
    CHECK(b.TryConsume(1, 3 * kSec + 1000000));  // +1 ms -> 1 token
  }

  // rate 0 = unlimited (crash level/realtime lifts the rate limit).
  {
    io::TokenBucket b(0, 0);
    CHECK(b.unlimited());
    CHECK(b.TryConsume(1ull << 40, 0));
    CHECK(b.TryConsume(1ull << 40, 123 * kSec));
  }

  // Re-arming a bucket (per capture) resets tokens to the burst.
  {
    io::TokenBucket b(1000, 100);
    CHECK(b.TryConsume(100, 0));
    CHECK(!b.TryConsume(100, 0));
    b.SetRate(1000, 100);
    CHECK(b.TryConsume(100, 0));
  }

  // W7: a one-hour pause must not overflow the accrual — the bucket just tops
  // up to burst.
  {
    io::TokenBucket b(20000000, 8000000);  // 20 MB/s, 8 MB burst
    CHECK(b.TryConsume(8000000, 0));       // drain
    CHECK(b.TryConsume(8000000, 3600ull * 1000000000ull));  // +1 h: full again
    CHECK(b.tokens() == 0);
    // huge rate + multi-second delta: still capped at burst, no wrap.
    io::TokenBucket fast(1000000000000ull, 4000);  // 1 TB/s
    CHECK(fast.TryConsume(4000, 0));
    CHECK(fast.TryConsume(4000, 10000000000ull));  // +10 s
    CHECK(fast.tokens() == 0);
    CHECK(!fast.TryConsume(1, 10000000000ull));    // burst exhausted exactly
    // tiny rate: 60 s pause accrues 60 B, not the burst.
    io::TokenBucket slow(1, 1000000);  // 1 B/s
    CHECK(slow.TryConsume(1000000, 0));
    CHECK(!slow.TryConsume(61, 60000000000ull));
    CHECK(slow.TryConsume(60, 60000000000ull));
  }
  return 0;
}
