#pragma once
// OBS-09: fixed-bucket latency histogram (p50/p90/p99/p999 + exact max).
//
// Bucket layout is log-linear ("HDR-style"): 8 sub-buckets per octave.
//   bucket(v) = 0                              for v == 0
//   bucket(v) = 1 + e * 8 + sub                for v >= 1
//     e   = floor(log2(v))
//     e >= 3: sub = top 3 mantissa bits below the leading 1
//     e <  3: sub = v - 2^e  (enumerates every value, so 0..15 are exact)
//   v > 2^41 - 1 saturates into the last bucket; max stays exact regardless.
//
// Resolution: for v >= 16 ns a bucket is a linear sub-bucket of the octave
// (1/8 of it), so the first sub-bucket of an octave spans [lo, 1.125*lo) and
// the later ones are narrower. Reported percentiles are the integer midpoint
// of the bucket holding the nearest-rank sample, so the honest worst-case
// relative error is +6.25% / -5.56% (half a sub-bucket at the low end of an
// octave); 0..15 ns report exactly. `max` is the observed maximum in ns
// (u64), not a bucket boundary. Merging histograms bucket-wise is valid for
// percentiles (proportions add); max is merged with max.
//
// Cost: one relaxed atomic increment per sample (~a few ns) plus a relaxed
// max update only when a new maximum appears. No allocation, no locks, no
// dynamic memory: the bucket array is a fixed member of 1,316 B (329 x u32)
// plus an 8 B max, i.e. 1,332 B per series for the object's lifetime. The
// class is safe for concurrent Record() from multiple threads; Reset() is a
// dump-path operation and must not race with Record() (the same dump-then-
// reset contract as PerfSampler, so a sample landing in the reset window may
// be lost from either window).
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace thyme {

class LatencyHistogram {
 public:
  static constexpr uint32_t kSubBits = 3;
  static constexpr uint32_t kSubBuckets = 1u << kSubBits;  // 8
  static constexpr uint32_t kMaxExp = 40;                  // up to 2^41-1 ns
  static constexpr uint32_t kBuckets = 1 + (kMaxExp + 1) * kSubBuckets;
  static constexpr std::size_t kBucketBytes = kBuckets * sizeof(uint32_t);

  struct Snapshot {
    uint64_t count = 0;
    uint64_t max = 0;
    uint64_t p50 = 0;
    uint64_t p90 = 0;
    uint64_t p99 = 0;
    uint64_t p999 = 0;
  };

  LatencyHistogram() : buckets_{} { Reset(); }
  LatencyHistogram(const LatencyHistogram&) = delete;
  LatencyHistogram& operator=(const LatencyHistogram&) = delete;

  // Hot path: one relaxed atomic increment (plus a compare-load for max).
  void Record(uint64_t v) {
    buckets_[BucketOf(v)].fetch_add(1, std::memory_order_relaxed);
    uint64_t cur = max_.load(std::memory_order_relaxed);
    while (v > cur) {
      if (max_.compare_exchange_weak(cur, v, std::memory_order_relaxed,
                                     std::memory_order_relaxed)) {
        break;
      }
    }
  }

  // Dump-path reset (dump-then-reset window boundary; see header note).
  void Reset() {
    for (auto& b : buckets_) {
      b.store(0, std::memory_order_relaxed);
    }
    max_.store(0, std::memory_order_relaxed);
  }

  uint64_t count() const {
    uint64_t n = 0;
    for (const auto& b : buckets_) {
      n += b.load(std::memory_order_relaxed);
    }
    return n;
  }

  uint64_t max() const { return max_.load(std::memory_order_relaxed); }

  uint32_t bucket_count(uint32_t idx) const {
    return buckets_[idx].load(std::memory_order_relaxed);
  }

  Snapshot Compute() const {
    Snapshot s;
    uint32_t counts[kBuckets];
    uint64_t total = 0;
    for (uint32_t i = 0; i < kBuckets; ++i) {
      counts[i] = buckets_[i].load(std::memory_order_relaxed);
      total += counts[i];
    }
    s.count = total;
    s.max = max_.load(std::memory_order_relaxed);
    if (total == 0) return s;
    s.p50 = Percentile(counts, total, 50, 100);
    s.p90 = Percentile(counts, total, 90, 100);
    s.p99 = Percentile(counts, total, 99, 100);
    s.p999 = Percentile(counts, total, 999, 1000);
    return s;
  }

  // Nearest-rank percentile from a bucket-count array (bucket 0 = exact
  // zero). Returns the midpoint of the bucket that holds the rank. Shared
  // with the offline analyzer so on-disk and merged numbers use one rule.
  template <typename CountT>
  static uint64_t Percentile(const CountT* counts, uint64_t total,
                             uint32_t num, uint32_t den) {
    if (total == 0 || den == 0) return 0;
    uint64_t rank =
        (total * static_cast<uint64_t>(num) + den - 1) / static_cast<uint64_t>(den);
    if (rank == 0) rank = 1;
    uint64_t acc = 0;
    for (uint32_t i = 0; i < kBuckets; ++i) {
      acc += static_cast<uint64_t>(counts[i]);
      if (acc >= rank) return BucketMid(i);
    }
    return BucketMid(kBuckets - 1);
  }

  static uint32_t BucketOf(uint64_t v) {
    if (v == 0) return 0;
    const uint32_t e = FloorLog2(v);
    if (e > kMaxExp) return kBuckets - 1;
    uint32_t sub = 0;
    if (e >= kSubBits) {
      sub = static_cast<uint32_t>((v >> (e - kSubBits)) & (kSubBuckets - 1));
    } else {
      sub = static_cast<uint32_t>(v - (1ull << e));
    }
    return 1 + e * kSubBuckets + sub;
  }

  static uint64_t BucketLower(uint32_t idx) {
    if (idx == 0) return 0;
    const uint32_t d = idx - 1;
    const uint32_t e = d / kSubBuckets;
    const uint32_t sub = d % kSubBuckets;
    if (e < kSubBits) return (1ull << e) + sub;
    return (static_cast<uint64_t>(kSubBuckets) + sub) << (e - kSubBits);
  }

  static uint64_t BucketUpper(uint32_t idx) {
    if (idx == 0) return 0;
    const uint32_t d = idx - 1;
    const uint32_t e = d / kSubBuckets;
    const uint32_t sub = d % kSubBuckets;
    if (e < kSubBits) return (1ull << e) + sub;
    return ((static_cast<uint64_t>(kSubBuckets) + sub + 1)
            << (e - kSubBits)) - 1;
  }

  static uint64_t BucketMid(uint32_t idx) {
    const uint64_t lo = BucketLower(idx);
    return lo + (BucketUpper(idx) - lo) / 2;
  }

 private:
  static uint32_t FloorLog2(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return 63u - static_cast<uint32_t>(__builtin_clzll(v));
#else
    uint32_t e = 0;
    while ((v >>= 1) != 0) ++e;
    return e;
#endif
  }

  std::atomic<uint32_t> buckets_[kBuckets];
  std::atomic<uint64_t> max_{0};
};

}  // namespace thyme
