#include "thyme/check.h"
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/spsc_ring.h"
#include "test_wait.h"
#include <ctime>

using namespace thyme;


static uint64_t Tns() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

class CounterThread final : public CyclicThread {
 public:
  using CyclicThread::CyclicThread;
  std::atomic<uint32_t> ticks{0};
 protected:
  void Loop() override { ticks.fetch_add(1); }
};

// Alternates productive loops (consume+produce frame nibble 0) and idle
// loops, to lock the work-record flags semantics for the frame join.
class NibbleZeroThread final : public CyclicThread {
 public:
  using CyclicThread::CyclicThread;
  std::atomic<uint32_t> ticks{0};
 protected:
  void Loop() override {
    if (ticks.fetch_add(1) % 2 == 0) {
      NoteInFrame(0);
      NoteOutFrame(0);
    }
  }
};

class PopThread final : public CyclicThread {
 public:
  SpscRing<8>* q = nullptr;
  std::atomic<uint64_t> first_ns{0};
  // TST-07: the first pop is a handshake, not a poll target -- Loop signals
  // the eventfd and the test blocks in poll() with a deadline instead of
  // sleeping 80 ms and hoping the wake landed.
  thyme_test::EventFd first_pop;
  using CyclicThread::CyclicThread;
 protected:
  void Loop() override {
    uint32_t v = 0;
    uint32_t g = 0;
    if (q && q->try_pop(v, g) && first_ns.load() == 0) {
      struct timespec ts {};
      clock_gettime(CLOCK_MONOTONIC, &ts);
      first_ns.store(static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
                     static_cast<uint64_t>(ts.tv_nsec));
      first_pop.Signal();
    }
  }
};

int main() {
  HealthSlot health{};
  CounterThread th("ctr", 50, &health); /* 50 Hz */
  th.Start();
  const uint64_t ticks_t0 = Tns();
  CHECK(thyme_test::WaitFor([&] { return th.ticks.load() >= 8; }));
  th.RequestStop();
  th.Join();
  CHECK(Tns() - ticks_t0 < 1000000000ULL); /* 8 ticks at 50 Hz = 160 ms */
  CHECK(th.ticks.load() >= 8);  /* ~10 expected, allow jitter */
  CHECK(th.ticks.load() <= 16);
  CHECK(health.alive_counter.load() >= 8);

  HealthSlot hp{};
  CounterThread pin("pin", 50, &hp);
  ThreadPlacement pl{};
  pl.ncpus = 1;
  pl.cpus[0] = 0;
  pl.policy = ThreadPlacement::Policy::Fifo;
  pl.priority = 10;
  pl.fail = ThreadPlacement::Fail::BestEffort;
  pin.SetPlacement(pl);
  CHECK(pin.Start());
  CHECK(thyme_test::WaitFor([&] { return pin.loop_count() >= 1; }));
  pin.RequestStop();
  pin.Join();
  CHECK(pin.loop_count() >= 1);

  HealthSlot hw{};
  SpscRing<8> rq;
  PopThread wt("wait", 10, &hw); /* T=100ms */
  wt.q = &rq;
  Waitable* srcs[] = {&rq};
  wt.SetWaitInputs(srcs, 1);
  uint64_t start_ns = Tns();
  CHECK(wt.Start());
  rq.push_latest(1, 0);
  CHECK(wt.first_pop.Wait(2000000000ULL));
  wt.RequestStop();
  wt.Join();
  CHECK(wt.first_ns.load() != 0);
  CHECK(wt.first_ns.load() - start_ns < 80000000ULL);

  HealthSlot hs{};
  SpscRing<8> emptyq;
  PopThread st("stopw", 1, &hs); /* T=1s */
  st.q = &emptyq;
  Waitable* es[] = {&emptyq};
  st.SetWaitInputs(es, 1);
  CHECK(st.Start());
  CHECK(thyme_test::WaitFor([&] { return st.loop_count() >= 1; }));
  st.RequestStop();
  st.Join();

  HealthSlot hz0{};
  CounterThread bad("badhz", 0, &hz0);
  CHECK(!bad.Start());

  // Frame-attribution flags: nibble 0 is a legitimate frame (seq % 16 == 0),
  // so the work record carries in/out validity in flags (bit2 = consumed,
  // bit1 = produced) instead of relying on io != 0.
  {
    HealthSlot hz1{};
    NibbleZeroThread nz("nib0", 100, &hz1); /* 100 Hz */
    CHECK(nz.Start());
    CHECK(thyme_test::WaitFor([&] { return nz.ticks.load() >= 6; }));
    nz.RequestStop();
    nz.Join();
    CHECK(nz.ticks.load() >= 6);
    PerfLogWriter w;
    nz.work_log().Flush(w, 9);
    const uint8_t* p = w.data() + 4;
    CHECK(p[0] == 3 && p[1] == 9);  // work block, node 9
    const uint32_t cnt = ReadU32(p + 2);
    CHECK(cnt == nz.ticks.load());
    for (uint32_t i = 0; i < cnt; ++i) {
      const uint8_t* r = p + 6 + i * 10;
      if (i % 2 == 0) {  // productive loop, in/out nibbles both 0
        CHECK(r[7] == 0);
        CHECK((r[9] & 0x6) == 0x6);
      } else {           // idle loop: neither consumed nor produced
        CHECK((r[9] & 0x6) == 0);
      }
    }
  }
  return 0;
}
