// PERF-07: what does the seqlock payload copy cost relative to std::memcpy?
// Measures the RT-19 helpers (relaxed atomic word/byte copies) against
// std::memcpy for the frame sizes the ADR asks about. This answers the
// "is atomic payload copy acceptable for large frames?" question; it asserts
// only correctness (byte-identical output), never throughput, so a slow CI
// box can never fail it.

#include "thyme/runtime/detail/atomic_payload.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr std::size_t kAlign = 64;

using CopyFn = void (*)(void* dst, const void* src, std::size_t n);

struct Buffer {
  Buffer() = default;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  ~Buffer() { std::free(p_); }

  bool Allocate(std::size_t bytes) {
    void* raw = nullptr;
    if (posix_memalign(&raw, kAlign, bytes) != 0) return false;
    p_ = raw;
    return true;
  }
  unsigned char* data() const {
    return static_cast<unsigned char*>(p_);
  }

 private:
  void* p_{nullptr};
};

void MemcpyCopy(void* dst, const void* src, std::size_t n) {
  std::memcpy(dst, src, n);
}

void AtomicCopyOut(void* dst, const void* src, std::size_t n) {
  thyme::detail::AtomicPayloadCopyOut(dst, src, n);
}

void AtomicCopyIn(void* dst, const void* src, std::size_t n) {
  thyme::detail::AtomicPayloadCopyIn(dst, src, n);
}

struct Result {
  double gbps{0.0};
  bool identical{false};
};

Result Measure(CopyFn fn, void* dst, const void* src, std::size_t n,
               std::size_t iters) {
  const std::size_t warmup = std::min<std::size_t>(iters, 3);
  for (std::size_t i = 0; i < warmup; ++i) {
    fn(dst, src, n);
    std::atomic_signal_fence(std::memory_order_seq_cst);
  }
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < iters; ++i) {
    fn(dst, src, n);
    std::atomic_signal_fence(std::memory_order_seq_cst);
  }
  const auto t1 = std::chrono::steady_clock::now();

  Result r;
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  if (secs > 0.0) {
    r.gbps = static_cast<double>(n) * static_cast<double>(iters) / secs / 1e9;
  }
  std::memset(dst, 0, n);
  fn(dst, src, n);
  r.identical = std::memcmp(dst, src, n) == 0;
  return r;
}

struct Case {
  const char* size_label;
  std::size_t size;
  std::size_t src_off;
  std::size_t dst_off;
  std::size_t iters;
};

struct Op {
  const char* name;
  CopyFn fn;
};

}  // namespace

int main() {
  // Iteration counts keep the whole run around a second: small sizes need
  // many calls to outweigh timer noise, 19 MiB needs only a few.
  const Case cases[] = {
      {"64 B", 64, 0, 0, 200000},
      {"4 KiB", 4 * 1024, 0, 0, 20000},
      {"64 KiB", 64 * 1024, 0, 0, 2000},
      {"1 MiB", 1024 * 1024, 0, 0, 300},
      {"19 MiB", 19 * 1024 * 1024, 0, 0, 30},
      // Unaligned source: CopyOut's atomic side falls back to byte-wise;
      // CopyIn's atomic side (dst) is still word-aligned.
      {"4 KiB (src+1)", 4 * 1024, 1, 0, 20000},
      // Unaligned destination: the CopyPayloadTo(user buffer) shape.
      {"4 KiB (dst+1)", 4 * 1024, 0, 1, 20000},
  };
  const Op ops[] = {
      {"memcpy", MemcpyCopy},
      {"AtomicPayloadCopyOut", AtomicCopyOut},
      {"AtomicPayloadCopyIn", AtomicCopyIn},
  };

  std::printf("%-16s %-24s %10s %10s\n", "size", "op", "GB/s", "vs memcpy");
  bool all_identical = true;
  for (const Case& c : cases) {
    Buffer src;
    Buffer dst;
    if (!src.Allocate(c.size + kAlign) || !dst.Allocate(c.size + kAlign)) {
      std::fprintf(stderr, "bench_atomic_payload: allocation failed\n");
      return 1;
    }
    unsigned char* s = src.data();
    for (std::size_t i = 0; i < c.size + kAlign; ++i) {
      s[i] = static_cast<unsigned char>((i * 31u + 7u) & 0xFFu);
    }
    const void* src_ptr = s + c.src_off;

    double ref_gbps = 0.0;
    for (const Op& op : ops) {
      const Result r = Measure(op.fn, dst.data() + c.dst_off, src_ptr, c.size,
                               c.iters);
      if (!r.identical) {
        std::fprintf(stderr, "bench_atomic_payload: %s mismatch at %s\n",
                     op.name, c.size_label);
        all_identical = false;
      }
      if (op.fn == MemcpyCopy) ref_gbps = r.gbps;
      const double ratio =
          (ref_gbps > 0.0) ? r.gbps / ref_gbps : 0.0;
      std::printf("%-16s %-24s %10.2f %10.2f\n", c.size_label, op.name, r.gbps,
                  ratio);
    }
  }

  if (!all_identical) return 1;
  return 0;
}
