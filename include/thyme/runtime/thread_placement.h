#pragma once
// ThreadPlacement: per-thread (= per-node) scheduling configuration shared
// by CyclicThread and EventThread (contract: docs/reference/scheduling-model.md §2).
// Split out of cyclic_thread.h (SCH-08) so sched_state.h can compare
// requested-vs-actual state without a header cycle.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace thyme {
struct ThreadPlacement {
  static constexpr std::size_t kMaxCpus = 64;
  int cpus[kMaxCpus]{};
  std::size_t ncpus = 0;
  enum class Policy { Unset, Fifo, Rr, Other };
  Policy policy = Policy::Unset;
  // Linux RT priority 1..99. gen_graph's RM policy uses 1..89; 90..99 is
  // system-reserved (docs/reference/scheduling-model.md §2).
  int priority = 10;
  enum class Fail { BestEffort, Require };
  Fail fail = Fail::BestEffort;
  // SCH-13: first cyclic wake offset inside the period. Default Off keeps
  // ThreadMain's historical `now + period` cadence. Phase is not a kernel
  // scheduling attribute and is not compared by SchedMismatchMask.
  uint64_t phase_ns = 0;
  enum class PhaseMode { Off, BestEffort, Require };
  PhaseMode phase_mode = PhaseMode::Off;
  // 0 = inherit the process epoch published by the first Start() (or now).
  uint64_t start_epoch_ns = 0;
};

// First cyclic deadline: epoch + (phase_ns % period), then the next slot
// strictly in the future when the thread starts late. Exact slot times are
// kept (no catch-up burst). period_ns == 0 returns now_ns.
inline uint64_t AlignCyclicDeadline(uint64_t epoch_ns, uint64_t phase_ns,
                                    uint64_t period_ns, uint64_t now_ns) {
  if (period_ns == 0) return now_ns;
  const uint64_t offset = phase_ns % period_ns;
  const uint64_t first = epoch_ns + offset;
  if (now_ns <= first) return first;
  const uint64_t elapsed = now_ns - first;
  const uint64_t periods = (elapsed % period_ns == 0)
                               ? (elapsed / period_ns)
                               : (elapsed / period_ns + 1);
  return first + periods * period_ns;
}

// Process-local CLOCK_MONOTONIC epoch shared by CyclicThreads whose
// start_epoch_ns is 0. First Start() publishes; later Starts keep it.
inline std::atomic<uint64_t>& SchedEpochStorage() {
  static std::atomic<uint64_t> epoch{0};
  return epoch;
}
inline void SetSchedEpochNs(uint64_t ns) {
  SchedEpochStorage().store(ns, std::memory_order_relaxed);
}
inline uint64_t GetSchedEpochNs() {
  return SchedEpochStorage().load(std::memory_order_relaxed);
}
inline bool TryPublishSchedEpochNs(uint64_t ns) {
  uint64_t cur = 0;
  return SchedEpochStorage().compare_exchange_strong(
      cur, ns, std::memory_order_relaxed, std::memory_order_relaxed);
}
}  // namespace thyme
