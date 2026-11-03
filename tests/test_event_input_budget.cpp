// SCH-03: per-input dispatch budget. Two inputs at different rates (30
// messages/cycle vs 2) share one EventThread; with a budget the fast input is
// capped per cycle so the slow input is served in the first batch instead of
// waiting for the whole fast queue to drain.
#include "thyme/check.h"
#include "thyme/runtime/event_thread.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/notif_pipe.h"

#include <atomic>
#include <cstdint>
#include <ctime>
#include <unistd.h>

using namespace thyme;

namespace {

uint64_t Tns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

void WaitMsgs(const std::atomic<uint64_t>& cnt, uint64_t want) {
  const uint64_t deadline = Tns() + 3000000000ULL;
  while (cnt.load(std::memory_order_relaxed) < want && Tns() < deadline) {
    usleep(1000);
  }
}

class BudgetThread final : public EventThread {
 public:
  NotifPipe<uint32_t, 64>* qs[2] = {nullptr, nullptr};
  std::atomic<uint64_t> msgs{0};
  std::atomic<uint32_t> max_per_cycle[2] = {{0}, {0}};
  // Messages consumed from input 0 before input 1 was first served.
  std::atomic<uint32_t> fast_before_first_slow{0};
  bool saw_slow = false;
  using EventThread::EventThread;

 protected:
  void Dispatch() override {
    for (std::size_t i = 0; i < 2; ++i) {
      uint32_t n = 0;
      uint32_t v = 0;
      // Same protocol the generated Dispatch emits when input_budget > 0:
      // budget check before TryPop, consume only after a successful pop.
      while (InputBudgetAvailable(i) && qs[i]->TryPop(v)) {
        ConsumeInputBudget(i);
        if (i == 1 && !saw_slow) {
          saw_slow = true;
          fast_before_first_slow.store(
              static_cast<uint32_t>(msgs.load(std::memory_order_relaxed)));
        }
        NoteHandled();
        ++n;
        msgs.fetch_add(1, std::memory_order_relaxed);
      }
      uint32_t prev = max_per_cycle[i].load(std::memory_order_relaxed);
      while (n > prev &&
             !max_per_cycle[i].compare_exchange_weak(
                 prev, n, std::memory_order_relaxed)) {
      }
    }
  }
};

}  // namespace

int main() {
  HealthSlot health{};
  NotifPipe<uint32_t, 64> fast_q;
  NotifPipe<uint32_t, 64> slow_q;
  BudgetThread th("budget", &health, 100);
  th.qs[0] = &fast_q;
  th.qs[1] = &slow_q;
  Waitable* srcs[] = {&fast_q, &slow_q};
  th.SetWaitInputs(srcs, 2);
  th.SetInputBudget(4, 2);
  CHECK(th.input_budget() == 4);
  CHECK(th.input_budget_slots() == 2);

  for (uint32_t i = 0; i < 30; ++i) CHECK(fast_q.Push(i));
  CHECK(slow_q.Push(100));
  CHECK(slow_q.Push(101));

  CHECK(th.Start());
  WaitMsgs(th.msgs, 32);
  th.RequestStop();
  th.Join();

  CHECK(th.msgs.load() == 32);
  // No dispatch consumed more than the per-input cap.
  CHECK(th.max_per_cycle[0].load() <= 4);
  CHECK(th.max_per_cycle[1].load() <= 2);
  // The slow input was served before the fast queue drained: without the
  // budget the first batch would consume all 30 fast messages first.
  CHECK(th.fast_before_first_slow.load() <= 4);
  // Exhaustion is counted once per capped dispatch cycle, per input. The
  // slow input (2 messages < 4) is never capped.
  CHECK(th.input_budget_exhausted_count(0) >= 1);
  CHECK(th.input_budget_exhausted_count(1) == 0);
  CHECK(th.input_budget_exhausted_total() ==
        th.input_budget_exhausted_count(0) +
            th.input_budget_exhausted_count(1));
  CHECK(th.input_budget_exhausted_count(2) == 0);  // out of range
  CHECK(health.drop_cnt.load() == 0);

  // Default (0) keeps the old drain-everything behaviour and has no budget
  // counters: the first batch takes all fast messages before the slow input.
  HealthSlot unlimited_health{};
  NotifPipe<uint32_t, 64> fq2;
  NotifPipe<uint32_t, 64> sq2;
  BudgetThread th2("unlimited", &unlimited_health, 100);
  th2.qs[0] = &fq2;
  th2.qs[1] = &sq2;
  Waitable* srcs2[] = {&fq2, &sq2};
  th2.SetWaitInputs(srcs2, 2);
  for (uint32_t i = 0; i < 12; ++i) CHECK(fq2.Push(i));
  CHECK(sq2.Push(200));
  CHECK(th2.Start());
  WaitMsgs(th2.msgs, 13);
  th2.RequestStop();
  th2.Join();
  CHECK(th2.input_budget() == 0);
  CHECK(th2.fast_before_first_slow.load() == 12);
  CHECK(th2.input_budget_exhausted_total() == 0);

  return 0;
}
