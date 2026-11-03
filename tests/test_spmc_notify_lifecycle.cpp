// RT-08: ShmSpmcRing's process-local notify registry.
//
// The audit found that notify_targets_/n_notify_ are NOT setup-only: a reader
// can Bind (and a host can call SetNotify) while the ring is already being
// published — late attach is a legitimate pattern, and test_shm_spmc_begin_commit
// already binds readers after the first Push. The registry therefore uses
// atomic publication (CAS slot reservation + release store into the slot; the
// fan-out loads the count and each slot with acquire) instead of relying on a
// setup-only contract, and `notifying_` is a relaxed atomic re-entrancy guard.
//
// This test covers: setup registration, the re-entrancy guard, a sequential
// late attach, and concurrent attach while a producer thread publishes. The
// TSan variant (test_spmc_notify_race_tsan) is the actual race proof; here we
// assert the observable contract (a target registered during publishing is
// notified by a later publish, and nothing crashes).
#include "thyme/check.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/waitable.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <thread>
#include <unistd.h>

using namespace thyme;

namespace {

struct Payload {
  uint64_t a;
};

struct CountingTarget : Waitable {
  bool empty() const override { return true; }
  void WaitUntil(uint64_t) override {}
  void Notify() override { count.fetch_add(1, std::memory_order_relaxed); }
  std::atomic<int> count{0};
};

// A target whose Notify forwards back into the ring: the `notifying_` guard
// must stop the recursion after one fan-out round.
template <std::size_t Cap>
struct ReentrantTarget : Waitable {
  ShmSpmcRing<Payload, Cap>* ring = nullptr;
  std::atomic<int> count{0};
  bool empty() const override { return true; }
  void WaitUntil(uint64_t) override {}
  void Notify() override {
    count.fetch_add(1, std::memory_order_relaxed);
    if (ring != nullptr) ring->Notify();
  }
};

struct UnlinkName {
  char name[64]{};
  explicit UnlinkName(const char* n) {
    std::snprintf(name, sizeof(name), "%s", n);
    ShmRegion::UnlinkName(name);
  }
  ~UnlinkName() { ShmRegion::UnlinkName(name); }
  UnlinkName(const UnlinkName&) = delete;
  UnlinkName& operator=(const UnlinkName&) = delete;
};

uint64_t MonoNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

}  // namespace

int main() {
  char name[64];
  std::snprintf(name, sizeof(name), "/c9spmcnotify_%d",
                static_cast<int>(getpid()));
  UnlinkName cleaner(name);
  ShmRegion::UnlinkName(name);

  ShmSpmcRing<Payload, 4> producer;
  CHECK(producer.Create(name));

  // 1. Setup-time registration (what Bind/SetWaitInputs do) is accepted, and
  // one publish fans out once to each target; the re-entrant target's forward
  // into the ring is suppressed by the notifying_ guard.
  CountingTarget counter;
  CHECK(producer.AddNotifyTarget(&counter));
  ReentrantTarget<4> reentrant;
  reentrant.ring = &producer;
  CHECK(producer.AddNotifyTarget(&reentrant));

  producer.Push(Payload{1});
  CHECK(counter.count.load(std::memory_order_relaxed) == 1);
  CHECK(reentrant.count.load(std::memory_order_relaxed) == 1);

  producer.Push(Payload{2});
  CHECK(counter.count.load(std::memory_order_relaxed) == 2);

  // 2. Capacity: the registry stops at kAbiMaxSpmcReaders and rejects extras
  // without taking a slot.
  {
    ShmSpmcRing<Payload, 2> full;
    char fname[64];
    std::snprintf(fname, sizeof(fname), "/c9spmcnotify_full_%d",
                  static_cast<int>(getpid()));
    UnlinkName fcleaner(fname);
    ShmRegion::UnlinkName(fname);
    CHECK(full.Create(fname));
    CountingTarget extra[kAbiMaxSpmcReaders + 1];
    for (std::size_t i = 0; i < kAbiMaxSpmcReaders; ++i) {
      CHECK(full.AddNotifyTarget(&extra[i]));
    }
    CHECK(!full.AddNotifyTarget(&extra[kAbiMaxSpmcReaders]));
    CHECK(full.layout()->reader_slot_mask.load(std::memory_order_relaxed) == 0);
    SpmcReader<Payload, 2> blocked;
    CHECK(!blocked.Bind(&full, SpmcRead::Follow));
    CHECK(blocked.reader_slot() == kAbiMaxSpmcReaders);
    CHECK(full.layout()->reader_slot_mask.load(std::memory_order_relaxed) == 0);
    full.Close();
  }

  // 3. Sequential late attach: registers after publishes, then receives the
  // next publish (the registry is not frozen by the first Notify).
  CountingTarget late;
  CHECK(producer.AddNotifyTarget(&late));
  CHECK(late.count.load(std::memory_order_relaxed) == 0);
  producer.Push(Payload{3});
  CHECK(late.count.load(std::memory_order_relaxed) == 1);

  // 4. Concurrent attach while the producer publishes: attacher threads Bind a
  // reader (which registers the reader itself) and SetNotify a counting target
  // from their own thread while the publisher keeps calling Notify. The
  // registry reserves slots atomically, so no target is lost or torn; after
  // the threads join, one more publish must reach every registered target.
  // The TSan variant (test_spmc_notify_race_tsan) proves the absence of a data
  // race on the registry fields themselves.
  {
    char cname[64];
    std::snprintf(cname, sizeof(cname), "/c9spmcnotify_race_%d",
                  static_cast<int>(getpid()));
    UnlinkName ccleaner(cname);
    ShmRegion::UnlinkName(cname);
    ShmSpmcRing<Payload, 4> rp;
    CHECK(rp.Create(cname));

    constexpr int kAttachers = 4;  // 2 registrations each == capacity (8)
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> pushed{0};
    std::thread publisher([&] {
      for (uint64_t i = 0; !stop.load(std::memory_order_relaxed); ++i) {
        rp.Push(Payload{1000 + i});
        pushed.fetch_add(1, std::memory_order_relaxed);
      }
    });

    // Let the producer get going: registration is then genuinely concurrent
    // with Notify rather than racing an idle ring.
    const uint64_t spin_until = MonoNs() + 20000000ULL;  // 20 ms
    while (pushed.load(std::memory_order_relaxed) < 64 && MonoNs() < spin_until) {
      std::this_thread::yield();
    }

    std::unique_ptr<SpmcReader<Payload, 4>> readers[kAttachers];
    std::unique_ptr<CountingTarget> targets[kAttachers];
    for (int i = 0; i < kAttachers; ++i) {
      readers[i].reset(new SpmcReader<Payload, 4>());
      targets[i].reset(new CountingTarget());
    }

    std::thread attachers[kAttachers];
    for (int i = 0; i < kAttachers; ++i) {
      attachers[i] = std::thread([&, i] {
        CHECK(readers[i]->Bind(&rp, (i & 1) ? SpmcRead::Latest
                                            : SpmcRead::Follow));
        readers[i]->SetNotify(targets[i].get());
      });
    }
    for (auto& t : attachers) t.join();

    stop.store(true, std::memory_order_relaxed);
    publisher.join();

    // Every attached target receives the final publish. A failed/skipped
    // registration leaves a count of 0 here.
    rp.Push(Payload{9999});
    for (int i = 0; i < kAttachers; ++i) {
      CHECK(targets[i]->count.load(std::memory_order_relaxed) >= 1);
    }
    rp.Close();
  }

  producer.Close();
  std::printf("test_spmc_notify_lifecycle: ok\n");
  return 0;
}
