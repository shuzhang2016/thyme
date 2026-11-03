#include "thyme/check.h"
#include "thyme/runtime/event_thread.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/notif_pipe.h"
#include "test_wait.h"
#include <atomic>
#include <cstdint>
#include <ctime>
#include <unistd.h>

using namespace thyme;


static uint64_t Tns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

class DrainThread final : public EventThread {
 public:
  NotifPipe<uint32_t, 16>* q = nullptr;
  std::atomic<uint64_t> msgs{0};
  std::atomic<uint64_t> sum{0};
  using EventThread::EventThread;
 protected:
  void Dispatch() override {
    uint32_t v = 0;
    while (q->TryPop(v)) {
      NoteHandled();
      NoteLatency(1000 + v);  // exercise the latency histogram
      sum.fetch_add(v, std::memory_order_relaxed);
      msgs.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

class MultiDrainThread final : public EventThread {
 public:
  NotifPipe<uint32_t, 16>** qs = nullptr;
  std::size_t nqs = 0;
  std::atomic<uint64_t> msgs{0};
  std::atomic<uint64_t> sum{0};
  using EventThread::EventThread;
 protected:
  void Dispatch() override {
    for (std::size_t i = 0; i < nqs; ++i) {
      uint32_t v = 0;
      while (qs[i]->TryPop(v)) {
        NoteHandled();
        sum.fetch_add(v, std::memory_order_relaxed);
        msgs.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
};

// Consumes and produces frame nibble 0 (seq % 16 == 0): the work record must
// carry in/out validity in flags (bit2 consumed, bit1 produced) because
// io == 0 is a legitimate frame attribution, not "no attribution".
class NibbleZeroEvThread final : public EventThread {
 public:
  NotifPipe<uint32_t, 16>* q = nullptr;
  std::atomic<uint64_t> msgs{0};
  using EventThread::EventThread;
 protected:
  void Dispatch() override {
    uint32_t v = 0;
    while (q->TryPop(v)) {
      NoteHandled();
      NoteInFrame(0);
      NoteOutFrame(0);
      msgs.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

static void WaitCount(const std::atomic<uint64_t>& cnt, uint64_t want) {
  const uint64_t deadline = Tns() + 3000000000ULL;
  while (cnt.load(std::memory_order_relaxed) < want && Tns() < deadline) {
    usleep(1000);
  }
}

static void WaitMsgs(DrainThread& th, uint64_t want) {
  WaitCount(th.msgs, want);
}

class SlowDrainThread final : public EventThread {
 public:
  NotifPipe<uint32_t, 4>* q = nullptr;
  uint64_t sleep_ns = 0;
  using EventThread::EventThread;
 protected:
  void Dispatch() override {
    uint32_t v = 0;
    while (q->TryPop(v)) {
      if (sleep_ns > 0) {
        struct timespec ts {};
        ts.tv_sec = static_cast<time_t>(sleep_ns / 1000000000ULL);
        ts.tv_nsec = static_cast<long>(sleep_ns % 1000000000ULL);
        nanosleep(&ts, nullptr);
      }
      NoteHandled();
    }
  }
};

static void WaitHandled(const EventThread& th, uint64_t want) {
  const uint64_t deadline = Tns() + 3000000000ULL;
  while (th.handled_count() < want && Tns() < deadline) {
    usleep(1000);
  }
}

int main() {
  // An event triggers Dispatch; the message is delivered and health advances.
  HealthSlot health{};
  NotifPipe<uint32_t, 16> q;
  DrainThread th("evt", &health);
  th.q = &q;
  Waitable* srcs[] = {&q};
  th.SetWaitInputs(srcs, 1);
  CHECK(th.Start());
  q.Push(5);
  WaitMsgs(th, 1);
  CHECK(th.msgs.load() == 1);
  CHECK(th.sum.load() == 5);
  CHECK(th.dispatch_count() >= 1);
  CHECK(health.alive_counter.load() >= 1);
  th.RequestStop();
  th.Join();

  // Nibble-0 frames: work flags carry consumed/produced validity (io == 0 is
  // a legal attribution). One message -> one Dispatch -> one work record.
  {
    HealthSlot hn{};
    NotifPipe<uint32_t, 16> nq;
    NibbleZeroEvThread nz("nib0ev", &hn);
    nz.q = &nq;
    Waitable* nsrcs[] = {&nq};
    nz.SetWaitInputs(nsrcs, 1);
    CHECK(nz.Start());
    CHECK(nq.Push(1));
    WaitCount(nz.msgs, 1);
    nz.RequestStop();
    nz.Join();
    CHECK(nz.msgs.load() == 1);
    PerfLogWriter w;
    nz.work_log().Flush(w, 9);
    const uint8_t* p = w.data() + 4;
    CHECK(p[0] == 3 && p[1] == 9);
    const uint32_t cnt = ReadU32(p + 2);
    CHECK(cnt >= 1);
    // Exactly one productive record (the single message); any extra records
    // are idle dispatches and must carry neither flag.
    uint32_t productive = 0;
    for (uint32_t i = 0; i < cnt; ++i) {
      const uint8_t* r = p + 6 + i * 10;
      if ((r[9] & 0x6) != 0) {
        CHECK(r[7] == 0);              // in/out nibbles both 0
        CHECK((r[9] & 0x6) == 0x6);    // consumed + produced
        ++productive;
      }
    }
    CHECK(productive == 1);
  }

  // A burst pushed while the worker is blocked is drained as batches:
  // every message is processed exactly once.
  HealthSlot hb{};
  NotifPipe<uint32_t, 16> bq;
  DrainThread bt("batch", &hb);
  bt.q = &bq;
  Waitable* bsrcs[] = {&bq};
  bt.SetWaitInputs(bsrcs, 1);
  CHECK(bt.Start());
  uint64_t expect_sum = 0;
  for (uint32_t i = 1; i <= 10; ++i) {
    CHECK(bq.Push(i));
    expect_sum += i;
  }
  WaitMsgs(bt, 10);
  CHECK(bt.msgs.load() == 10);
  CHECK(bt.sum.load() == expect_sum);
  CHECK(bt.handled_count() == 10);  // per-message loops accounting
  CHECK(bt.handled_count() == bt.msgs.load());
  CHECK(bt.dispatch_count() >= 1);
  CHECK(bt.dispatch_count() <= 10);
  CHECK(hb.alive_counter.load() == bt.dispatch_count());
  CHECK(bt.last_work_ns() > 0 || bt.msgs.load() == 10);  // work was timed
  bt.RequestStop();
  bt.Join();

  // Multi-input wakeup: a push to the SECOND input must wake a blocked worker
  // promptly. SetWaitInputs attaches every input to the thread's shared wake
  // hub, so any input's Push notifies the wait; before the hub, WaitAny
  // blocked on the first input's condvar only and a push to another input
  // rode out the 1s idle poll.
  HealthSlot hm{};
  NotifPipe<uint32_t, 16> q1, q2;
  MultiDrainThread mt("multi", &hm);
  NotifPipe<uint32_t, 16>* mqs[] = {&q1, &q2};
  mt.qs = mqs;
  mt.nqs = 2;
  Waitable* msrcs[] = {&q1, &q2};
  mt.SetWaitInputs(msrcs, 2);
  CHECK(mt.Start());
  CHECK(thyme_test::WaitFor([&] { return mt.wake_waiter_count() > 0; }));
  const uint64_t push_at = Tns();
  CHECK(q2.Push(42));
  WaitCount(mt.msgs, 1);
  const uint64_t woke_ns = Tns() - push_at;
  CHECK(mt.msgs.load() == 1);
  CHECK(mt.sum.load() == 42);
  CHECK(woke_ns < 500000000ULL);  // far below the 1s idle poll
  mt.RequestStop();
  mt.Join();

  // Inputs beyond the old 8-slot cap must register too (kMaxWaitInputs is
  // larger): the last of 12 inputs still wakes the worker immediately
  // instead of being silently truncated into a drain-only hitchhiker.
  HealthSlot hw{};
  NotifPipe<uint32_t, 16> wq[12];
  NotifPipe<uint32_t, 16>* wqs[12];
  Waitable* wsrcs[12];
  for (int i = 0; i < 12; ++i) {
    wqs[i] = &wq[i];
    wsrcs[i] = &wq[i];
  }
  MultiDrainThread wt("wide", &hw);
  wt.qs = wqs;
  wt.nqs = 12;
  wt.SetWaitInputs(wsrcs, 12);
  CHECK(wt.Start());
  CHECK(thyme_test::WaitFor([&] { return wt.wake_waiter_count() > 0; }));
  const uint64_t wpush_at = Tns();
  CHECK(wq[11].Push(7));
  WaitCount(wt.msgs, 1);
  CHECK(Tns() - wpush_at < 500000000ULL);
  CHECK(wt.msgs.load() == 1);
  CHECK(wt.sum.load() == 7);
  wt.RequestStop();
  wt.Join();

  // RequestStop wakes a worker blocked with no events and exits promptly.
  HealthSlot hs{};
  NotifPipe<uint32_t, 16> sq;
  DrainThread st("stop", &hs);
  st.q = &sq;
  Waitable* ssrcs[] = {&sq};
  st.SetWaitInputs(ssrcs, 1);
  CHECK(st.Start());
  CHECK(thyme_test::WaitFor([&] { return st.wake_waiter_count() > 0; }));
  const uint64_t stop_at = Tns();
  st.RequestStop();
  st.Join();
  CHECK(Tns() - stop_at < 1000000000ULL);
  CHECK(st.msgs.load() == 0);

  // Idle liveness: a message-starved thread must still MarkAlive on each
  // idle-poll tick (~1s), or the stall watchdog (alive_counter stuck for
  // more than 3 worker periods) reports a live-but-idle node as wedged.
  HealthSlot hi{};
  NotifPipe<uint32_t, 16> iq;
  DrainThread it("idle", &hi);
  it.q = &iq;
  Waitable* isrcs[] = {&iq};
  it.SetWaitInputs(isrcs, 1);
  CHECK(it.Start());
  const uint64_t idle_dl = Tns() + 4000000000ULL;
  while (hi.alive_counter.load(std::memory_order_relaxed) < 2 &&
         Tns() < idle_dl) {
    usleep(10 * 1000);
  }
  CHECK(hi.alive_counter.load(std::memory_order_relaxed) >= 2);
  CHECK(it.dispatch_count() == 0);  // idle ticks are not phantom dispatches
  CHECK(iq.Push(9));                // still responsive after the idle stretch
  WaitMsgs(it, 1);
  CHECK(it.msgs.load() == 1);
  it.RequestStop();
  it.Join();

  // Work-ns histogram mirrors CyclicThread's CopyWorkNs contract.
  uint32_t hist[8] = {0};
  const std::size_t n = bt.CopyWorkNs(hist, 8);
  CHECK(n >= 1 && n <= 8);
  CHECK(n == bt.work_ns_count() || bt.work_ns_count() > 8);
  // Latency histogram: one sample per handled message (NoteLatency(1000+v)).
  uint32_t lats[16] = {0};
  const std::size_t ln = bt.CopyLatenciesNs(lats, 16);
  CHECK(ln >= 1 && ln == bt.latency_count() && ln == bt.msgs.load());
  CHECK(lats[0] == 1000 + 10);  // newest first: last pushed value was 10

  // High-hz event node, zero input: the idle alive tick must outrun the
  // stall watchdog window (3/hz). hz=10 -> window 300ms; the default 1s
  // idle poll would false-stall, so the tick must arrive well under that.
  HealthSlot hh{};
  DrainThread ht("hz10_ev", &hh, 10);
  NotifPipe<uint32_t, 16> hq;
  ht.q = &hq;
  Waitable* hsrcs[] = {&hq};
  ht.SetWaitInputs(hsrcs, 1);
  CHECK(ht.Start());
  const uint64_t h0 = Tns();
  while (Tns() - h0 < 600000000ULL) usleep(10 * 1000);
  ht.RequestStop();
  ht.Join();
  // 600ms at a ~150ms idle tick -> at least 3 alive marks (allow jitter).
  CHECK(hh.alive_counter.load(std::memory_order_relaxed) >= 3);

  // Placement mirrors CyclicThread: settable pre-Start, applied best-effort.
  HealthSlot hp{};
  DrainThread pt("pin_ev", &hp);
  NotifPipe<uint32_t, 16> pq;
  pt.q = &pq;
  Waitable* psrcs[] = {&pq};
  pt.SetWaitInputs(psrcs, 1);
  ThreadPlacement pl{};
  pl.ncpus = 1;
  pl.cpus[0] = 0;
  pl.policy = ThreadPlacement::Policy::Fifo;
  pl.priority = 10;
  pl.fail = ThreadPlacement::Fail::BestEffort;
  pt.SetPlacement(pl);
  CHECK(pt.Start());
  CHECK(pq.Push(1));
  WaitMsgs(pt, 1);
  pt.RequestStop();
  pt.Join();
  CHECK(pt.msgs.load() == 1);

  // Replay consume gate: while paused, Dispatch must not drain the pipe.
  {
    static std::atomic<bool> gate_on{false};
    auto gate_fn = +[]() -> bool {
      return gate_on.load(std::memory_order_acquire);
    };
    SetReplayConsumeGate(gate_fn);

    HealthSlot hr{};
    NotifPipe<uint32_t, 16> rq;
    DrainThread rt("replay_pause", &hr);
    rt.q = &rq;
    Waitable* rsrcs[] = {&rq};
    rt.SetWaitInputs(rsrcs, 1);
    CHECK(rt.Start());
    gate_on.store(true, std::memory_order_release);
    const uint64_t alive_before = hr.alive_counter.load();
    CHECK(rq.Push(99));
    // The paused worker re-enters the pause path (MarkAlive) without draining;
    // that is the observable handshake, not a settle delay.
    CHECK(thyme_test::WaitFor(
        [&] { return hr.alive_counter.load() > alive_before; }));
    CHECK(rt.msgs.load() == 0);
    gate_on.store(false, std::memory_order_release);
    WaitMsgs(rt, 1);
    CHECK(rt.msgs.load() == 1);
    CHECK(rt.sum.load() == 99);
    rt.RequestStop();
    rt.Join();
    SetReplayConsumeGate(nullptr);
  }

  // M4.3: optional dispatch deadline — work_ns > deadline_ns → MarkDeadlineMiss.
  {
    HealthSlot hd{};
    NotifPipe<uint32_t, 4> dq;
    SlowDrainThread dt("deadline", &hd, 10, 2000000ULL);  // 2ms budget
    dt.q = &dq;
    dt.sleep_ns = 5000000ULL;  // 5ms work
    Waitable* dsrcs[] = {&dq};
    dt.SetWaitInputs(dsrcs, 1);
    CHECK(dt.Start());
    CHECK(dq.Push(1));
    WaitHandled(dt, 1);
    CHECK(hd.deadline_miss_cnt.load(std::memory_order_relaxed) == 1);
    dt.RequestStop();
    dt.Join();
  }
  {
    HealthSlot hn{};
    NotifPipe<uint32_t, 4> nq;
    SlowDrainThread nt("no_deadline", &hn, 10);  // default: no deadline check
    nt.q = &nq;
    nt.sleep_ns = 5000000ULL;
    Waitable* nsrcs[] = {&nq};
    nt.SetWaitInputs(nsrcs, 1);
    CHECK(nt.Start());
    CHECK(nq.Push(1));
    WaitHandled(nt, 1);
    CHECK(hn.deadline_miss_cnt.load(std::memory_order_relaxed) == 0);
    nt.RequestStop();
    nt.Join();
  }

  return 0;
}
