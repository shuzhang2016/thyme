// SCH-04: overrun policy behaviour for cyclic and event workers.
//
// Each policy is observable independent of the SCH-08 reason classification:
// one reason counter and one policy-action counter increment per miss, and
// the action changes cadence/cooldown (degrade), calls the restart hook
// (restart), or leaves the cadence alone (skip). Timing bounds are generous:
// this is a unit test, not an SLO measurement.
#include "thyme/check.h"
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/event_thread.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/overrun.h"

#include <atomic>
#include <cstdint>
#include <ctime>
#include <string>
#include <unistd.h>

using namespace thyme;

namespace {

uint64_t Tns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

void SleepNs(uint64_t ns) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
  nanosleep(&ts, nullptr);
}

void WaitFor(bool (*pred)(void*), void* ctx, uint64_t timeout_ns) {
  const uint64_t deadline = Tns() + timeout_ns;
  while (!pred(ctx) && Tns() < deadline) usleep(1000);
}

uint64_t SumReasons(const CyclicThread& t) {
  return t.overrun_long_callback_count() + t.overrun_queue_backlog_count() +
         t.overrun_os_preemption_count();
}
uint64_t SumReasons(const EventThread& t) {
  return t.overrun_long_callback_count() + t.overrun_queue_backlog_count() +
         t.overrun_os_preemption_count();
}

class CyclicPolicyThread final : public CyclicThread {
 public:
  std::atomic<uint64_t> work_ns{0};
  std::atomic<uint32_t> slow_calls{0};
  std::atomic<uint64_t> restarts{0};
  std::atomic<uint64_t> calls{0};
  using CyclicThread::CyclicThread;

 protected:
  void Loop() override {
    calls.fetch_add(1, std::memory_order_relaxed);
    uint64_t ns = work_ns.load(std::memory_order_relaxed);
    if (slow_calls.load(std::memory_order_relaxed) > 0) {
      slow_calls.fetch_sub(1, std::memory_order_relaxed);
      ns = 15000000ull;  // 15 ms: over the 10 ms period
    }
    SleepNs(ns);
  }
  void OnOverrunRestart() override {
    restarts.fetch_add(1, std::memory_order_relaxed);
  }
};

class EventPolicyThread final : public EventThread {
 public:
  NotifPipe<uint32_t, 16>* q = nullptr;
  uint64_t sleep_ns = 0;
  std::atomic<uint64_t> msgs{0};
  std::atomic<uint64_t> restarts{0};
  using EventThread::EventThread;

 protected:
  void Dispatch() override {
    uint32_t v = 0;
    if (q->TryPop(v)) {
      SleepNs(sleep_ns);
      NoteHandled();
      msgs.fetch_add(1, std::memory_order_relaxed);
    }
  }
  void OnOverrunRestart() override {
    restarts.fetch_add(1, std::memory_order_relaxed);
  }
};

bool CyclicDegraded(void* ctx) {
  return static_cast<CyclicThread*>(ctx)->degraded();
}
bool CyclicRecovered(void* ctx) {
  return !static_cast<CyclicThread*>(ctx)->degraded();
}

}  // namespace

int main() {
  overrun_session::ResetForTest();

  // ---- cyclic skip (default) -------------------------------------------
  {
    HealthSlot health{};
    CyclicPolicyThread th("cyc_skip", 100, &health);  // 10 ms period
    th.SetOverrunPolicy(OverrunPolicy::kSkip);
    th.work_ns.store(15000000ull);
    const uint64_t start = Tns();
    CHECK(th.Start());
    while (Tns() - start < 120000000ULL) usleep(1000);
    th.RequestStop();
    th.Join();
    CHECK(th.loop_count() >= 3);
    CHECK(health.deadline_miss_cnt.load() > 0);
    CHECK(th.overrun_skip_actions() == health.deadline_miss_cnt.load());
    CHECK(th.overrun_degrade_actions() == 0);
    CHECK(th.overrun_restart_actions() == 0);
    CHECK(SumReasons(th) == health.deadline_miss_cnt.load());
    CHECK(!th.degraded());
    CHECK(th.effective_period_ns() == 10000000ull);
    // No busy loop: the worker is bounded by its 15 ms work, not spinning.
    CHECK(th.loop_count() < 20);
    CHECK(th.restarts.load() == 0);
  }

  // ---- cyclic degrade: period doubles, then recovers -------------------
  {
    HealthSlot health{};
    CyclicPolicyThread th("cyc_degrade", 100, &health);
    th.SetOverrunPolicy(OverrunPolicy::kDegrade);
    th.slow_calls.store(1);  // one 15 ms call over a 10 ms period
    CHECK(th.Start());
    WaitFor(CyclicDegraded, &th, 2000000000ULL);
    CHECK(th.degraded());
    CHECK(th.effective_period_ns() == 20000000ull);
    CHECK(th.overrun_degrade_actions() >= 1);
    CHECK(th.overrun_skip_actions() == 0);
    // Clean cycles recover the cadence after kDegradeRecoverCycles batches.
    WaitFor(CyclicRecovered, &th, 2000000000ULL);
    CHECK(!th.degraded());
    CHECK(th.effective_period_ns() == 10000000ull);
    th.RequestStop();
    th.Join();
    CHECK(th.overrun_degrade_actions() ==
          health.deadline_miss_cnt.load());
    CHECK(SumReasons(th) == health.deadline_miss_cnt.load());
  }

  // ---- cyclic restart: hook runs on the worker, cadence re-arms --------
  {
    HealthSlot health{};
    CyclicPolicyThread th("cyc_restart", 200, &health);  // 5 ms period
    th.SetOverrunPolicy(OverrunPolicy::kRestart);
    th.work_ns.store(8000000ull);  // always over budget
    const uint64_t start = Tns();
    CHECK(th.Start());
    while (Tns() - start < 100000000ULL) usleep(1000);
    th.RequestStop();
    th.Join();
    CHECK(health.deadline_miss_cnt.load() > 0);
    CHECK(th.overrun_restart_actions() == health.deadline_miss_cnt.load());
    CHECK(th.restarts.load() == th.overrun_restart_actions());
    CHECK(th.overrun_skip_actions() == 0);
    CHECK(th.overrun_degrade_actions() == 0);
    CHECK(SumReasons(th) == health.deadline_miss_cnt.load());
    CHECK(!th.degraded());
  }

  // ---- event skip ------------------------------------------------------
  {
    HealthSlot health{};
    NotifPipe<uint32_t, 16> q;
    EventPolicyThread th("ev_skip", &health, 10, 2000000ULL);  // 2 ms budget
    th.q = &q;
    th.sleep_ns = 5000000ULL;  // 5 ms per message
    Waitable* srcs[] = {&q};
    th.SetWaitInputs(srcs, 1);
    th.SetOverrunPolicy(OverrunPolicy::kSkip);
    CHECK(th.Start());
    CHECK(q.Push(1));
    CHECK(q.Push(2));
    CHECK(q.Push(3));
    const uint64_t deadline = Tns() + 3000000000ULL;
    while (th.msgs.load() < 3 && Tns() < deadline) usleep(1000);
    th.RequestStop();
    th.Join();
    CHECK(th.msgs.load() == 3);
    CHECK(health.deadline_miss_cnt.load() == 3);
    CHECK(th.overrun_skip_actions() == 3);
    CHECK(th.overrun_degrade_actions() == 0);
    CHECK(th.overrun_restart_actions() == 0);
    CHECK(SumReasons(th) == 3);
    CHECK(th.dispatch_count() == 3);  // one message per batch, no spin
  }

  // ---- event degrade: cooldown between over-budget batches -------------
  {
    HealthSlot health{};
    NotifPipe<uint32_t, 16> q;
    EventPolicyThread th("ev_degrade", &health, 10, 2000000ULL);
    th.q = &q;
    th.sleep_ns = 5000000ULL;
    Waitable* srcs[] = {&q};
    th.SetWaitInputs(srcs, 1);
    th.SetOverrunPolicy(OverrunPolicy::kDegrade);
    const uint64_t start = Tns();
    CHECK(th.Start());
    CHECK(q.Push(1));
    CHECK(q.Push(2));
    CHECK(q.Push(3));
    const uint64_t deadline = Tns() + 3000000000ULL;
    while (th.msgs.load() < 3 && Tns() < deadline) usleep(1000);
    const uint64_t elapsed = Tns() - start;
    CHECK(th.degraded());  // every batch still misses -> no recovery
    th.RequestStop();
    th.Join();
    CHECK(th.msgs.load() == 3);
    CHECK(th.overrun_degrade_actions() == 3);
    CHECK(th.overrun_skip_actions() == 0);
    CHECK(health.deadline_miss_cnt.load() == 3);
    // 3 x 5 ms work + at least two 2 ms cooldowns; generous lower bound.
    CHECK(elapsed >= 15000000ULL);
    CHECK(th.dispatch_count() < 10);  // cooldown is a sleep, not a busy loop
  }

  // ---- event restart: hook once per miss -------------------------------
  {
    HealthSlot health{};
    NotifPipe<uint32_t, 16> q;
    EventPolicyThread th("ev_restart", &health, 10, 2000000ULL);
    th.q = &q;
    th.sleep_ns = 5000000ULL;
    Waitable* srcs[] = {&q};
    th.SetWaitInputs(srcs, 1);
    th.SetOverrunPolicy(OverrunPolicy::kRestart);
    CHECK(th.Start());
    CHECK(q.Push(1));
    CHECK(q.Push(2));
    const uint64_t deadline = Tns() + 3000000000ULL;
    while (th.msgs.load() < 2 && Tns() < deadline) usleep(1000);
    th.RequestStop();
    th.Join();
    CHECK(th.msgs.load() == 2);
    CHECK(th.overrun_restart_actions() == 2);
    CHECK(th.restarts.load() == 2);
    CHECK(th.overrun_skip_actions() == 0);
    CHECK(health.deadline_miss_cnt.load() == 2);
    CHECK(SumReasons(th) == 2);
    CHECK(!th.degraded());
  }

  // ---- policy parser + session marker ----------------------------------
  {
    OverrunPolicy p = OverrunPolicy::kSkip;
    CHECK(ParseOverrunPolicy("skip", &p) && p == OverrunPolicy::kSkip);
    CHECK(ParseOverrunPolicy("degrade", &p) && p == OverrunPolicy::kDegrade);
    CHECK(ParseOverrunPolicy("restart", &p) && p == OverrunPolicy::kRestart);
    CHECK(!ParseOverrunPolicy("yolo", &p));
    CHECK(ParseOverrunPolicy("", &p) && p == OverrunPolicy::kSkip);

    CHECK(overrun_session::count() > 0);
    CHECK(overrun_session::last_policy() == OverrunPolicy::kRestart);
    CHECK(std::string(OverrunPolicyName(OverrunPolicy::kDegrade)) ==
          "degrade");
  }

  return 0;
}
