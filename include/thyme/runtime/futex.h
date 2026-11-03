#pragma once
#include <atomic>
#include <cstdint>
#include <ctime>
#ifdef __linux__
#include <climits>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace thyme {

// Minimal futex wrappers shared by ShmSpscRing and EventThread's wake hub.
// A futex word works both in-process and across processes (same physical
// page), which is what makes it the common wake primitive for NotifPipe
// (in-process) and shm rings (cross-process) alike. Off Linux these compile
// to no-ops; callers provide their own fallback (condvar / poll).
//
// Wakers choose the wake width explicitly (PERF-14): a single waiter can be
// woken with FutexWakeOne, a broadcast epoch with FutexWakeN(registered
// readers), and a waker that already tracks its sleepers can pass their count
// to FutexWakeN. FutexWakeN(addr, 0) is a no-op, which is the no-waiter fast
// path: the word still gets bumped by the caller, so a later FUTEX_WAIT
// snapshots the new value (EAGAIN) or re-checks its condition after register-
// then-check, and no wakeup is lost.

// Wakes at most `count` sleepers. Negative counts are treated as 0 (no
// syscall). INFINITE (INT_MAX) wakes every sleeper on the word.
inline void FutexWakeN(std::atomic<uint32_t>* addr, int count) {
#ifdef __linux__
  if (count <= 0) return;
  syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAKE, count,
          nullptr, nullptr, 0);
#else
  (void)addr;
  (void)count;
#endif
}

// True wake-one: unblocks at most one sleeper. Safe for a word with a known
// single consumer (SPSC ring, one event thread); a shared/broadcast epoch
// must use FutexWakeN/FutexWakeAll or slower waiters are stranded.
inline void FutexWakeOne(std::atomic<uint32_t>* addr) { FutexWakeN(addr, 1); }

// Wake every sleeper (thundering-herd only where the semantics require it:
// broadcast readers sharing one epoch).
inline void FutexWakeAll(std::atomic<uint32_t>* addr) {
  FutexWakeN(addr, INT_MAX);
}

// Wakes `waiters` sleepers when the caller tracked them. Callers pass the
// registration count read from their waiter counter after bumping the word.
// The count is only a hint: a waiter that registers after the count was read
// re-checks the condition after its seq_cst registration (register-then-
// check), so skipping its wake cannot lose it.
inline void FutexWakeWaiters(std::atomic<uint32_t>* addr, uint32_t waiters) {
#ifdef __linux__
  const int n = waiters > static_cast<uint32_t>(INT_MAX)
                    ? INT_MAX
                    : static_cast<int>(waiters);
  FutexWakeN(addr, n);
#else
  (void)addr;
  (void)waiters;
#endif
}

// Sleeps while *addr == val, at most `rel` (relative timeout). Returns on
// wake, timeout, EINTR, or EAGAIN (value changed before the wait); callers
// must re-check their condition in a loop. No-op off Linux.
inline void FutexWait(std::atomic<uint32_t>* addr, uint32_t val,
                      const struct timespec* rel) {
#ifdef __linux__
  syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAIT, val, rel,
          nullptr, 0);
#else
  (void)addr;
  (void)val;
  (void)rel;
#endif
}

}  // namespace thyme
