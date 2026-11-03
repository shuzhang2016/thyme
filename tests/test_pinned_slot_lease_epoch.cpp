// test_pinned_slot_lease_epoch.cpp — lease ABI v4: the epoch/pin
// ordering invariant.
//   1. a HELD pin has its epoch set (acquire stamps epoch after validation,
//      before the final seq re-check — so a returned lease always has one);
//   2. the zombie state pin>0 && epoch==0 (reader killed between the pin CAS
//      and the epoch stamp) is stealable — the writer steals immediately
//      instead of skipping forever;
//   3. concurrent acquire/release churn never produces a permanent pin.
//      Transient pin>0 && epoch==0 is legal during an in-flight acquire
//      (pin happens before validation in v4), so the monitor only flags the
//      state when it persists on an unchanged pin_gen.
//   4. a FAILED acquire (final seq re-check fails after the stamp) rolls its
//      epoch stamp back — repeated failures on a slot with an expired/dead
//      pin must not refresh that pin's epoch, so the writer still steals
//      within the configured timeout. A successful live lease keeps its fresh
//      epoch and is never stolen (this must not regress).
#include "thyme/check.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <unistd.h>

using namespace thyme;

namespace {

struct ChurnArg {
  const char* name;
  uint64_t frame_index;
  int iters;
};

void* ChurnRun(void* p) {
  auto* a = static_cast<ChurnArg*>(p);
  PinnedSlotReader r;  // per-thread reader: counters such as torn_reads_ are not atomic
  CHECK(r.Open(a->name, 1000000000ULL));
  for (int i = 0; i < a->iters; ++i) {
    PinnedSlotReader::Lease l = r.Acquire(a->frame_index);
    CHECK(l.valid());  // writer stopped: frames are stable, acquire must succeed
  }                    // each iteration's destructor releases
  r.Close();
  return nullptr;
}

struct MonArg {
  const PinnedSlotLeaseCell* cell;
  std::atomic<bool>* stop;
  std::atomic<uint64_t>* violations;
};

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

void* MonRun(void* p) {
  auto* a = static_cast<MonArg*>(p);
  uint64_t suspect_pg = 0;
  uint64_t suspect_since = 0;
  while (!a->stop->load(std::memory_order_relaxed)) {
    const uint64_t pg = a->cell->pin_gen.load(std::memory_order_acquire);
    if (LeasePinOf(pg) == 0) {
      suspect_pg = 0;
      continue;
    }
    const uint64_t e = a->cell->epoch.load(std::memory_order_acquire);
    const uint64_t pg2 = a->cell->pin_gen.load(std::memory_order_acquire);
    if (LeasePinOf(pg2) > 0 && e == 0) {
      // v4: a legitimate transient exists between acquire's pin and its epoch
      // stamp. Only the same pin_gen with no epoch for >1ms counts as a
      // permanent-pin violation.
      if (suspect_pg == pg2) {
        if (NowNs() - suspect_since > 1000000ULL) {
          a->violations->fetch_add(1, std::memory_order_relaxed);
          suspect_pg = 0;
        }
      } else {
        suspect_pg = pg2;
        suspect_since = NowNs();
      }
    } else {
      suspect_pg = 0;
    }
  }
  return nullptr;
}

void SleepMs(uint64_t ms) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(ms / 1000ULL);
  ts.tv_nsec = static_cast<long>((ms % 1000ULL) * 1000000ULL);
  nanosleep(&ts, nullptr);
}

// Hook context: lands the writer's mid-Begin state (seq=0) exactly in
// the window between the epoch stamp and the final seq re-check, through the
// test's own RW mapping (the reader's payload mapping is read-only). The
// writer would then read the fresh epoch and skip, restoring seq; the test
// restores it explicitly once AcquireLease has returned.
struct StampHookCtx {
  PinnedSlotLeaseCell* cell;
  std::atomic<uint64_t>* slot_seq;
  uint64_t dead_epoch;
  uint64_t saved_seq;
  int calls;
};

void StampHook(void* p) {
  auto* c = static_cast<StampHookCtx*>(p);
  // The stamp must already be visible here — this is the buggy window.
  CHECK(c->cell->epoch.load(std::memory_order_acquire) != c->dead_epoch);
  c->saved_seq = c->slot_seq->load(std::memory_order_acquire);
  c->slot_seq->store(0, std::memory_order_seq_cst);
  c->calls += 1;
}

}  // namespace

int main() {
  char name[96];
  std::snprintf(name, sizeof(name), "/c9lease_epoch_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());

  PinnedSlotWriter w;
  CHECK(w.Create(name, 128, 2, 0, 50000000ULL));  // steal timeout 50 ms
  PinnedSlotReader r;
  CHECK(r.Open(name, 1000000000ULL));
  for (uint8_t f = 0; f < 2; ++f) {
    PinnedSlotWriter::BeginView bv = w.Begin(128);
    std::memset(bv.payload, f, 128);
    w.Commit();
  }
  auto* cells = reinterpret_cast<PinnedSlotLeaseCell*>(
      const_cast<PinnedSlotLeaseHeader*>(r.lease()) + 1);

  // ---- 1. live-pin invariant: a held lease always has an epoch (stamped after validation) ----
  {
    PinnedSlotReader::Lease l = r.Acquire(0);
    CHECK(l.valid());
    const uint64_t pg = cells[0].pin_gen.load(std::memory_order_acquire);
    CHECK(LeasePinOf(pg) == 1);
    CHECK(cells[0].epoch.load(std::memory_order_acquire) != 0);
  }
  CHECK(LeasePinOf(cells[0].pin_gen.load(std::memory_order_acquire)) == 0);

  // ---- 2. construct pin>0 && epoch==0 (v2 leftover state): writer steals immediately ----
  {
    PinnedSlotReader::Lease l = r.Acquire(0);
    CHECK(l.valid());
    cells[0].epoch.store(0, std::memory_order_release);  // simulate being killed in the window
    PinnedSlotWriter::BeginView bv = w.Begin(128);  // frame 2 -> slot 0
    CHECK(bv.payload != nullptr);                 // steal, not skip
    CHECK(!w.pending_skipped());
    std::memset(bv.payload, 0x5A, 128);
    w.Commit();
    CHECK(w.lease()->lease_steal.load(std::memory_order_acquire) == 1);
    CHECK(LeasePinOf(cells[0].pin_gen.load(std::memory_order_acquire)) == 0);
    // l destructs: the slot has advanced generations, gen mismatch -> release
    // skips, no underflow (semantics unchanged).
  }
  CHECK(LeasePinOf(cells[0].pin_gen.load(std::memory_order_acquire)) == 0);

  // ---- 3. concurrent acquire/release churn: no permanent pins, zero monitor violations ----
  {
    const uint64_t idx = w.head() - 1;  // frame 2 -> slot 0
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> violations{0};
    MonArg ma{&cells[idx % 2], &stop, &violations};
    pthread_t mon{};
    CHECK(pthread_create(&mon, nullptr, MonRun, &ma) == 0);
    pthread_t ths[4];
    ChurnArg args[4];
    for (int t = 0; t < 4; ++t) {
      args[t] = ChurnArg{name, idx, 20000};
      CHECK(pthread_create(&ths[t], nullptr, ChurnRun, &args[t]) == 0);
    }
    for (auto& th : ths) CHECK(pthread_join(th, nullptr) == 0);
    stop.store(true, std::memory_order_relaxed);
    CHECK(pthread_join(mon, nullptr) == 0);
    CHECK(violations.load(std::memory_order_relaxed) == 0);
    const uint64_t pg = cells[idx % 2].pin_gen.load(std::memory_order_acquire);
    CHECK(LeasePinOf(pg) == 0);  // no leaked/permanent pin
  }

  // ---- 4. a failed acquire must not refresh a dead pin's epoch ----
  // Dead pin + repeatedly failing acquires: each attempt stamps after validation
  // succeeds, then fails the final seq re-check. Before the fix the stamp stayed
  // on the cell and blocked steal indefinitely; after the fix a failed attempt
  // rolls back its own stamp with a CAS, the dead pin stays expired, and the
  // writer steals immediately the next time it wraps to that slot.
  {
    // Dead reader: a lease pinning frame 2 (slot 0) is never used or released
    // again — to the protocol that is an expired pin.
    const uint64_t dead_idx = w.head() - 1;  // frame 2 -> slot 0
    CHECK(dead_idx == 2);

    ShmRegion rw;  // the test's own RW mapping: the reader's payload mapping is read-only
    CHECK(rw.OpenRetry(name, PinnedSlotWriter::RegionBytes(128, 2, 0),
                       1000000000ULL, /*read_only=*/false));
    auto* rw_base = static_cast<uint8_t*>(rw.data());
    auto* slot = reinterpret_cast<PinnedSlotHeader*>(
        rw_base + PinnedSlotWriter::SlotsOffset(0) +
        (dead_idx % 2) * PinnedSlotWriter::SlotStride(128));
    CHECK(slot->seq.load(std::memory_order_acquire) == dead_idx + 1);

    PinnedSlotReader::Lease dead = r.Acquire(dead_idx);
    CHECK(dead.valid());
    const uint64_t dead_epoch =
        cells[dead_idx % 2].epoch.load(std::memory_order_acquire);
    CHECK(dead_epoch != 0);
    SleepMs(80);  // past the 50ms steal timeout: the dead pin has expired

    StampHookCtx hctx{&cells[dead_idx % 2], &slot->seq, dead_epoch, 0, 0};
    r.set_epoch_stamp_test_hook(&StampHook, &hctx);
    for (int i = 0; i < 32; ++i) {
      PinnedSlotReader::View v;
      uint32_t gen = 0;
      CHECK(r.AcquireLease(dead_idx, v, &gen) ==
            PinnedSlotReader::ReadResult::kTorn);
      CHECK(hctx.calls == i + 1);              // the hook really entered the stamp/re-check window
      CHECK(hctx.saved_seq == dead_idx + 1);   // the frame is still the one in the slot
      // A failed attempt must roll back its own stamp; the dead pin's epoch stays expired.
      CHECK(cells[dead_idx % 2].epoch.load(std::memory_order_acquire) ==
            dead_epoch);
      CHECK(LeasePinOf(cells[dead_idx % 2].pin_gen.load(
                std::memory_order_acquire)) == 1);  // only the dead pin remains
      // the writer would read a fresh epoch, skip and restore seq; the test restores it instead.
      slot->seq.store(hctx.saved_seq, std::memory_order_seq_cst);
    }
    r.set_epoch_stamp_test_hook(nullptr, nullptr);
    CHECK(cells[dead_idx % 2].epoch.load(std::memory_order_acquire) ==
          dead_epoch);  // still not refreshed after repeated failures

    // The next Begin wrapping to slot 0 must steal immediately (the dead pin
    // expired long ago), not wait another timeout because a failed attempt
    // refreshed the epoch.
    PinnedSlotWriter::BeginView bv = w.Begin(128);  // frame 3 -> slot 1
    CHECK(bv.payload != nullptr);
    std::memset(bv.payload, 3, 128);
    w.Commit();
    const uint64_t steals_before = w.lease()->lease_steal.load(
        std::memory_order_acquire);
    bv = w.Begin(128);  // frame 4 -> slot 0: expired dead pin
    CHECK(bv.payload != nullptr);
    CHECK(!w.pending_skipped());
    std::memset(bv.payload, 4, 128);
    w.Commit();
    CHECK(w.lease()->lease_steal.load(std::memory_order_acquire) ==
          steals_before + 1);
    CHECK(LeasePinOf(cells[dead_idx % 2].pin_gen.load(
              std::memory_order_acquire)) == 0);
    dead.Reset();  // generation advanced: gen mismatch -> skip, no underflow
    CHECK(LeasePinOf(cells[dead_idx % 2].pin_gen.load(
              std::memory_order_acquire)) == 0);

    // No regression: a successfully returned live lease has a fresh epoch, so
    // the writer must skip rather than steal.
    const uint64_t live_idx = w.head() - 1;  // frame 4 -> slot 0
    CHECK(live_idx == 4);
    PinnedSlotReader::Lease live = r.Acquire(live_idx);
    CHECK(live.valid());
    CHECK(cells[live_idx % 2].epoch.load(std::memory_order_acquire) != 0);
    const uint64_t steals_live = w.lease()->lease_steal.load(
        std::memory_order_acquire);
    bv = w.Begin(128);  // frame 5 -> slot 1: not pinned, normal write
    CHECK(bv.payload != nullptr);
    std::memset(bv.payload, 5, 128);
    w.Commit();
    bv = w.Begin(128);  // frame 6 -> slot 0: live lease, fresh epoch -> skip
    CHECK(bv.payload == nullptr);
    CHECK(w.pending_skipped());
    w.Commit();
    CHECK(w.lease()->lease_steal.load(std::memory_order_acquire) == steals_live);
    live.Reset();
    CHECK(LeasePinOf(cells[live_idx % 2].pin_gen.load(
              std::memory_order_acquire)) == 0);
  }

  std::fprintf(stderr, "lease epoch: invariant held, zombie state stolen, "
                       "churn clean, failed acquire rolls its stamp back\n");
  r.Close();
  w.Close();
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  return 0;
}
