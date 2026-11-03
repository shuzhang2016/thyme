#pragma once
// TST-07: bounded test synchronisation.
//
// A fixed sleep used as "let the other thread get there" encodes a guess
// about scheduler timing: too short and the test is racy, too long and every
// run pays for the worst case. This header replaces that class of delay with
// two explicit handshakes:
//
//   * WaitFor(pred) — wait for an observable condition (an atomic counter,
//     a registered-waiter count, a file appearing) under a monotonic
//     deadline. The short poll interval is the wait mechanism of last
//     resort, not the assertion; the condition is re-checked on every pass
//     and the deadline bounds a broken peer.
//   * EventFd — an eventfd the test thread signals when it reaches the
//     point another thread is waiting for. Wait() blocks in poll() with a
//     deadline, so there is no sleep ladder and no lost-wakeup window.
//
// Both fail closed: WaitFor returns false at the deadline and EventFd::Wait
// returns false on timeout, so a CHECK turns a stalled peer into a test
// failure instead of a hang. These helpers are for sequencing only; a test
// whose subject is a rate, a mute window or a quiet period must still use a
// real time window (documented in docs/guides/test-matrix.md, TST-07).
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <poll.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/eventfd.h>
#endif

namespace thyme_test {

inline uint64_t MonoNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

inline void SleepNs(uint64_t ns) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
  clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
}

// Re-checks `pred` until it is true or `timeout_ns` elapses. The caller owns
// the predicate; it must be safe to call concurrently with the peer that
// makes it true (typically an atomic load).
template <typename Pred>
inline bool WaitFor(Pred pred, uint64_t timeout_ns = 5000000000ULL,
                    uint64_t poll_ns = 100000ULL) {
  const uint64_t deadline = MonoNs() + timeout_ns;
  for (;;) {
    if (pred()) return true;
    const uint64_t now = MonoNs();
    if (now >= deadline) return false;
    const uint64_t remain = deadline - now;
    SleepNs(remain < poll_ns ? remain : poll_ns);
  }
}

#if defined(__linux__)
// One-shot (counter) eventfd handshake. Signal() is async-signal-safe and
// non-blocking; Wait() consumes one unit and returns false at the deadline.
class EventFd {
 public:
  EventFd() : fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}
  ~EventFd() {
    if (fd_ >= 0) ::close(fd_);
  }
  EventFd(const EventFd&) = delete;
  EventFd& operator=(const EventFd&) = delete;

  bool ok() const { return fd_ >= 0; }

  bool Signal(uint64_t count = 1) {
    if (fd_ < 0) return false;
    const uint64_t value = count;
    ssize_t n = 0;
    do {
      n = ::write(fd_, &value, sizeof(value));
    } while (n < 0 && errno == EINTR);
    return n == static_cast<ssize_t>(sizeof(value));
  }

  // Blocks in poll() until a signal arrives or `timeout_ns` elapses.
  bool Wait(uint64_t timeout_ns = 5000000000ULL) {
    if (fd_ < 0) return false;
    const uint64_t deadline = MonoNs() + timeout_ns;
    for (;;) {
      const uint64_t now = MonoNs();
      if (now >= deadline) return false;
      const uint64_t remain = deadline - now;
      const uint64_t slice_ns =
          (remain + 999999ULL) / 1000000ULL > 50
              ? 50
              : (remain + 999999ULL) / 1000000ULL;
      const int slice_ms = static_cast<int>(slice_ns);
      struct pollfd pfd {};
      pfd.fd = fd_;
      pfd.events = POLLIN;
      const int rc = ::poll(&pfd, 1, slice_ms);
      if (rc < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (rc == 0) continue;  // deadline re-check
      uint64_t value = 0;
      const ssize_t n = ::read(fd_, &value, sizeof(value));
      if (n == static_cast<ssize_t>(sizeof(value))) return true;
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && errno == EAGAIN) continue;
      return false;
    }
  }

 private:
  int fd_;
};
#endif

}  // namespace thyme_test
