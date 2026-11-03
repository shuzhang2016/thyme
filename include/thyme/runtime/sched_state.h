#pragma once
// sched_state.h — SCH-08: actual (not requested) scheduling state of a
// runtime thread, and requested-vs-actual mismatch detection.
//
// ApplyThreadPlacement (placement.h) is best-effort by default: asking for
// SCHED_FIFO without CAP_SYS_NICE fails with EPERM and the thread silently
// stays on SCHED_OTHER. Requested config therefore says nothing about what
// the kernel actually scheduled. This component reads the ACTUAL
// policy/priority/affinity once at thread startup (pthread_getschedparam /
// pthread_getaffinity_np — cheap, no /proc parsing) and compares it against
// the requested ThreadPlacement so a silently-degraded real-time thread
// shows up as a mismatch flag instead of a missed-deadline mystery.
//
// Contract: read-once at startup (Start path), never on the loop/dispatch
// hot path. Non-Linux platforms report valid=false and never mismatch.

#include "thyme/runtime/thread_placement.h"

#include <cstdint>
#include <pthread.h>

#if defined(__linux__)
#include <sched.h>
#endif

namespace thyme {

// Actual scheduling state of one thread. cpu_mask is a bitset of up to 128
// CPUs (lo = cpus 0..63, hi = 64..127); ncpu counts the set bits.
struct SchedState {
  int policy = -1;    // SCHED_OTHER / SCHED_FIFO / SCHED_RR / ... (-1 unknown)
  int priority = -1;  // sched_param.sched_priority (-1 unknown)
  uint64_t cpu_mask_lo = 0;
  uint64_t cpu_mask_hi = 0;
  uint32_t ncpu = 0;
  bool valid = false;

  bool CpuSet(int cpu) const {
    if (cpu < 0 || cpu >= 128) return false;
    const uint64_t bit = 1ull << (cpu & 63);
    return (cpu < 64 ? cpu_mask_lo : cpu_mask_hi) & bit;
  }
};

// Mismatch bits between the requested placement and the actual state.
enum SchedMismatch : uint32_t {
  kSchedMismatchNone = 0,
  kSchedMismatchPolicy = 1u << 0,
  kSchedMismatchPriority = 1u << 1,
  kSchedMismatchAffinity = 1u << 2,
};

// Reads the actual scheduling state of `thread`. False (and out->valid
// stays false) when the platform cannot report it.
inline bool ReadSchedState(pthread_t thread, SchedState* out) {
  if (out == nullptr) return false;
  *out = SchedState{};
#if defined(__linux__)
  int policy = 0;
  struct sched_param sp {};
  if (pthread_getschedparam(thread, &policy, &sp) != 0) return false;
  out->policy = policy;
  out->priority = sp.sched_priority;
  cpu_set_t set;
  CPU_ZERO(&set);
  if (pthread_getaffinity_np(thread, sizeof(set), &set) != 0) return false;
  const int ncpu = CPU_COUNT(&set);
  for (int cpu = 0; cpu < CPU_SETSIZE && cpu < 128; ++cpu) {
    if (CPU_ISSET(cpu, &set)) {
      if (cpu < 64) {
        out->cpu_mask_lo |= 1ull << cpu;
      } else {
        out->cpu_mask_hi |= 1ull << (cpu - 64);
      }
    }
  }
  out->ncpu = static_cast<uint32_t>(ncpu > 128 ? 128 : ncpu);
  out->valid = true;
  return true;
#else
  (void)thread;
  return false;
#endif
}

// Requested-vs-actual comparison. Only explicit requests are compared:
// Policy::Unset is the documented best-effort "try FIFO, ignore failure"
// path (scheduling-model.md §2), so an unprivileged OTHER thread is the
// expected outcome there, not a warning. For SCHED_OTHER the kernel pins
// sched_priority to 0, so the priority bit is only meaningful for the
// real-time policies. Affinity is compared only when the request named
// CPUs. Returns kSchedMismatchNone when the actual state is unreadable
// (nothing to compare against).
inline uint32_t SchedMismatchMask(const ThreadPlacement& requested,
                                  const SchedState& actual) {
  if (!actual.valid) return kSchedMismatchNone;
  uint32_t mask = kSchedMismatchNone;
#if defined(__linux__)
  int want_policy = -1;
  switch (requested.policy) {
    case ThreadPlacement::Policy::Fifo: want_policy = SCHED_FIFO; break;
    case ThreadPlacement::Policy::Rr: want_policy = SCHED_RR; break;
    case ThreadPlacement::Policy::Other: want_policy = SCHED_OTHER; break;
    case ThreadPlacement::Policy::Unset: break;
  }
  if (want_policy >= 0) {
    if (actual.policy != want_policy) {
      mask |= kSchedMismatchPolicy;
    } else if (want_policy == SCHED_FIFO || want_policy == SCHED_RR) {
      if (actual.priority != requested.priority) {
        mask |= kSchedMismatchPriority;
      }
    }
  }
  if (requested.ncpus > 0) {
    std::size_t n = requested.ncpus;
    if (n > ThreadPlacement::kMaxCpus) n = ThreadPlacement::kMaxCpus;
    for (std::size_t i = 0; i < n; ++i) {
      if (!actual.CpuSet(requested.cpus[i])) {
        mask |= kSchedMismatchAffinity;
        break;
      }
    }
  }
#else
  (void)requested;
  (void)actual;
#endif
  return mask;
}

}  // namespace thyme
