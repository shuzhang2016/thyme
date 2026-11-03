// test_histogram.cpp — OBS-09: fixed-bucket latency histogram.
//
// Covered:
//   * bucket mapping: monotonic, bounds always contain the recorded value,
//     0..15 ns exact, saturation at the top, documented sub-bucket width;
//   * known distributions -> expected nearest-rank percentiles within the
//     bucket's own bounds, exact max;
//   * concurrent Record() from several threads loses no increments;
//   * Reset() clears counts and max;
//   * zero allocation per sample (global operator new sentinel): the bucket
//     array is a fixed member, sized by kBucketBytes;
//   * hot-path cost sanity (loose ceiling, reported not asserted tightly).
#include "thyme/check.h"
#include "thyme/runtime/histogram.h"
#include "thyme/runtime/perf_log.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <pthread.h>

using namespace thyme;

namespace {

bool InBucket(uint64_t v, uint64_t expected) {
  const uint32_t b = LatencyHistogram::BucketOf(expected);
  return v >= LatencyHistogram::BucketLower(b) &&
         v <= LatencyHistogram::BucketUpper(b);
}

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

// ---------------------------------------------------------------------------
// Allocation sentinel (same technique as test_core_no_alloc): counts global
// operator new calls while armed; the histogram hot path must not allocate.
// ---------------------------------------------------------------------------
std::atomic<bool> g_armed{false};
std::atomic<uint64_t> g_allocs{0};

void Arm() {
  g_allocs.store(0, std::memory_order_relaxed);
  g_armed.store(true, std::memory_order_release);
}
uint64_t Disarm() {
  g_armed.store(false, std::memory_order_release);
  return g_allocs.load(std::memory_order_relaxed);
}
bool Armed() { return g_armed.load(std::memory_order_acquire); }

void* Allocate(std::size_t size) { return std::malloc(size == 0 ? 1 : size); }
void* AllocateAligned(std::size_t size, std::size_t alignment) {
  void* p = nullptr;
  const std::size_t align =
      alignment < sizeof(void*) ? sizeof(void*) : alignment;
  if (posix_memalign(&p, align, size == 0 ? 1 : size) != 0) return nullptr;
  return p;
}

}  // namespace

void* operator new(std::size_t size) {
  if (Armed()) g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* const p = Allocate(size);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t size) {
  if (Armed()) g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* const p = Allocate(size);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  if (Armed()) g_allocs.fetch_add(1, std::memory_order_relaxed);
  return Allocate(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  if (Armed()) g_allocs.fetch_add(1, std::memory_order_relaxed);
  return Allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
  if (Armed()) g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* const p =
      AllocateAligned(size, static_cast<std::size_t>(alignment));
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  if (Armed()) g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* const p =
      AllocateAligned(size, static_cast<std::size_t>(alignment));
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept {
  std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}

namespace {

struct WorkerArg {
  LatencyHistogram* h;
  uint64_t n;
};

void* RecordWorker(void* p) {
  WorkerArg* a = static_cast<WorkerArg*>(p);
  for (uint64_t i = 0; i < a->n; ++i) {
    a->h->Record(42 + (i & 1));  // two adjacent values
  }
  return nullptr;
}

}  // namespace

int main() {
  // --- bucket mapping ------------------------------------------------------
  {
    using H = LatencyHistogram;
    CHECK(H::kBuckets == 329);
    CHECK(H::kBucketBytes == 329 * sizeof(uint32_t));  // 1,316 B per series
    uint32_t prev = 0;
    for (uint64_t v = 0; v <= 4096; ++v) {
      const uint32_t b = H::BucketOf(v);
      CHECK(b < H::kBuckets);
      CHECK(H::BucketLower(b) <= v);
      CHECK(v <= H::BucketUpper(b));
      CHECK(b >= prev);  // monotonic in v
      prev = b;
    }
    // 0..15 ns are exact single-value buckets (also proves bucket 0 <-> 0).
    for (uint64_t v = 0; v <= 15; ++v) {
      const uint32_t b = H::BucketOf(v);
      CHECK(H::BucketLower(b) == v);
      CHECK(H::BucketUpper(b) == v);
    }
    // Sub-buckets are at most 1/8 of an octave wide (12.5% at the low end);
    // the midpoint estimate never errs by more than half of that.
    for (uint32_t b = 1; b < H::kBuckets; ++b) {
      const uint64_t lo = H::BucketLower(b);
      const uint64_t hi = H::BucketUpper(b);
      CHECK(hi >= lo);
      if (lo >= 16) CHECK(hi * 1000 <= lo * 1125);
      const uint64_t mid = H::BucketMid(b);
      CHECK(mid >= lo && mid <= hi);
      CHECK((mid - lo) * 100 <= lo * 7);   // <= ~6.25% below
      CHECK((hi - mid) * 100 <= hi * 7);   // <= ~5.6% above
    }
    // Above the supported range everything saturates into the last bucket.
    CHECK(H::BucketOf(1ull << 41) == H::kBuckets - 1);
    CHECK(H::BucketOf(~0ull) == H::kBuckets - 1);
  }

  // --- known distribution -> expected percentiles --------------------------
  {
    LatencyHistogram h;
    // 9000 x 1us, 900 x 10us, 99 x 100us, 1 x 1ms.
    for (int i = 0; i < 9000; ++i) h.Record(1000);
    for (int i = 0; i < 900; ++i) h.Record(10000);
    for (int i = 0; i < 99; ++i) h.Record(100000);
    h.Record(1000000);
    CHECK(h.count() == 10000);
    const LatencyHistogram::Snapshot s = h.Compute();
    CHECK(s.count == 10000);
    CHECK(s.max == 1000000);  // exact
    // Nearest-rank: p50 -> 1us, p90 -> 1us, p99 -> 10us, p999 -> 100us.
    CHECK(InBucket(s.p50, 1000));
    CHECK(InBucket(s.p90, 1000));
    CHECK(InBucket(s.p99, 10000));
    CHECK(InBucket(s.p999, 100000));
    // Uniform 1..10000 ns: p50 lands within the bucket of the median value.
    LatencyHistogram u;
    for (uint64_t v = 1; v <= 10000; ++v) u.Record(v);
    const LatencyHistogram::Snapshot us = u.Compute();
    CHECK(InBucket(us.p50, 5000));
    CHECK(us.max == 10000);
    // Zero samples are their own bucket and report 0 exactly.
    LatencyHistogram z;
    for (int i = 0; i < 10; ++i) z.Record(0);
    for (int i = 0; i < 10; ++i) z.Record(3);
    const LatencyHistogram::Snapshot zs = z.Compute();
    CHECK(zs.p50 == 0);
    CHECK(zs.p90 == 3);
    CHECK(zs.max == 3);
  }

  // --- p999 keeps a single-sample tail ------------------------------------
  {
    LatencyHistogram h;
    for (int i = 0; i < 1000; ++i) h.Record(100);
    h.Record(1000000000ull);
    const LatencyHistogram::Snapshot s = h.Compute();
    CHECK(s.count == 1001);
    CHECK(InBucket(s.p999, 100));  // rank 1000 of 1001 is still the 100 ns run
    CHECK(s.max == 1000000000ull);
  }

  // --- concurrent Record(): no lost increments ----------------------------
  {
    LatencyHistogram h;
    constexpr int kThreads = 4;
    constexpr uint64_t kPer = 50000;
    pthread_t t[kThreads];
    WorkerArg arg[2] = {{&h, kPer}, {&h, kPer}};
    WorkerArg args[kThreads];
    for (int i = 0; i < kThreads; ++i) args[i] = arg[i % 2];
    for (int i = 0; i < kThreads; ++i) {
      CHECK(pthread_create(&t[i], nullptr, &RecordWorker, &args[i]) == 0);
    }
    for (int i = 0; i < kThreads; ++i) CHECK(pthread_join(t[i], nullptr) == 0);
    const LatencyHistogram::Snapshot s = h.Compute();
    CHECK(s.count == kThreads * kPer);
    CHECK(s.max == 43);
    CHECK(InBucket(s.p50, 42) || InBucket(s.p50, 43));
  }

  // --- Reset clears counts and max ----------------------------------------
  {
    LatencyHistogram h;
    h.Record(1);
    h.Record(5000000);
    h.Reset();
    CHECK(h.count() == 0);
    CHECK(h.max() == 0);
    const LatencyHistogram::Snapshot s = h.Compute();
    CHECK(s.count == 0 && s.max == 0 && s.p50 == 0 && s.p999 == 0);
  }

  // --- zero allocation after init ------------------------------------------
  {
    LatencyHistogram h;
    PerfSampler ps;  // ring allocated here, before arming
    Arm();
    for (int i = 0; i < 100000; ++i) {
      h.Record(static_cast<uint64_t>(i & 0xFFFF));
      ps.Note(static_cast<uint64_t>(i) << 14, static_cast<uint64_t>(i & 0xFFF));
    }
    const uint64_t allocs = Disarm();
    CHECK(allocs == 0);
    CHECK(h.count() == 100000);
    CHECK(ps.histogram().count() == 100000);
  }

  // --- hot-path cost sanity (reported, loosely bounded) --------------------
  {
    LatencyHistogram h;
    constexpr uint64_t kN = 1000000;
    uint64_t sink = 0;
    const uint64_t t0 = NowNs();
    for (uint64_t i = 0; i < kN; ++i) {
      h.Record(1000 + (i & 0x3FF));
    }
    const uint64_t t1 = NowNs();
    sink = h.max();
    std::printf("histogram Record: %.2f ns/op (%llu samples)\n",
                static_cast<double>(t1 - t0) / static_cast<double>(kN),
                static_cast<unsigned long long>(kN));
    CHECK(sink == 2023);
    // Very loose ceiling: this is a sanity check, not a benchmark. Sanitizer
    // builds are slower, hence the generous bound.
    CHECK((t1 - t0) / kN < 2000);
  }
  return 0;
}
