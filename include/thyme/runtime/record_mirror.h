#pragma once

#include "thyme/contracts/abi.h"
#include "thyme/contracts/struct_desc.h"
#include "thyme/runtime/detail/atomic_payload.h"
#include "thyme/runtime/log.h"
#include "thyme/runtime/shm_page.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace thyme {
// Max POD payload per mirror channel (512 KB — covers full-field perception
// contracts such as FvRoadMapResult_t ~290 KB).
inline constexpr uint32_t kRecordMirrorMaxPayload = kAbiMaxPayloadBytes;
inline constexpr uint32_t kRecordMirrorMaxSlots = kAbiMaxStructCount;
// Per-channel capacities are laid out inside a single shared arena whose
// aggregate budget lives in contracts/abi.h (kRecordMirrorMaxArenaBytes).

// Fixed per-slot metadata (seqlock + sizes + arena offsets). The payload
// bytes live in RecordMirrorPage::payload_arena; each channel's region is
// described by payload_offset / payload_capacity, written by the producer in
// Init before the header.magic release-store so a reader that passes
// Validate() observes a fully-initialized offset table.
struct alignas(64) RecordMirrorSlotHeader {
  std::atomic<uint32_t> seq{0};
  std::atomic<uint64_t> mono_ns{0};
  std::atomic<uint32_t> source_sequence{0};
  std::atomic<uint32_t> payload_size{0};
  uint32_t payload_offset = 0;    // into payload_arena (immutable after Init)
  uint32_t payload_capacity = 0;  // round_up(contract payload_size, 64)
};

// RecordMirrorPage v3 slot metadata (docs/reference/abi-compatibility.md). The probe
// reads payload_size/payload_offset from this header, so the offsets are ABI;
// a change requires a kVersion bump.
static_assert(sizeof(RecordMirrorSlotHeader) == 64,
              "mirror slot header is one cache line per struct");
static_assert(offsetof(RecordMirrorSlotHeader, mono_ns) == 8,
              "slot metadata field order is the v2 wire layout");
static_assert(offsetof(RecordMirrorSlotHeader, payload_size) == 20,
              "slot metadata field order is the v2 wire layout");
static_assert(offsetof(RecordMirrorSlotHeader, payload_offset) == 24,
              "offset table is read by out-of-process probes");
static_assert(offsetof(RecordMirrorSlotHeader, payload_capacity) == 28,
              "offset table is read by out-of-process probes");
static_assert(std::is_standard_layout<RecordMirrorSlotHeader>::value,
              "RecordMirrorSlotHeader must be standard layout for SHM");

// RT-09: bounded seqlock retry budget of ReadSlot (one greppable constant for
// the retry-loop audit; exhaustion reports failure with no payload copied).
inline constexpr int kMirrorReadRetries = 16;

// RecordMirrorPage embeds a ~2 MB payload arena (sizeof ≈ 2.1 MB). NEVER
// stack-allocate — use SHM (record_mirror::Client / ShmRegion::Create) or the
// heap (std::make_unique). A few stack instances overflow the default 8 MB
// thread stack under Release builds.
struct alignas(64) RecordMirrorPage {
  static constexpr uint64_t kMagic = 0x5448594D454D4952ULL;  // "THYMEMIR"
  // Version 4 (ARCH-11): a 64 B ShmSuperHeader leads the page, so attach can
  // validate magic/version/layout hash/page size/features before reading the
  // table. Version 3 (ABI-11) keyed struct_table entries; v1..v3 pages are
  // rejected, no dual-read.
  static constexpr uint32_t kVersion = 4;

  struct alignas(64) Header {
    std::atomic<uint64_t> magic{0};
    std::atomic<uint32_t> version{0};
    std::atomic<uint32_t> session_id{0};
    std::atomic<uint32_t> slot_count{0};
    std::atomic<uint64_t> write_generation{0};
    std::atomic<uint64_t> mirror_drop_cnt{0};
    std::atomic<uint64_t> mirror_odd_skips{0};
    // Subset of mirror_drop_cnt: an oversize drop increments both counters
    // (TryWriteSlot/Write return false → the tap calls AddMirrorDrop, and
    // RecordOversizeDrop also bumps this). Consumers must not sum the two.
    std::atomic<uint64_t> oversize_drop_cnt{0};
  };

  ShmSuperHeader super;
  Header header;
  alignas(64) ThymeStructDesc struct_table[kRecordMirrorMaxSlots];
  alignas(64) RecordMirrorSlotHeader slot_headers[kRecordMirrorMaxSlots];
  // The creator initializes the arena before publishing the SHM page; the
  // default member initializer also makes non-SHM test instances deterministic.
  alignas(64) uint8_t payload_arena[kRecordMirrorMaxArenaBytes] = {};

  uint8_t* PayloadPtr(uint16_t sid) {
    return payload_arena + slot_headers[sid].payload_offset;
  }
  const uint8_t* PayloadPtr(uint16_t sid) const {
    return payload_arena + slot_headers[sid].payload_offset;
  }
  uint32_t SlotCapacity(uint16_t sid) const {
    return slot_headers[sid].payload_capacity;
  }

  static constexpr uint32_t RoundUpCapacity(uint32_t size) {
    return (size + 63u) & ~63u;
  }

  static constexpr uint64_t LayoutHash() {
    return ShmLayoutHash(
        kMagic, kVersion, static_cast<uint64_t>(sizeof(RecordMirrorPage)),
        static_cast<uint64_t>(alignof(RecordMirrorPage)),
        static_cast<uint64_t>(offsetof(RecordMirrorPage, super)),
        static_cast<uint64_t>(offsetof(RecordMirrorPage, header)),
        static_cast<uint64_t>(offsetof(RecordMirrorPage, struct_table)),
        static_cast<uint64_t>(offsetof(RecordMirrorPage, slot_headers)),
        static_cast<uint64_t>(offsetof(RecordMirrorPage, payload_arena)),
        kRecordMirrorMaxSlots, kRecordMirrorMaxArenaBytes);
  }

  static constexpr ShmPageSpec Spec() {
    return ShmPageSpec{kMagic, kVersion, LayoutHash(),
                       static_cast<uint64_t>(sizeof(RecordMirrorPage)), 0};
  }

  bool Init(uint32_t session_id, const ThymeStructDesc* descs, uint32_t count) {
    // Invalidate first: a reader that already passed Validate() must not observe
    // a half-rewritten offset table while magic still holds kMagic. Publish the
    // new layout only at the trailing super-header magic store below.
    super.magic.store(0, std::memory_order_release);
    if (descs == nullptr || count == 0 || count > kRecordMirrorMaxSlots) {
      return false;
    }
    for (uint32_t i = 0; i < kRecordMirrorMaxSlots; ++i) {
      slot_headers[i].seq.store(0, std::memory_order_relaxed);
      slot_headers[i].mono_ns.store(0, std::memory_order_relaxed);
      slot_headers[i].source_sequence.store(0, std::memory_order_relaxed);
      slot_headers[i].payload_size.store(0, std::memory_order_relaxed);
      slot_headers[i].payload_offset = 0;
      slot_headers[i].payload_capacity = 0;
      struct_table[i] = {};
    }
    uint32_t arena_cursor = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t psz = descs[i].payload_size;
      // A zero-payload contract would round its capacity to 0 and become a
      // permanently dead slot; reject it up front.
      if (psz == 0) return false;
      const uint32_t cap = RoundUpCapacity(psz);
      if (psz > kRecordMirrorMaxPayload ||
          cap > kRecordMirrorMaxArenaBytes - arena_cursor) {
        return false;
      }
      struct_table[i] = descs[i];
      struct_table[i].struct_id = static_cast<uint16_t>(i);
      slot_headers[i].payload_offset = arena_cursor;
      slot_headers[i].payload_capacity = cap;
      arena_cursor += cap;
    }
    header.write_generation.store(0, std::memory_order_relaxed);
    header.oversize_drop_cnt.store(0, std::memory_order_relaxed);
    header.slot_count.store(count, std::memory_order_relaxed);
    header.session_id.store(session_id, std::memory_order_relaxed);
    header.version.store(kVersion, std::memory_order_relaxed);
    InitShmSuperHeader(&super, Spec());
    return true;
  }

  bool Validate(ShmPageError* error = nullptr) const {
    if (ValidateShmSuperHeader(this, sizeof(*this), Spec(), error) !=
        ShmPageErrc::kOk) {
      return false;
    }
    if (header.version.load(std::memory_order_relaxed) != kVersion) {
      if (error != nullptr) {
        *error = ShmPageError{ShmPageErrc::kBadAbiVersion, kVersion,
                              header.version.load(std::memory_order_relaxed)};
      }
      return false;
    }
    const uint32_t count = header.slot_count.load(std::memory_order_relaxed);
    if (count > kRecordMirrorMaxSlots) {
      if (error != nullptr) {
        *error = ShmPageError{ShmPageErrc::kBadGeometry, kRecordMirrorMaxSlots,
                              count};
      }
      return false;
    }
    return true;
  }

  uint32_t slot_count() const {
    return header.slot_count.load(std::memory_order_acquire);
  }

  uint64_t mirror_drop_count() const {
    return header.mirror_drop_cnt.load(std::memory_order_acquire);
  }

  uint64_t mirror_odd_skip_count() const {
    return header.mirror_odd_skips.load(std::memory_order_acquire);
  }

  uint64_t oversize_drop_count() const {
    return header.oversize_drop_cnt.load(std::memory_order_acquire);
  }

  void AddMirrorDrop(uint64_t n = 1) {
    header.mirror_drop_cnt.fetch_add(n, std::memory_order_relaxed);
  }

  // Single-producer write: unconditional seqlock bump (no contention check).
  // NEVER mix Write and TryWriteSlot on the same slot — Write ignores the
  // tap-side CAS, so a mixed slot can tear. Pick one discipline per slot.
  bool WriteSlot(uint16_t struct_id, const void* data, uint32_t size,
                 uint64_t mono_ns, uint32_t source_sequence) {
    if (!Validate() || struct_id >= slot_count()) return false;
    if (!Write(struct_id, data, size, mono_ns, source_sequence)) return false;
    header.write_generation.fetch_add(1, std::memory_order_release);
    return true;
  }

  // Non-blocking tap: acquire seqlock (even->odd) before write; drop if
  // contended. All writers of a tap slot must use TryWriteSlot; mixing with
  // the unconditional Write/WriteSlot path breaks the mutual exclusion.
  bool TryWriteSlot(uint16_t struct_id, const void* data, uint32_t size,
                    uint64_t mono_ns, uint32_t source_sequence) {
    if (!Validate() || struct_id >= slot_count()) return false;
    if (data == nullptr || size == 0) return false;
    if (size > SlotCapacity(struct_id)) {
      RecordOversizeDrop(struct_id, size);
      return false;
    }
    RecordMirrorSlotHeader& slot = slot_headers[struct_id];
    uint32_t expected = slot.seq.load(std::memory_order_acquire);
    for (;;) {
      if (expected & 1u) {
        header.mirror_odd_skips.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      if (slot.seq.compare_exchange_weak(expected, expected + 1,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        break;
      }
    }
    // The CAS is acq_rel: it synchronizes the lock handoff, but on ARM the
    // payload/header stores below are only ordered against the seq release by
    // this fence (same store-store ordering argument as Write()'s fence).
    // On x86/TSO this is a no-op; the comment is the contract.
    std::atomic_thread_fence(std::memory_order_release);
    slot.mono_ns.store(mono_ns, std::memory_order_relaxed);
    slot.source_sequence.store(source_sequence, std::memory_order_relaxed);
    slot.payload_size.store(size, std::memory_order_relaxed);
    detail::AtomicPayloadCopyIn(PayloadPtr(struct_id), data, size);
    slot.seq.fetch_add(1, std::memory_order_release);
    header.write_generation.fetch_add(1, std::memory_order_release);
    return true;
  }

  // RT-09: torn_drops (caller-owned, optional) counts samples whose read
  // exhausted the seqlock budget. Caller-owned because readers usually map
  // the page read-only and cannot write a page-side counter — the same
  // convention as RecordMirrorRingPage::ReadRingSample.
  bool ReadSlot(uint16_t struct_id, uint8_t* out, uint32_t out_cap,
                uint32_t& size, uint64_t& mono_ns,
                uint32_t& source_sequence,
                uint32_t* stable_seq = nullptr,
                uint64_t* torn_drops = nullptr) const {
    if (!Validate() || struct_id >= slot_count()) return false;
    const RecordMirrorSlotHeader& slot = slot_headers[struct_id];
    const uint32_t cap = slot.payload_capacity;
    for (int i = 0; i < kMirrorReadRetries; ++i) {
      const uint32_t s0 = slot.seq.load(std::memory_order_acquire);
      if (s0 == 0) return false;
      if (s0 & 1u) continue;
      const uint32_t psz = slot.payload_size.load(std::memory_order_acquire);
      const uint64_t m = slot.mono_ns.load(std::memory_order_acquire);
      const uint32_t ss = slot.source_sequence.load(std::memory_order_acquire);
      const uint32_t off = slot.payload_offset;
      if (psz == 0 || psz > cap || psz > out_cap) return false;
      // Indirect addressing: reject any payload whose arena range [off, off+psz)
      // escapes the mapped region, so a corrupted offset table cannot make a
      // reader memcpy past the SHM mapping.
      if (off > kRecordMirrorMaxArenaBytes ||
          psz > kRecordMirrorMaxArenaBytes - off) return false;
      detail::AtomicPayloadCopyOut(out, payload_arena + off,
                                   psz);  // off: validated above
      const uint32_t s1 = slot.seq.load(std::memory_order_acquire);
      if (s1 == s0) {
        size = psz;
        mono_ns = m;
        source_sequence = ss;
        if (stable_seq != nullptr) *stable_seq = s0;
        return true;
      }
    }
    if (torn_drops != nullptr) *torn_drops += 1;  // retry budget exhausted
    return false;
  }

 private:
  // Unconditional seqlock write for WriteSlot (no CAS; single producer).
  bool Write(uint16_t struct_id, const void* data, uint32_t size, uint64_t mono,
             uint32_t src_seq) {
    if (data == nullptr || size == 0) return false;
    if (size > SlotCapacity(struct_id)) {
      RecordOversizeDrop(struct_id, size);
      return false;
    }
    RecordMirrorSlotHeader& slot = slot_headers[struct_id];
    slot.seq.fetch_add(1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    slot.mono_ns.store(mono, std::memory_order_relaxed);
    slot.source_sequence.store(src_seq, std::memory_order_relaxed);
    slot.payload_size.store(size, std::memory_order_relaxed);
    detail::AtomicPayloadCopyIn(PayloadPtr(struct_id), data, size);
    slot.seq.fetch_add(1, std::memory_order_release);
    return true;
  }

  void RecordOversizeDrop(uint16_t struct_id, uint32_t size) {
    header.oversize_drop_cnt.fetch_add(1, std::memory_order_relaxed);
    LogOversizeOnce(struct_id, size);
  }

  // OBS-08: per-write error path — rate-limited (first, then every 1000th)
  // so a mis-sized producer cannot flood the log; the drop itself is always
  // counted in oversize_drop_cnt.
  void LogOversizeOnce(uint16_t struct_id, uint32_t size) const {
    THYME_LOG_WARN_RATED(
        ::thyme::log::kOnceThenEvery1000,
        "record_mirror: oversized payload dropped struct_id=%u size=%u "
        "capacity=%u",
        static_cast<unsigned>(struct_id), static_cast<unsigned>(size),
        static_cast<unsigned>(slot_headers[struct_id].payload_capacity));
  }
};

static_assert(std::is_standard_layout<RecordMirrorPage>::value,
              "RecordMirrorPage must be standard layout for SHM");
// RecordMirrorPage page layout, kVersion == 4 (docs/reference/abi-compatibility.md).
// The page is mapped at the manifest's slot_bytes, and an attacher validates
// magic/version — a layout change here requires a version bump.
static_assert(sizeof(RecordMirrorPage::Header) == 64,
              "mirror page header is one cache line");
static_assert(offsetof(RecordMirrorPage::Header, version) == 8,
              "version offset is page ABI");
static_assert(offsetof(RecordMirrorPage::Header, session_id) == 12,
              "session_id offset is page ABI");
static_assert(offsetof(RecordMirrorPage::Header, slot_count) == 16,
              "slot_count offset is page ABI");
static_assert(offsetof(RecordMirrorPage::Header, write_generation) == 24,
              "generation/counter order is page ABI");
static_assert(offsetof(RecordMirrorPage::Header, oversize_drop_cnt) == 48,
              "generation/counter order is page ABI");
static_assert(offsetof(RecordMirrorPage, super) == 0,
              "super-header leads the page");
static_assert(offsetof(RecordMirrorPage, header) ==
                  sizeof(ShmSuperHeader),
              "body header follows the super-header");
static_assert(offsetof(RecordMirrorPage, struct_table) ==
                  sizeof(ShmSuperHeader) + sizeof(RecordMirrorPage::Header),
              "struct table starts right after the body header");
static_assert(offsetof(RecordMirrorPage, slot_headers) ==
                  sizeof(ShmSuperHeader) + sizeof(RecordMirrorPage::Header) +
                      kRecordMirrorMaxSlots * sizeof(ThymeStructDesc),
              "slot metadata follows the struct table");
static_assert(offsetof(RecordMirrorPage, payload_arena) ==
                  sizeof(ShmSuperHeader) + sizeof(RecordMirrorPage::Header) +
                      kRecordMirrorMaxSlots *
                          (sizeof(ThymeStructDesc) +
                           sizeof(RecordMirrorSlotHeader)),
              "payload arena follows the fixed tables");
static_assert(sizeof(RecordMirrorPage) ==
                  offsetof(RecordMirrorPage, payload_arena) + 2097152,
              "page size is part of the manifest slot_bytes ABI");
static_assert(std::atomic<uint32_t>::is_always_lock_free &&
                  std::atomic<uint64_t>::is_always_lock_free,
              "RecordMirrorPage lives in SHM: its atomics must be lock-free "
              "so cross-process seqlock readers/writers make progress");

}  // namespace thyme
