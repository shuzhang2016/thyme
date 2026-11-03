#pragma once

#include "thyme/contracts/abi.h"
#include "thyme/contracts/struct_desc.h"
#include "thyme/runtime/pinned_slot_gate.h"
#include "thyme/runtime/detail/atomic_payload.h"
#include "thyme/runtime/log.h"
#include "thyme/runtime/record_mirror.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace thyme {

// RecordMirrorRingPage extends the record mirror from "latest value per
// struct" (RecordMirrorPage, magic "THYMEMIR" v3) to a per-struct time-window
// sample ring (triggered acquisition, design doc 4.2). It is a separate page
// type with its own magic/version so an attacher can never confuse the two
// ABIs; RecordMirrorPage stays untouched for existing consumers.
//
// Layout (single shm region, variable size — NEVER stack-allocate; size it
// with RequiredBytes() and map through record_mirror::RingClient or
// ShmRegion):
//
//   RecordMirrorRingPage
//   ├── Header: magic/version/session/struct_count/config_hash/arena_bytes/
//   │           write_generation/mirror_odd_skips/oversize_drop_cnt
//   ├── ThymeStructDesc struct_table[64]        (ABI-11 keyed, contract-generated)
//   ├── RecordMirrorRingDesc ring_descs[64]     (geometry + ProducerGate)
//   └── arena[arena_bytes]                       (not a member; ArenaBase())
//        per struct: ring of ring_depth slots, each
//        [RecordMirrorRingSlotHeader][payload_capacity bytes]
//
// The arena size is computed from the per-struct ring spec at Create time
// (slot count N is a runtime parameter per struct), replacing the fixed 2 MB
// arena of RecordMirrorPage. The header carries config_hash so a recorder
// attaching to the page can verify it was built from the same configuration.

inline constexpr uint32_t kRecordMirrorRingMaxStructs = kAbiMaxStructCount;

// RT-09: bounded seqlock retry budget of PeekRingSample/ReadRingSample (one
// greppable constant for the retry-loop audit; exhaustion counts into the
// caller-owned *torn_drops and returns false).
inline constexpr int kMirrorSampleReadRetries = 16;

// Per-struct ring configuration supplied at Create time. ring_depth = number
// of retained samples (derived from the trigger window x channel rate);
// ring_depth == 1 degenerates to the old latest-value semantics.
struct RecordMirrorRingSpec {
  uint32_t payload_size = 0;  // contract payload bytes per sample
  uint32_t ring_depth = 1;    // retained samples per struct
};

// Per-slot metadata of one ring sample. seq is a CAS seqlock in the
// record_mirror.h discipline: 0 = never written, odd = write in progress,
// even >= 2 = published. publish_index is the absolute sample index (monotonic
// per struct, from 0); the slot holding index p sits at p % ring_depth.
// source_sequence is producer-local wire-format provenance: it wraps at 2^32
// and is neither a globally unique sample key nor an ordering key.
struct alignas(64) RecordMirrorRingSlotHeader {
  std::atomic<uint32_t> seq{0};
  std::atomic<uint32_t> payload_size{0};
  std::atomic<uint64_t> mono_ns{0};
  std::atomic<uint64_t> publish_index{0};
  std::atomic<uint32_t> source_sequence{0};  // wraps; not an ordering key
  std::atomic<uint32_t> reserved{0};
};

static_assert(sizeof(RecordMirrorRingSlotHeader) == 64,
              "ring slot header must fill exactly one cache line");
// RecordMirrorRingPage v4 slot metadata: slot_stride is
// sizeof(header) + RoundUp64(payload_size) and the payload follows the header
// inside each slot (SlotPayloadOffset), so these offsets are ABI.
static_assert(offsetof(RecordMirrorRingSlotHeader, payload_size) == 4,
              "ring slot field order is the v4 wire layout");
static_assert(offsetof(RecordMirrorRingSlotHeader, mono_ns) == 8,
              "ring slot field order is the v4 wire layout");
static_assert(offsetof(RecordMirrorRingSlotHeader, publish_index) == 16,
              "publish_index is read by drain re-validation");
static_assert(offsetof(RecordMirrorRingSlotHeader, source_sequence) == 24,
              "source_sequence is read by drain re-validation");
static_assert(offsetof(RecordMirrorRingSlotHeader, reserved) == 28,
              "slot header tail is padding to the 64B cache line");
static_assert(std::is_standard_layout<RecordMirrorRingSlotHeader>::value &&
                  std::is_trivially_copyable<RecordMirrorRingSlotHeader>::value,
              "RecordMirrorRingSlotHeader must be POD for SHM");

// Per-struct ring geometry and replay gate (written by Init before the magic
// release-store). The gate occupies its own cache line so independent structs
// can be replayed without blocking each other's real producers.
struct alignas(64) RecordMirrorRingDesc {
  std::atomic<uint64_t> ring_head{0};  // total samples published (monotonic)
  uint64_t slots_offset = 0;           // from page base to first slot header
  uint32_t ring_depth = 0;
  uint32_t slot_stride = 0;        // sizeof(slot header) + payload_capacity
  uint32_t payload_capacity = 0;   // round_up(payload_size, 64)
  uint32_t reserved0 = 0;
  uint64_t reserved1 = 0;
  alignas(64) ProducerGate gate;
};

static_assert(sizeof(RecordMirrorRingDesc) == 128,
              "ring descriptor must fill exactly two cache lines");
static_assert(offsetof(RecordMirrorRingDesc, slots_offset) == 8,
              "ring geometry offsets are the v6 page ABI");
static_assert(offsetof(RecordMirrorRingDesc, ring_depth) == 16,
              "ring geometry offsets are the v6 page ABI");
static_assert(offsetof(RecordMirrorRingDesc, slot_stride) == 20,
              "ring geometry offsets are the v6 page ABI");
static_assert(offsetof(RecordMirrorRingDesc, payload_capacity) == 24,
              "ring geometry offsets are the v6 page ABI");
static_assert(offsetof(RecordMirrorRingDesc, gate) == 64,
              "per-struct producer gate owns the second descriptor line");
static_assert(std::is_standard_layout<RecordMirrorRingDesc>::value &&
                  std::is_trivially_copyable<RecordMirrorRingDesc>::value,
              "RecordMirrorRingDesc must be POD for SHM");

// Descriptor handed to the recorder by HarvestWindow: the recorder later
// re-validates the sample with ReadRingSample(publish_index, ..., stable_seq)
// at drain time (zero-copy swap protocol, design doc 4.4).
struct RecordMirrorRingSampleDesc {
  uint64_t publish_index = 0;
  uint64_t mono_ns = 0;
  uint32_t payload_size = 0;
  uint32_t slot_seq = 0;  // slot seqlock value observed at harvest
};

struct alignas(64) RecordMirrorRingPage {
  static constexpr uint64_t kMagic = 0x5448594D45525247ULL;  // "THYMERRG"
  // Version 6 (REP-10): every ring descriptor embeds a 64 B ProducerGate.
  // page_bytes covers the fixed tables only (the arena is appended).
  // Version 5 added the ARCH-11 super-header; v1..v5 pages are rejected.
  static constexpr uint32_t kVersion = 6;

  // RT-10: two cache lines by design. Fields end at offset 72; the 56-byte
  // tail is alignment headroom (the probe/attacher indexes struct_table from
  // offsetof(Header), so the size is page ABI). Reusing that hole for a new
  // counter keeps sizeof/offsets stable but is still a page change — update
  // docs/reference/cacheline-audit.md and the layout asserts with it.
  struct alignas(64) Header {
    std::atomic<uint64_t> magic{0};
    std::atomic<uint32_t> version{0};
    std::atomic<uint32_t> session_id{0};
    std::atomic<uint32_t> struct_count{0};
    std::atomic<uint32_t> reserved{0};
    std::atomic<uint64_t> config_hash{0};
    std::atomic<uint64_t> arena_bytes{0};  // arena sized at Create from specs
    std::atomic<uint64_t> write_generation{0};
    std::atomic<uint64_t> reserved2{0};
    // Write-side CAS contention skips (same semantics as RecordMirrorPage).
    std::atomic<uint64_t> mirror_odd_skips{0};
    // Subset of the TryWriteRing failure count: payloads exceeding the slot
    // capacity.
    std::atomic<uint64_t> oversize_drop_cnt{0};
  };

  ShmSuperHeader super;
  Header header;
  alignas(64) ThymeStructDesc struct_table[kRecordMirrorRingMaxStructs];
  alignas(64) RecordMirrorRingDesc ring_descs[kRecordMirrorRingMaxStructs];
  // The payload arena follows this struct in the same mapping; it is NOT a
  // member because its size is a Create-time parameter. See ArenaBase().

  static constexpr uint32_t RoundUpCapacity(uint32_t size) {
    return (size + 63u) & ~63u;
  }

  static constexpr uint64_t LayoutHash() {
    return ShmLayoutHash(
        kMagic, kVersion,
        static_cast<uint64_t>(sizeof(RecordMirrorRingPage)),
        static_cast<uint64_t>(alignof(RecordMirrorRingPage)),
        static_cast<uint64_t>(offsetof(RecordMirrorRingPage, super)),
        static_cast<uint64_t>(offsetof(RecordMirrorRingPage, header)),
        static_cast<uint64_t>(offsetof(RecordMirrorRingPage, struct_table)),
        static_cast<uint64_t>(offsetof(RecordMirrorRingPage, ring_descs)),
        static_cast<uint64_t>(sizeof(RecordMirrorRingDesc)),
        static_cast<uint64_t>(offsetof(RecordMirrorRingDesc, gate)),
        static_cast<uint64_t>(sizeof(ProducerGate)),
        kRecordMirrorRingMaxStructs);
  }

  static constexpr ShmPageSpec Spec() {
    return ShmPageSpec{kMagic, kVersion, LayoutHash(),
                       static_cast<uint64_t>(sizeof(RecordMirrorRingPage)), 0};
  }

  uint8_t* ArenaBase() { return reinterpret_cast<uint8_t*>(this + 1); }
  const uint8_t* ArenaBase() const {
    return reinterpret_cast<const uint8_t*>(this + 1);
  }

  // FNV-1a over the shape-defining config fields so an attacher can verify
  // both sides built the page from the same ring configuration.
  static uint64_t HashConfig(const ThymeStructDesc* descs,
                             const RecordMirrorRingSpec* specs,
                             uint32_t count) {
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&h](uint64_t v) {
      for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= 1099511628211ULL;
      }
    };
    mix(count);
    if (descs != nullptr && specs != nullptr) {
      for (uint32_t i = 0; i < count; ++i) {
        mix(descs[i].payload_size);
        mix(descs[i].contract_version);
        // ABI-11: identity is part of the page configuration, so a ring built
        // for a different channel/layout is rejected by the config hash as
        // well as by its version.
        mix(descs[i].layout_hash);
        const char* key = ThymeStructDescKey(descs[i]);
        for (const char* p = key; *p != '\0'; ++p) {
          mix(static_cast<unsigned char>(*p));
        }
        mix(0xFFu);
        mix(specs[i].payload_size);
        mix(specs[i].ring_depth);
      }
    }
    return h;
  }

  // Aggregate arena bytes for the given specs, or 0 on invalid input
  // (zero payload, zero depth, payload over kRecordMirrorMaxPayload, or
  // arithmetic overflow). Overflow-safe: all math in uint64.
  static uint64_t ComputeArenaBytes(const ThymeStructDesc* descs,
                                    const RecordMirrorRingSpec* specs,
                                    uint32_t count) {
    if (descs == nullptr || specs == nullptr || count == 0 ||
        count > kRecordMirrorRingMaxStructs) {
      return 0;
    }
    uint64_t arena = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t psz = specs[i].payload_size;
      const uint32_t depth = specs[i].ring_depth;
      if (psz == 0 || psz > kRecordMirrorMaxPayload || depth == 0) return 0;
      const uint64_t stride =
          sizeof(RecordMirrorRingSlotHeader) + RoundUpCapacity(psz);
      const uint64_t ring_bytes = stride * depth;
      if (ring_bytes / depth != stride) return 0;  // overflow
      if (arena + ring_bytes < arena) return 0;    // overflow
      arena += ring_bytes;
    }
    return arena;
  }

  // Total shm region bytes for the given specs (0 on invalid input).
  static uint64_t RequiredBytes(const ThymeStructDesc* descs,
                                const RecordMirrorRingSpec* specs,
                                uint32_t count) {
    const uint64_t arena = ComputeArenaBytes(descs, specs, count);
    if (arena == 0) return 0;
    return sizeof(RecordMirrorRingPage) + arena;
  }

  bool Init(uint32_t session_id, const ThymeStructDesc* descs,
            const RecordMirrorRingSpec* specs, uint32_t count) {
    // Invalidate first: a reader that already passed Validate() must not
    // observe a half-rewritten ring table while magic still holds kMagic.
    super.magic.store(0, std::memory_order_release);
    const uint64_t arena = ComputeArenaBytes(descs, specs, count);
    if (arena == 0) return false;
    for (uint32_t i = 0; i < kRecordMirrorRingMaxStructs; ++i) {
      ring_descs[i].ring_head.store(0, std::memory_order_relaxed);
      ring_descs[i].slots_offset = 0;
      ring_descs[i].ring_depth = 0;
      ring_descs[i].slot_stride = 0;
      ring_descs[i].payload_capacity = 0;
      ResetGate(ring_descs[i].gate);
      struct_table[i] = {};
    }
    uint64_t arena_cursor = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t cap = RoundUpCapacity(specs[i].payload_size);
      struct_table[i] = descs[i];
      struct_table[i].struct_id = static_cast<uint16_t>(i);
      ring_descs[i].slots_offset = arena_cursor;
      ring_descs[i].ring_depth = specs[i].ring_depth;
      ring_descs[i].slot_stride =
          static_cast<uint32_t>(sizeof(RecordMirrorRingSlotHeader)) + cap;
      ring_descs[i].payload_capacity = cap;
      arena_cursor += ring_descs[i].slot_stride *
                      static_cast<uint64_t>(specs[i].ring_depth);
    }
    // Reset every slot seqlock so a re-Init on reused memory cannot leave a
    // stale published state visible under the new config.
    for (uint32_t i = 0; i < count; ++i) {
      for (uint32_t s = 0; s < specs[i].ring_depth; ++s) {
        RecordMirrorRingSlotHeader* sh =
            SlotHeaderPtr(static_cast<uint16_t>(i), s);
        sh->seq.store(0, std::memory_order_relaxed);
        sh->payload_size.store(0, std::memory_order_relaxed);
        sh->mono_ns.store(0, std::memory_order_relaxed);
        sh->publish_index.store(0, std::memory_order_relaxed);
        sh->source_sequence.store(0, std::memory_order_relaxed);
      }
    }
    header.write_generation.store(0, std::memory_order_relaxed);
    header.reserved2.store(0, std::memory_order_relaxed);
    header.mirror_odd_skips.store(0, std::memory_order_relaxed);
    header.oversize_drop_cnt.store(0, std::memory_order_relaxed);
    header.arena_bytes.store(arena, std::memory_order_relaxed);
    header.config_hash.store(HashConfig(descs, specs, count),
                             std::memory_order_relaxed);
    header.struct_count.store(count, std::memory_order_relaxed);
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
    const uint32_t count = header.struct_count.load(std::memory_order_relaxed);
    if (count > kRecordMirrorRingMaxStructs) {
      if (error != nullptr) {
        *error = ShmPageError{ShmPageErrc::kBadGeometry,
                              kRecordMirrorRingMaxStructs, count};
      }
      return false;
    }
    return true;
  }

  // Config consistency check for attachers (expected_hash == 0 skips it).
  bool ValidateConfig(uint64_t expected_hash) const {
    return Validate() &&
           (expected_hash == 0 ||
            header.config_hash.load(std::memory_order_acquire) ==
                expected_hash);
  }

  uint32_t struct_count() const {
    return header.struct_count.load(std::memory_order_acquire);
  }
  uint64_t config_hash() const {
    return header.config_hash.load(std::memory_order_acquire);
  }
  uint64_t arena_bytes() const {
    return header.arena_bytes.load(std::memory_order_acquire);
  }
  uint64_t mirror_odd_skip_count() const {
    return header.mirror_odd_skips.load(std::memory_order_acquire);
  }
  uint64_t oversize_drop_count() const {
    return header.oversize_drop_cnt.load(std::memory_order_acquire);
  }
  uint64_t write_generation() const {
    return header.write_generation.load(std::memory_order_acquire);
  }

  ProducerGate* producer_gate(uint16_t struct_id) {
    if (!Validate() || struct_id >= struct_count()) return nullptr;
    return &ring_descs[struct_id].gate;
  }
  const ProducerGate* producer_gate(uint16_t struct_id) const {
    if (!Validate() || struct_id >= struct_count()) return nullptr;
    return &ring_descs[struct_id].gate;
  }

  // Absolute number of samples ever published for struct_id (the next
  // publish_index). Valid retained window is
  // [head > depth ? head - depth : 0, head).
  uint64_t RingHead(uint16_t struct_id) const {
    if (!Validate() || struct_id >= struct_count()) return 0;
    return ring_descs[struct_id].ring_head.load(std::memory_order_acquire);
  }

  uint32_t RingDepth(uint16_t struct_id) const {
    if (!Validate() || struct_id >= struct_count()) return 0;
    return ring_descs[struct_id].ring_depth;
  }

  // Non-blocking single-writer tap: appends one sample to the struct's ring,
  // evicting the oldest sample when the ring is full. CAS seqlock discipline
  // matches RecordMirrorPage::TryWriteSlot (odd skip counted, never blocks).
  // Payloads over the slot capacity count an oversize drop and return false.
  // writer_token == 0 is a real producer; a replay passes its owned gate token.
  bool TryWriteRing(uint16_t struct_id, const void* data, uint32_t size,
                    uint64_t mono_ns, uint32_t source_sequence,
                    uint64_t writer_token = kProducerGateUnheld) {
    if (!Validate() || struct_id >= struct_count()) return false;
    RecordMirrorRingDesc& desc = ring_descs[struct_id];
    if (desc.gate.owner_token.load(std::memory_order_relaxed) !=
            kProducerGateUnheld &&
        !ProducerGateAllowsWrite(desc.gate, writer_token,
                                 ProducerGateNowNs())) {
      return false;
    }
    if (data == nullptr || size == 0) return false;
    if (size > desc.payload_capacity) {
      RecordOversizeDrop(struct_id, size);
      return false;
    }
    const uint64_t pos = desc.ring_head.load(std::memory_order_relaxed);
    RecordMirrorRingSlotHeader& slot =
        *SlotHeaderPtr(struct_id, static_cast<uint32_t>(pos % desc.ring_depth));
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
    // this fence (same store-store ordering argument as
    // RecordMirrorPage::Write's fence). On x86/TSO this is a no-op; the
    // comment is the contract.
    std::atomic_thread_fence(std::memory_order_release);
    slot.mono_ns.store(mono_ns, std::memory_order_relaxed);
    slot.publish_index.store(pos, std::memory_order_relaxed);
    slot.source_sequence.store(source_sequence, std::memory_order_relaxed);
    slot.payload_size.store(size, std::memory_order_relaxed);
    detail::AtomicPayloadCopyIn(SlotPayloadPtr(struct_id, pos), data, size);
    if (desc.gate.owner_token.load(std::memory_order_relaxed) !=
            kProducerGateUnheld &&
        !ProducerGateAllowsWrite(desc.gate, writer_token,
                                 ProducerGateNowNs())) {
      // A gate appeared mid-write: close the seqlock but do not publish or
      // advance ring_head. The next write reuses and overwrites this slot.
      slot.seq.fetch_add(1, std::memory_order_release);
      return false;
    }
    slot.seq.fetch_add(1, std::memory_order_release);
    desc.ring_head.store(pos + 1, std::memory_order_release);
    header.write_generation.fetch_add(1, std::memory_order_release);
    return true;
  }

  // Header-only read of the sample with absolute publish_index (no payload
  // copy) — the recorder's post-window continuous-harvest primitive (design
  // doc 4.4 step 2): cheap enough to poll per tick on every struct. Returns
  // false when the sample is evicted (overrun hole) or never written; torn
  // reads retry within a budget then count into *torn_drops (caller-owned,
  // read-only-mapping safe) and return false.
  bool PeekRingSample(uint16_t struct_id, uint64_t publish_index,
                      RecordMirrorRingSampleDesc& out,
                      uint64_t* torn_drops = nullptr) const {
    if (!Validate() || struct_id >= struct_count()) return false;
    const RecordMirrorRingDesc& desc = ring_descs[struct_id];
    const uint32_t slot_idx =
        static_cast<uint32_t>(publish_index % desc.ring_depth);
    const RecordMirrorRingSlotHeader& slot =
        *SlotHeaderPtr(struct_id, slot_idx);
    for (int i = 0; i < kMirrorSampleReadRetries; ++i) {
      const uint32_t s0 = slot.seq.load(std::memory_order_acquire);
      if (s0 == 0) return false;  // never written
      if (s0 & 1u) continue;      // writer in progress
      const uint64_t p = slot.publish_index.load(std::memory_order_acquire);
      if (p != publish_index) return false;  // evicted (overrun hole)
      const uint64_t m = slot.mono_ns.load(std::memory_order_acquire);
      const uint32_t psz = slot.payload_size.load(std::memory_order_acquire);
      const uint32_t s1 = slot.seq.load(std::memory_order_acquire);
      if (s1 == s0) {
        out.publish_index = p;
        out.mono_ns = m;
        out.payload_size = psz;
        out.slot_seq = s0;
        return true;
      }
    }
    if (torn_drops != nullptr) *torn_drops += 1;
    return false;
  }

  // Read the sample with absolute publish_index into out. Returns false when
  // the slot no longer holds that index (evicted — a normal overrun hole, NOT
  // counted) or when the read tears across the retry budget. Torn reads never
  // block; they are counted into the caller-owned *torn_drops when given
  // (process-local: readers usually map the page read-only, so a shm-side
  // counter would segfault — same convention as the channels' drop_count()).
  bool ReadRingSample(uint16_t struct_id, uint64_t publish_index, uint8_t* out,
                      uint32_t out_cap, uint32_t& size, uint64_t& mono_ns,
                      uint32_t& source_sequence, uint32_t* stable_seq = nullptr,
                      uint64_t* torn_drops = nullptr) const {
    if (!Validate() || struct_id >= struct_count()) return false;
    const RecordMirrorRingDesc& desc = ring_descs[struct_id];
    const uint64_t arena = header.arena_bytes.load(std::memory_order_acquire);
    const uint32_t slot_idx =
        static_cast<uint32_t>(publish_index % desc.ring_depth);
    const RecordMirrorRingSlotHeader& slot =
        *SlotHeaderPtr(struct_id, slot_idx);
    for (int i = 0; i < kMirrorSampleReadRetries; ++i) {
      const uint32_t s0 = slot.seq.load(std::memory_order_acquire);
      if (s0 == 0) return false;  // never written
      if (s0 & 1u) continue;      // writer in progress
      const uint64_t p = slot.publish_index.load(std::memory_order_acquire);
      if (p != publish_index) return false;  // evicted (overrun hole)
      const uint32_t psz = slot.payload_size.load(std::memory_order_acquire);
      const uint64_t m = slot.mono_ns.load(std::memory_order_acquire);
      const uint32_t ss = slot.source_sequence.load(std::memory_order_acquire);
      if (psz == 0 || psz > desc.payload_capacity || psz > out_cap) {
        return false;
      }
      // Indirect addressing: reject any payload whose arena range escapes the
      // mapped region, so a corrupted ring table cannot make a reader memcpy
      // past the SHM mapping.
      const uint64_t off = SlotPayloadOffset(desc, slot_idx);
      if (off > arena || psz > arena - off) return false;
      detail::AtomicPayloadCopyOut(out, ArenaBase() + off, psz);
      const uint32_t s1 = slot.seq.load(std::memory_order_acquire);
      if (s1 == s0) {
        size = psz;
        mono_ns = m;
        source_sequence = ss;
        if (stable_seq != nullptr) *stable_seq = s0;
        return true;
      }
    }
    if (torn_drops != nullptr) *torn_drops += 1;
    return false;
  }

  // Harvest descriptors for samples with mono_ns in [t_begin_ns, t_end_ns]
  // from the currently retained window of struct_id. Header-only reads (no
  // payload copy); each descriptor's slot_seq lets the drain-side re-validate
  // Harvest descriptors for samples with mono_ns in [t_begin_ns, t_end_ns]
  // from the currently retained window of struct_id. Header-only reads (no
  // payload copy); each descriptor's slot_seq lets the drain-side re-validate
  // via ReadRingSample before serializing. Torn/odd slots are skipped and, if
  // torn_drops is given, counted into the caller-owned counter (process-local:
  // safe for read-only-mapped readers). Returns the number of descriptors.
  uint32_t HarvestWindow(uint16_t struct_id, uint64_t t_begin_ns,
                         uint64_t t_end_ns, RecordMirrorRingSampleDesc* out,
                         uint32_t out_cap, uint64_t* torn_drops = nullptr) const {
    if (!Validate() || struct_id >= struct_count()) return 0;
    if (out == nullptr || out_cap == 0) return 0;
    const RecordMirrorRingDesc& desc = ring_descs[struct_id];
    const uint64_t head = desc.ring_head.load(std::memory_order_acquire);
    if (head == 0) return 0;
    const uint64_t oldest =
        head > desc.ring_depth ? head - desc.ring_depth : 0;
    uint32_t n = 0;
    for (uint64_t p = oldest; p < head && n < out_cap; ++p) {
      const uint32_t slot_idx =
          static_cast<uint32_t>(p % desc.ring_depth);
      const RecordMirrorRingSlotHeader& slot =
          *SlotHeaderPtr(struct_id, slot_idx);
      const uint32_t s0 = slot.seq.load(std::memory_order_acquire);
      if (s0 == 0 || (s0 & 1u) != 0) {
        if (torn_drops != nullptr) *torn_drops += 1;
        continue;
      }
      const uint64_t pi = slot.publish_index.load(std::memory_order_acquire);
      const uint64_t m = slot.mono_ns.load(std::memory_order_acquire);
      const uint32_t psz = slot.payload_size.load(std::memory_order_acquire);
      const uint32_t s1 = slot.seq.load(std::memory_order_acquire);
      if (s1 != s0) {
        if (torn_drops != nullptr) *torn_drops += 1;
        continue;
      }
      if (pi != p) continue;  // raced an eviction; head moved on, harmless
      if (m < t_begin_ns || m > t_end_ns) continue;
      out[n].publish_index = pi;
      out[n].mono_ns = m;
      out[n].payload_size = psz;
      out[n].slot_seq = s0;
      ++n;
    }
    return n;
  }

 private:
  static void ResetGate(ProducerGate& gate) {
    gate.owner_token.store(kProducerGateUnheld, std::memory_order_relaxed);
    gate.owner_pid.store(0, std::memory_order_relaxed);
    gate.owner_start.store(0, std::memory_order_relaxed);
    gate.heartbeat_ns.store(0, std::memory_order_relaxed);
    gate.timeout_ns.store(0, std::memory_order_relaxed);
    gate.generation.store(0, std::memory_order_relaxed);
    gate.rejects.store(0, std::memory_order_relaxed);
    gate.steals.store(0, std::memory_order_relaxed);
  }

  RecordMirrorRingSlotHeader* SlotHeaderPtr(uint16_t struct_id,
                                            uint32_t slot_idx) {
    return reinterpret_cast<RecordMirrorRingSlotHeader*>(
        ArenaBase() + ring_descs[struct_id].slots_offset +
        static_cast<uint64_t>(slot_idx) * ring_descs[struct_id].slot_stride);
  }
  const RecordMirrorRingSlotHeader* SlotHeaderPtr(uint16_t struct_id,
                                                  uint32_t slot_idx) const {
    return reinterpret_cast<const RecordMirrorRingSlotHeader*>(
        ArenaBase() + ring_descs[struct_id].slots_offset +
        static_cast<uint64_t>(slot_idx) * ring_descs[struct_id].slot_stride);
  }

  static uint64_t SlotPayloadOffset(const RecordMirrorRingDesc& desc,
                                    uint32_t slot_idx) {
    return desc.slots_offset +
           static_cast<uint64_t>(slot_idx) * desc.slot_stride +
           sizeof(RecordMirrorRingSlotHeader);
  }

  uint8_t* SlotPayloadPtr(uint16_t struct_id, uint64_t publish_index) {
    const RecordMirrorRingDesc& desc = ring_descs[struct_id];
    return ArenaBase() +
           SlotPayloadOffset(
               desc, static_cast<uint32_t>(publish_index % desc.ring_depth));
  }

  void RecordOversizeDrop(uint16_t struct_id, uint32_t size) {
    header.oversize_drop_cnt.fetch_add(1, std::memory_order_relaxed);
    LogOversizeOnce(struct_id, size);
  }

  // OBS-08: same rate-limited error path as RecordMirrorPage.
  void LogOversizeOnce(uint16_t struct_id, uint32_t size) const {
    THYME_LOG_WARN_RATED(
        ::thyme::log::kOnceThenEvery1000,
        "record_mirror_ring: oversized payload dropped struct_id=%u size=%u "
        "capacity=%u",
        static_cast<unsigned>(struct_id), static_cast<unsigned>(size),
        static_cast<unsigned>(
            ring_descs[struct_id].payload_capacity));
  }
};

static_assert(std::is_standard_layout<RecordMirrorRingPage>::value,
              "RecordMirrorRingPage must be standard layout for SHM");
// RecordMirrorRingPage page layout, kVersion == 6
// (docs/reference/abi-compatibility.md; RecordMirrorPage is v4). ArenaBase() is
// `this + 1`, so sizeof defines where
// the arena starts and RequiredBytes() adds the Create-time arena — a layout
// change requires a version bump, and attachers check config_hash too.
static_assert(sizeof(RecordMirrorRingPage::Header) == 128,
              "ring page header is two cache lines (alignas pads 72 -> 128)");
static_assert(offsetof(RecordMirrorRingPage::Header, version) == 8,
              "version offset is page ABI");
static_assert(offsetof(RecordMirrorRingPage::Header, struct_count) == 16,
              "struct_count offset is page ABI");
static_assert(offsetof(RecordMirrorRingPage::Header, config_hash) == 24,
              "config_hash offset is page ABI");
static_assert(offsetof(RecordMirrorRingPage::Header, arena_bytes) == 32,
              "arena_bytes offset is page ABI");
static_assert(offsetof(RecordMirrorRingPage::Header, write_generation) == 40,
              "generation/counter order is page ABI");
static_assert(offsetof(RecordMirrorRingPage::Header, oversize_drop_cnt) == 64,
              "generation/counter order is page ABI");
static_assert(offsetof(RecordMirrorRingPage, super) == 0,
              "super-header leads the page");
static_assert(offsetof(RecordMirrorRingPage, header) ==
                  sizeof(ShmSuperHeader),
              "body header follows the super-header");
static_assert(offsetof(RecordMirrorRingPage, struct_table) ==
                  sizeof(ShmSuperHeader) + sizeof(RecordMirrorRingPage::Header),
              "struct table starts right after the body header");
static_assert(offsetof(RecordMirrorRingPage, ring_descs) ==
                  sizeof(ShmSuperHeader) + sizeof(RecordMirrorRingPage::Header) +
                      kRecordMirrorRingMaxStructs * sizeof(ThymeStructDesc),
              "ring descriptors follow the struct table");
static_assert(sizeof(RecordMirrorRingPage) ==
                  offsetof(RecordMirrorRingPage, ring_descs) +
                      kRecordMirrorRingMaxStructs *
                          sizeof(RecordMirrorRingDesc),
              "ArenaBase() (this + 1) must start right after the tables");
static_assert(std::atomic<uint32_t>::is_always_lock_free &&
                  std::atomic<uint64_t>::is_always_lock_free,
              "RecordMirrorRingPage lives in SHM: its atomics must be "
              "lock-free so cross-process seqlock readers/writers make "
              "progress");

}  // namespace thyme
