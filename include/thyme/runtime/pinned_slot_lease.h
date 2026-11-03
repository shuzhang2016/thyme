#pragma once

#include "thyme/runtime/shm_page.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace thyme {

inline constexpr uint64_t kPinnedSlotLeaseMagic =
    0x5448594D4C534531ULL;  // "THYMLSE1"
// v6: ARCH-11 super-header before the lease body/cells; v5 added
// copy-active/owner recovery. v1..v5 are rejected.
inline constexpr uint32_t kPinnedSlotLeaseVersion = 6;
// Default timeout for reclaiming a dead reader's lease; the writer may
// override it at Create.
inline constexpr uint64_t kPinnedSlotLeaseDefaultStealTimeoutNs =
    5000000000ULL;  // 5 s

// ---- lease table (separate shm object "<name>.lease", BF-01) ----
struct alignas(64) PinnedSlotLeaseHeader {
  ShmSuperHeader super;
  std::atomic<uint64_t> magic{0};      // kPinnedSlotLeaseMagic
  std::atomic<uint32_t> version{0};    // kPinnedSlotLeaseVersion
  std::atomic<uint32_t> slot_count{0};
  std::atomic<uint64_t> steal_timeout_ns{0};  // BF-04: dead-reader steal
  std::atomic<uint64_t> pinned_skip{0};       // BF-03: writer skipped pins
  std::atomic<uint64_t> lease_steal{0};       // BF-04: expired leases stolen
  uint64_t reserved[3] = {};

  static constexpr uint64_t LayoutHash() {
    return ShmLayoutHash(
        kPinnedSlotLeaseMagic, kPinnedSlotLeaseVersion,
        static_cast<uint64_t>(sizeof(PinnedSlotLeaseHeader)),
        static_cast<uint64_t>(alignof(PinnedSlotLeaseHeader)),
        static_cast<uint64_t>(offsetof(PinnedSlotLeaseHeader, super)),
        static_cast<uint64_t>(offsetof(PinnedSlotLeaseHeader, slot_count)),
        static_cast<uint64_t>(offsetof(PinnedSlotLeaseHeader, steal_timeout_ns)),
        static_cast<uint64_t>(offsetof(PinnedSlotLeaseHeader, pinned_skip)),
        static_cast<uint64_t>(offsetof(PinnedSlotLeaseHeader, lease_steal)));
  }

  // page_bytes covers the header only; the cell table is appended.
  static constexpr ShmPageSpec Spec() {
    return ShmPageSpec{kPinnedSlotLeaseMagic, kPinnedSlotLeaseVersion, LayoutHash(),
                       static_cast<uint64_t>(sizeof(PinnedSlotLeaseHeader)), 0};
  }
};

// Lease page layout, kPinnedSlotLeaseVersion == 6 (ARCH-11 super-header; v5
// added copy-active/owner recovery). LeaseRegionBytes() = sizeof(header) +
// slot_count * sizeof(cell); cells start after this header.
static_assert(sizeof(PinnedSlotLeaseHeader) == 128,
              "lease header is super-header + one body cache line");
static_assert(offsetof(PinnedSlotLeaseHeader, super) == 0,
              "super-header leads the lease page");
static_assert(offsetof(PinnedSlotLeaseHeader, version) == 72,
              "version offset is lease ABI");
static_assert(offsetof(PinnedSlotLeaseHeader, slot_count) == 76,
              "lease geometry offsets are lease ABI");
static_assert(offsetof(PinnedSlotLeaseHeader, steal_timeout_ns) == 80,
              "steal timeout is read by every attacher");
static_assert(offsetof(PinnedSlotLeaseHeader, pinned_skip) == 88,
              "lease counters are probe/observability ABI");
static_assert(offsetof(PinnedSlotLeaseHeader, lease_steal) == 96,
              "lease counters are probe/observability ABI");
static_assert(std::is_standard_layout<PinnedSlotLeaseHeader>::value &&
                  std::is_trivially_copyable<PinnedSlotLeaseHeader>::value,
              "PinnedSlotLeaseHeader must be POD for SHM");

// Cell: packs a 64-bit word = (generation << 32) | copy_active << 31 | pin.
// The generation prevents release-after-steal underflow/mis-decrement:
// acquire records the generation, steal increments it and clears the pin, and
// release decrements only on a generation match — a dead reader's old guard
// can never touch a new reader's pin.
// Invariant (lease ABI v4/v5): after a successful acquire returns with
// pin > 0, the epoch is this acquire's stamp time (fresh). The epoch is only
// stamped after validation passes; an attempt whose final seq re-check fails
// rolls its own stamp CAS back — a failed attempt must not refresh a dead
// pin's epoch, otherwise the writer would never reach the steal timeout. The
// rollback only applies while its own stamp is still the current value, so a
// concurrent successful lease's newer epoch is unaffected. When pin==0 the
// epoch may retain a stale value.
// v5: copy_active and pin/gen are arbitrated by the same CAS; pin increments
// and decrements must preserve active.
// While active, the writer must not overwrite the payload even if a
// concurrent release decremented the pin to 0.
struct alignas(64) PinnedSlotLeaseCell {
  std::atomic<uint64_t> pin_gen{0};  // (gen << 32) | active << 31 | pin
  std::atomic<uint64_t> epoch{0};    // last successful acquire time (mono ns)
  std::atomic<uint64_t> copy_owner_pid{0};
  std::atomic<uint64_t> copy_owner_start{0};
  uint64_t reserved[4] = {};
};

static_assert(sizeof(PinnedSlotLeaseCell) == 64,
              "lease cell must fill exactly one cache line");
static_assert(offsetof(PinnedSlotLeaseCell, epoch) == 8,
              "pin_gen/epoch offsets are the v5 lease word layout");
static_assert(offsetof(PinnedSlotLeaseCell, copy_owner_pid) == 16,
              "copy owner identity is lease ABI v5");
static_assert(offsetof(PinnedSlotLeaseCell, copy_owner_start) == 24,
              "copy owner identity is lease ABI v5");
static_assert(std::is_standard_layout<PinnedSlotLeaseCell>::value &&
                  std::is_trivially_copyable<PinnedSlotLeaseCell>::value,
              "PinnedSlotLeaseCell must be POD for SHM");

inline constexpr uint64_t kPinnedSlotLeaseCopyActive = 1ULL << 31;
inline constexpr uint32_t kPinnedSlotLeaseMaxPin = 0x7FFFFFFFu;

inline uint32_t LeasePinOf(uint64_t word) {
  return static_cast<uint32_t>(word & kPinnedSlotLeaseMaxPin);
}
inline uint32_t LeaseGenOf(uint64_t word) {
  return static_cast<uint32_t>(word >> 32);
}
inline bool LeaseCopyActiveOf(uint64_t word) {
  return (word & kPinnedSlotLeaseCopyActive) != 0;
}
inline uint64_t LeasePack(uint32_t gen, uint32_t pin, bool copy_active = false) {
  return (static_cast<uint64_t>(gen) << 32) | pin |
         (copy_active ? kPinnedSlotLeaseCopyActive : 0);
}

enum class LeaseCopyBeginResult : uint8_t {
  kOk,
  kBusy,        // another copier owns or is publishing the owner identity
  kInvalid,     // generation mismatch or no pin; copying is forbidden
  kUnavailable, // no reliable process start token; use the atomic fallback
};

inline void LeaseCopyOwnerRelease(PinnedSlotLeaseCell& cell, uint64_t pid) {
  if (cell.copy_owner_pid.load(std::memory_order_acquire) != pid) {
    return;
  }
  cell.copy_owner_start.store(0, std::memory_order_release);
  uint64_t expected = pid;
  (void)cell.copy_owner_pid.compare_exchange_strong(
      expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
}

inline LeaseCopyBeginResult LeaseCopyBegin(PinnedSlotLeaseCell& cell, uint32_t gen,
                                           uint64_t pid, uint64_t start) {
  if (pid == 0 || start == 0) {
    return LeaseCopyBeginResult::kUnavailable;
  }
  uint64_t cur = cell.pin_gen.load(std::memory_order_acquire);
  if (LeaseGenOf(cur) != gen || LeasePinOf(cur) == 0) {
    return LeaseCopyBeginResult::kInvalid;
  }
  if (LeaseCopyActiveOf(cur)) {
    return LeaseCopyBeginResult::kBusy;
  }

  uint64_t expected = 0;
  if (!cell.copy_owner_pid.compare_exchange_strong(
          expected, pid, std::memory_order_acq_rel, std::memory_order_acquire)) {
    cur = cell.pin_gen.load(std::memory_order_acquire);
    return LeaseGenOf(cur) == gen && LeasePinOf(cur) > 0
               ? LeaseCopyBeginResult::kBusy
               : LeaseCopyBeginResult::kInvalid;
  }
  cell.copy_owner_start.store(start, std::memory_order_release);
  for (;;) {
    if (LeaseGenOf(cur) != gen || LeasePinOf(cur) == 0) {
      LeaseCopyOwnerRelease(cell, pid);
      return LeaseCopyBeginResult::kInvalid;
    }
    if (LeaseCopyActiveOf(cur)) {
      LeaseCopyOwnerRelease(cell, pid);
      return LeaseCopyBeginResult::kBusy;
    }
    if (cell.pin_gen.compare_exchange_weak(
            cur, LeasePack(gen, LeasePinOf(cur), true),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
      return LeaseCopyBeginResult::kOk;
    }
  }
}

inline void LeaseCopyEnd(PinnedSlotLeaseCell& cell, uint32_t gen, uint64_t pid) {
  uint64_t cur = cell.pin_gen.load(std::memory_order_acquire);
  for (;;) {
    if (LeaseGenOf(cur) != gen || !LeaseCopyActiveOf(cur)) {
      break;
    }
    if (cell.pin_gen.compare_exchange_weak(
            cur, LeasePack(gen, LeasePinOf(cur)), std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      break;
    }
  }
  LeaseCopyOwnerRelease(cell, pid);
}

// BF-12: outcome of Lease::Release(). The generation-matched unpin is the only
// safe way to drop a pin, so Release must tell the caller whether it actually
// still owned the pin:
//   kOk              — generation matched and pin was decremented (the lease
//                      was still valid at release time).
//   kAlreadyReleased — this lease no longer owns a pin (never acquired,
//                      moved-from, already released, or the reader was
//                      closed / read-only).
//   kStolen          — the writer stole the slot (generation advanced) while
//                      the lease was held; the pin was already cleared, so
//                      nothing was decremented. The caller must not use the
//                      borrowed payload after this point.
enum class LeaseReleaseResult : uint8_t {
  kOk,
  kAlreadyReleased,
  kStolen,
};

}  // namespace thyme
