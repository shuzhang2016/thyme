// replay backpressure: pause freezes inject, stop aborts PushOrWait.
//
// The replay runner (RunReplay) moved to the lab (plan 04); the thyme-side
// backpressure contract it honored lives in the replay_mode switches plus
// NotifPipe::PushOrWait. This test drives that contract directly: a small
// inject loop mirrors the runner's pause/stop gates and injects through
// PushOrWait with the replay-side gate, so pause/stop semantics stay covered
// after the runner left the core.
#include "thyme/check.h"
#include "thyme/app/replay_mode.h"
#include "thyme/runtime/event_thread.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/notif_pipe.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

std::atomic<int> g_injected{0};

void CountInject() {
  g_injected.fetch_add(1, std::memory_order_relaxed);
}

void StopAfterFirst() {
  g_injected.fetch_add(1, std::memory_order_relaxed);
  thyme::app::replay_mode::RequestStop();
}

// Mirrors the (now lab-owned) replay runner's inject loop: hold while
// replay_mode::Paused(), abort on StopRequested(), inject through the pipe via
// NotifPipe::PushOrWait with the replay-side gate. on_inject runs after each
// successful push so a stop-inject callback can abort the next iteration.
int InjectInto(thyme::NotifPipe<int, 4>& q, int entries, void (*on_inject)()) {
  using namespace thyme::app::replay_mode;
  int injected = 0;
  for (int i = 0; i < entries; ++i) {
    while (Paused() && !StopRequested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (StopRequested()) break;
    thyme::PushWaitGate gate = MakePushWaitGate(5000000ULL);
    if (!q.PushOrWait(i, gate)) break;
    if (on_inject) on_inject();
    ++injected;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  return injected;
}

class DrainOne final : public thyme::EventThread {
 public:
  thyme::NotifPipe<int, 4>* q = nullptr;
  std::atomic<int> popped{0};
  DrainOne() : EventThread("drain", &health_, 10) {}
 protected:
  void Dispatch() override {
    int v = 0;
    while (q->TryPop(v)) popped.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  thyme::HealthSlot health_{};
};

}  // namespace

int main() {
  using namespace thyme::app::replay_mode;

  CHECK(!Paused());
  CHECK(!StopRequested());
  SetPaused(true);
  CHECK(Paused());
  SetPaused(false);
  CHECK(!Paused());

  RequestStop();
  CHECK(StopRequested());
  ClearStop();
  CHECK(!StopRequested());

  // PushOrWait aborts when replay stop is requested.
  {
    thyme::NotifPipe<int, 2> q;
    ClearStop();
    SetActive(true);
    CHECK(q.Push(1));
    CHECK(q.Push(2));  // full

    std::thread stopper([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      RequestStop();
    });
    thyme::PushWaitGate gate = MakePushWaitGate(5000000ULL);
    CHECK(!q.PushOrWait(3, gate));
    stopper.join();
    SetActive(false);
    ClearStop();
  }

  // Pause holds the inject loop; resume finishes the rest.
  {
    thyme::NotifPipe<int, 4> q;
    g_injected.store(0);
    ClearStop();
    SetPaused(false);
    SetActive(true);

    std::thread runner([&] { InjectInto(q, 2, CountInject); });

    for (int i = 0; i < 200 && g_injected.load() < 1; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(g_injected.load() >= 1);
    SetPaused(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int at_pause = g_injected.load();
    CHECK(at_pause >= 1);
    CHECK(at_pause < 2);

    SetPaused(false);
    runner.join();
    CHECK(g_injected.load() == 2);
    CHECK(!Paused());
    SetActive(false);
    ClearStop();
  }

  // RequestStop aborts the inject loop before all entries are injected.
  {
    thyme::NotifPipe<int, 4> q;
    g_injected.store(0);
    ClearStop();
    SetPaused(false);
    SetActive(true);
    const int injected = InjectInto(q, 2, StopAfterFirst);
    CHECK(injected == 1);
    CHECK(g_injected.load() == 1);
    SetActive(false);
    ClearStop();
  }

  // EventThread registered gate: global replay pause freezes pipe drain.
  {
    thyme::NotifPipe<int, 4> q;
    DrainOne th;
    th.q = &q;
    thyme::Waitable* srcs[] = {&q};
    th.SetWaitInputs(srcs, 1);
    SetActive(true);
    SetPaused(true);
    CHECK(th.Start());
    CHECK(q.Push(7));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(th.popped.load() == 0);
    SetPaused(false);
    for (int i = 0; i < 200 && th.popped.load() < 1; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(th.popped.load() == 1);
    th.RequestStop();
    th.Join();
    SetActive(false);
    ClearStop();
  }

  std::printf("test_replay_backpressure ok\n");
  return 0;
}
