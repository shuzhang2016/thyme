#include "thyme/check.h"
#include "thyme/runtime/app.h"
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/event_thread.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/notif_pipe.h"
#include "test_wait.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__)
#define THYME_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define THYME_TEST_ASAN 1
#else
#define THYME_TEST_ASAN 0
#endif
#else
#define THYME_TEST_ASAN 0
#endif

using namespace thyme;

#ifdef __linux__
// Runs App::Prepare() in a forked child (Prepare() is one-shot per process).
// A zero memlock limit makes mlockall fail without depending on host caps.
bool PreparedMemoryFlagsInChild(const char* value, uint32_t expected,
                                bool zero_limit = false) {
  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    if (zero_limit) {
      unsetenv("THYME_NO_MLOCK");
      const struct rlimit limit {0, 0};
      if (setrlimit(RLIMIT_MEMLOCK, &limit) != 0) {
        _exit(2);
      }
    } else {
      setenv("THYME_NO_MLOCK", value, 1);
    }
    const bool ok = App::Prepare();
    const uint32_t mask = kMemoryMlockDisabled | kMemoryMlockFailed;
    const uint32_t flags = App::memory_flags();
    _exit(ok && !App::memory_locked() && (flags & mask) == expected ? 0 : 1);
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st));
  return WEXITSTATUS(st) == 0;
}
#endif


class CounterThread final : public CyclicThread {
 public:
  using CyclicThread::CyclicThread;
 protected:
  void Loop() override {}
};

class PipeDrainThread final : public EventThread {
 public:
  NotifPipe<uint32_t, 8>* q = nullptr;
  using EventThread::EventThread;
 protected:
  void Dispatch() override {
    uint32_t v = 0;
    while (q->TryPop(v)) NoteHandled();
  }
};

int main() {
#ifdef __linux__
  // THYME_NO_MLOCK opt-out: truthy values skip mlockall (memory_locked() is
  // false regardless of CAP_IPC_LOCK); false-y values do not opt out. Must
  // run before this process's own Prepare() — the children inherit the
  // one-shot g_prepared flag.
  CHECK(PreparedMemoryFlagsInChild("1", kMemoryMlockDisabled));
  CHECK(PreparedMemoryFlagsInChild("true", kMemoryMlockDisabled));
  CHECK(PreparedMemoryFlagsInChild("YES", kMemoryMlockDisabled));
  CHECK(PreparedMemoryFlagsInChild("on", kMemoryMlockDisabled));
#if THYME_TEST_ASAN
  std::fprintf(stderr, "test_app: skip zero-RLIMIT case because ASan intercepts "
                       "mlockall\n");
#else
  CHECK(PreparedMemoryFlagsInChild(nullptr, kMemoryMlockFailed, true));
#endif
  {
    // "0" must not opt out; capture the child's stderr to assert the opt-out
    // notice is NOT printed in this branch.
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      dup2(pipefd[1], STDERR_FILENO);
      setenv("THYME_NO_MLOCK", "0", 1);
      _exit(App::Prepare() ? 0 : 1);
    }
    close(pipefd[1]);
    char buf[512] = {};
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(n >= 0);
    buf[n] = '\0';
    CHECK(std::strstr(buf, "THYME_NO_MLOCK") == nullptr);
  }
#endif
  CHECK(App::Prepare());
  CHECK(App::Prepare());
#ifdef __APPLE__
  CHECK(!App::memory_locked());
  CHECK(!App::malloc_tuned());
  CHECK(App::memory_flags() == kMemoryUnsupported);
#endif
  CHECK(!App::stop_requested());

  HealthSlot health{};
  CounterThread th("ctr", 50, &health);

  // An App manages cyclic and event threads through the same
  // Start/RequestStop/Join lifecycle.
  HealthSlot ehealth{};
  NotifPipe<uint32_t, 8> q;
  PipeDrainThread eth("evt", &ehealth);
  eth.q = &q;
  Waitable* ws[] = {&q};
  eth.SetWaitInputs(ws, 1);

  App app;
  CHECK(app.Add(&th));
  CHECK(app.Add(&eth));
  CHECK(app.Start());
  CHECK(th.health() == &health);
  CHECK(eth.health() == &ehealth);
  CHECK(health.memory_flags.load() == App::memory_flags());
  CHECK(ehealth.memory_flags.load() == App::memory_flags());
  q.Push(1);
  q.Push(2);
  CHECK(thyme_test::WaitFor([&] { return eth.handled_count() >= 2; }));
  q.Push(3);
  CHECK(thyme_test::WaitFor([&] { return eth.handled_count() >= 3; }));
  // The event sink is ready before the 50 Hz cyclic thread has completed its
  // first 8 loops; keep the cadence assertion on the same observable counter
  // instead of a 200 ms wall-clock sleep.
  CHECK(thyme_test::WaitFor([&] { return th.loop_count() >= 8; }));
  app.RequestStop();
  app.Join();
  CHECK(App::stop_requested());
  CHECK(th.loop_count() >= 8);
  CHECK(th.loop_count() <= 16);
  CHECK(eth.handled_count() == 3);
  CHECK(eth.dispatch_count() >= 1);
  CHECK(ehealth.alive_counter.load() >= 1);

  // Start launches event sinks first: a message queued before Start is
  // drained even though the cyclic producer is added (and started) after.
  {
    HealthSlot h2{};
    HealthSlot eh2{};
    CounterThread th2("ctr2", 50, &h2);
    NotifPipe<uint32_t, 8> q2;
    PipeDrainThread eth2("evt2", &eh2);
    eth2.q = &q2;
    Waitable* ws2[] = {&q2};
    eth2.SetWaitInputs(ws2, 1);
    App app2;
    CHECK(app2.Add(&eth2));
    CHECK(app2.Add(&th2));
    CHECK(q2.Push(7));
    CHECK(app2.Start());
    CHECK(thyme_test::WaitFor([&] { return eth2.handled_count() >= 1; }));
    CHECK(eth2.handled_count() == 1);
    app2.RequestStop();
    app2.Join();
  }

  // Separate 64+64 arrays used to succeed; health ABI is one combined ceiling.
  {
    HealthSlot chs[App::kMaxThreads];
    HealthSlot ehs[App::kMaxThreads];
    std::deque<CounterThread> cyc;
    std::deque<PipeDrainThread> evs;
    for (std::size_t i = 0; i < App::kMaxThreads; ++i) {
      cyc.emplace_back("c", 1, &chs[i]);
      evs.emplace_back("e", &ehs[i]);
    }
    App cap;
    for (std::size_t i = 0; i < App::kMaxThreads; ++i) {
      CHECK(cap.Add(&cyc[i]));
    }
    for (std::size_t i = 0; i < App::kMaxThreads; ++i) {
      CHECK(!cap.Add(&evs[i]));
    }
  }
  return 0;
}
