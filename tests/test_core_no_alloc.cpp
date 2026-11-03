// test_core_no_alloc.cpp — TST-13: zero-allocation sentinel for the core
// runtime after App::Start().
//
// Every global operator new/delete in this binary is replaced by a counting
// version; the counter is only active while "armed". The measured window
// covers N cyclic ticks after the App's threads are up and warmed, and each
// tick drives the two hot paths a generated node uses:
//   * a record tap write: RecordMirrorRingPage::TryWriteRing (what
//     RecordTap emits for a recorded struct), and
//   * an event path: NotifPipe -> EventThread::Dispatch.
// The assertion is that the window contains exactly 0 allocations.
//
// What is intentionally NOT measured:
//   * Startup. App::Start()/pthread_create allocate thread stacks and TLS, and
//     the rule (docs/reference/coding-rules.md CR-01, PERF-11) is "no allocation after
//     Prepare()/Start() on the hot path" — so the counter is armed only after
//     Start() and after the threads have run enough warm-up ticks for any
//     one-time lazy init to settle.
//   * Teardown. RequestStop()/Join() may allocate; the counter is disarmed
//     first.
//   * The recorder thread (TriggeredRecorder). Its drain path is covered
//     separately (REC-20); pulling it in here would need disk I/O and would
//     reintroduce per-sample vectors that are known allocations by design.
//
// The counter is process-global rather than per-thread: the core rule is
// process-wide, and a per-thread view would silently ignore threads this
// binary does not instrument. Residual blind spot: an allocation that starts
// before Arm() and completes after it is not counted (the armed check happens
// on entry).
//
// Timing: no sleeps are used for correctness. Progress is a busy-poll on the
// threads' atomic counters (sched_yield between polls); the monotonic deadline
// exists only to turn a hang into a clear failure.

#include "thyme/check.h"
#include "thyme/runtime/app.h"
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/event_thread.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <sched.h>
#include <unistd.h>

#ifdef __linux__
#include <link.h>
#endif

using namespace thyme;

namespace {

// ---------------------------------------------------------------------------
// Allocation sentinel state. Atomics only: the counter itself must never
// allocate, and it is read/written from every thread.
// ---------------------------------------------------------------------------
std::atomic<bool> g_armed{false};
std::atomic<std::uint64_t> g_allocs{0};
std::atomic<std::uintptr_t> g_first_pc{0};
std::atomic<std::uint64_t> g_first_size{0};

bool Armed() noexcept { return g_armed.load(std::memory_order_acquire); }

void NoteAllocation(std::size_t size, std::uintptr_t pc) noexcept {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  std::uintptr_t expected = 0;
  if (g_first_pc.compare_exchange_strong(expected, pc,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    g_first_size.store(static_cast<std::uint64_t>(size),
                       std::memory_order_relaxed);
  }
}

void Arm() {
  g_allocs.store(0, std::memory_order_relaxed);
  g_first_pc.store(0, std::memory_order_relaxed);
  g_first_size.store(0, std::memory_order_relaxed);
  g_armed.store(true, std::memory_order_release);
}

void Disarm() { g_armed.store(false, std::memory_order_release); }

std::uint64_t AllocationCount() {
  return g_allocs.load(std::memory_order_relaxed);
}

// malloc-backed allocation helpers for the replaced operators. They must not
// go through operator new (that would recurse).
void* Allocate(std::size_t size) {
  return std::malloc(size == 0 ? 1 : size);
}

void* AllocateAligned(std::size_t size, std::size_t alignment) {
  void* p = nullptr;
  const std::size_t align =
      alignment < sizeof(void*) ? sizeof(void*) : alignment;
  if (posix_memalign(&p, align, size == 0 ? 1 : size) != 0) return nullptr;
  return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// Global operator new/delete replacement (C++17: sized + aligned + nothrow).
// Each new records the return address of its caller so a failure can name the
// allocation site without a backtrace.
// ---------------------------------------------------------------------------

void* operator new(std::size_t size) {
  if (Armed()) {
    NoteAllocation(
        size, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
  }
  void* const p = Allocate(size);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

void* operator new[](std::size_t size) {
  if (Armed()) {
    NoteAllocation(
        size, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
  }
  void* const p = Allocate(size);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  if (Armed()) {
    NoteAllocation(
        size, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
  }
  return Allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  if (Armed()) {
    NoteAllocation(
        size, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
  }
  return Allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  if (Armed()) {
    NoteAllocation(
        size, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
  }
  void* const p =
      AllocateAligned(size, static_cast<std::size_t>(alignment));
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  if (Armed()) {
    NoteAllocation(
        size, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
  }
  void* const p =
      AllocateAligned(size, static_cast<std::size_t>(alignment));
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  if (Armed()) {
    NoteAllocation(
        size, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
  }
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  if (Armed()) {
    NoteAllocation(
        size, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
  }
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::align_val_t,
                      const std::nothrow_t&) noexcept {
  std::free(p);
}

namespace {

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
constexpr std::uint32_t kPeriodHz = 1000;  // 1 ms tick: fast and deterministic
constexpr std::uint64_t kWarmupTicks = 16;
constexpr std::uint64_t kWarmupHandled = 4;
constexpr std::uint64_t kMeasuredTicks = 50;
// Hang guard only: progress is polled, never slept on.
constexpr std::uint64_t kWaitBudgetNs = 5000000000ULL;

// Tap payload: a small POD, like a generated contract struct.
struct Pod {
  std::uint64_t seq;
  std::uint32_t b;
  std::uint32_t c;
};

std::uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

template <typename Pred>
bool WaitFor(Pred pred, std::uint64_t deadline_ns) {
  while (!pred()) {
    if (NowNs() >= deadline_ns) return false;
    sched_yield();
  }
  return true;
}

// Cyclic producer: one tap write into the record mirror ring plus one event
// message per tick. This is the hot path the sentinel is meant to police.
class TapCyclic final : public CyclicThread {
 public:
  TapCyclic(HealthSlot* health, RecordMirrorRingPage* mirror,
            NotifPipe<std::uint32_t, 8>* out)
      : CyclicThread("tst13_cyc", kPeriodHz, health),
        mirror_(mirror),
        out_(out) {}

  // Read only after Join (single-writer counters).
  std::uint64_t write_fail_count() const { return write_fail_; }
  std::uint64_t pushed_count() const { return pushed_; }

 protected:
  void Loop() override {
    ++seq_;
    const Pod sample{seq_, 0x10u, 0x20u};
    if (!mirror_->TryWriteRing(0, &sample,
                               static_cast<std::uint32_t>(sizeof(sample)),
                               NowNs(), static_cast<std::uint32_t>(seq_))) {
      ++write_fail_;
    }
    if (out_->Push(static_cast<std::uint32_t>(seq_))) ++pushed_;
  }

 private:
  RecordMirrorRingPage* mirror_;
  NotifPipe<std::uint32_t, 8>* out_;
  std::uint64_t seq_{0};
  std::uint64_t write_fail_{0};
  std::uint64_t pushed_{0};
};

class PipeDrain final : public EventThread {
 public:
  PipeDrain(HealthSlot* health, NotifPipe<std::uint32_t, 8>* in)
      : EventThread("tst13_evt", health, kPeriodHz), in_(in) {}

 protected:
  void Dispatch() override {
    std::uint32_t v = 0;
    while (in_->TryPop(v)) NoteHandled();
  }

 private:
  NotifPipe<std::uint32_t, 8>* in_;
};

// Load bias of the main executable, so a recorded PC can be printed as a
// symbol offset (`addr2line -e test_core_no_alloc <offset>`). Called only on
// the failure path, after Disarm(): it may allocate internally.
std::uintptr_t MainObjectBase() {
#ifdef __linux__
  std::uintptr_t base = 0;
  dl_iterate_phdr(
      [](struct dl_phdr_info* info, std::size_t, void* data) -> int {
        *static_cast<std::uintptr_t*>(data) =
            static_cast<std::uintptr_t>(info->dlpi_addr);
        return 1;  // the first entry is the main object; stop there
      },
      &base);
  return base;
#else
  return 0;
#endif
}

void PrintFirstSite(std::uintptr_t pc, std::uint64_t size) {
  const std::uintptr_t base = MainObjectBase();
  if (pc == 0) {
    std::fprintf(stderr, "TST-13: first allocation size unknown\n");
  } else if (base != 0) {
    std::fprintf(stderr,
                 "TST-13: first allocation: %" PRIu64
                 " bytes, pc = executable+0x%" PRIxPTR
                 " (resolve with: addr2line -e test_core_no_alloc 0x%" PRIxPTR
                 ")\n",
                 size, pc - base, pc - base);
  } else {
    std::fprintf(stderr,
                 "TST-13: first allocation: %" PRIu64 " bytes, pc = 0x%" PRIxPTR
                 "\n",
                 size, pc);
  }
}

}  // namespace

int main() {
  CHECK(App::Prepare());

  // --- Record mirror ring page: the tap target. Created before Start() so the
  // shm mapping and one-time Init stay outside the measured window.
  char shm_name[64] = {};
  std::snprintf(shm_name, sizeof(shm_name), "/tst13_mirror_%ld",
                static_cast<long>(getpid()));
  ShmRegion::UnlinkName(shm_name);

  ThymeStructDesc descs[1] = {};
  descs[0].struct_id = 0;
  descs[0].contract_version = 1;
  descs[0].payload_size = static_cast<std::uint32_t>(sizeof(Pod));
  std::snprintf(descs[0].name, sizeof(descs[0].name), "Tst13Pod");

  const RecordMirrorRingSpec specs[1] = {
      {static_cast<std::uint32_t>(sizeof(Pod)), 8}};
  const std::uint64_t page_bytes =
      RecordMirrorRingPage::RequiredBytes(descs, specs, 1);
  CHECK(page_bytes != 0);

  ShmRegion region;
  RecordMirrorRingPage* const mirror = region.Create<RecordMirrorRingPage>(
      shm_name, static_cast<std::size_t>(page_bytes));
  CHECK(mirror != nullptr);
  CHECK(mirror->Init(1, descs, specs, 1));
  CHECK(mirror->Validate());

  // --- Minimal App: one cyclic producer (tap + event message) and one event
  // consumer.
  HealthSlot cyc_health{};
  HealthSlot evt_health{};
  NotifPipe<std::uint32_t, 8> notif;
  TapCyclic cyc(&cyc_health, mirror, &notif);
  PipeDrain evt(&evt_health, &notif);
  Waitable* const evt_inputs[] = {&notif};
  evt.SetWaitInputs(evt_inputs, 1);

  App app;
  CHECK(app.Add(&evt));
  CHECK(app.Add(&cyc));
  CHECK(app.Start());

  // Warm-up: both threads are up and have run the hot path a few times, so
  // one-time lazy setup (thread/TLS/allocator paths, the event hub's first
  // futex wait, the first dispatch) has already happened.
  if (!WaitFor(
          [&] {
            return cyc.loop_count() >= kWarmupTicks &&
                   evt.handled_count() >= kWarmupHandled;
          },
          NowNs() + kWaitBudgetNs)) {
    std::fprintf(stderr,
                 "TST-13: warm-up timed out (ticks=%" PRIu64
                 ", handled=%" PRIu64 ")\n",
                 cyc.loop_count(), evt.handled_count());
    app.RequestStop();
    app.Join();
    ShmRegion::UnlinkName(shm_name);
    return 1;
  }

  // --- Sentinel self-check: prove the interposition is live and gated by the
  // armed flag. Runs after warm-up but before the measured window, whose
  // counter is reset by Arm().
  CHECK(AllocationCount() == 0);
  Arm();
  // volatile: keeps the deliberate pair visible so the optimizer cannot elide
  // it (and keeps GCC's -Wmismatched-new-delete tracer from following the
  // pointer into the malloc-backed replacement below).
  void* volatile probe = ::operator new(64);
  ::operator delete(probe);
  Disarm();
  const std::uint64_t control_allocs = AllocationCount();
  if (control_allocs != 1) {
    std::fprintf(stderr,
                 "TST-13: sentinel self-check failed: %" PRIu64
                 " allocations for one deliberate new (expected 1; >1 means"
                 " the runtime allocated during the probe too)\n",
                 control_allocs);
    app.RequestStop();
    app.Join();
    ShmRegion::UnlinkName(shm_name);
    return 1;
  }

  // --- Measured window: N cyclic ticks, each with its tap write and event
  // dispatch. Only the worker threads run alongside the poll below, so any
  // allocation counted here is a core hot-path violation.
  Arm();
  const std::uint64_t tick_begin = cyc.loop_count();
  const std::uint64_t handled_begin = evt.handled_count();
  const std::uint64_t dispatch_begin = evt.dispatch_count();
  const bool progressed = WaitFor(
      [&] { return cyc.loop_count() >= tick_begin + kMeasuredTicks; },
      NowNs() + kWaitBudgetNs);
  Disarm();
  const std::uint64_t allocs = AllocationCount();
  const std::uintptr_t first_pc = g_first_pc.load(std::memory_order_relaxed);
  const std::uint64_t first_size =
      g_first_size.load(std::memory_order_relaxed);

  app.RequestStop();
  app.Join();

  // Non-timing post-conditions first: they run without the counter armed.
  if (!progressed || cyc.loop_count() < tick_begin + kMeasuredTicks) {
    std::fprintf(stderr,
                 "TST-13: only %" PRIu64 " of %" PRIu64
                 " measured ticks ran (deadline exceeded)\n",
                 cyc.loop_count() - tick_begin, kMeasuredTicks);
    ShmRegion::UnlinkName(shm_name);
    return 1;
  }
  if (evt.dispatch_count() == dispatch_begin ||
      evt.handled_count() == handled_begin) {
    std::fprintf(stderr,
                 "TST-13: event path did not run in the measured window"
                 " (dispatches=%" PRIu64 ", handled=%" PRIu64 ")\n",
                 evt.dispatch_count() - dispatch_begin,
                 evt.handled_count() - handled_begin);
    ShmRegion::UnlinkName(shm_name);
    return 1;
  }
  if (cyc.write_fail_count() != 0) {
    std::fprintf(stderr,
                 "TST-13: tap write was rejected %" PRIu64 " times\n",
                 cyc.write_fail_count());
    ShmRegion::UnlinkName(shm_name);
    return 1;
  }
  if (cyc.pushed_count() == 0) {
    std::fprintf(stderr, "TST-13: event producer never pushed\n");
    ShmRegion::UnlinkName(shm_name);
    return 1;
  }
  if (mirror->RingHead(0) == 0) {
    std::fprintf(stderr, "TST-13: record mirror ring stayed empty\n");
    ShmRegion::UnlinkName(shm_name);
    return 1;
  }

  const std::uint64_t tick_delta = cyc.loop_count() - tick_begin;
  const std::uint64_t handled_delta = evt.handled_count() - handled_begin;
  const std::uint64_t head = mirror->RingHead(0);

  // The verdict.
  if (allocs != 0) {
    std::fprintf(stderr,
                 "TST-13: FAIL: %" PRIu64
                 " heap allocation(s) in the measured window"
                 " (%" PRIu64 " ticks, %" PRIu64 " events, ring head %" PRIu64
                 ")\n",
                 allocs, tick_delta, handled_delta, head);
    PrintFirstSite(first_pc, first_size);
    std::fprintf(stderr,
                 "TST-13: core must not allocate after App::Start()"
                 " (docs/reference/coding-rules.md CR-01)\n");
    ShmRegion::UnlinkName(shm_name);
    return 1;
  }

  std::printf(
      "TST-13: 0 allocations across %" PRIu64 " ticks (ring head %" PRIu64
      ", %" PRIu64 " events dispatched)\n",
      tick_delta, head, handled_delta);

  ShmRegion::UnlinkName(shm_name);
  return 0;
}
