// test_lease_observability.cpp — BF-12: Lease::still_valid() follows the
// generation (steal invalidates it while valid() stays true), Release()
// reports kOk/kStolen/kAlreadyReleased, generation()/held_ns()/deadline_ns()
// are exposed, and lease_invalidated counts a stolen lease once.
#include "thyme/check.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

using namespace thyme;

namespace {

constexpr uint64_t kStealTimeoutNs = 20000000ULL;  // 20 ms

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

void WriteFrame(PinnedSlotWriter& w, uint8_t tag, uint64_t mono_ns) {
  PinnedSlotWriter::BeginView bv = w.Begin(128);
  CHECK(bv.payload != nullptr);
  std::memset(bv.payload, tag, 128);
  w.Commit(mono_ns);
}

// Keep writing until the writer steals the pinned slot (lease_steal grows).
// Holes created by pinned-skip Begins are expected; the helper returns after a
// successful steal write, so w.head() - 1 is a published frame.
bool ForceSteal(PinnedSlotWriter& w, uint64_t steal_before) {
  const uint64_t deadline = NowNs() + 3000000000ULL;
  while (NowNs() < deadline) {
    PinnedSlotWriter::BeginView bv = w.Begin(128);
    if (bv.payload != nullptr) {
      std::memset(bv.payload, 0xEE, 128);
    }
    w.Commit();  // no-op for a pinned-skip Begin
    if (w.lease()->lease_steal.load(std::memory_order_acquire) > steal_before) {
      return true;
    }
    SleepNs(2000000ULL);  // 2 ms
  }
  return false;
}

}  // namespace

int main() {
  char name[96];
  std::snprintf(name, sizeof(name), "/c9lease_obs_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());

  PinnedSlotWriter w;
  CHECK(w.Create(name, 128, 2, 0, kStealTimeoutNs));
  PinnedSlotReader r;
  CHECK(r.Open(name, 1000000000ULL));
  WriteFrame(w, 0x11, 1000);
  WriteFrame(w, 0x22, 2000);

  // --- Normal lifecycle: valid() == acquired, still_valid() until Release. ---
  {
    PinnedSlotReader::Lease l = r.Acquire(0);
    CHECK(l.valid());
    CHECK(l.still_valid());
    CHECK(l.generation() == 0);  // fresh channel
    CHECK(l.steal_timeout_ns() == kStealTimeoutNs);
    const uint64_t deadline = l.deadline_ns();
    CHECK(deadline > 0);
    CHECK(deadline <= NowNs() + kStealTimeoutNs);

    SleepNs(2000000ULL);  // 2 ms
    CHECK(l.held_ns() >= 1000000ULL);

    CHECK(l.Release() == LeaseReleaseResult::kOk);
    CHECK(!l.valid());
    CHECK(!l.still_valid());  // released, not stolen
    const uint64_t frozen = l.held_ns();
    SleepNs(1000000ULL);
    CHECK(l.held_ns() == frozen);  // freezes at Release
    CHECK(l.Release() == LeaseReleaseResult::kAlreadyReleased);
    CHECK(r.lease_invalidated_count() == 0);
  }

  // --- Steal detected by still_valid(): valid() stays true, still_valid()
  // goes false, Release reports kStolen, counter increments exactly once. ---
  {
    PinnedSlotReader::Lease l = r.Acquire(0);
    CHECK(l.valid());
    CHECK(l.still_valid());
    const uint64_t steal_before =
        w.lease()->lease_steal.load(std::memory_order_acquire);
    CHECK(ForceSteal(w, steal_before));

    CHECK(l.valid());        // acquire succeeded; this does NOT mean alive
    CHECK(!l.still_valid()); // generation advanced: writer stole the slot
    CHECK(!l.still_valid()); // repeated observation does not double count
    CHECK(r.lease_invalidated_count() == 1);

    CHECK(l.Release() == LeaseReleaseResult::kStolen);
    CHECK(r.lease_invalidated_count() == 1);  // already counted above
    CHECK(l.Release() == LeaseReleaseResult::kAlreadyReleased);
  }

  // --- Steal detected by Release() directly. ---
  {
    const uint64_t idx = w.head() - 1;  // last published (steal) frame
    PinnedSlotReader::Lease l = r.Acquire(idx);
    CHECK(l.valid());
    CHECK(l.still_valid());
    CHECK(l.generation() >= 1);  // the slot was stolen at least once
    const uint64_t steal_before =
        w.lease()->lease_steal.load(std::memory_order_acquire);
    CHECK(ForceSteal(w, steal_before));

    CHECK(l.Release() == LeaseReleaseResult::kStolen);
    CHECK(r.lease_invalidated_count() == 2);
    CHECK(l.Release() == LeaseReleaseResult::kAlreadyReleased);
  }

  r.Close();
  w.Close();
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  return 0;
}
