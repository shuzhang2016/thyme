#include "thyme/check.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/notif_pipe.h"
#include "test_wait.h"
#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <ctime>

using namespace thyme;


static uint64_t Tns() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

struct WaitArg { NotifPipe<uint32_t, 8>* q; uint64_t deadline; };
static void* Waiter(void* p) {
  auto* a = static_cast<WaitArg*>(p);
  a->q->WaitUntil(a->deadline);
  return nullptr;
}

struct NotifyArg { NotifPipe<uint32_t, 8>* q; int n; };
static void* NotifySpam(void* p) {
  auto* a = static_cast<NotifyArg*>(p);
  for (int i = 0; i < a->n; ++i) a->q->Notify();
  return nullptr;
}

struct PushArg { NotifPipe<uint32_t, 8>* q; std::atomic<bool>* stop; };
static void* PushLoop(void* vp) {
  auto* a = static_cast<PushArg*>(vp);
  uint32_t n = 0;
  while (!a->stop->load(std::memory_order_relaxed)) {
    if (a->q->Push(++n)) {}
  }
  return nullptr;
}

// Records Notify() calls; stands in for the EventThread wake hub.
struct RecWaitable : Waitable {
  std::atomic<int> notifies{0};
  bool empty() const override { return true; }
  void WaitUntil(uint64_t) override {}
  void Notify() override { notifies.fetch_add(1, std::memory_order_relaxed); }
};

int main() {
  // Full-delivery FIFO: every pushed value arrives, in order.
  NotifPipe<uint32_t, 8> q;
  uint32_t v = 0;
  CHECK(q.empty());
  CHECK(!q.TryPop(v));
  for (uint32_t i = 0; i < 8; ++i) CHECK(q.Push(i + 1));
  for (uint32_t i = 0; i < 8; ++i) {
    CHECK(q.TryPop(v));
    CHECK(v == i + 1);
  }
  CHECK(q.empty());

  // Full: Push returns false, nothing dropped.
  for (uint32_t i = 0; i < 8; ++i) CHECK(q.Push(i));
  CHECK(!q.Push(99));
  CHECK(q.TryPop(v));
  CHECK(q.Push(99));  // room again

  NotifPipe<uint32_t, 8> drain;
  while (drain.TryPop(v)) {}

  // WaitUntil wakes promptly on Push.
  NotifPipe<uint32_t, 8> wq;
  WaitArg arg{&wq, Tns() + 5000000000ULL};
  pthread_t th{};
  CHECK(pthread_create(&th, nullptr, Waiter, &arg) == 0);
  CHECK(thyme_test::WaitFor([&] { return wq.waiter_count() > 0; }));
  const uint64_t push_at = Tns();
  wq.Push(7);
  CHECK(pthread_join(th, nullptr) == 0);
  CHECK(Tns() - push_at < 2000000000ULL);
  CHECK(wq.TryPop(v) && v == 7);

  // WaitUntil times out on its own when nothing is pushed.
  NotifPipe<uint32_t, 8> tq;
  const uint64_t dl = Tns() + 100000000ULL;
  tq.WaitUntil(dl);
  CHECK(Tns() >= dl);

  // Notify is idempotent: a waiter wakes on bare Notify, and concurrent
  // Notify spam plus timeout rounds never wedge or crash the pipe.
  NotifPipe<uint32_t, 8> nq;
  for (int round = 0; round < 5; ++round) {
    WaitArg warg{&nq, Tns() + 2000000000ULL};
    pthread_t wth{}, nth{};
    NotifyArg narg{&nq, 50};
    CHECK(pthread_create(&wth, nullptr, Waiter, &warg) == 0);
    CHECK(thyme_test::WaitFor([&] { return nq.waiter_count() > 0; }));
    CHECK(pthread_create(&nth, nullptr, NotifySpam, &narg) == 0);
    nq.Notify();
    nq.Notify();
    CHECK(pthread_join(wth, nullptr) == 0);
    CHECK(pthread_join(nth, nullptr) == 0);
    CHECK(nq.empty());
  }

  // SetNotify: a successful Push additionally notifies the attached target
  // (EventThread wires every input pipe to its shared wake hub this way).
  NotifPipe<uint32_t, 8> hq;
  RecWaitable hub;
  CHECK(hq.Push(1));  // no hook attached: plain push, nothing to notify
  hq.SetNotify(&hub);
  CHECK(hub.notifies.load(std::memory_order_relaxed) == 0);
  CHECK(hq.Push(2));
  CHECK(hub.notifies.load(std::memory_order_relaxed) == 1);
  uint32_t got = 0;
  CHECK(hq.TryPop(got) && got == 1);
  CHECK(hq.TryPop(got) && got == 2);

  // A failed (full) push enqueues nothing, so it must not notify either.
  NotifPipe<uint32_t, 8> fq;
  RecWaitable hub2;
  fq.SetNotify(&hub2);
  for (uint32_t i = 0; i < 8; ++i) CHECK(fq.Push(i));
  CHECK(hub2.notifies.load(std::memory_order_relaxed) == 8);
  CHECK(!fq.Push(99));
  CHECK(hub2.notifies.load(std::memory_order_relaxed) == 8);

  // Detach notify during concurrent Push: consumer teardown must not race crash.
  NotifPipe<uint32_t, 8> dq;
  RecWaitable hub3;
  dq.SetNotify(&hub3);
  std::atomic<bool> stop{false};
  PushArg parg{&dq, &stop};
  pthread_t pth{};
  CHECK(pthread_create(&pth, nullptr, PushLoop, &parg) == 0);
  struct timespec slp2{0, 5L * 1000 * 1000};
  for (int i = 0; i < 20; ++i) {
    dq.SetNotify(i & 1 ? &hub3 : nullptr);
    nanosleep(&slp2, nullptr);
  }
  stop.store(true, std::memory_order_relaxed);
  CHECK(pthread_join(pth, nullptr) == 0);
  while (dq.TryPop(v)) {}

  // PushOrWait: blocks until consumer frees a slot; abort stops the wait.
  NotifPipe<uint32_t, 4> pwq;
  std::atomic<bool> pw_stop{false};
  struct PwArg {
    NotifPipe<uint32_t, 4>* q;
    std::atomic<bool>* done;
    std::atomic<bool>* stop;
  };
  auto* pw_done = new std::atomic<bool>{false};
  PwArg pw{&pwq, pw_done, &pw_stop};
  auto PushOrWaitLoop = +[](void* vp) -> void* {
    auto* a = static_cast<PwArg*>(vp);
    PushWaitGate gate;
    gate.aborted = +[](void* ctx) -> bool {
      return static_cast<std::atomic<bool>*>(ctx)->load(std::memory_order_relaxed);
    };
    gate.ctx = a->stop;
    gate.slice_ns = 50000000ULL;
    CHECK(a->q->PushOrWait(42, gate));
    a->done->store(true, std::memory_order_release);
    return nullptr;
  };
  for (uint32_t i = 0; i < 4; ++i) CHECK(pwq.Push(i));
  pthread_t pwth{};
  CHECK(pthread_create(&pwth, nullptr, PushOrWaitLoop, &pw) == 0);
  CHECK(thyme_test::WaitFor([&] { return pwq.slot_waiter_count() > 0; }));
  CHECK(!pw_done->load(std::memory_order_acquire));
  CHECK(pwq.TryPop(v));
  CHECK(pthread_join(pwth, nullptr) == 0);
  CHECK(pw_done->load(std::memory_order_acquire));
  bool saw42 = false;
  while (pwq.TryPop(v)) {
    if (v == 42) saw42 = true;
  }
  CHECK(saw42);
  delete pw_done;

  // PushOrWait returns false when aborted while full.
  NotifPipe<uint32_t, 4> awq;
  for (uint32_t i = 0; i < 4; ++i) CHECK(awq.Push(i));
  std::atomic<bool> aw_abort{false};
  std::atomic<bool> aw_done{false};
  PwArg aw{&awq, &aw_done, &aw_abort};
  auto AbortWait = +[](void* vp) -> void* {
    auto* a = static_cast<PwArg*>(vp);
    PushWaitGate gate;
    gate.aborted = +[](void* ctx) -> bool {
      return static_cast<std::atomic<bool>*>(ctx)->load(std::memory_order_relaxed);
    };
    gate.ctx = a->stop;
    gate.slice_ns = 20000000ULL;
    CHECK(!a->q->PushOrWait(99, gate));
    a->done->store(true, std::memory_order_release);
    return nullptr;
  };
  pthread_t awth{};
  CHECK(pthread_create(&awth, nullptr, AbortWait, &aw) == 0);
  CHECK(thyme_test::WaitFor([&] { return awq.slot_waiter_count() > 0; }));
  aw_abort.store(true, std::memory_order_relaxed);
  CHECK(pthread_join(awth, nullptr) == 0);
  CHECK(aw_done.load(std::memory_order_acquire));

  // M4.2: generated PublishAndFork credits producer health on full pipe.
  // Mirrors the generated helper's in-place shape (RT-16).
  NotifPipe<uint32_t, 4> pfq;
  HealthSlot health{};
  auto PublishAndFork = [&](NotifPipe<uint32_t, 4>& pipe, uint32_t val) {
    uint32_t* slot = pipe.BeginWrite();
    if (slot == nullptr) {
      health.AddDrops(1);
      return;
    }
    *slot = val;
    pipe.CommitWrite();
  };
  for (uint32_t i = 0; i < 4; ++i) PublishAndFork(pfq, i);
  CHECK(health.drop_cnt.load(std::memory_order_relaxed) == 0);
  PublishAndFork(pfq, 99);
  PublishAndFork(pfq, 100);
  CHECK(health.drop_cnt.load(std::memory_order_relaxed) == 2);
  uint32_t pop = 0;
  while (pfq.TryPop(pop)) {}
  PublishAndFork(pfq, 200);
  CHECK(health.drop_cnt.load(std::memory_order_relaxed) == 2);

  // RT-16: in-place publish delivers in order and notifies the attached hub
  // exactly once per CommitWrite (same protocol as Push).
  NotifPipe<uint32_t, 4> iq;
  RecWaitable hub4;
  iq.SetNotify(&hub4);
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t* slot = iq.BeginWrite();
    CHECK(slot != nullptr);
    *slot = i + 1;
    iq.CommitWrite();
  }
  CHECK(hub4.notifies.load(std::memory_order_relaxed) == 4);
  for (uint32_t i = 0; i < 4; ++i) {
    CHECK(iq.TryPop(v));
    CHECK(v == i + 1);
  }
  CHECK(iq.empty());

  // Full pipe: BeginWrite hands back nullptr (the failed-Push case) and
  // publishes/notifies nothing.
  NotifPipe<uint32_t, 2> fq2;
  RecWaitable hub5;
  fq2.SetNotify(&hub5);
  CHECK(fq2.BeginWrite() != nullptr);
  fq2.CommitWrite();
  CHECK(fq2.BeginWrite() != nullptr);
  fq2.CommitWrite();
  CHECK(hub5.notifies.load(std::memory_order_relaxed) == 2);
  CHECK(fq2.BeginWrite() == nullptr);
  CHECK(hub5.notifies.load(std::memory_order_relaxed) == 2);
  while (fq2.TryPop(v)) {}

  // Abort semantics: an un-committed slot is never visible to the consumer
  // and AbortWrite recycles it, so the pipe neither loses capacity nor
  // publishes half-filled data.
  NotifPipe<uint32_t, 2> aq;
  uint32_t* slot = aq.BeginWrite();
  CHECK(slot != nullptr);
  *slot = 77;
  aq.AbortWrite();
  CHECK(aq.empty());
  CHECK(!aq.TryPop(v));
  slot = aq.BeginWrite();  // capacity fully recovered
  CHECK(slot != nullptr);
  *slot = 88;
  aq.CommitWrite();
  CHECK(aq.BeginWrite() != nullptr);
  CHECK(aq.TryPop(v));
  CHECK(v == 88);
  aq.AbortWrite();  // recycles the in-flight BeginWrite above
  aq.AbortWrite();  // stray abort with nothing in flight is a no-op

  return 0;
}
