// RT-09 (SHM half): bounded seqlock retry loops of the cross-process readers
// expose their retry counts. Named SHM regions are required by ShmSpmcRing and
// PinnedSlotStore, so this binary stays out of the MR gate like the other
// /dev/shm tests; the in-process half is test_retry_accounting (mr_gate).
//
// Forcing the retry is deterministic: leave the slot the reader is about to
// inspect in its "writer in progress" state (seq==0) without advancing head,
// or corrupt data_len past the slot size so the header re-check loop spins.
// A successful read must not move the counters.
#include "thyme/check.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <unistd.h>

using namespace thyme;

namespace {

struct Payload {
  uint64_t a;
  uint32_t b;
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

}  // namespace

int main() {
  // ---- SpmcReader::TryPop / ReadLatest: process-local retry counters. ----
  {
    char name[64];
    std::snprintf(name, sizeof(name), "/c9rtry_spmc_%d",
                  static_cast<int>(getpid()));
    UnlinkName cleaner(name);
    ShmRegion::UnlinkName(name);

    constexpr std::size_t kCap = 4;
    ShmSpmcRing<Payload, kCap> producer;
    CHECK(producer.Create(name));
    ShmSpmcRing<Payload, kCap> consumer;
    CHECK(consumer.Open(name, 1000000000ULL));

    SpmcReader<Payload, kCap> follow;
    CHECK(follow.Bind(&consumer, SpmcRead::Follow));
    CHECK(follow.retry_count() == 0 && follow.torn_count() == 0);

    producer.Push(Payload{11, 11});
    Payload got{};
    CHECK(follow.TryPop(got) && got.a == 11);
    CHECK(follow.retry_count() == 0);

    // Second publish, then force the slot the cursor points at back to
    // seq==0 (writer mid-commit). TryPop burns its whole budget.
    producer.Push(Payload{12, 12});
    producer.layout()->slots[1].seq.store(0, std::memory_order_relaxed);
    CHECK(!follow.TryPop(got));
    CHECK(follow.retry_count() == kSpmcReadRetries);
    CHECK(follow.torn_count() == 0);

    // Publishing the slot makes the same frame readable; counters freeze.
    producer.layout()->slots[1].seq.store(2, std::memory_order_release);
    CHECK(follow.TryPop(got) && got.a == 12);
    CHECK(follow.retry_count() == kSpmcReadRetries);

    // ReadLatest has its own budget on the newest slot.
    SpmcReader<Payload, kCap> latest;
    CHECK(latest.Bind(&consumer, SpmcRead::Latest));
    producer.Push(Payload{13, 13});  // head 3, slot 2 published
    producer.layout()->slots[2].seq.store(0, std::memory_order_relaxed);
    Payload last{};
    CHECK(!latest.ReadLatest(last));
    CHECK(latest.retry_count() == kSpmcReadRetries);

    producer.layout()->slots[2].seq.store(3, std::memory_order_release);
    CHECK(latest.ReadLatest(last) && last.a == 13);
    CHECK(latest.retry_count() == kSpmcReadRetries);

    producer.Close();
    consumer.Close();
  }

  // ---- PinnedSlotReader::TryRead / ReadFrame: same forcing strategy. ----
  {
    char name[64];
    std::snprintf(name, sizeof(name), "/c9rtry_bf_%d",
                  static_cast<int>(getpid()));
    UnlinkName cleaner(name);
    // One slot: the writer's Begin() rewrites exactly the slot the reader is
    // about to validate, without advancing head.
    PinnedSlotWriter w;
    CHECK(w.Create(name, 256, 1, 0));

    PinnedSlotWriter::BeginView first = w.Begin(128);
    CHECK(first.payload != nullptr);
    w.Commit(1000);

    PinnedSlotReader r;
    CHECK(r.Open(name, 1000000000ULL));
    CHECK(r.retry_count() == 0 && r.torn_read_count() == 0);

    // Begin without Commit leaves seq==0 while head still points at it.
    PinnedSlotWriter::BeginView open = w.Begin(64);
    CHECK(open.payload != nullptr);
    PinnedSlotReader::View v;
    CHECK(!r.TryRead(v));  // cursor unchanged
    CHECK(r.retry_count() == kPinnedSlotReadRetries);
    CHECK(r.torn_read_count() == 0);
    w.Commit(2000);
    CHECK(r.TryRead(v) && v.data_len == 64);
    CHECK(r.retry_count() == kPinnedSlotReadRetries);

    // A header whose data_len exceeds the slot makes ReadFrame re-read until
    // the budget is gone; it reports kTorn and one torn read.
    PinnedSlotWriter::BeginView second = w.Begin(32);
    CHECK(second.payload != nullptr);
    w.Commit(3000);
    second.header->data_len.store(257, std::memory_order_relaxed);
    const uint64_t before = r.retry_count();
    CHECK(r.ReadFrame(2, v) == PinnedSlotReader::ReadResult::kTorn);
    CHECK(r.retry_count() == before + kPinnedSlotReadRetries);
    CHECK(r.torn_read_count() == 1);

    r.Close();
    w.Close();
  }

  std::printf("test_retry_accounting_shm: ok\n");
  return 0;
}
