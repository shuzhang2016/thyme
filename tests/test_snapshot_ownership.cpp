// test_snapshot_ownership.cpp — BF-13: HeaderSnapshot is metadata-only
// (CopyTo never copies payload), CopyPayloadTo copies bytes only while the
// source can still prove it is intact, and the legacy Snapshot alias keeps
// source compatibility. The negative case documents that a HeaderSnapshot
// payload must not be read after its lease was released; the supported path
// is the guarded copy (or ReadFrameStable's owned scratch), so the test never
// dereferences the stale pointer.
#include "thyme/check.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <unistd.h>

using namespace thyme;

static_assert(
    std::is_same<PinnedSlotReader::View::Snapshot,
                 PinnedSlotReader::View::HeaderSnapshot>::value,
    "Snapshot must remain a compatibility alias of HeaderSnapshot");
static_assert(
    std::is_copy_constructible<PinnedSlotReader::View::HeaderSnapshot>::value,
    "HeaderSnapshot must be copyable (it owns all metadata)");
static_assert(
    std::is_copy_assignable<PinnedSlotReader::View::HeaderSnapshot>::value,
    "HeaderSnapshot must be copy-assignable");

namespace {

void WriteFrame(PinnedSlotWriter& w, uint8_t tag, uint64_t mono_ns) {
  PinnedSlotWriter::BeginView bv = w.Begin(128);
  CHECK(bv.payload != nullptr);
  std::memset(bv.payload, tag, 128);
  w.Commit(mono_ns);
}

}  // namespace

int main() {
  char name[96];
  std::snprintf(name, sizeof(name), "/c9snapshot_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());

  PinnedSlotWriter w;
  CHECK(w.Create(name, 128, 4, 0));
  PinnedSlotReader r;
  CHECK(r.Open(name, 1000000000ULL));
  WriteFrame(w, 0x11, 1000);
  WriteFrame(w, 0x22, 2000);

  // --- CopyTo: metadata only; payload stays a borrowed pointer. ---
  {
    PinnedSlotReader::View v;
    CHECK(r.ReadFrame(1, v) == PinnedSlotReader::ReadResult::kOk);
    PinnedSlotReader::View::HeaderSnapshot snap;
    v.CopyTo(snap);
    CHECK(snap.data_len == 128);
    CHECK(snap.frame_index == 1);
    CHECK(snap.slot_seq == 2);
    CHECK(snap.mono_ns == v.mono_ns);
    CHECK(snap.crc32 == v.crc32);
    CHECK(snap.crc_ok);
    CHECK(snap.payload == v.payload);  // borrowed, not copied
    const PinnedSlotReader::View::HeaderSnapshot copy = snap;
    CHECK(copy.payload == snap.payload);

    uint8_t buf[128];
    std::memset(buf, 0, sizeof(buf));
    CHECK(v.CopyPayloadTo(buf, sizeof(buf)));
    CHECK(buf[0] == 0x22);
    CHECK(buf[127] == 0x22);
    // Too small: refuse instead of truncating.
    CHECK(!v.CopyPayloadTo(buf, 127));
  }

  // --- Lease-backed view: guarded copy works while valid, refuses after
  // release. snap.payload is NOT a supported read path once the lease is
  // gone; this test deliberately never dereferences it after Release. ---
  {
    PinnedSlotReader::Lease lease = r.Acquire(1);
    CHECK(lease.valid());
    CHECK(lease.still_valid());

    PinnedSlotReader::View::HeaderSnapshot snap;
    lease.view().CopyTo(snap);
    CHECK(snap.payload == lease.view().payload);

    uint8_t buf[128];
    std::memset(buf, 0, sizeof(buf));
    CHECK(lease.CopyPayloadTo(buf, sizeof(buf)));
    CHECK(buf[0] == 0x22);
    CHECK(lease.view().CopyPayloadTo(buf, sizeof(buf)));  // view link checks pin

    CHECK(lease.Release() == LeaseReleaseResult::kOk);
    // Negative case: the borrowed payload has no lifetime guarantee now. The
    // documented guard refuses instead of copying; the snapshot's pointer is
    // still non-null and must not be read here.
    CHECK(snap.payload != nullptr);
    CHECK(!lease.view().CopyPayloadTo(buf, sizeof(buf)));
    CHECK(!lease.CopyPayloadTo(buf, sizeof(buf)));
  }

  // --- Documented cross-lease retention path: ReadFrameStable owns the
  // bytes in caller scratch and stays readable after the reader moves on. ---
  {
    uint8_t scratch[128];
    PinnedSlotReader::View stable;
    CHECK(r.ReadFrameStable(1, scratch, sizeof(scratch), stable) ==
          PinnedSlotReader::ReadResult::kOk);
    CHECK(stable.payload == scratch);
    uint8_t buf[128];
    CHECK(stable.CopyPayloadTo(buf, sizeof(buf)));
    CHECK(buf[0] == 0x22);
    // Overwrite the slot (frames 2..5 wrap the 4 slots); the scratch copy is
    // unaffected because ReadFrameStable validated it before returning.
    WriteFrame(w, 0x33, 3000);
    WriteFrame(w, 0x44, 4000);
    WriteFrame(w, 0x55, 5000);
    WriteFrame(w, 0x66, 6000);
    CHECK(scratch[0] == 0x22);
    CHECK(buf[0] == 0x22);
  }

  r.Close();
  w.Close();
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  return 0;
}
