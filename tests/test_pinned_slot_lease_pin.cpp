// test_pinned_slot_lease_pin.cpp — reader leases pin slots;
// the writer skips pinned slots (head advances, frame not published -> hole)
// and never overwrites or waits; effective capacity is N - R.
#include "thyme/check.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

using namespace thyme;

namespace {

void WriteFrame(PinnedSlotWriter& w, uint8_t tag, bool* skipped = nullptr) {
  PinnedSlotWriter::BeginView bv = w.Begin(256);
  if (skipped != nullptr) *skipped = bv.payload == nullptr;
  if (bv.payload == nullptr) {
    w.Commit();  // skipped begin: no-op commit
    return;
  }
  std::memset(bv.payload, tag, 256);
  w.Commit();
}

uint32_t PinOf(const PinnedSlotReader& r, uint64_t frame_index) {
  return LeasePinOf(reinterpret_cast<const PinnedSlotLeaseCell*>(r.lease() + 1)
                        [frame_index % 4]
                            .pin_gen.load(std::memory_order_acquire));
}

}  // namespace

int main() {
  char name[96];
  std::snprintf(name, sizeof(name), "/c9lease_pin_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());

  PinnedSlotWriter w;
  CHECK(w.Create(name, 256, 4, 0));
  PinnedSlotReader r;
  CHECK(r.Open(name, 1000000000ULL));

  for (uint8_t f = 0; f < 4; ++f) WriteFrame(w, f);

  // Acquire validates + pins; release drops the pin.
  {
    PinnedSlotReader::View v;
    uint32_t gen = 0;
    CHECK(r.AcquireLease(2, v, &gen) == PinnedSlotReader::ReadResult::kOk);
    CHECK(v.data_len == 256);
    CHECK(v.payload[0] == 2);
    CHECK(PinOf(r, 2) == 1);
    CHECK(PinOf(r, 3) == 0);
    // Second lease on the same slot stacks (same generation).
    PinnedSlotReader::View v2;
    uint32_t gen2 = 0;
    CHECK(r.AcquireLease(2, v2, &gen2) == PinnedSlotReader::ReadResult::kOk);
    CHECK(gen2 == gen);
    CHECK(PinOf(r, 2) == 2);
    r.ReleaseLease(2, gen);
    CHECK(PinOf(r, 2) == 1);
    r.ReleaseLease(2, gen2);
    CHECK(PinOf(r, 2) == 0);
  }
  // Holes / evicted frames cannot be leased. Boundary: 4 frames into 4 slots,
  // frame 0 is still there (h-f == count is not an overrun); unpublished index
  // fails.
  {
    PinnedSlotReader::View v;
    uint32_t gen = 0;
    CHECK(r.AcquireLease(0, v, &gen) == PinnedSlotReader::ReadResult::kOk);
    CHECK(v.payload[0] == 0);
    r.ReleaseLease(0, gen);
    CHECK(r.AcquireLease(99, v, &gen) ==
          PinnedSlotReader::ReadResult::kNotPublished);
  }
  // RAII: destruction releases.
  {
    PinnedSlotReader::Lease l = r.Acquire(1);
    CHECK(l.valid());
    CHECK(PinOf(r, 1) == 1);
    CHECK(l.view().payload[0] == 1);
    l.Reset();
    CHECK(!l.valid());
    CHECK(PinOf(r, 1) == 0);
    {
      PinnedSlotReader::Lease l2 = r.Acquire(1);
      CHECK(l2.valid());
    }  // dtor releases
    CHECK(PinOf(r, 1) == 0);
  }

  // Pinned slot -> Begin skips (no overwrite, no wait), head advances,
  // the follow reader sees a hole.
  {
    PinnedSlotReader::Lease hold = r.Acquire(2);  // pin slot 2 (frame 2)
    CHECK(hold.valid());
    bool skipped = false;
    WriteFrame(w, 0xF0, &skipped);  // frame 4 -> slot 0
    CHECK(!skipped);
    WriteFrame(w, 0xF1, &skipped);  // frame 5 -> slot 1
    CHECK(!skipped);
    WriteFrame(w, 0xF2, &skipped);  // frame 6 -> slot 2: PINNED -> skip
    CHECK(skipped);
    WriteFrame(w, 0xF3, &skipped);  // frame 7 -> slot 3
    CHECK(!skipped);
    CHECK(w.lease()->pinned_skip.load(std::memory_order_acquire) == 1);
    // The pinned slot still holds frame 2, byte-intact.
    CHECK(hold.view().payload[0] == 2);
    CHECK(hold.view().payload[255] == 2);

    // Reader cursor follows: gap-skip aligns the cursor to the retained-window
    // start 4; frame 6 is a skip hole and is never delivered.
    PinnedSlotReader::View v;
    CHECK(r.TryRead(v) && v.frame_index == 4);
    bool saw_hole_at_6 = false;
    uint64_t last_seen = 4;
    while (r.TryRead(v)) {
      if (v.frame_index == 6) saw_hole_at_6 = true;  // must not appear
      last_seen = v.frame_index;
    }
    CHECK(!saw_hole_at_6);
    CHECK(last_seen == 7);
    CHECK(PinOf(r, 6 % 4) == 1);  // slot 2's pin still belongs to frame 2
    hold.Reset();
    CHECK(PinOf(r, 2) == 0);
    // skipped frame 6 cannot be leased (the slot holds frame 2); frame 0 is evicted.
    PinnedSlotReader::View v2;
    uint32_t gen = 0;
    CHECK(r.AcquireLease(6, v2, &gen) == PinnedSlotReader::ReadResult::kOverrun);
    CHECK(r.AcquireLease(0, v2, &gen) == PinnedSlotReader::ReadResult::kOverrun);
  }

  r.Close();
  w.Close();
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  return 0;
}
