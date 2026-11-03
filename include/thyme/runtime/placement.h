#pragma once
#include "thyme/runtime/cyclic_thread.h"
#include <pthread.h>
#include <sched.h>

namespace thyme {

// Result of ApplyThreadPlacement: which step failed under Fail::Require.
enum class PlacementResult { kOk, kAffinity, kSched };

// Applies cpu affinity and scheduling policy/priority to an already-created
// thread. Best-effort steps that fail are ignored (EPERM without privileges
// is expected); a failed step under Fail::Require is reported via the return
// value so the caller can tear the thread down. Shared by CyclicThread and
// EventThread.
inline PlacementResult ApplyThreadPlacement(pthread_t thread,
                                            const ThreadPlacement& p) {
#ifdef __linux__
  if (p.ncpus > 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    std::size_t n = p.ncpus;
    if (n > ThreadPlacement::kMaxCpus) n = ThreadPlacement::kMaxCpus;
    for (std::size_t i = 0; i < n; ++i) {
      CPU_SET(p.cpus[i], &set);
    }
    if (pthread_setaffinity_np(thread, sizeof(set), &set) != 0 &&
        p.fail == ThreadPlacement::Fail::Require) {
      return PlacementResult::kAffinity;
    }
  }
#endif

  struct sched_param sp {};
  int policy = SCHED_FIFO;
  bool require = false;
  switch (p.policy) {
    case ThreadPlacement::Policy::Unset:
#ifdef __linux__
      // Best-effort FIFO with the requested priority. Before PERF-02 this
      // hard-coded 10, which silently discarded a graph-generated RM
      // priority; the struct default is still 10, so callers that never set
      // a priority keep the previous behavior.
      sp.sched_priority = p.priority;
      if (pthread_setschedparam(thread, SCHED_FIFO, &sp) != 0) {
        /* EPERM is expected without privileges; ignore. */
      }
#endif
      return PlacementResult::kOk;
    case ThreadPlacement::Policy::Fifo:
      policy = SCHED_FIFO;
      sp.sched_priority = p.priority;
      require = (p.fail == ThreadPlacement::Fail::Require);
      break;
    case ThreadPlacement::Policy::Rr:
      policy = SCHED_RR;
      sp.sched_priority = p.priority;
      require = (p.fail == ThreadPlacement::Fail::Require);
      break;
    case ThreadPlacement::Policy::Other:
      policy = SCHED_OTHER;
      sp.sched_priority = p.priority;
#ifdef __linux__
      require = (p.fail == ThreadPlacement::Fail::Require);
#else
      require = false;
#endif
      break;
  }
  if (pthread_setschedparam(thread, policy, &sp) != 0 && require) {
    return PlacementResult::kSched;
  }
  return PlacementResult::kOk;
}

}  // namespace thyme
