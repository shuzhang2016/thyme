// PERF-14: wake-path syscall budget.
//
// Proves the wake path issues at most one futex wake per publish and no
// syscall at all when nobody is registered as a sleeper:
//   * ShmSpscRing::Push with no epoch waiter: 0 FUTEX_WAKE for N pushes, yet
//     the epoch still advances and a later WaitUntil sees the data.
//   * EventThread's WakeHub with no blocked worker: 0 FUTEX_WAKE.
//   * One push with exactly one blocked worker: exactly 1 FUTEX_WAKE.
//   * ShmSpscRing.Push with a same-process hub target that bumps the shared
//     wake word: exactly 1 FUTEX_WAKE and one word bump (the ring's direct
//     wake-word path is coalesced into the hub's).
//   * ShmSpmcRing: one bump per Push (the reader self-notify recursion must
//     not re-bump) and one wake syscall for all registered readers, none
//     when no reader is sleeping.
//
// Syscall counts are hermetic: the scenario runs in a forked child under
// ptrace(PTRACE_SYSCALL), and the child brackets the measured window with
// writes to a marker pipe. No external strace/perf is needed. If ptrace is
// unavailable (sandbox), the functional assertions still run in the child
// and only the count assertion is reported as skipped.
#include "thyme/check.h"
#include "thyme/runtime/event_thread.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <thread>

#ifdef __linux__
#include <csignal>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace thyme;

namespace {

#ifdef __linux__

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

// Polls a predicate with a bounded deadline; used to wait for a worker to
// register as a futex sleeper before measuring, so the measured window holds
// no sleeps and no reliance on scheduler timing.
template <typename Pred>
bool WaitFor(Pred pred, uint64_t timeout_ns = 2000000000ULL) {
  const uint64_t deadline = NowNs() + timeout_ns;
  while (!pred()) {
    if (NowNs() >= deadline) return false;
    struct timespec ts {0, 1000000};  // 1ms
    nanosleep(&ts, nullptr);
  }
  return true;
}

class PipeDrain final : public EventThread {
 public:
  explicit PipeDrain(HealthSlot* health) : EventThread("wake_pipe", health) {}
  NotifPipe<uint32_t, 32>* q = nullptr;
  std::atomic<uint64_t> msgs{0};

 protected:
  void Dispatch() override {
    uint32_t v = 0;
    while (q->TryPop(v)) {
      NoteHandled();
      msgs.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

template <std::size_t Cap>
class RingDrain final : public EventThread {
 public:
  explicit RingDrain(HealthSlot* health) : EventThread("wake_ring", health) {}
  ShmSpscRing<uint64_t, Cap>* q = nullptr;
  std::atomic<uint64_t> msgs{0};

 protected:
  void Dispatch() override {
    uint64_t v = 0;
    while (q->TryPop(v)) {
      NoteHandled();
      msgs.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

// Kernel ABI of struct ptrace_syscall_info (linux/ptrace.h) without pulling
// that header in: glibc's <sys/ptrace.h> already provides the request enum
// and PTRACE_SYSCALL_INFO_ENTRY, while including both headers redefines the
// PTRACE_* macros as plain ints. Only entry.nr/entry.args are read here.
struct WakeSyscallInfo {
  uint8_t op;
  uint8_t pad[3];
  uint32_t arch;
  uint64_t instruction_pointer;
  uint64_t stack_pointer;
  union {
    struct {
      uint64_t nr;
      uint64_t args[6];
    } entry;
    struct {
      int64_t rval;
      uint8_t is_error;
    } exit_info;
    struct {
      uint64_t nr;
      uint64_t args[6];
      uint32_t ret_data;
    } seccomp;
  };
};

struct WakeTrace {
  bool supported = false;
  int futex_wakes = 0;
  int child_exit = -1;  // exit status, or -signal when the child died
};

void MarkerWrite(int fd, char c) {
  // The tracer toggles its counting window on this write's syscall entry.
  if (::write(fd, &c, 1) != 1) _exit(74);
}

// Runs `body(marker_fd)` in a forked child under PTRACE_SYSCALL and returns
// the number of FUTEX_WAKE syscalls entered while the child's marker window
// is open. The child must write 'S' to open and 'E' to close the window.
template <typename Body>
WakeTrace TraceWakeSyscalls(Body body) {
  WakeTrace trace;
  int marker[2];
  if (pipe(marker) != 0) return trace;
  const pid_t pid = fork();
  if (pid < 0) {
    close(marker[0]);
    close(marker[1]);
    return trace;
  }
  if (pid == 0) {
    close(marker[0]);
    // If tracing is unavailable the body still runs (functional checks) and
    // the parent just cannot count syscalls.
    if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) == 0) {
      raise(SIGSTOP);  // wait for the tracer to attach
    }
    body(marker[1]);
    _exit(0);
  }
  close(marker[1]);
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) {
    close(marker[0]);
    return trace;
  }
  if (!WIFSTOPPED(st) || WSTOPSIG(st) != SIGSTOP) {
    if (WIFEXITED(st)) trace.child_exit = WEXITSTATUS(st);
    if (WIFSIGNALED(st)) trace.child_exit = -WTERMSIG(st);
    close(marker[0]);
    return trace;
  }
  const void* options = reinterpret_cast<const void*>(
      static_cast<uintptr_t>(PTRACE_O_TRACESYSGOOD));
  if (ptrace(PTRACE_SETOPTIONS, pid, nullptr, options) != 0) {
    // ptrace is blocked (sandbox): detach and let the child finish untraced.
    ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
    waitpid(pid, &st, 0);
    if (WIFEXITED(st)) trace.child_exit = WEXITSTATUS(st);
    if (WIFSIGNALED(st)) trace.child_exit = -WTERMSIG(st);
    close(marker[0]);
    return trace;
  }
  bool counting = false;
  bool ok = true;
  for (;;) {
    if (ptrace(PTRACE_SYSCALL, pid, nullptr, nullptr) != 0) {
      ok = false;
      break;
    }
    if (waitpid(pid, &st, 0) < 0) {
      ok = false;
      break;
    }
    if (WIFEXITED(st)) {
      trace.child_exit = WEXITSTATUS(st);
      break;
    }
    if (WIFSIGNALED(st)) {
      trace.child_exit = -WTERMSIG(st);
      break;
    }
    if (!WIFSTOPPED(st)) continue;
    const int sig = WSTOPSIG(st);
    if (sig != (SIGTRAP | 0x80)) continue;  // not a syscall-stop
    WakeSyscallInfo info {};
    const long n = ptrace(PTRACE_GET_SYSCALL_INFO, pid,
                          reinterpret_cast<void*>(sizeof(info)),
                          static_cast<void*>(&info));
    if (n <= 0) {
      ok = false;
      break;
    }
    if (info.op != static_cast<uint8_t>(PTRACE_SYSCALL_INFO_ENTRY)) continue;
    const uint64_t nr = info.entry.nr;
    if (nr == static_cast<uint64_t>(SYS_futex)) {
      if (!counting) continue;
      const long op = static_cast<long>(info.entry.args[1]);
      if ((op & FUTEX_CMD_MASK) == FUTEX_WAKE) ++trace.futex_wakes;
    } else if (nr == static_cast<uint64_t>(SYS_write) &&
               info.entry.args[0] == static_cast<uint64_t>(marker[1])) {
      counting = !counting;
    }
  }
  if (!ok) {
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
  }
  close(marker[0]);
  trace.supported = ok;
  return trace;
}

// Enforces a syscall budget. ptrace-unavailable environments keep the
// functional behavior checks but skip the count, so the suite stays green in
// restricted sandboxes while unrestricted CI gets the hard assertion.
void ExpectWakes(const WakeTrace& trace, int want, const char* what) {
  CHECK(trace.child_exit == 0);
  if (!trace.supported) {
    std::fprintf(stderr, "PERF-14: ptrace unavailable, count skipped (%s)\n",
                 what);
    return;
  }
  std::fprintf(stderr, "PERF-14: %-44s FUTEX_WAKE=%d (budget %d)\n", what,
               trace.futex_wakes, want);
  CHECK(trace.futex_wakes == want);
}

#endif  // __linux__

}  // namespace

#ifdef __linux__
int main() {
  // 1. No epoch waiter: pushes must not issue a futex wake, but the epoch
  //    still advances so the pending publish is observable later.
  const WakeTrace no_waiter = TraceWakeSyscalls([](int marker) {
    char name[64];
    std::snprintf(name, sizeof(name), "/c9p14a_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(name);
    ShmSpscRing<uint64_t, 256> producer;
    ShmSpscRing<uint64_t, 256> consumer;
    CHECK(producer.Create(name));
    CHECK(consumer.Open(name, 1000000000ULL));
    const uint32_t epoch0 = producer.epoch();
    MarkerWrite(marker, 'S');
    for (uint64_t i = 0; i < 128; ++i) CHECK(producer.Push(i));
    MarkerWrite(marker, 'E');
    CHECK(producer.epoch() - epoch0 == 128u);
    CHECK(consumer.epoch_waiters() == 0u);
    const uint64_t t0 = NowNs();
    consumer.WaitUntil(t0 + 2000000000ULL);
    CHECK(NowNs() - t0 < 500000000ULL);  // data was visible, no timeout
    uint64_t v = 0;
    CHECK(consumer.TryPop(v) && v == 0);
    producer.Close();
    consumer.Close();
    ShmRegion::UnlinkName(name);
  });
  ExpectWakes(no_waiter, 0, "ring push without waiter");

  // 2. EventThread hub with no blocked worker: a push notifies the hub but
  //    the hub must skip the futex syscall (the bumped word is the state a
  //    later wait observes).
  const WakeTrace hub_idle = TraceWakeSyscalls([](int marker) {
    HealthSlot health{};
    NotifPipe<uint32_t, 32> q;
    PipeDrain th(&health);
    th.q = &q;
    Waitable* srcs[] = {&q};
    th.SetWaitInputs(srcs, 1);
    CHECK(th.wake_waiter_count() == 0u);
    MarkerWrite(marker, 'S');
    for (uint32_t i = 0; i < 16; ++i) CHECK(q.Push(i));
    MarkerWrite(marker, 'E');
    CHECK(th.handled_count() == 0u);  // worker never ran
  });
  ExpectWakes(hub_idle, 0, "hub notify without waiter");

  // 3. Exactly one blocked worker + one push: exactly one wake.
  const WakeTrace hub_one = TraceWakeSyscalls([](int marker) {
    HealthSlot health{};
    NotifPipe<uint32_t, 32> q;
    PipeDrain th(&health);
    th.q = &q;
    Waitable* srcs[] = {&q};
    th.SetWaitInputs(srcs, 1);
    CHECK(th.Start());
    CHECK(WaitFor([&] { return th.wake_waiter_count() == 1u; }));
    MarkerWrite(marker, 'S');
    CHECK(q.Push(7));
    MarkerWrite(marker, 'E');
    CHECK(WaitFor([&] { return th.msgs.load() == 1u; }));
    th.RequestStop();
    th.Join();
    CHECK(th.msgs.load() == 1u);
  });
  ExpectWakes(hub_one, 1, "hub notify with one waiter");

  // 4. Ring + shared wake word + hub target: the ring's direct wake-word bump
  //    is coalesced with the hub's, so one push bumps the word once and
  //    issues exactly one futex wake (the pre-PERF-14 path issued three).
  const WakeTrace coalesced = TraceWakeSyscalls([](int marker) {
    char ring_name[64];
    char wake_name[64];
    std::snprintf(ring_name, sizeof(ring_name), "/c9p14c_%d",
                  static_cast<int>(getpid()));
    std::snprintf(wake_name, sizeof(wake_name), "/c9p14w_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(ring_name);
    ShmRegion::UnlinkName(wake_name);

    ShmSpscRing<uint64_t, 8> ring;
    CHECK(ring.Create(ring_name));
    ShmRegion wake_region;
    CHECK(wake_region.Create(wake_name, sizeof(std::atomic<uint32_t>)));
    auto* wake = new (wake_region.data()) std::atomic<uint32_t>(0);

    HealthSlot health{};
    RingDrain<8> th(&health);
    th.q = &ring;
    Waitable* srcs[] = {&ring};
    th.SetWaitInputs(srcs, 1);
    th.SetWakeWord(wake);
    CHECK(th.Start());
    CHECK(WaitFor([&] { return th.wake_waiter_count() == 1u; }));

    MarkerWrite(marker, 'S');
    CHECK(ring.Push(1));
    MarkerWrite(marker, 'E');
    CHECK(wake->load(std::memory_order_seq_cst) == 1u);
    CHECK(WaitFor([&] { return th.msgs.load() == 1u; }));

    // Outside the measured window: one more push must bump the word exactly
    // once again (the ring must not also bump it directly).
    CHECK(ring.Push(2));
    CHECK(wake->load(std::memory_order_seq_cst) == 2u);
    CHECK(WaitFor([&] { return th.msgs.load() == 2u; }));

    th.RequestStop();
    th.Join();
    ring.Close();
    wake_region.Unmap();
    ShmRegion::UnlinkName(ring_name);
    ShmRegion::UnlinkName(wake_name);
  });
  ExpectWakes(coalesced, 1, "ring push with hub + shared wake word");

  // 5. SPMC: one epoch bump per Push even though every bound reader is a
  //    notify target; one FUTEX_WAKE wakes both registered readers, and a
  //    push with no registered reader issues none.
  const WakeTrace spmc = TraceWakeSyscalls([](int marker) {
    char name[64];
    std::snprintf(name, sizeof(name), "/c9p14s_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(name);
    ShmSpmcRing<uint64_t, 8> ring;
    CHECK(ring.Create(name));
    SpmcReader<uint64_t, 8> r1;
    SpmcReader<uint64_t, 8> r2;
    CHECK(r1.Bind(&ring, SpmcRead::Follow));
    CHECK(r2.Bind(&ring, SpmcRead::Follow));

    std::atomic<uint64_t> lat1_ms{9999};
    std::atomic<uint64_t> lat2_ms{9999};
    std::thread t1([&] {
      const uint64_t started = NowNs();
      r1.WaitUntil(started + 2000000000ULL);
      lat1_ms.store((NowNs() - started) / 1000000ULL,
                    std::memory_order_relaxed);
    });
    std::thread t2([&] {
      const uint64_t started = NowNs();
      r2.WaitUntil(started + 2000000000ULL);
      lat2_ms.store((NowNs() - started) / 1000000ULL,
                    std::memory_order_relaxed);
    });
    // Both readers registered as sleepers before the measured push.
    CHECK(WaitFor([&] {
      return ring.layout()->epoch_waiters.load(std::memory_order_seq_cst) == 2u;
    }));
    const uint32_t epoch0 = ring.epoch()->load(std::memory_order_seq_cst);
    MarkerWrite(marker, 'S');
    ring.Push(42);
    MarkerWrite(marker, 'E');
    CHECK(ring.epoch()->load(std::memory_order_seq_cst) == epoch0 + 1u);
    t1.join();
    t2.join();
    CHECK(lat1_ms.load(std::memory_order_relaxed) < 500u);
    CHECK(lat2_ms.load(std::memory_order_relaxed) < 500u);

    // No registered sleeper now: a push must not issue a wake syscall.
    CHECK(WaitFor([&] {
      return ring.layout()->epoch_waiters.load(std::memory_order_seq_cst) == 0u;
    }));
    MarkerWrite(marker, 'S');
    ring.Push(43);
    MarkerWrite(marker, 'E');
    CHECK(ring.epoch()->load(std::memory_order_seq_cst) == epoch0 + 2u);

    ring.Close();
    ShmRegion::UnlinkName(name);
  });
  ExpectWakes(spmc, 1, "spmc push wakes two readers with one syscall");

  // 6. Lost-wakeup stress (same process): every pushed message must be
  //    delivered promptly while pushes race the consumer's registration. A
  //    missed wake would ride out the 1s idle poll, so the bounded total
  //    elapsed time is the assertion.
  {
    char ring_name[64];
    char wake_name[64];
    std::snprintf(ring_name, sizeof(ring_name), "/c9p14d_%d",
                  static_cast<int>(getpid()));
    std::snprintf(wake_name, sizeof(wake_name), "/c9p14e_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(ring_name);
    ShmRegion::UnlinkName(wake_name);

    ShmSpscRing<uint64_t, 1024> ring;
    CHECK(ring.Create(ring_name));
    ShmRegion wake_region;
    CHECK(wake_region.Create(wake_name, sizeof(std::atomic<uint32_t>)));
    auto* wake = new (wake_region.data()) std::atomic<uint32_t>(0);

    HealthSlot health{};
    RingDrain<1024> th(&health);
    th.q = &ring;
    Waitable* srcs[] = {&ring};
    th.SetWaitInputs(srcs, 1);
    th.SetWakeWord(wake);
    CHECK(th.Start());
    CHECK(WaitFor([&] { return th.wake_waiter_count() == 1u; }));

    const uint64_t started = NowNs();
    constexpr uint32_t kMsgs = 300;
    for (uint32_t i = 0; i < kMsgs; ++i) {
      CHECK(ring.Push(i));
      // Vary the gap so pushes land around registration/wake transitions.
      struct timespec gap {0, static_cast<long>((i % 7) * 5000)};
      nanosleep(&gap, nullptr);
    }
    CHECK(WaitFor([&] { return th.msgs.load() == kMsgs; }, 3000000000ULL));
    const uint64_t elapsed = NowNs() - started;
    CHECK(th.msgs.load() == kMsgs);
    // Far below the 1s idle-poll fallback: a lost wake would show up here.
    CHECK(elapsed < 500000000ULL);

    th.RequestStop();
    th.Join();
    ring.Close();
    wake_region.Unmap();
    ShmRegion::UnlinkName(ring_name);
    ShmRegion::UnlinkName(wake_name);
  }

  // 7. SPMC lost-wakeup stress: two follow readers must both observe every
  //    frame (broadcast woken by reader count, never wake-one).
  {
    char name[64];
    std::snprintf(name, sizeof(name), "/c9p14f_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(name);
    ShmSpmcRing<uint64_t, 8> ring;
    CHECK(ring.Create(name));
    SpmcReader<uint64_t, 8> r1;
    SpmcReader<uint64_t, 8> r2;
    CHECK(r1.Bind(&ring, SpmcRead::Follow));
    CHECK(r2.Bind(&ring, SpmcRead::Follow));

    uint64_t got1 = 0;
    uint64_t got2 = 0;
    for (uint32_t i = 0; i < 64; ++i) {
      std::thread t1([&] {
        const uint64_t started = NowNs();
        r1.WaitUntil(started + 2000000000ULL);
      });
      std::thread t2([&] {
        const uint64_t started = NowNs();
        r2.WaitUntil(started + 2000000000ULL);
      });
      CHECK(WaitFor([&] {
        return ring.layout()->epoch_waiters.load(std::memory_order_seq_cst) ==
               2u;
      }));
      ring.Push(i);
      t1.join();
      t2.join();
      uint64_t v1 = 0;
      uint64_t v2 = 0;
      CHECK(r1.TryPop(v1) && v1 == i);
      CHECK(r2.TryPop(v2) && v2 == i);
      ++got1;
      ++got2;
    }
    CHECK(got1 == 64u);
    CHECK(got2 == 64u);
    ring.Close();
    ShmRegion::UnlinkName(name);
  }

  // 8. No-waiter push microbenchmark (kHz-class event traffic): the fast
  //    path is one epoch bump plus a counter load, no syscall. Printed as
  //    PERF-14 evidence; the loose bound only catches pathological regressions.
  {
    char name[64];
    std::snprintf(name, sizeof(name), "/c9p14m_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(name);
    ShmSpscRing<uint64_t, 1024> ring;
    CHECK(ring.Create(name));
    constexpr uint32_t kOps = 200000;
    uint64_t sink = 0;
    const uint64_t t0 = NowNs();
    for (uint32_t i = 0; i < kOps; ++i) {
      CHECK(ring.Push(i));
      uint64_t v = 0;
      CHECK(ring.TryPop(v));
      sink += v;
    }
    const uint64_t elapsed = NowNs() - t0;
    CHECK(sink == (static_cast<uint64_t>(kOps) * (kOps - 1)) / 2);
    const double ns_per_op = static_cast<double>(elapsed) / kOps;
    std::fprintf(stderr,
                 "PERF-14: no-waiter push+pop %.0f ns/op (%.0f kmsg/s)\n",
                 ns_per_op, 1e6 / ns_per_op);
    CHECK(ns_per_op < 10000.0);
    ring.Close();
    ShmRegion::UnlinkName(name);
  }

  std::fprintf(stderr, "PERF-14 wake-path test: PASS\n");
  return 0;
}
#else
int main() {
  // The measured scenarios rely on the Linux futex path and ptrace; the
  // delivery behavior is covered by the cross-platform ring/event tests.
  std::fprintf(stderr, "PERF-14 wake-path test: skipped (non-Linux)\n");
  return 0;
}
#endif
