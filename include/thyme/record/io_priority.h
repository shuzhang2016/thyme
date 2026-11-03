#pragma once

// io_priority.h — ioprio_set wrapper for the drain path (design 6.3).
// The host's drain thread calls SetIoPriorityIdle() at startup; crash-level
// captures temporarily raise to best-effort. Failure (old kernel, missing
// CAP_SYS_ADMIN for real-time class, etc.) is logged once and never fails
// the caller: I/O priority is a best-effort QoS hint.
//
// cgroup v2 io.max is a deployment-side complement and intentionally not
// implemented here (reserved: the deployment moves the recorder into its own
// cgroup and sets io.max; see design 6.3, verification steps).

#include "thyme/runtime/log.h"

#include <cstdint>
#include <cstdio>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace thyme {
namespace io {

#if defined(__linux__)
inline constexpr int kIoprioWhoProcess = 1;
inline constexpr int kIoprioClassBestEffort = 2;
inline constexpr int kIoprioClassIdle = 3;
inline constexpr int kIoprioClassShift = 13;
#endif

// who == 0 targets the calling thread.
inline bool SetIoPriority(int cls, int data) {
#if defined(__linux__)
  const int prio = (cls << kIoprioClassShift) | data;
  if (syscall(SYS_ioprio_set, kIoprioWhoProcess, 0, prio) == 0) return true;
  THYME_LOG_WARN("io_priority: ioprio_set(class=%d) failed; continuing "
                 "without I/O priority hints",
                 cls);
  return false;
#else
  (void)cls;
  (void)data;
  return false;
#endif
}

inline bool SetIoPriorityIdle() { return SetIoPriority(kIoprioClassIdle, 0); }

// Best-effort highest level (crash-level persistence, 6.3).
inline bool SetIoPriorityBestEffort() {
  return SetIoPriority(kIoprioClassBestEffort, 0);
}

}  // namespace io
}  // namespace thyme
