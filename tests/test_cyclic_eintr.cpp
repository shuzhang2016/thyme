#include "thyme/check.h"
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/health_slot.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#if !defined(_WIN32)
#include <pthread.h>
#include <unistd.h>
#endif

using namespace thyme;


namespace {
#if !defined(_WIN32)
class EintrThread final : public CyclicThread {
 public:
  using CyclicThread::CyclicThread;
  std::atomic<pthread_t> self{};
  std::atomic<uint32_t> loops{0};

 protected:
  void Loop() override {
    pthread_t me = pthread_self();
    self.store(me, std::memory_order_relaxed);
    loops.fetch_add(1, std::memory_order_relaxed);
  }
};
#endif
}  // namespace

// Signals interrupting clock_nanosleep must not cause an early wake: each
// EINTR early-return would run Loop() ahead of schedule and inflate the loop
// count well past what the period allows.
int main() {
#if defined(_WIN32)
  return 0;
#else
  std::signal(SIGUSR1, [](int) {});  // no-op handler so delivery is survivable

  HealthSlot health{};
  EintrThread th("eintr", 50, &health);  // 20 ms period
  CHECK(th.Start());

  // Wait for the worker to publish its pthread_t.
  pthread_t target{};
  const auto give_up =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < give_up) {
    target = th.self.load(std::memory_order_relaxed);
    if (target != pthread_t{}) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(target != pthread_t{});

  std::atomic<bool> sig_stop{false};
  std::thread signaler([&]() {
    for (int i = 0; i < 40 && !sig_stop.load(std::memory_order_relaxed); ++i) {
      pthread_kill(target, SIGUSR1);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  th.RequestStop();
  th.Join();
  sig_stop.store(true, std::memory_order_relaxed);
  signaler.join();

  // 250 ms at 50 Hz is ~12 iterations. With ~40 signals in flight, an
  // EINTR-unsafe SleepUntil would add tens of extra Loop() runs.
  const uint32_t n = th.loops.load(std::memory_order_relaxed);
  CHECK(n >= 5);
  CHECK(n <= 20);
  return 0;
#endif
}
