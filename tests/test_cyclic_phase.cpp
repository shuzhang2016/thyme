// test_cyclic_phase.cpp — SCH-13 first-wake alignment.
//
// AlignCyclicDeadline is the contract (mod period, late start does not
// catch up). CyclicThread applies it only when phase_mode != Off; default
// Off keeps now+period.
#include "thyme/check.h"
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/thread_placement.h"
#include "test_wait.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

using namespace thyme;

namespace {

class StampThread final : public CyclicThread {
 public:
  using CyclicThread::CyclicThread;
  std::atomic<uint64_t> first_ns{0};
  thyme_test::EventFd first_loop;

 protected:
  void Loop() override {
    uint64_t expected = 0;
    const uint64_t now = thyme_test::MonoNs();
    if (first_ns.compare_exchange_strong(expected, now)) {
      first_loop.Signal();
    }
  }
};

void TestAlignMath() {
  CHECK(AlignCyclicDeadline(1000, 0, 100, 1000) == 1000);
  CHECK(AlignCyclicDeadline(1000, 30, 100, 1000) == 1030);
  CHECK(AlignCyclicDeadline(1000, 130, 100, 1000) == 1030);
  CHECK(AlignCyclicDeadline(1000, 0, 100, 1030) == 1100);
  CHECK(AlignCyclicDeadline(1000, 0, 100, 1100) == 1100);
  CHECK(AlignCyclicDeadline(1000, 40, 100, 1150) == 1240);
  CHECK(AlignCyclicDeadline(1000, 0, 0, 999) == 999);
}

void TestDefaultOffWaitsOnePeriod() {
  HealthSlot health{};
  StampThread th("off", 50, &health);
  const uint64_t t0 = thyme_test::MonoNs();
  CHECK(th.Start());
  CHECK(th.first_loop.Wait(1000000000ULL));
  th.RequestStop();
  th.Join();
  const uint64_t first = th.first_ns.load();
  CHECK(first >= t0 + 10000000ULL);
}

void TestLateStartDoesNotCatchUp() {
  HealthSlot health{};
  StampThread th("late", 20, &health);
  const uint64_t epoch = thyme_test::MonoNs();
  ThreadPlacement p{};
  p.phase_mode = ThreadPlacement::PhaseMode::BestEffort;
  p.phase_ns = 0;
  p.start_epoch_ns = epoch;
  th.SetPlacement(p);
  thyme_test::SleepNs(10000000ULL);
  CHECK(th.Start());
  CHECK(th.first_loop.Wait(1000000000ULL));
  th.RequestStop();
  th.Join();
  const uint64_t first = th.first_ns.load();
  CHECK(first >= epoch + 40000000ULL);
  CHECK(first <= epoch + 80000000ULL);
}

void TestStaggeredFirstWake() {
  HealthSlot ha{};
  HealthSlot hb{};
  StampThread a("phase0", 20, &ha);
  StampThread b("phase20", 20, &hb);
  const uint64_t epoch = thyme_test::MonoNs();
  ThreadPlacement pa{};
  pa.phase_mode = ThreadPlacement::PhaseMode::BestEffort;
  pa.phase_ns = 0;
  pa.start_epoch_ns = epoch;
  ThreadPlacement pb = pa;
  pb.phase_ns = 20000000ULL;
  a.SetPlacement(pa);
  b.SetPlacement(pb);
  CHECK(a.Start());
  CHECK(b.Start());
  CHECK(a.first_loop.Wait(1000000000ULL));
  CHECK(b.first_loop.Wait(1000000000ULL));
  a.RequestStop();
  b.RequestStop();
  a.Join();
  b.Join();
  const uint64_t fa = a.first_ns.load();
  const uint64_t fb = b.first_ns.load();
  CHECK(fa >= epoch);
  CHECK(fb >= epoch);
  const uint64_t period = 50000000ULL;
  const uint64_t ma = (fa - epoch) % period;
  const uint64_t mb = (fb - epoch) % period;
  CHECK(ma < 12000000ULL || ma > 38000000ULL);
  CHECK(mb >= 8000000ULL && mb <= 32000000ULL);
}

}  // namespace

int main() {
  TestAlignMath();
  TestDefaultOffWaitsOnePeriod();
  TestLateStartDoesNotCatchUp();
  TestStaggeredFirstWake();
  std::printf("test_cyclic_phase OK\n");
  return 0;
}
