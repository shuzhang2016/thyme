#pragma once
#include "thyme/contracts/abi.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/shm_page.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace thyme {
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "SharedHealthPage requires lock-free uint32 atomics");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "SharedHealthPage requires lock-free uint64 atomics");
static_assert(std::is_trivially_copyable<HealthSlot>::value,
              "HealthSlot must be trivially copyable for shared-memory use");
static_assert(sizeof(HealthSlot) == 64,
              "HealthSlot is one cacheline; new fields must fit the padding");

// Layout identity of the health slot (v3, RT-23). It is part of the page
// layout hash, so a field move/width change cannot silently keep the same
// layout_hash.
inline constexpr uint64_t HashHealthSlotLayout() {
  return ShmLayoutHash(
      0x48534C4F5433ULL /* "HSLOT3" */, sizeof(HealthSlot),
      alignof(HealthSlot), offsetof(HealthSlot, alive_counter),
      sizeof(std::atomic<uint32_t>), offsetof(HealthSlot, state),
      sizeof(std::atomic<uint32_t>), offsetof(HealthSlot, last_finish_mono_ns),
      sizeof(std::atomic<uint64_t>), offsetof(HealthSlot, output_sequence),
      sizeof(std::atomic<uint32_t>), offsetof(HealthSlot, deadline_miss_cnt),
      sizeof(std::atomic<uint32_t>), offsetof(HealthSlot, drop_cnt),
      sizeof(std::atomic<uint64_t>), offsetof(HealthSlot, error_code),
      sizeof(std::atomic<uint32_t>), offsetof(HealthSlot, sched_mismatch_cnt),
      sizeof(std::atomic<uint32_t>), offsetof(HealthSlot, memory_flags),
      sizeof(std::atomic<uint32_t>));
}

// ARCH-11 / RT-23 page layout. Every page starts with the 64 B
// ShmSuperHeader (magic@0, version@8, layout_hash@16, page_bytes@24,
// feature_bits@32, reserved-zero) so supervisor, roles and the probe share one
// validation path. Health-specific identity follows the super-header and the
// slot array starts on the next cache line:
//
//   offset 0    ShmSuperHeader (64 B)
//   offset 64   Header (64 B: session_id/worker_count/control + reserved-zero)
//   offset 128  HealthSlot slots[kMaxWorkers] (64 B each)
//
// v3 changes versus v2: the super-header replaces the in-page magic/version
// words (same offsets, so a magic-first probe still classifies the page), the
// health identity moved to its own 64 B cell, and RT-23 widened drop_cnt to
// 64 bits. v2 pages are rejected by the version check, not dual-read.
struct alignas(64) SharedHealthPage {
  static constexpr uint32_t kMaxWorkers =
      static_cast<uint32_t>(kAbiMaxThreads);
  static constexpr uint64_t kMagic = 0x5448594D455F4850ULL;  // "THYME_HP"
  // v2: kMaxWorkers raised 16 -> 64 (L4 sensor topologies).
  // v3: ARCH-11 super-header + RT-23 64-bit drop counters.
  static constexpr uint32_t kVersion = 3;
  // No optional behaviour is negotiated today; the field exists so a future
  // reader can refuse pages that require a feature it does not implement.
  static constexpr uint64_t kFeatureBits = 0;

  struct alignas(64) Header {
    std::atomic<uint32_t> session_id{0};
    std::atomic<uint32_t> worker_count{0};
    std::atomic<uint32_t> control{0};  // reserved/unused; Init stores 0
    uint32_t reserved0 = 0;
    uint64_t reserved[6] = {};
  };

  ShmSuperHeader super;
  Header header;
  alignas(64) HealthSlot slots[kMaxWorkers];

  // Full page description used by ShmRegion::CreateDescribed/OpenDescribed.
  static constexpr ShmPageSpec Spec();

  uint64_t magic() const {
    return super.magic.load(std::memory_order_acquire);
  }
  uint32_t abi_version() const {
    return super.abi_version.load(std::memory_order_acquire);
  }
  uint64_t layout_hash() const {
    return super.layout_hash.load(std::memory_order_acquire);
  }
  uint64_t page_bytes() const {
    return super.page_bytes.load(std::memory_order_acquire);
  }
  uint64_t feature_bits() const {
    return super.feature_bits.load(std::memory_order_acquire);
  }
  uint32_t session_id() const {
    return header.session_id.load(std::memory_order_acquire);
  }
  uint32_t worker_count() const {
    return header.worker_count.load(std::memory_order_acquire);
  }
  uint32_t control() const {
    return header.control.load(std::memory_order_acquire);
  }

  // Init failure (worker_count > kMaxWorkers) release-stores magic=0 and
  // invalidates a previously valid page; magic is the sole validity gate;
  // other header fields may be stale. On success the payload fields are
  // written before the super-header's magic-last release store, so a reader
  // that observes a valid magic also observes zeroed slots.
  bool Init(uint32_t session_id, uint32_t worker_count) {
    super.magic.store(0, std::memory_order_release);
    if (worker_count > kMaxWorkers) return false;
    header.session_id.store(0, std::memory_order_relaxed);
    header.worker_count.store(0, std::memory_order_relaxed);
    header.control.store(0, std::memory_order_relaxed);
    header.reserved0 = 0;
    for (uint64_t& value : header.reserved) value = 0;
    for (uint32_t i = 0; i < kMaxWorkers; ++i) {
      HealthSlot& s = slots[i];
      s.alive_counter.store(0, std::memory_order_relaxed);
      s.state.store(0, std::memory_order_relaxed);
      s.last_finish_mono_ns.store(0, std::memory_order_relaxed);
      s.output_sequence.store(0, std::memory_order_relaxed);
      s.deadline_miss_cnt.store(0, std::memory_order_relaxed);
      s.drop_cnt.store(0, std::memory_order_relaxed);
      s.error_code.store(0, std::memory_order_relaxed);
      s.sched_mismatch_cnt.store(0, std::memory_order_relaxed);
      s.memory_flags.store(0, std::memory_order_relaxed);
    }
    header.worker_count.store(worker_count, std::memory_order_relaxed);
    header.session_id.store(session_id, std::memory_order_relaxed);
    // Writer: relaxed payload/header fields, then release-store magic.
    InitShmSuperHeader(&super, Spec());
    return true;
  }

  // Full ARCH-11 attach validation: magic, ABI version, layout hash, page
  // bytes, feature bits and reserved words. `error` receives the first
  // mismatch with expected/actual values for diagnostics.
  bool Validate(ShmPageError* error = nullptr) const {
    if (ValidateShmSuperHeader(this, sizeof(*this), Spec(), error) !=
        ShmPageErrc::kOk) {
      return false;
    }
    if (header.reserved0 != 0) {
      if (error != nullptr) {
        *error = ShmPageError{ShmPageErrc::kBadReserved, 0, header.reserved0};
      }
      return false;
    }
    for (const uint64_t value : header.reserved) {
      if (value != 0) {
        if (error != nullptr) {
          *error = ShmPageError{ShmPageErrc::kBadReserved, 0, value};
        }
        return false;
      }
    }
    return true;
  }
};

// Whole-page layout (SharedHealthPage v3). The probe walks slots by offsetof
// and supervisors map exactly sizeof(SharedHealthPage).
static_assert(offsetof(SharedHealthPage, super) == 0,
              "super-header leads the page (magic@0, version@8)");
static_assert(offsetof(SharedHealthPage, super) +
                      offsetof(ShmSuperHeader, magic) ==
                  0,
              "page magic stays at offset 0");
static_assert(offsetof(ShmSuperHeader, abi_version) == 8, "version at 8");
static_assert(sizeof(SharedHealthPage::Header) == 64,
              "health identity is one cache line");
static_assert(offsetof(SharedHealthPage, header) == sizeof(ShmSuperHeader),
              "health fields follow the super-header");
static_assert(offsetof(SharedHealthPage::Header, session_id) == 0,
              "session_id offset is page ABI (OPS-02 identity check)");
static_assert(offsetof(SharedHealthPage::Header, worker_count) == 4,
              "worker_count offset is page ABI");
static_assert(offsetof(SharedHealthPage::Header, control) == 8,
              "control offset is page ABI");
static_assert(offsetof(SharedHealthPage, slots) ==
                  sizeof(ShmSuperHeader) + sizeof(SharedHealthPage::Header),
              "slots start right after the super-header + health identity");
static_assert(offsetof(SharedHealthPage, slots) % 64 == 0,
              "slots are cacheline aligned");
static_assert(sizeof(SharedHealthPage) ==
                  sizeof(ShmSuperHeader) +
                      sizeof(SharedHealthPage::Header) +
                      SharedHealthPage::kMaxWorkers * sizeof(HealthSlot),
              "page size is the manifest slot_bytes ABI");

inline constexpr uint64_t SharedHealthPageLayoutHash() {
  return ShmLayoutHash(
      0x53485033ULL /* "SHP3" */, SharedHealthPage::kVersion,
      sizeof(SharedHealthPage), alignof(SharedHealthPage),
      offsetof(SharedHealthPage, super), sizeof(ShmSuperHeader),
      offsetof(SharedHealthPage, header), sizeof(SharedHealthPage::Header),
      offsetof(SharedHealthPage::Header, session_id),
      sizeof(std::atomic<uint32_t>),
      offsetof(SharedHealthPage::Header, worker_count),
      sizeof(std::atomic<uint32_t>),
      offsetof(SharedHealthPage::Header, control),
      sizeof(std::atomic<uint32_t>), offsetof(SharedHealthPage, slots),
      sizeof(HealthSlot), SharedHealthPage::kMaxWorkers,
      HashHealthSlotLayout());
}
static_assert(SharedHealthPageLayoutHash() != 0,
              "0 is reserved for an unknown layout identity");

inline constexpr ShmPageSpec SharedHealthPage::Spec() {
  return ShmPageSpec{SharedHealthPage::kMagic, SharedHealthPage::kVersion,
                     SharedHealthPageLayoutHash(), sizeof(SharedHealthPage),
                     SharedHealthPage::kFeatureBits};
}

}  // namespace thyme
