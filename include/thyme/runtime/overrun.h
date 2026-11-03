#pragma once
// overrun.h — SCH-08: overrun reason classification.
//
// Deadline/overrun accounting today only counts (scheduling-model.md §4).
// This classifier attributes each miss to one primary, MEASURABLE reason
// category, using the dispatch-latency vs execution-time split:
//
//   - kOsPreemption: more than half the budget was already lost between the
//     wake (WaitAny return / cyclic wake) and the callback start. The thread
//     was runnable but did not get a CPU in time. This is a LOWER BOUND:
//     preemption inside the callback itself is indistinguishable from work
//     without /proc reads on the hot path (forbidden, OBS-04 H1) and lands
//     in kLongCallback.
//   - kQueueBacklog: the callback finished over budget AND inputs were still
//     pending right after the batch — the queue had accumulated more than
//     one batch could drain within the budget.
//   - kLongCallback: the callback's own execution blew the budget (the
//     residual category).
//
// One miss increments exactly one counter, so the three counters sum to the
// HealthSlot deadline-miss count.

#include <atomic>
#include <cstdint>
#include <string>

namespace thyme {

enum class OverrunReason : uint32_t {
  kLongCallback = 0,
  kQueueBacklog = 1,
  kOsPreemption = 2,
};

// SCH-04: what a worker does after a deadline miss. The reason classifier
// above is orthogonal: every miss still increments exactly one reason
// counter (SCH-08), while the policy counter records the ACTION taken.
//
//   kSkip    — count and continue at the normal cadence (existing
//              behaviour; the default). Cyclic threads keep the spiral-of-
//              death snap; event threads keep draining.
//   kDegrade — reduce the node's rate to at most half for a bounded window
//              of cycles/batches, then recover. Cyclic: the effective
//              period doubles (never more; one miss must not spiral the
//              node down). Event: a cooldown equal to the dispatch budget
//              is inserted before each batch while degraded.
//   kRestart — re-arm the node: invoke OnOverrunRestart() on the worker
//              thread and restart the cadence from now (cyclic) / continue
//              draining (event). The framework does NOT clear input rings or
//              application state; the hook owns that and runs on the RT
//              thread, so it must be non-blocking and allocation-free.
enum class OverrunPolicy : uint8_t {
  kSkip = 0,
  kDegrade = 1,
  kRestart = 2,
};

inline const char* OverrunPolicyName(OverrunPolicy p) {
  switch (p) {
    case OverrunPolicy::kSkip: return "skip";
    case OverrunPolicy::kDegrade: return "degrade";
    case OverrunPolicy::kRestart: return "restart";
  }
  return "skip";
}

inline bool ParseOverrunPolicy(const std::string& s, OverrunPolicy* out) {
  if (out == nullptr) return false;
  if (s.empty() || s == "skip") {
    *out = OverrunPolicy::kSkip;
    return true;
  }
  if (s == "degrade") {
    *out = OverrunPolicy::kDegrade;
    return true;
  }
  if (s == "restart") {
    *out = OverrunPolicy::kRestart;
    return true;
  }
  return false;
}

// Process-wide session marker (SCH-04). A node records one event per
// deadline miss — never per message — so this stays off the hot path. It is
// what a status/probe exporter or a same-process recorder host can poll to
// mark a capture/session as having run degraded. Cross-process capture
// packages are a documented follow-up: the recorder daemon would need a
// shared-memory counter (ABI change), so the package flag is not written by
// this counter today (see docs/reference/priority-inversion-audit.md).
namespace overrun_session {

// C++17 inline variables: constant-initialized before main, no function-local
// static guard on the (rare) miss path.
inline std::atomic<uint64_t> g_count{0};
inline std::atomic<uint32_t> g_last_policy{0};

inline void Note(OverrunPolicy p) {
  g_count.fetch_add(1, std::memory_order_relaxed);
  g_last_policy.store(static_cast<uint32_t>(p), std::memory_order_relaxed);
}
inline uint64_t count() { return g_count.load(std::memory_order_relaxed); }
inline OverrunPolicy last_policy() {
  return static_cast<OverrunPolicy>(
      g_last_policy.load(std::memory_order_relaxed));
}
inline void ResetForTest() {
  g_count.store(0, std::memory_order_relaxed);
  g_last_policy.store(0, std::memory_order_relaxed);
}

}  // namespace overrun_session

// exec_ns: measured callback execution (Dispatch/Loop body).
// latency_ns: wake-to-callback-start latency (0 when not measured).
// backlog_after: inputs still pending right after the over-budget batch.
// deadline_ns: the budget that was exceeded (must be > 0).
inline OverrunReason ClassifyOverrun(uint64_t exec_ns, uint64_t latency_ns,
                                     bool backlog_after, uint64_t deadline_ns) {
  if (latency_ns > deadline_ns / 2) return OverrunReason::kOsPreemption;
  if (backlog_after) return OverrunReason::kQueueBacklog;
  (void)exec_ns;  // exec > deadline is the caller's entry condition
  return OverrunReason::kLongCallback;
}

}  // namespace thyme
