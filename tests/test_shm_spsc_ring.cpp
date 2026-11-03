// Cross-process SPSC full-delivery queue in shared memory, with futex-based
// Waitable notification (backlog: SHM SPSC ring).
#include "thyme/check.h"
#include "thyme/runtime/shm_spsc_ring.h"
#include "thyme/runtime/waitable.h"
#include "test_wait.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

using namespace thyme;

namespace {

struct Payload {
  uint64_t a;
  uint32_t b;
};

uint64_t MonoNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

// Counting Waitable target for the SetNotify hook test.
struct CountingTarget : Waitable {
  bool empty() const override { return true; }
  void WaitUntil(uint64_t) override {}
  void Notify() override { count.fetch_add(1, std::memory_order_relaxed); }
  std::atomic<int> count{0};
};

}  // namespace

int main() {
  char name[64];
  std::snprintf(name, sizeof(name), "/c9ssr_%d", static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);

  ShmSpscRing<Payload, 4> producer;
  CHECK(producer.Create(name));
  ShmSpscRing<Payload, 4> consumer;
  CHECK(consumer.Open(name, 1000000000ULL));

  // FIFO full delivery across two mappings of the same shm object.
  for (uint64_t i = 0; i < 3; ++i) {
    CHECK(producer.Push(Payload{i, static_cast<uint32_t>(i)}));
  }
  for (uint64_t i = 0; i < 3; ++i) {
    Payload p {};
    CHECK(consumer.TryPop(p));
    CHECK(p.a == i && p.b == static_cast<uint32_t>(i));
  }
  CHECK(consumer.empty());

  // Full ring: Push refuses (no overwrite), pop restores capacity.
  for (uint32_t i = 0; i < 4; ++i) CHECK(producer.Push(Payload{i, i}));
  CHECK(!producer.Push(Payload{9, 9}));
  CHECK(producer.push_fail_count() == 1);
  Payload p {};
  CHECK(consumer.TryPop(p));
  CHECK(producer.Push(Payload{9, 9}));

  // WaitUntil times out when empty and nothing arrives.
  for (int i = 0; i < 4; ++i) { Payload d {}; consumer.TryPop(d); }
  CHECK(consumer.empty());
  const uint64_t t0 = MonoNs();
  consumer.WaitUntil(t0 + 50000000ULL);  // 50ms
  const uint64_t waited = MonoNs() - t0;
  CHECK(waited >= 40000000ULL);   // did not return early
  CHECK(waited < 1000000000ULL);  // did not hang

  // WaitUntil wakes promptly on a push from the peer mapping (thread stands
  // in for the peer process; the fork test below covers true cross-process).
  std::atomic<uint64_t> wake_latency_ms{0};
  std::thread waiter([&] {
    const uint64_t start = MonoNs();
    consumer.WaitUntil(start + 2000000000ULL);
    wake_latency_ms.store((MonoNs() - start) / 1000000ULL,
                          std::memory_order_relaxed);
  });
  CHECK(thyme_test::WaitFor([&] { return consumer.epoch_waiters() > 0; }));
  producer.Push(Payload{42, 42});
  waiter.join();
  CHECK(wake_latency_ms.load(std::memory_order_relaxed) < 500);
  CHECK(consumer.TryPop(p) && p.a == 42);

  // PERF-14: the no-waiter fast path skips the futex wake, so the waiter
  // registration race is the correctness boundary. Push repeatedly while a
  // fresh waiter registers and blocks; every iteration must observe its own
  // message promptly (a lost wake would ride out the 2s deadline).
  CHECK(consumer.epoch_waiters() == 0);
  for (uint32_t i = 0; i < 200; ++i) {
    std::atomic<uint64_t> elapsed_ms{9999};
    std::thread racing_waiter([&] {
      const uint64_t start = MonoNs();
      consumer.WaitUntil(start + 2000000000ULL);
      elapsed_ms.store((MonoNs() - start) / 1000000ULL,
                       std::memory_order_relaxed);
    });
    // Vary the push point across registration and FUTEX_WAIT entry.
    struct timespec gap {0, static_cast<long>((i % 11) * 3000)};
    nanosleep(&gap, nullptr);
    CHECK(producer.Push(Payload{i, i}));
    racing_waiter.join();
    CHECK(elapsed_ms.load(std::memory_order_relaxed) < 500);
    Payload got_i {};
    CHECK(consumer.TryPop(got_i) && got_i.a == i && got_i.b == i);
  }
  CHECK(producer.epoch_waiters() == 0);

  // SetNotify hook fires on successful Push only. The hook is producer-side:
  // Push runs on the producer's mapping, so the target must be attached to
  // the producer wrapper (consumer wake-up across processes is the futex).
  CountingTarget target;
  producer.SetNotify(&target);
  CHECK(producer.Push(Payload{1, 1}));
  CHECK(target.count.load(std::memory_order_relaxed) == 1);
  producer.SetNotify(nullptr);

  // True cross-process: child pushes, parent is woken from WaitUntil.
  for (int i = 0; i < 4; ++i) { Payload d {}; consumer.TryPop(d); }
  CHECK(consumer.empty());
  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    producer.Push(Payload{77, 77});
    _exit(0);
  }
  const uint64_t w0 = MonoNs();
  consumer.WaitUntil(w0 + 2000000000ULL);
  const uint64_t cross_ms = (MonoNs() - w0) / 1000000ULL;
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  CHECK(cross_ms < 500);
  CHECK(consumer.TryPop(p) && p.a == 77);

  // Layout is part of the SHM contract: trivially copyable payload, power-of
  // -two capacity, and the ring object itself is POD-in-shm.
  CHECK((ShmSpscRing<Payload, 4>::RegionBytes() ==
         sizeof(typename ShmSpscRing<Payload, 4>::Layout)));

  producer.Close();
  consumer.Close();
  ShmRegion::UnlinkName(name);

  // Supervisor-owned lifecycle (Phase 0.A): after Unlink, Open-only fails;
  // after Prepare (Unlink+RawInit), producer and consumer Open succeed;
  // a second Prepare recovers leftover state after a crash simulation.
  {
    char life[64];
    std::snprintf(life, sizeof(life), "/c9ssrlife_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(life);

    ShmSpscRing<Payload, 4> orphan;
    CHECK(!orphan.Open(life, 50000000ULL));

    CHECK((ShmSpscRing<Payload, 4>::Prepare(life)));

    ShmSpscRing<Payload, 4> prep_prod;
    ShmSpscRing<Payload, 4> prep_cons;
    CHECK(prep_prod.Open(life, 1000000000ULL));
    CHECK(prep_cons.Open(life, 1000000000ULL));
    CHECK(prep_prod.Push(Payload{3, 3}));
    Payload got{};
    CHECK(prep_cons.TryPop(got) && got.a == 3 && got.b == 3);

    // ARCH-11 v4: a same-size page with a different T or Cap has a different
    // layout hash and must be refused instead of reading shifted slots.
    {
      ShmSpscRing<uint64_t, 4> wrong_payload;
      CHECK(!wrong_payload.Open(life, 0));
      ShmSpscRing<Payload, 8> wrong_cap;
      CHECK(!wrong_cap.Open(life, 0));
    }
    prep_prod.Close();
    prep_cons.Close();

    // Crash simulation: named object still present; Prepare Unlink+Init again.
    CHECK((ShmSpscRing<Payload, 4>::Prepare(life)));
    CHECK(prep_prod.Open(life, 1000000000ULL));
    CHECK(prep_cons.Open(life, 1000000000ULL));
    prep_prod.Close();
    prep_cons.Close();
    ShmRegion::UnlinkName(life);
  }
  return 0;
}
