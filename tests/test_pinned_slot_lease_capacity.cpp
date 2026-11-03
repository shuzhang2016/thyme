// test_pinned_slot_lease_capacity.cpp — capacity semantics: with R of N
// slots pinned, the writer's effective capacity is N - R: wraps skip pinned
// slots (holes) while unpinned slots keep publishing.
#include "thyme/check.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

using namespace thyme;

int main() {
  char name[96];
  std::snprintf(name, sizeof(name), "/c9lease_cap_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());

  constexpr uint32_t kSlots = 4;
  PinnedSlotWriter w;
  CHECK(w.Create(name, 128, kSlots, 0));
  PinnedSlotReader r;
  CHECK(r.Open(name, 1000000000ULL));

  // Baseline: no pins — 4 frames, all published.
  for (uint8_t f = 0; f < 4; ++f) {
    PinnedSlotWriter::BeginView bv = w.Begin(128);
    CHECK(bv.payload != nullptr);
    std::memset(bv.payload, f, 128);
    w.Commit();
  }
  CHECK(w.head() == 4);
  CHECK(w.pinned_skip_count() == 0);

  // Pin slots of frames 0 and 1 (R=2). The next 4 frames wrap the ring:
  // frames 4,5 land on pinned slots -> skipped; 6,7 publish.
  PinnedSlotReader::Lease l0 = r.Acquire(0);
  PinnedSlotReader::Lease l1 = r.Acquire(1);
  CHECK(l0.valid() && l1.valid());
  uint64_t published = 0;
  for (uint8_t f = 0; f < 4; ++f) {
    PinnedSlotWriter::BeginView bv = w.Begin(128);
    if (bv.payload != nullptr) {
      std::memset(bv.payload, 0xC0 + f, 128);
      ++published;
    }
    w.Commit();
  }
  CHECK(published == 2);  // capacity N - R = 2
  CHECK(w.head() == 8);
  CHECK(w.pinned_skip_count() == 2);
  // Pinned content is byte-intact.
  CHECK(l0.view().payload[0] == 0);
  CHECK(l1.view().payload[0] == 1);

  // Release both: capacity restored.
  l0.Reset();
  l1.Reset();
  published = 0;
  for (uint8_t f = 0; f < 4; ++f) {
    PinnedSlotWriter::BeginView bv = w.Begin(128);
    if (bv.payload != nullptr) {
      std::memset(bv.payload, 0xD0 + f, 128);
      ++published;
    }
    w.Commit();
  }
  CHECK(published == 4);
  CHECK(w.pinned_skip_count() == 2);  // no longer increases

  r.Close();
  w.Close();
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  return 0;
}
