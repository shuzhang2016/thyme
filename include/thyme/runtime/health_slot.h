#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace thyme {

inline constexpr uint32_t kMemoryMlockDisabled = 1u << 0;
inline constexpr uint32_t kMemoryMlockFailed = 1u << 1;
inline constexpr uint32_t kMemoryMalloptFailed = 1u << 2;
inline constexpr uint32_t kMemoryUnsupported = 1u << 3;

inline const char* MemoryHealthStateName(uint32_t flags) {
  constexpr uint32_t kKnown = kMemoryMlockDisabled | kMemoryMlockFailed |
                              kMemoryMalloptFailed | kMemoryUnsupported;
  constexpr uint32_t kFailed = kMemoryMlockFailed | kMemoryMalloptFailed;
  if ((flags & kFailed) != 0 || (flags & ~kKnown) != 0) {
    return "degraded";
  }
  if ((flags & kMemoryUnsupported) != 0) {
    return "unsupported";
  }
  if ((flags & kMemoryMlockDisabled) != 0) {
    return "disabled";
  }
  return "ok";
}

struct alignas(64) HealthSlot {
  std::atomic<uint32_t> alive_counter{0};
  std::atomic<uint32_t> state{0};
  std::atomic<uint64_t> last_finish_mono_ns{0};
  std::atomic<uint32_t> output_sequence{0};
  std::atomic<uint32_t> deadline_miss_cnt{0};
  // RT-23: 64-bit so a drop delta above 2^32 is counted exactly instead of
  // saturating or truncating. The page version (SharedHealthPage v3) owns the
  // layout change; see docs/reference/abi-compatibility.md.
  std::atomic<uint64_t> drop_cnt{0};
  std::atomic<uint32_t> error_code{0};
  // SCH-08: requested-vs-actual scheduling mismatch count for this thread
  // (asked FIFO, got OTHER, ...). Lives in the slot padding; sizeof stays
  // one cacheline so the SharedHealthPage layout is unchanged.
  std::atomic<uint32_t> sched_mismatch_cnt{0};
  // PERF-04: process-wide App::Prepare memory hardening result, republished
  // per worker. Zero means mlockall and mallopt both succeeded.
  std::atomic<uint32_t> memory_flags{0};

  void MarkAlive(uint64_t mono_ns) {
    last_finish_mono_ns.store(mono_ns, std::memory_order_relaxed);
    alive_counter.fetch_add(1, std::memory_order_release);
  }
  void MarkDeadlineMiss() { deadline_miss_cnt.fetch_add(1, std::memory_order_relaxed); }
  // Exact 64-bit addition (RT-23). The counter is a monotone event count, so
  // plain wrap at 2^64 is the only boundary and takes ~10^11 years at 1M
  // drops/s; there is no 32-bit saturation path any more.
  void AddDrops(uint64_t n) {
    drop_cnt.fetch_add(n, std::memory_order_relaxed);
  }
  void SetError(uint32_t e) { error_code.store(e, std::memory_order_relaxed); }
  void MarkSchedMismatch() {
    sched_mismatch_cnt.fetch_add(1, std::memory_order_relaxed);
  }
  void SetMemoryFlags(uint32_t flags) {
    memory_flags.store(flags, std::memory_order_relaxed);
  }
};

// SHM slot layout, part of SharedHealthPage v3 (docs/reference/abi-compatibility.md):
// the supervisor/probe index slots by kMaxWorkers * sizeof(HealthSlot) and
// dump fields at these offsets, so a field move is a page version bump.
static_assert(sizeof(HealthSlot) == 64,
              "HealthSlot must stay one cache line");
static_assert(offsetof(HealthSlot, state) == 4,
              "health slot field order is the v3 wire layout");
static_assert(offsetof(HealthSlot, last_finish_mono_ns) == 8,
              "health slot field order is the v3 wire layout");
static_assert(offsetof(HealthSlot, output_sequence) == 16,
              "health slot field order is the v3 wire layout");
static_assert(offsetof(HealthSlot, deadline_miss_cnt) == 20,
              "health slot field order is the v3 wire layout");
static_assert(offsetof(HealthSlot, drop_cnt) == 24,
              "RT-23 drop_cnt is the naturally aligned 64-bit word at 24");
static_assert(sizeof(HealthSlot::drop_cnt) == 8,
              "RT-23 drop_cnt is 64-bit");
static_assert(offsetof(HealthSlot, error_code) == 32,
              "health slot field order is the v3 wire layout");
static_assert(offsetof(HealthSlot, sched_mismatch_cnt) == 36,
              "SCH-08 field lives in the padding; sizeof must stay 64");
static_assert(offsetof(HealthSlot, memory_flags) == 40,
              "PERF-04 field lives in the padding; sizeof must stay 64");
static_assert(alignof(HealthSlot) == 64,
              "one HealthSlot per cache line (false sharing isolation)");

}  // namespace thyme
