// test_pinned_slot_lease_race_debug.cpp — (-UNDEBUG variant): AcquireLease's
// failure path must remove the debug watch installed by ReadFrame — under
// benign writer/reader contention it returns kTorn, never SIGABRT (a stale
// watch left by an invalid Lease destructor once killed the recorder).
#include "thyme/check.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace thyme;

namespace {

uint64_t NowNs() {
  struct timespec ts {};
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

struct WArg {
  PinnedSlotWriter* w;
  std::atomic<bool>* stop;
};

void* WriterRun(void* p) {
  auto* a = static_cast<WArg*>(p);
  uint8_t f = 0;
  while (!a->stop->load(std::memory_order_relaxed)) {
    PinnedSlotWriter::BeginView bv = a->w->Begin(128);
    if (bv.payload != nullptr) {
      std::memset(bv.payload, f++, 128);
    }
    a->w->Commit();
  }
  return nullptr;
}

}  // namespace

int main() {
  char name[96];
  std::snprintf(name, sizeof(name), "/c9lrace_%d", static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());

  PinnedSlotWriter w;
  CHECK(w.Create(name, 128, 2, 0));  // 2 slots: tightest contention
  PinnedSlotReader r;
  CHECK(r.Open(name, 1000000000ULL));

  std::atomic<bool> stop{false};
  WArg wa{&w, &stop};
  pthread_t th{};
  CHECK(pthread_create(&th, nullptr, WriterRun, &wa) == 0);

  uint64_t ok = 0, torn = 0, other = 0;
  // Event-driven rather than a fixed time window: run until "contention really
  // happened (torn>0) and there are enough samples (ok+torn>=100)"; the
  // deadline is only a fallback for abnormal environments. A fixed 500ms window
  // is unreliable under full load (-j12) / coverage builds: when the writer
  // thread is starved head stays 0, and when the reader is slowed head-1 is
  // often evicted as kOverrun; either way the window may collect too few
  // samples or see no contention at all — that is a scheduling artifact, not
  // product behavior.
  const uint64_t deadline = NowNs() + 20000000000ULL;  // 20 s fallback
  while (torn == 0 || ok + torn < 100) {
    CHECK(NowNs() < deadline);
    const uint64_t head = r.head();
    if (head == 0) {
      SleepNs(1000000ULL);  // writer not scheduled yet: yield the CPU instead of spinning
      continue;
    }
    // Hottest slot: the newest frame. Under benign contention Acquire must
    // return kTorn, not SIGABRT when the invalid Lease destructs (the process
    // finishing alive is the assertion).
    PinnedSlotReader::ReadResult rr = PinnedSlotReader::ReadResult::kTorn;
    PinnedSlotReader::Lease l = r.Acquire(head - 1, &rr);
    if (l.valid()) {
      ++ok;
    } else if (rr == PinnedSlotReader::ReadResult::kTorn) {
      ++torn;
    } else {
      ++other;
    }
  }
  stop.store(true, std::memory_order_relaxed);
  CHECK(pthread_join(th, nullptr) == 0);
  CHECK(torn > 0);       // contention really happened (guaranteed by the loop exit; defensive)
  // kOverrun (head-1 already evicted when the writer is very fast) is also
  // legal; the key is no SIGABRT.
  std::fprintf(stderr, "lease race: ok=%llu torn=%llu overrun=%llu (no SIGABRT)\n",
                 static_cast<unsigned long long>(ok),
                 static_cast<unsigned long long>(torn),
                 static_cast<unsigned long long>(other));

  // A Lease must not outlive the reader's Close(). The writer thread has been
  // joined, so the parent is single-threaded at fork and the abort in the child
  // is safe.
  // head-1 cannot be Acquired directly: during contention, while the reader
  // holds a pin the writer pinned-skips — head advances without publishing, so
  // the last frame may be a hole. After the join the main thread publishes one
  // more frame without contention, so last is certainly a published frame (no
  // active pin now, Begin cannot skip).
  {
    PinnedSlotWriter::BeginView bv = w.Begin(128);
    CHECK(bv.payload != nullptr);
    std::memset(bv.payload, 0xA5, 128);
    w.Commit();
  }
  const uint64_t last = r.head() - 1;
  // Control group: Reset then Close -> clean exit (no leaked pin).
  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    PinnedSlotReader::Lease l = r.Acquire(last, nullptr);
    if (!l.valid()) _exit(2);
    l.Reset();
    r.Close();
    _exit(0);
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  // Violation: Close before the Lease destructs -> the debug assertion SIGABRTs.
  // The child abort leaks one pin, but the channel is destroyed afterwards, so
  // it does not matter.
  pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    {
      PinnedSlotReader::Lease l = r.Acquire(last, nullptr);
      if (!l.valid()) _exit(2);
      r.Close();
    }  // ~Lease -> Reset -> the assertion
    _exit(0);  // reached only if the assertion failed
  }
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
  std::fprintf(stderr, "lease lifetime: Close-before-Reset aborts as required\n");

  r.Close();
  w.Close();
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  return 0;
}
