// RT-06: SpscRing's publish/wake ordering.
//
// The wake protocol is "publish, then probe the waiter counter" against
// "register, then re-check readiness"; the no-waiter fast path is only sound
// because the two halves cannot both miss each other (see the ordering note in
// spsc_ring.h). These guards cover the three obligations of that protocol:
//
//   1. No lost wakeup when a consumer registers while a producer publishes.
//      Bounded iterations, a producer/consumer handshake on atomics (no sleeps
//      for sequencing); every WaitUntil that reaches its deadline without data
//      is counted, and the run requires zero — a lost wake shows up as one
//      deadline expiry per miss.
//   2. The publish is ordered before the wake: a payload written through
//      NotifPipe's in-place path is complete and in production order whenever
//      the consumer observes it after a wake (torn/stale slots fail here, and
//      the TSan variant of this file fails on an unsynchronised handoff).
//   3. A publish with no waiter registered survives the skip path: a later
//      WaitUntil returns on readiness, not on its deadline.
#include "thyme/check.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/spsc_ring.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <thread>

using namespace thyme;

namespace {

uint64_t MonoNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

// Bounded spin handshake: sequences the two threads by observation only, so
// the measured path never depends on a sleep. Returns false on timeout.
template <typename Pred>
bool SpinUntil(Pred pred, uint64_t timeout_ns) {
  const uint64_t deadline = MonoNs() + timeout_ns;
  while (!pred()) {
    if (MonoNs() >= deadline) return false;
  }
  return true;
}

// Registration race: one publish and one wait per iteration. Even iterations
// let the consumer arm (store `armed`) before the producer publishes, so the
// publish lands inside the registration window — the case the store-load edge
// exists for. Odd iterations publish as soon as the previous iteration is
// delivered, i.e. before the consumer arms, so Notify usually takes the
// no-waiter skip path and the consumer's own re-check has to find the entry.
void RegistrationRace() {
  constexpr uint32_t kIters = 400;
  constexpr uint64_t kDeadlineNs = 100000000ULL;  // 100 ms
  constexpr uint64_t kHandshakeNs = 5000000000ULL;

  SpscRing<8> q;
  std::atomic<uint32_t> armed{0};
  std::atomic<uint32_t> delivered{0};
  std::atomic<uint32_t> timeouts{0};
  std::atomic<uint32_t> mismatches{0};

  std::thread consumer([&] {
    for (uint32_t i = 0; i < kIters; ++i) {
      armed.store(i + 1, std::memory_order_release);
      const uint64_t deadline = MonoNs() + kDeadlineNs;
      uint32_t v = 0;
      uint32_t g = 0;
      bool got = false;
      while (!got) {
        if (MonoNs() >= deadline) {
          // Slept to the deadline with no wake: a lost (or never-issued)
          // wakeup. Drain whatever arrived late so the next iteration starts
          // from an empty ring.
          timeouts.fetch_add(1, std::memory_order_relaxed);
          while (q.try_pop(v, g)) {
          }
          break;
        }
        q.WaitUntil(deadline);
        got = q.try_pop(v, g);
      }
      if (got && v != i) mismatches.fetch_add(1, std::memory_order_relaxed);
      delivered.store(i + 1, std::memory_order_release);
    }
  });

  bool handshake_ok = true;
  for (uint32_t i = 0; i < kIters; ++i) {
    if ((i & 1u) == 0) {
      const bool armed_ok =
          SpinUntil([&] { return armed.load(std::memory_order_acquire) >= i + 1; },
                    kHandshakeNs);
      handshake_ok = handshake_ok && armed_ok;
    }
    q.push_latest(i, 0);
    const bool delivered_ok = SpinUntil(
        [&] { return delivered.load(std::memory_order_acquire) >= i + 1; },
        kHandshakeNs);
    handshake_ok = handshake_ok && delivered_ok;
  }
  consumer.join();

  CHECK(handshake_ok);
  CHECK(delivered.load(std::memory_order_acquire) == kIters);
  CHECK(timeouts.load(std::memory_order_relaxed) == 0);
  CHECK(mismatches.load(std::memory_order_relaxed) == 0);
  std::fprintf(stderr, "RT-06 registration race: %u iters, 0 lost wakeups\n",
               kIters);
}

// Payload ordering across the wake. NotifPipe is the in-process carrier that
// hands a real T to the consumer, so it exercises exactly the publish -> wake
// -> read sequence the ring protocol orders.
struct PairPayload {
  uint32_t seq;
  uint32_t inv;  // ~seq
};

void PayloadOrdering() {
  constexpr uint32_t kMsgs = 20000;
  constexpr uint64_t kDeadlineNs = 200000000ULL;  // 200 ms

  NotifPipe<PairPayload, 64> pipe;
  std::atomic<uint32_t> torn{0};
  std::atomic<uint32_t> reordered{0};

  std::thread consumer([&] {
    PairPayload v{};
    uint32_t count = 0;
    while (count < kMsgs) {
      if (pipe.TryPop(v)) {
        if (v.inv != ~v.seq) torn.fetch_add(1, std::memory_order_relaxed);
        if (v.seq != count) reordered.fetch_add(1, std::memory_order_relaxed);
        ++count;
        continue;
      }
      // Register, re-check, sleep (the ring's WaitUntil).
      pipe.WaitUntil(MonoNs() + kDeadlineNs);
    }
  });

  uint32_t written = 0;
  while (written < kMsgs) {
    PairPayload* slot = pipe.BeginWrite();
    if (slot == nullptr) continue;  // full: the consumer drains, no backpressure
    slot->seq = written;
    slot->inv = ~written;
    pipe.CommitWrite();
    ++written;
  }
  consumer.join();

  CHECK(torn.load(std::memory_order_relaxed) == 0);
  CHECK(reordered.load(std::memory_order_relaxed) == 0);
  std::fprintf(stderr, "RT-06 payload ordering: %u messages intact\n", kMsgs);
}

// Skip path: publishing with nobody registered must not need a wake for a
// later WaitUntil to see the entry. The failure mode is "returns on the
// deadline" (deadline is 500 ms, the assertion allows 100 ms of slack).
void PublishWithoutWaiter() {
  SpscRing<8> q;
  CHECK(q.waiter_count() == 0);
  q.push_latest(42, 7);
  CHECK(q.waiter_count() == 0);  // Notify took the skip path

  const uint64_t t0 = MonoNs();
  q.WaitUntil(t0 + 500000000ULL);
  const uint64_t elapsed = MonoNs() - t0;
  CHECK(elapsed < 100000000ULL);

  uint32_t v = 0;
  uint32_t g = 0;
  CHECK(q.try_pop(v, g) && v == 42 && g == 7);
  std::fprintf(stderr, "RT-06 skip path: publish observed in %llu ns\n",
               static_cast<unsigned long long>(elapsed));
}

}  // namespace

int main() {
  PublishWithoutWaiter();
  RegistrationRace();
  PayloadOrdering();
  std::fprintf(stderr, "RT-06 notify ordering: PASS\n");
  return 0;
}
