// Cross-process event wake-up: an EventThread waiting on a shm futex word is
// woken promptly when a producer in ANOTHER process pushes into a shm ring
// wired to that word (the in-process hub notify cannot cross processes).
#include "thyme/check.h"
#include "thyme/runtime/event_thread.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spsc_ring.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

using namespace thyme;

namespace {

struct WakeWord {
  std::atomic<uint32_t> word{0};
};

class RingDrainThread final : public EventThread {
 public:
  ShmSpscRing<uint64_t, 8>* q = nullptr;
  std::atomic<uint64_t> msgs{0};
  std::atomic<uint64_t> first_latency_ms{0};
  using EventThread::EventThread;
  std::atomic<uint64_t> t0{0};
 protected:
  void Dispatch() override {
    uint64_t v = 0;
    while (q->TryPop(v)) {
      NoteHandled();
      if (msgs.fetch_add(1, std::memory_order_relaxed) == 0) {
        first_latency_ms.store(v, std::memory_order_relaxed);
      }
    }
  }
};

uint64_t MonoMs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
         static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
}

}  // namespace

int main() {
  char ring_name[64], wake_name[64];
  std::snprintf(ring_name, sizeof(ring_name), "/c9wwr_%d",
                static_cast<int>(getpid()));
  std::snprintf(wake_name, sizeof(wake_name), "/c9www_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(ring_name);
  ShmRegion::UnlinkName(wake_name);

  // Supervisor Prepare: Unlink+Init ring and wake; roles only Open.
  CHECK((ShmSpscRing<uint64_t, 8>::Prepare(ring_name)));
  {
    ShmRegion::UnlinkName(wake_name);
    ShmRegion prep_wake;
    CHECK(prep_wake.Create(wake_name, sizeof(WakeWord)));
    new (prep_wake.data()) WakeWord{};
  }

  ShmSpscRing<uint64_t, 8> consumer_ring;
  CHECK(consumer_ring.Open(ring_name, 1000000000ULL));
  ShmRegion wake_region;
  CHECK(wake_region.OpenRetry(wake_name, sizeof(WakeWord), 1000000000ULL));
  auto* wake = static_cast<WakeWord*>(wake_region.data());
  CHECK(wake != nullptr);

  HealthSlot health{};
  RingDrainThread th("ev_shm", &health);
  th.q = &consumer_ring;
  Waitable* srcs[] = {&consumer_ring};
  th.SetWaitInputs(srcs, 1);
  th.SetWakeWord(&wake->word);
  CHECK(th.Start());

  // Producer in a child process: Open-only (no Create), wake word attached.
  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    ShmSpscRing<uint64_t, 8> producer_ring;
    if (!producer_ring.Open(ring_name, 1000000000ULL)) _exit(2);
    ShmRegion prod_wake_region;
    auto* prod_wake =
        prod_wake_region.OpenRetry(wake_name, sizeof(WakeWord), 1000000000ULL)
            ? static_cast<WakeWord*>(prod_wake_region.data())
            : nullptr;
    if (prod_wake == nullptr) _exit(3);
    producer_ring.SetWakeWord(&prod_wake->word);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!producer_ring.Push(MonoMs())) _exit(4);
    _exit(0);
  }

  // The thread must drain the message promptly (bounded by notify, not by
  // the 1s idle poll).
  const uint64_t deadline = MonoMs() + 3000;
  while (th.msgs.load(std::memory_order_relaxed) == 0 && MonoMs() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  CHECK(th.msgs.load(std::memory_order_relaxed) == 1);
  const uint64_t sent_at = th.first_latency_ms.load(std::memory_order_relaxed);
  // Push carried the send timestamp; end-to-end latency must be well below
  // the 1s idle-poll fallback. Cross-process wake needs Linux futex; off
  // Linux the hub falls back to idle poll so only delivery is asserted.
#ifdef __linux__
  const int64_t latency = static_cast<int64_t>(MonoMs()) -
                          static_cast<int64_t>(sent_at);
  CHECK(latency >= 0 && latency < 500);
#else
  (void)sent_at;
#endif

  // PERF-14 lost-wakeup stress: the hub skips the futex wake when its
  // waiter counter reads zero, and that read races a consumer that is about
  // to (re-)block. Push a burst with varied gaps; every message must be
  // delivered, and the whole burst must finish far below the 1s idle-poll
  // fallback — a missed wake would add a full idle tick each time.
  {
    constexpr uint32_t kBurst = 300;
    const uint64_t before = th.msgs.load(std::memory_order_relaxed);
    const uint64_t burst_start = MonoMs();
    for (uint32_t i = 0; i < kBurst; ++i) {
      while (!consumer_ring.Push(i)) {  // ring cap is 8: let the consumer drain
        CHECK(MonoMs() - burst_start < 3000);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      std::this_thread::sleep_for(std::chrono::microseconds((i % 7) * 20));
    }
    const uint64_t want = before + kBurst;
    const uint64_t burst_deadline = MonoMs() + 3000;
    while (th.msgs.load(std::memory_order_relaxed) < want &&
           MonoMs() < burst_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(th.msgs.load(std::memory_order_relaxed) == want);
    CHECK(MonoMs() - burst_start < 500);
  }

  th.RequestStop();
  th.Join();
  consumer_ring.Close();
  wake_region.Unmap();
  ShmRegion::UnlinkName(ring_name);
  ShmRegion::UnlinkName(wake_name);
  return 0;
}
