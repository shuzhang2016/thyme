// RT-10: cacheline / false-sharing audit gate.
//
// The audit (docs/reference/cacheline-audit.md) records sizeof/alignof/offsetof for the
// SHM pages and the hot in-process structures. This binary pins those numbers
// with static_asserts, so introducing new padding (a new field that pushes a
// later field off its line, or grows a page) fails the build and forces the
// audit table to be updated in the same change.
//
// It intentionally does NOT re-assert what the owning headers already lock
// (RT-07) one-for-one; it pins the *audit table itself* and the invariants the
// table claims: hot shared words own a line, page sub-tables start on a line,
// and the per-struct sizes recorded in the doc stay exact.
//
// -Wpadded is not usable as an automated gate here: every intentional
// alignas(64) hole in these layouts would be reported, i.e. the gate would be
// all false positives. The manual procedure is in docs/guides/test-matrix.md.
#include "thyme/app/mp_control.h"
#include "thyme/check.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/record_mirror.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shared_health.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"
#include "thyme/runtime/spsc_ring.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

using namespace thyme;

namespace {

struct Pod8 {
  uint64_t a;
};

using SpscLayout = ShmSpscRing<Pod8, 4>::Layout;
using SpmcLayout = ShmSpmcRing<Pod8, 4>::Layout;
using SpmcCell = SpmcLayout::Cell;

// Every audited SHM struct is fixed-width (lock-free atomics only), so these
// numbers are architecture-independent ABI, not a host detail.
static_assert(sizeof(PinnedSlotPageHeader) == 384, "audit: channel header");
static_assert(alignof(PinnedSlotPageHeader) == 64, "audit: channel header");
static_assert(sizeof(PinnedSlotHeader) == 64, "audit: slot header");
static_assert(sizeof(PinnedSlotFooter) == 64, "audit: slot footer");
static_assert(sizeof(PinnedSlotLeaseHeader) == 128, "audit: lease header");
static_assert(sizeof(PinnedSlotLeaseCell) == 64, "audit: lease cell");
static_assert(alignof(PinnedSlotLeaseCell) == 64,
              "audit: one lease cell per line (writer vs readers)");
static_assert(sizeof(ProducerGate) == 64, "audit: producer gate");
static_assert(alignof(ProducerGate) == 64, "audit: producer gate owns a line");

static_assert(sizeof(HealthSlot) == 64, "audit: health slot");
static_assert(sizeof(SharedHealthPage::Header) == 64, "audit: health header");
static_assert(sizeof(ShmSuperHeader) == 64, "audit: super header");
static_assert(sizeof(SharedHealthPage) == 128u + 64u * kAbiMaxThreads,
              "audit: health page (ABI-13 profile-aware)");

static_assert(sizeof(app::MpControl) == 320, "audit: mp control page");
static_assert(alignof(app::MpControl) == 64,
              "audit: mp control leads with the 64 B super-header");
static_assert(__builtin_offsetof(app::MpControl, stop) == 64,
              "audit: control scalars follow the super-header");
static_assert(__builtin_offsetof(app::MpControl, trigger_relay_sent) == 80,
              "audit: trigger relay counters stay 8-byte aligned");
static_assert(__builtin_offsetof(app::MpControl, proc_loops) == 96,
              "audit: proc_loops start after the scalar block");
static_assert(__builtin_offsetof(app::MpControl, cross_frames) == 224,
              "audit: cross frame counters follow proc_loops");
static_assert(__builtin_offsetof(app::MpControl, reserved) == 288,
              "audit: explicit tail starts after the arrays");

static_assert(sizeof(RecordMirrorSlotHeader) == 64, "audit: mirror slot");
static_assert(sizeof(RecordMirrorPage::Header) == 64, "audit: mirror header");
static_assert(sizeof(RecordMirrorPage) ==
                  128u + 192u * kRecordMirrorMaxSlots +
                      kRecordMirrorMaxArenaBytes,
              "audit: mirror page (ABI-13 profile-aware)");
static_assert(offsetof(RecordMirrorPage, struct_table) == 128,
              "audit: table");
static_assert(offsetof(RecordMirrorPage, slot_headers) ==
                  128u + 128u * kRecordMirrorMaxSlots,
              "audit: slot metadata after the struct table");
static_assert(offsetof(RecordMirrorPage, payload_arena) ==
                  128u + 192u * kRecordMirrorMaxSlots,
              "audit: arena starts on a line");

static_assert(sizeof(RecordMirrorRingSlotHeader) == 64, "audit: ring slot");
static_assert(sizeof(RecordMirrorRingDesc) == 128, "audit: ring desc");
static_assert(offsetof(RecordMirrorRingDesc, gate) == 64,
              "audit: per-struct gate owns its cache line");
static_assert(sizeof(RecordMirrorRingPage::Header) == 128,
              "audit: ring header is two lines (56 B tail headroom)");
static_assert(sizeof(RecordMirrorRingPage) ==
                  192u + 256u * kRecordMirrorRingMaxStructs,
              "audit: ring page (ABI-13 profile-aware)");
static_assert(offsetof(RecordMirrorRingPage, struct_table) == 192,
              "audit: ring tables follow super-header + two header lines");
static_assert(offsetof(RecordMirrorRingPage, ring_descs) ==
                  192u + 128u * kRecordMirrorRingMaxStructs,
              "audit: ring descs after the struct table (ABI-13 profile-aware)");

static_assert(sizeof(detail::LatestSlotLayout<Pod8>::Cell) == 64,
              "audit: latest slot cell");
static_assert(sizeof(LatestSlot<Pod8>) == 192, "audit: latest slot page");
static_assert(alignof(LatestSlot<Pod8>) == 64,
              "audit: latest slot page is 64-aligned (covers slot_[0])");

static_assert(sizeof(ShmSpscRing<Pod8, 4>::Layout) == 576,
              "audit: spsc ring page");
static_assert(offsetof(SpscLayout, slots) == 320,
              "audit: spsc cells start on line 5");
static_assert(sizeof(SpmcCell) == 64,
              "audit: spmc cell");
static_assert(sizeof(ShmSpmcRing<Pod8, 4>::Layout) == 576,
              "audit: spmc ring page");
static_assert(offsetof(SpmcLayout, slots) == 320,
              "audit: spmc cells start on line 5");

// Hot shared words that must not share a line with an unrelated hot word.
static_assert(offsetof(PinnedSlotPageHeader, head) % 64 == 0, "audit: head");
static_assert(offsetof(PinnedSlotPageHeader, epoch) % 64 == 0,
              "audit: epoch futex word");
static_assert(offsetof(PinnedSlotPageHeader, gate) % 64 == 0, "audit: gate");
static_assert(offsetof(SpscLayout, head) % 64 == 0,
              "audit: spsc head");
static_assert(offsetof(SpscLayout, tail) % 64 == 0,
              "audit: spsc tail owns its line (producer/consumer split)");
static_assert(offsetof(SpscLayout, epoch) % 64 == 0,
              "audit: spsc epoch");
static_assert(offsetof(SpmcLayout, head) % 64 == 0,
              "audit: spmc head");
static_assert(offsetof(SpmcLayout, epoch) % 64 == 0,
              "audit: spmc epoch");
static_assert(offsetof(SharedHealthPage, slots) % 64 == 0,
              "audit: one worker per health line");

// In-process hot structures: alignment is the audited property (sizes depend
// on libc's pthread types and are not ABI).
static_assert(alignof(SpscRing<4>) == 64,
              "audit: spsc head_/tail_ are line-aligned in-process");

// Regenerate the docs/reference/cacheline-audit.md table: `test_cacheline_audit --table`.
#define AUDIT_ROW(T) \
  std::printf("%-40s %8zu %8zu\n", #T, sizeof(T), alignof(T))

void PrintTable() {
  std::printf("%-40s %8s %8s\n", "struct", "sizeof", "alignof");
  AUDIT_ROW(PinnedSlotPageHeader);
  AUDIT_ROW(PinnedSlotHeader);
  AUDIT_ROW(PinnedSlotFooter);
  AUDIT_ROW(PinnedSlotLeaseHeader);
  AUDIT_ROW(PinnedSlotLeaseCell);
  AUDIT_ROW(ProducerGate);
  AUDIT_ROW(HealthSlot);
  AUDIT_ROW(SharedHealthPage::Header);
  AUDIT_ROW(SharedHealthPage);
  AUDIT_ROW(app::MpControl);
  AUDIT_ROW(RecordMirrorSlotHeader);
  AUDIT_ROW(RecordMirrorPage::Header);
  AUDIT_ROW(RecordMirrorPage);
  AUDIT_ROW(RecordMirrorRingSlotHeader);
  AUDIT_ROW(RecordMirrorRingDesc);
  AUDIT_ROW(RecordMirrorRingPage::Header);
  AUDIT_ROW(RecordMirrorRingPage);
  AUDIT_ROW(detail::LatestSlotLayout<Pod8>::Cell);
  AUDIT_ROW(LatestSlot<Pod8>);
  AUDIT_ROW(SpscLayout);
  AUDIT_ROW(SpmcCell);
  AUDIT_ROW(SpmcLayout);
  AUDIT_ROW(SpscRing<4>);
}

#undef AUDIT_ROW

}  // namespace

int main(int argc, char** argv) {
  // Runtime witness for the two lock-free assumptions the audit relies on.
  CHECK(std::atomic<uint32_t>::is_always_lock_free);
  CHECK(std::atomic<uint64_t>::is_always_lock_free);

  if (argc > 1 && std::strcmp(argv[1], "--table") == 0) {
    PrintTable();
    return 0;
  }
  std::printf("test_cacheline_audit: all audited layouts match the table\n");
  return 0;
}
