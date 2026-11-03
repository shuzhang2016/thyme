#include "thyme/check.h"
#include "thyme/runtime/object_pool.h"
#include "thyme/runtime/spsc_ring.h"
#include "test_wait.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <pthread.h>
#include <ctime>

using namespace thyme;


struct alignas(64) Cell { uint32_t x; };

static uint64_t Tns() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

struct WaitArg { SpscRing<8>* q; uint64_t deadline; };
static void* Waiter(void* p) {
  auto* a = static_cast<WaitArg*>(p);
  a->q->WaitUntil(a->deadline);
  return nullptr;
}

struct StressArg { SpscRing<8>* q; std::atomic<bool>* stop; };
static void* StressProducer(void* p) {
  auto* a = static_cast<StressArg*>(p);
  uint32_t seq = 0;
  while (!a->stop->load(std::memory_order_relaxed)) {
    a->q->push_latest(seq, 0);
    ++seq;
  }
  return nullptr;
}
static void* StressConsumer(void* p) {
  auto* a = static_cast<StressArg*>(p);
  uint32_t v = 0, g = 0;
  while (!a->stop->load(std::memory_order_relaxed)) {
    while (a->q->try_pop(v, g)) {}
  }
  return nullptr;
}

struct SeqStressArg {
  SpscRing<2>* q;
  std::atomic<bool>* stop;
  std::atomic<int>* tear;    // idx != gen (packed slot torn)
  std::atomic<int>* replay;  // duplicate / non-increasing delivery
};
static void* SeqStressProducer(void* p) {
  auto* a = static_cast<SeqStressArg*>(p);
  uint32_t seq = 1;
  while (!a->stop->load(std::memory_order_relaxed)) {
    a->q->push_latest(seq, seq);
    ++seq;
    if (seq == 0) seq = 1;
  }
  return nullptr;
}
static void* SeqStressConsumer(void* p) {
  auto* a = static_cast<SeqStressArg*>(p);
  uint32_t last = 0;
  uint32_t v = 0, g = 0;
  while (!a->stop->load(std::memory_order_relaxed)) {
    if (!a->q->try_pop(v, g)) continue;
    if (v != g) a->tear->fetch_add(1, std::memory_order_relaxed);
    if (last != 0 && v <= last) a->replay->fetch_add(1, std::memory_order_relaxed);
    last = v;
  }
  return nullptr;
}

int main() {
  CHECK((alignof(SpscRing<8>) >= 64));
  SpscRing<8> q;
  uint32_t v = 0, g = 0;
  CHECK(q.empty());
  CHECK(!q.try_pop(v, g));
  q.push_latest(3, 7);
  CHECK(q.try_pop(v, g) && v == 3 && g == 7);
  for (uint32_t i = 0; i < 8; ++i) q.push_latest(i, i + 100);
  uint32_t dropped_gen = 0;
  uint32_t dropped = q.push_latest(99, 199, &dropped_gen);
  CHECK(dropped == 0 && dropped_gen == 100 && q.drop_count() == 1);
  CHECK(q.try_pop(v, g) && v == 1 && g == 101);
  for (int i = 0; i < 6; ++i) CHECK(q.try_pop(v, g));
  CHECK(q.try_pop(v, g) && v == 99 && g == 199);

  // try_push: non-overwriting push, returns false when full.
  SpscRing<4> tq;
  CHECK(tq.try_push(1, 10));
  CHECK(tq.try_push(2, 20));
  CHECK(tq.try_push(3, 30));
  CHECK(tq.try_push(4, 40));
  CHECK(!tq.try_push(5, 50));  // full: rejected, nothing evicted
  CHECK(tq.drop_count() == 0);
  CHECK(tq.try_pop(v, g) && v == 1 && g == 10);
  CHECK(tq.try_pop(v, g) && v == 2 && g == 20);
  CHECK(tq.try_push(5, 50));  // room again after pops
  CHECK(tq.try_push(6, 60));
  CHECK(!tq.try_push(7, 70));  // full again
  uint32_t order[4] = {0};
  for (int i = 0; i < 4; ++i) {
    CHECK(tq.try_pop(v, g));
    order[i] = v;
  }
  CHECK(order[0] == 3 && order[1] == 4 && order[2] == 5 && order[3] == 6);
  CHECK(tq.empty());
  // Mixing try_push with overwriting push_latest: push_latest still evicts.
  CHECK(tq.try_push(7, 70));
  CHECK(tq.try_push(8, 80));
  CHECK(tq.try_push(9, 90));
  CHECK(tq.try_push(10, 100));
  dropped = tq.push_latest(11, 110, &dropped_gen);
  CHECK(dropped == 7 && dropped_gen == 70 && tq.drop_count() == 1);
  CHECK(tq.try_pop(v, g) && v == 8 && g == 80);
  while (tq.try_pop(v, g)) {}
  CHECK(tq.empty());

  SpscRing<4> rq;
  ObjectPool<Cell, 8> pool;
  uint32_t idx[4], gen[4];
  for (int i = 0; i < 4; ++i) {
    idx[i] = pool.acquire();
    gen[i] = pool.generation(idx[i]);
    pool.publish(idx[i]);
    rq.push_latest(idx[i], gen[i]);
  }
  const uint32_t extra = pool.acquire();
  CHECK((extra != ObjectPool<Cell, 8>::kInvalidIndex));
  const uint32_t extra_gen = pool.generation(extra);
  pool.publish(extra);
  dropped_gen = 0;
  dropped = rq.push_latest(extra, extra_gen, &dropped_gen);
  CHECK(dropped == idx[0] && dropped_gen == gen[0]);
  CHECK(pool.release(dropped, dropped_gen));
  CHECK(!pool.release(dropped, dropped_gen));
  uint32_t pop_idx = 0, pop_gen = 0;
  while (rq.try_pop(pop_idx, pop_gen)) {
    pool.release(pop_idx, pop_gen);
  }
  const uint32_t fresh = pool.acquire();
  const uint32_t fresh_gen = pool.generation(fresh);
  CHECK((fresh != ObjectPool<Cell, 8>::kInvalidIndex));
  // Releasing with a stale generation must fail *and* leave the slot's current
  // generation untouched (fresh still owns it in Writing state).
  CHECK(!pool.release(fresh, gen[0]));
  CHECK(pool.generation(fresh) == fresh_gen);
  pool.abort_write(fresh);

  SpscRing<8> wq;
  WaitArg arg{&wq, Tns() + 5000000000ULL};
  pthread_t th{};
  CHECK(pthread_create(&th, nullptr, Waiter, &arg) == 0);
  CHECK(thyme_test::WaitFor([&] { return wq.waiter_count() > 0; }));
  wq.push_latest(7, 0);
  CHECK(pthread_join(th, nullptr) == 0);
  uint32_t got = 0, got_gen = 0;
  CHECK(wq.try_pop(got, got_gen) && got == 7);

  // Notify must wake a registered waiter promptly (a 5 s deadline would hide
  // a missed wakeup behind the timeout).
  SpscRing<8> pq;
  WaitArg parg{&pq, Tns() + 5000000000ULL};
  pthread_t pth{};
  CHECK(pthread_create(&pth, nullptr, Waiter, &parg) == 0);
  CHECK(thyme_test::WaitFor([&] { return pq.waiter_count() > 0; }));
  const uint64_t push_at = Tns();
  pq.push_latest(9, 0);
  CHECK(pthread_join(pth, nullptr) == 0);
  CHECK(Tns() - push_at < 2000000000ULL);
  CHECK(pq.try_pop(got, got_gen) && got == 9);

  SpscRing<8> stress_q;
  std::atomic<bool> stress_stop{false};
  StressArg sarg{&stress_q, &stress_stop};
  pthread_t prod{}, cons{};
  CHECK(pthread_create(&prod, nullptr, StressProducer, &sarg) == 0);
  CHECK(pthread_create(&cons, nullptr, StressConsumer, &sarg) == 0);
  struct timespec sl2{0, 50L * 1000 * 1000};
  nanosleep(&sl2, nullptr);
  stress_stop.store(true, std::memory_order_relaxed);
  CHECK(pthread_join(prod, nullptr) == 0);
  CHECK(pthread_join(cons, nullptr) == 0);

  // Full-ring packed-slot consistency: producer pushes (idx=seq, gen=seq) into
  // a tiny ring so it saturates immediately. The packed slot is one atomic
  // 64-bit word, so a torn read would surface as idx != gen — assert none.
  // Full-ring drop-oldest must never deliver the same (or a smaller) value
  // twice: RT-02 moved both the producer's eviction and the consumer's pop to
  // a tail CAS, so a pop cannot lose its race with an eviction and re-observe
  // an older slot. Before that fix this ran at 13k-283k replays per 300 ms;
  // asserting 0 is the regression gate for it. A torn slot would surface as
  // idx != gen and is asserted separately.
  SpscRing<2> fq;
  std::atomic<bool> fstop{false};
  std::atomic<int> tears{0};
  std::atomic<int> replays{0};
  SeqStressArg farg{&fq, &fstop, &tears, &replays};
  pthread_t fp{}, fc{};
  CHECK(pthread_create(&fp, nullptr, SeqStressProducer, &farg) == 0);
  CHECK(pthread_create(&fc, nullptr, SeqStressConsumer, &farg) == 0);
  struct timespec sl3{0, 300L * 1000 * 1000};
  nanosleep(&sl3, nullptr);
  fstop.store(true, std::memory_order_relaxed);
  CHECK(pthread_join(fp, nullptr) == 0);
  CHECK(pthread_join(fc, nullptr) == 0);
  CHECK(tears.load(std::memory_order_relaxed) == 0);
  CHECK(replays.load(std::memory_order_relaxed) == 0);
  std::printf("full-ring: tears=%d replays=%d\n",
              tears.load(std::memory_order_relaxed),
              replays.load(std::memory_order_relaxed));
  return 0;
}
