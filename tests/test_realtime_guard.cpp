// SCH-06: the RT-context hook and the log-rotation guard.
//
// - CyclicThread::Loop and EventThread::Dispatch run with
//   rt::InRealtimeSection() true; ordinary threads see false.
// - RotatingFile::RotateIfNeeded defers rename()/unlink() when called from
//   an RT section (counted in LogBudget::deferred_rotations) and rotates on
//   the next non-RT call.
#include "thyme/check.h"
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/event_thread.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/realtime_guard.h"
#include "thyme/runtime/rotating_file.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
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

class GuardEventThread final : public EventThread {
 public:
  NotifPipe<uint32_t, 8>* q = nullptr;
  std::atomic<uint64_t> msgs{0};
  std::atomic<bool> in_rt{false};
  using EventThread::EventThread;

 protected:
  void Dispatch() override {
    in_rt.store(rt::InRealtimeSection(), std::memory_order_relaxed);
    uint32_t v = 0;
    while (q->TryPop(v)) {
      NoteHandled();
      msgs.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

class GuardCyclicThread final : public CyclicThread {
 public:
  std::atomic<bool> in_rt{false};
  std::atomic<uint64_t> loops{0};
  using CyclicThread::CyclicThread;

 protected:
  void Loop() override {
    in_rt.store(rt::InRealtimeSection(), std::memory_order_relaxed);
    loops.fetch_add(1, std::memory_order_relaxed);
  }
};

void WaitFor(const std::atomic<uint64_t>& v, uint64_t want) {
  const uint64_t deadline = Tns() + 3000000000ULL;
  while (v.load(std::memory_order_relaxed) < want && Tns() < deadline) {
    usleep(1000);
  }
}

}  // namespace

int main() {
  // Ordinary context is not RT, and nesting is counted.
  CHECK(!rt::InRealtimeSection());
  {
    rt::ScopedRealtimeSection outer;
    CHECK(rt::InRealtimeSection());
    {
      rt::ScopedRealtimeSection inner;
      CHECK(rt::InRealtimeSection());
    }
    CHECK(rt::InRealtimeSection());
  }
  CHECK(!rt::InRealtimeSection());

  // Worker callbacks run in the RT section.
  {
    HealthSlot health{};
    NotifPipe<uint32_t, 8> q;
    GuardEventThread th("rt_event", &health);
    th.q = &q;
    Waitable* srcs[] = {&q};
    th.SetWaitInputs(srcs, 1);
    CHECK(th.Start());
    CHECK(q.Push(1));
    WaitFor(th.msgs, 1);
    th.RequestStop();
    th.Join();
    CHECK(th.in_rt.load());
  }
  {
    HealthSlot health{};
    GuardCyclicThread th("rt_cyclic", 500, &health);  // 2 ms
    CHECK(th.Start());
    WaitFor(th.loops, 1);
    th.RequestStop();
    th.Join();
    CHECK(th.in_rt.load());
  }

  // Log rotation: forced to happen inside the live file once the cap is hit,
  // but deferred while the caller is in an RT section.
  logging::LogBudget::ResetForTest();
  char path[128];
  std::snprintf(path, sizeof(path), "/tmp/thyme_rt_guard_%d.log",
                static_cast<int>(getpid()));
  std::remove(path);
  std::remove((std::string(path) + ".1").c_str());
  logging::RotatingFile file;
  logging::RotateConfig cfg;
  cfg.path = path;
  cfg.max_bytes = 64;
  cfg.keep = 2;
  cfg.account = false;  // isolate from the process budget
  CHECK(file.Open(cfg));
  CHECK(file.rotations() == 0);

  char line[80];
  for (std::size_t i = 0; i < sizeof(line); ++i) line[i] = 'x';
  // Seed the live file past the cap without rotating (rotation is checked
  // before the append), so the next append has a rotation pending.
  CHECK(file.Append(line, sizeof(line)) ==
        static_cast<std::ptrdiff_t>(sizeof(line)));
  CHECK(file.size_bytes() >= cfg.max_bytes);
  CHECK(file.rotations() == 0);
  {
    rt::ScopedRealtimeSection rt_scope;
    CHECK(file.Append(line, sizeof(line)) ==
          static_cast<std::ptrdiff_t>(sizeof(line)));
    CHECK(file.rotations() == 0);  // deferred, not rotated in RT
    CHECK(logging::LogBudget::deferred_rotations() >= 1);
  }
  // Non-RT append (or explicit RotateIfNeeded) performs the deferred work.
  CHECK(file.Append(line, sizeof(line)) ==
        static_cast<std::ptrdiff_t>(sizeof(line)));
  CHECK(file.rotations() == 1);
  CHECK(file.RotateIfNeeded());  // rotates again (size already >= cap)
  CHECK(file.rotations() == 2);
  CHECK(logging::LogBudget::rotations() >= 2);

  file.Close();
  std::remove(path);
  std::remove((std::string(path) + ".1").c_str());
  std::remove((std::string(path) + ".2").c_str());

  return 0;
}
