#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <type_traits>

#include "thyme/runtime/detail/atomic_payload.h"
#ifndef NDEBUG
#include "thyme/runtime/detail/single_writer_guard.h"
#endif
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_page.h"
#include "thyme/runtime/write_section.h"

namespace thyme {
template <typename T>
struct LatestSlot;
template <typename T>
struct LatestSlotPage;
template <typename T>
struct LatestSlotLayoutModel;
namespace detail {
template <typename T>
struct LatestSlotLayout;
template <typename T>
struct LatestSlotTestAccess;
}  // namespace detail

// Page ABI version of the described LatestSlot<T> SHM page. v3 prefixes the
// unchanged v2 seqlock body (RT-03: u64 seq_) with the 64 B ShmSuperHeader
// from shm_page.h plus the fixed 64 B LatestSlotPageGeometry block, so the page
// itself carries a magic/version/layout identity AND the machine-readable cell
// geometry a reader needs to find the payload cells and the seqlock. A bare v2
// region (the old LatestSlot<T>-only page) has no header and cannot validate as
// a page. The value is stored in the page header, is reported by the OPS-06 ABI
// matrix (thyme_probe abi --matrix), and is locked by LatestSlotLayoutModel<T>
// below. v3 was never deployed: ARCH-11 folded the geometry block into it rather
// than publishing a v4; a v3 page without the geometry block and every v2 body
// page are rejected.
inline constexpr uint32_t kLatestSlotLayoutVersion = 3;
inline constexpr uint32_t kLatestSlotPageAbiVersion = kLatestSlotLayoutVersion;

// LatestSlot page magic: "THYLSLOT" in little-endian byte order. Distinct from
// every other SHM page magic so a mis-typed name or manifest entry fails
// validation instead of being decoded with the wrong geometry.
inline constexpr uint64_t kLatestSlotPageMagic = 0x544F4C534C594854ULL;

// RT-09: bounded seqlock retry budget of Read/ReadIfNew. One greppable
// constant for the retry-loop audit (docs/reference/cacheline-audit.md); exhaustion
// returns false and never hands out a torn value.
inline constexpr int kLatestSlotReadRetries = 16;

// RT-09: reader-side retry accounting for LatestSlot reads. Caller-owned and
// process-local on purpose: consumers usually map the page read-only, so the
// counter cannot live in the page (same convention as
// PinnedSlotReader::torn_read_count()). Pass nullptr (the default) for zero
// cost — both fields are touched only on the retry/torn failure path.
struct LatestSlotReadStats {
  uint64_t retries = 0;  // loop re-reads (writer in progress or torn copy)
  uint64_t torn = 0;     // copies invalidated by a publish mid-read
};

// Seqlock-backed latest-value slot with double-buffering.
// SINGLE WRITER: Write()/BeginWrite() are not safe for concurrent writers
// (the seq_ bump is not a CAS); use from one producer thread only. Readers
// may be concurrent.
// T must be trivially copyable: Read/Write copy raw bytes across processes.
// Writer bumps seq_ to odd (write in progress), writes the inactive slot,
// flips published_idx_, then bumps seq_ to even (publish complete).
// Reader retries while seq_ is odd and re-validates seq_ after the copy;
// on retry exhaustion it returns false. seq_==0 means "never written".
// RT-09: Read/ReadIfNew take an optional caller-owned LatestSlotReadStats* so
// a monitoring site can observe retries/tears without touching the hot path
// when it is absent (the page itself cannot carry reader counters: consumers
// map it read-only and share it across processes).
//
// seq_ is 64-bit (RT-03): the old u32 counter wrapped after 2^31 writes
// (hours-to-days at kHz publish rates), aliasing seq_==0 and earlier `seen`
// values. At u64 the odd/even state space is 2^63 publishes — unreachable in
// practice. At the theoretical 2^64 boundary the odd/even parity survives
// (UINT64_MAX is odd and wraps to even 0), but the single generation whose
// seq_==0 aliases "never written" (Read/ReadIfNew/HasNew return false once);
// the next publish recovers. ReadIfNew/HasNew compare full u64 states, so no
// wrap-unsafe ordering arithmetic exists anywhere in the protocol.
//
// RT-10 (cacheline layout, see docs/reference/cacheline-audit.md): the body is
// 64-byte aligned through Cell's alignas(64) (regions are page-aligned, and a
// v3 page header is exactly one cache line, so the body stays line-aligned);
// the two payload cells own one line each, and the control tail
// (published_idx_/reserved_/seq_) fits in a third line together with the
// process-local tap fields. The tap pointer sits on the seq_ line but is
// written only by SetRecordTap/ClearRecordTap, so it never bounces on publish.
/** \brief Single-writer latest-value slot (in-process object or the body of a
 *         described v3 SHM page).
 *  \note Read()/ReadIfNew() return a consistent snapshot; no borrow is handed
 *        out, so the next write may overwrite the payload immediately after
 *        the read returns. The slot owns its payload cells; a record tap, when
 *        installed, is borrowed and must outlive the slot. */
template <typename T>
struct LatestSlot {
  static_assert(std::is_trivially_copyable<T>::value,
                "LatestSlot<T> requires trivially copyable T (raw-byte copy "
                "across the seqlock, possibly over shared memory)");

  // Optional record tap (triggered acquisition, design doc 4.1): when armed,
  // every Write also appends the value to the process-local mirror ring.
  // nullptr page (default) = no recording, zero overhead. The tap pointer is
  // writer-process-local: when a LatestSlot lives in shm, readers in other
  // processes must never call Write and never dereference it. Tap failures
  // are counted in the page and never affect Write.
  void SetRecordTap(RecordMirrorRingPage* page, uint16_t struct_id) {
    tap_page_ = page;
    tap_struct_id_ = struct_id;
  }

  // Writer thread only, and not from inside BeginWrite/CommitWrite. After
  // this returns, further publishes do not copy into the mirror.
  void ClearRecordTap() {
    tap_page_ = nullptr;
    tap_struct_id_ = 0;
  }

  bool record_tap_armed() const { return tap_page_ != nullptr; }
  uint16_t record_tap_struct_id() const { return tap_struct_id_; }

  // Thin wrapper over the RT-15 in-place path so both paths share ONE publish
  // sequence. The copy from the caller buffer uses atomic bytes: a reader may
  // still be copying this slot (a second Write flips back onto it), and per
  // docs/design/adr/0001 the overlap is race-free only when both sides access the
  // payload atomically (RT-19).
  // Deprecated for new code (RT-16, docs/reference/coding-rules.md CR-02): call
  // BeginWrite/CommitWrite directly; this wrapper stays for existing callers
  // and replay/injection paths that already hold a materialized value.
  void Write(const T& v) {
    WriteSection<LatestSlot<T>> section(*this);
    detail::AtomicPayloadCopyIn(section.get(), &v, sizeof(T));
    section.commit();
  }

  // RT-15: in-place publish. Opens the write-side critical section (seq_
  // becomes odd) and returns the INACTIVE slot for the caller to fill with
  // PLAIN writes — the single writer owns that slot until CommitWrite, so no
  // atomic byte copy is needed for the in-place fill itself (ADR 0001
  // writer-side rule). Readers retry while seq_ is odd and never observe the
  // half-filled payload.
  //
  // Abort semantics: an abandoned BeginWrite (no CommitWrite) leaves seq_
  // odd, so Read/ReadIfNew keep failing instead of returning partial data.
  // AbortWrite() closes the section (seq_ back to even) WITHOUT flipping
  // published_idx_: readers keep the previously published value, though
  // ReadIfNew may report it once more because seq_ advanced. Single writer:
  // never interleave two BeginWrite/CommitWrite pairs.
  T* BeginWrite() {
#ifndef NDEBUG
    const uint32_t nxt = published_idx_.load(std::memory_order_relaxed) ^ 1u;
    T* const slot = &slot_[nxt].value;
    detail::SingleWriterGuard::Begin(
        "LatestSlot", this, slot,
        seq_.load(std::memory_order_relaxed) / 2u + 1u);
#endif
    // Enter write-side critical section: seq_ becomes odd.
    seq_.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
#ifndef NDEBUG
    return slot;
#else
    return &slot_[published_idx_.load(std::memory_order_relaxed) ^ 1u].value;
#endif
  }

  // Publishes the slot filled through BeginWrite. Ordering is exactly the
  // protocol Write always used: publish the freshly written slot
  // (published_idx_, release), then close the critical section (seq_ even,
  // release), then the record tap. Single writer: recomputing the inactive
  // index from published_idx_ yields the slot BeginWrite handed out.
  void CommitWrite() {
    const uint32_t nxt = published_idx_.load(std::memory_order_relaxed) ^ 1u;
#ifndef NDEBUG
    // Capture the publish sequence BEFORE closing the section: if seq_ is at
    // UINT64_MAX the fetch_add below wraps it to 0, and re-deriving the
    // expected sequence afterwards would report generation 1 instead of the
    // generation BeginWrite registered.
    const uint64_t publish_seq =
        seq_.load(std::memory_order_relaxed) / 2u + 1u;
    detail::SingleWriterGuard::Check(
        "CommitWrite", "LatestSlot", this, &slot_[nxt].value, publish_seq);
#endif
    published_idx_.store(nxt, std::memory_order_release);
    seq_.fetch_add(1, std::memory_order_release);
    if (tap_page_ != nullptr) {
      // source_sequence is a u32 provenance field in the versioned ring page
      // layout (RecordMirrorRingSlotHeader ABI) and stays u32: it wraps at
      // 2^32 publishes, which is acceptable for a diagnostic echo.
      tap_page_->TryWriteRing(tap_struct_id_, &slot_[nxt].value, sizeof(T),
                              MonoNs(),
                              static_cast<uint32_t>(
                                  seq_.load(std::memory_order_relaxed) / 2u));
    }
#ifndef NDEBUG
    detail::SingleWriterGuard::End(
        "CommitWrite", "LatestSlot", this, &slot_[nxt].value, publish_seq);
#endif
  }

  // Abort class 1/4 (write_section.h): seqlock rollback. Closes the odd seq_
  // without flipping published_idx_, so the previously published value stays
  // current. See BeginWrite for the ReadIfNew re-report wrinkle.
  void AbortWrite() {
#ifndef NDEBUG
    const uint32_t nxt = published_idx_.load(std::memory_order_relaxed) ^ 1u;
    const uint64_t publish_seq =
        seq_.load(std::memory_order_relaxed) / 2u + 1u;
    detail::SingleWriterGuard::Check("AbortWrite", "LatestSlot", this,
                                     &slot_[nxt].value, publish_seq);
#endif
    seq_.fetch_add(1, std::memory_order_release);
#ifndef NDEBUG
    detail::SingleWriterGuard::End("AbortWrite", "LatestSlot", this,
                                   &slot_[nxt].value, publish_seq);
#endif
  }

  // RT-09: on retry exhaustion Read returns false and (when stats is given)
  // records how many times it had to re-read. The counters are written only
  // on the failure path — a clean read touches them not at all.
  bool Read(T& out, LatestSlotReadStats* stats = nullptr) const {
    for (int i = 0; i < kMaxRetries; ++i) {
      const uint64_t s0 = seq_.load(std::memory_order_acquire);
      if (s0 == 0) return false;  // never written
      if (s0 & 1u) {              // writer in progress, retry
        if (stats != nullptr) stats->retries += 1;
        continue;
      }
      const uint32_t idx = published_idx_.load(std::memory_order_acquire);
      // The writer may start overwriting this slot (it writes the *inactive*
      // slot on its next cycle) while we copy it. A torn copy is caught by the
      // seq_ re-check below and retried. Both sides access the payload bytes
      // atomically (relaxed), so the overlap is race-free rather than a
      // documented benign race, and no TSan annotation is needed. Two
      // producers remain a contract violation (the seq_ bump is not a CAS),
      // but that is a logic bug, not something atomicity can detect.
      detail::AtomicPayloadCopyOut(&out, &slot_[idx].value, sizeof(T));
      const uint64_t s1 = seq_.load(std::memory_order_acquire);
      if (s1 == s0) return true;   // consistent copy
      if (stats != nullptr) {
        stats->retries += 1;
        stats->torn += 1;
      }
    }
    return false;
  }

  bool ReadIfNew(uint64_t& seen, T& out,
                 LatestSlotReadStats* stats = nullptr) const {
    for (int i = 0; i < kMaxRetries; ++i) {
      const uint64_t s0 = seq_.load(std::memory_order_acquire);
      if (s0 == 0 || s0 == seen) return false;
      if (s0 & 1u) {
        if (stats != nullptr) stats->retries += 1;
        continue;
      }
      const uint32_t idx = published_idx_.load(std::memory_order_acquire);
      // Atomic payload copy -- see Read() above.
      detail::AtomicPayloadCopyOut(&out, &slot_[idx].value, sizeof(T));
      const uint64_t s1 = seq_.load(std::memory_order_acquire);
      if (s1 == s0) {
        seen = s0;
        return true;
      }
      if (stats != nullptr) {
        stats->retries += 1;
        stats->torn += 1;
      }
    }
    return false;
  }

  // True if a frame newer than `seen` has been published. Does not sample the
  // slot, so callers must not touch seq_ directly (M4.1 — use this API only).
  bool HasNew(uint64_t seen) const {
    const uint64_t s = seq_.load(std::memory_order_acquire);
    return s != 0 && s != seen;
  }

 private:
  // RT-09: single greppable budget (shared with the audit of every 16-retry
  // loop in the runtime).
  static constexpr int kMaxRetries = kLatestSlotReadRetries;

  static uint64_t MonoNs() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
  }

  // Each slot owns a 64-byte-aligned cacheline cell: payloads are
  // natural-layout contract types, so the container provides cacheline
  // isolation (writer and reader never share a line across the two slots).
  struct alignas(64) Cell {
    T value;
  };
  static_assert(alignof(T) <= 64, "LatestSlot cells provide 64-byte alignment");
  // Cell stride is ABI: the probe derives published_idx_/seq_ from it and
  // regions are sized with round_up(sizeof(T), 64) per slot (RT-07).
  static_assert(sizeof(Cell) == ((sizeof(T) + 63u) / 64u) * 64u,
                "Cell stride must be RoundUp64(sizeof(T))");
  Cell slot_[2]{};
  std::atomic<uint32_t> published_idx_{0};
  uint32_t reserved_{0};  // pad: seq_ is 8-aligned (RT-03 layout v2)
  std::atomic<uint64_t> seq_{0};
  // Process-local record tap (see SetRecordTap); not part of the wire data.
  RecordMirrorRingPage* tap_page_{nullptr};
  uint16_t tap_struct_id_{0};

  template <typename>
  friend struct detail::LatestSlotLayout;
  template <typename>
  friend struct detail::LatestSlotTestAccess;
};

// Machine-readable geometry of one described LatestSlot page. The opaque
// ShmSuperHeader::layout_hash cannot be inverted, so a reader that only knows
// the header (the probe, a foreign peer) would have to guess
// RoundUp64(sizeof(T)) from the manifest to find published_idx_/seq_. This
// fixed block removes that guess: payload_bytes is the exact sizeof(T),
// cell_stride is RoundUp64(payload_bytes), slot_count is the fixed 2 cells.
// It is a full cache line, so the payload body stays 64-byte aligned.
// Publishes are ordered by the super-header magic (release) written last;
// readers acquire magic before touching this block.
struct alignas(64) LatestSlotPageGeometry {
  std::atomic<uint64_t> payload_bytes{0};
  std::atomic<uint64_t> cell_stride{0};
  std::atomic<uint32_t> slot_count{0};
  std::atomic<uint32_t> reserved0{0};
  uint64_t reserved[4] = {};
};

static_assert(sizeof(LatestSlotPageGeometry) == 64,
              "LatestSlot geometry block is one cache line");
static_assert(offsetof(LatestSlotPageGeometry, payload_bytes) == 0,
              "payload bytes lead the geometry block");
static_assert(offsetof(LatestSlotPageGeometry, cell_stride) == 8,
              "cell stride follows payload bytes");
static_assert(offsetof(LatestSlotPageGeometry, slot_count) == 16,
              "slot count follows the stride");
static_assert(offsetof(LatestSlotPageGeometry, reserved0) == 20,
              "reserved word keeps the block 8-aligned");

// Fixed offsets shared by every LatestSlot<T> page. The template model below
// and the type-erased probe/read path both use these constants; nothing
// re-derives "64 B header + geometry + 2 cells + 64 B control tail" by hand.
namespace latest_slot_page_layout {
inline constexpr std::size_t kHeaderBytes = sizeof(ShmSuperHeader);
inline constexpr std::size_t kGeometryBytes = sizeof(LatestSlotPageGeometry);
inline constexpr std::size_t kGeometryOffset = kHeaderBytes;
inline constexpr std::size_t kSlotsOffset = kGeometryOffset + kGeometryBytes;
inline constexpr std::size_t kSlotCount = 2;
// published_idx_/reserved_/seq_ + the process-local tap tail (64 B).
inline constexpr std::size_t kControlBytes = 64;
constexpr std::size_t RegionBytes(std::size_t cell_stride) {
  return kSlotsOffset + kSlotCount * cell_stride + kControlBytes;
}
}  // namespace latest_slot_page_layout

// Writes the geometry block before the page is published. Plain atomic stores:
// the release store of the super-header magic is the publish barrier.
inline void InitLatestSlotPageGeometry(LatestSlotPageGeometry* geometry,
                                       uint64_t payload_bytes,
                                       uint64_t cell_stride,
                                       uint32_t slot_count) {
  if (geometry == nullptr) return;
  geometry->payload_bytes.store(payload_bytes, std::memory_order_relaxed);
  geometry->cell_stride.store(cell_stride, std::memory_order_relaxed);
  geometry->slot_count.store(slot_count, std::memory_order_relaxed);
  geometry->reserved0.store(0, std::memory_order_relaxed);
  for (uint64_t& value : geometry->reserved) value = 0;
}

// Structural validation of a described LatestSlot page using ONLY the page's
// own header + geometry block (no payload type, no manifest). Checks magic,
// version, the reserved words, that the geometry is self-consistent and that
// the declared page size matches the derived region. The payload layout hash
// stays opaque here; callers that know T validate the exact payload/stride via
// ValidateLatestSlotPage<T>.
inline ShmPageErrc ValidateLatestSlotPageGeometry(
    const void* base, std::size_t mapped_bytes,
    ShmPageError* error = nullptr) {
  using namespace latest_slot_page_layout;
  auto fail = [error](ShmPageErrc code, uint64_t expected, uint64_t actual) {
    if (error != nullptr) *error = ShmPageError{code, expected, actual};
    return code;
  };
  if (error != nullptr) *error = ShmPageError{};
  if (base == nullptr || mapped_bytes < kSlotsOffset) {
    return fail(ShmPageErrc::kTooSmall, kSlotsOffset, mapped_bytes);
  }
  const auto* header = static_cast<const ShmSuperHeader*>(base);
  const uint64_t magic = header->magic.load(std::memory_order_acquire);
  if (magic == 0) {
    return fail(ShmPageErrc::kNotReady, kLatestSlotPageMagic, magic);
  }
  if (magic != kLatestSlotPageMagic) {
    return fail(ShmPageErrc::kBadMagic, kLatestSlotPageMagic, magic);
  }
  const uint32_t version =
      header->abi_version.load(std::memory_order_relaxed);
  if (version != kLatestSlotPageAbiVersion) {
    return fail(ShmPageErrc::kBadAbiVersion, kLatestSlotPageAbiVersion,
                version);
  }
  const uint64_t layout = header->layout_hash.load(std::memory_order_relaxed);
  if (layout == 0) {
    return fail(ShmPageErrc::kBadLayoutHash, 1, layout);
  }
  if (header->reserved0 != 0) {
    return fail(ShmPageErrc::kBadReserved, 0, header->reserved0);
  }
  for (const uint64_t value : header->reserved) {
    if (value != 0) return fail(ShmPageErrc::kBadReserved, 0, value);
  }
  const auto* geometry = reinterpret_cast<const LatestSlotPageGeometry*>(
      static_cast<const unsigned char*>(base) + kGeometryOffset);
  const uint64_t payload_bytes =
      geometry->payload_bytes.load(std::memory_order_relaxed);
  const uint64_t stride = geometry->cell_stride.load(std::memory_order_relaxed);
  const uint32_t slots =
      geometry->slot_count.load(std::memory_order_relaxed);
  if (payload_bytes == 0) {
    return fail(ShmPageErrc::kBadGeometry, 1, payload_bytes);
  }
  if (payload_bytes > mapped_bytes) {
    return fail(ShmPageErrc::kBadGeometry, mapped_bytes, payload_bytes);
  }
  const uint64_t rounded = ((payload_bytes + 63u) / 64u) * 64u;
  if (stride != rounded) {
    return fail(ShmPageErrc::kBadGeometry, rounded, stride);
  }
  if (slots != kSlotCount) {
    return fail(ShmPageErrc::kBadGeometry, kSlotCount, slots);
  }
  if (geometry->reserved0.load(std::memory_order_relaxed) != 0) {
    return fail(ShmPageErrc::kBadReserved, 0,
                geometry->reserved0.load(std::memory_order_relaxed));
  }
  for (const uint64_t value : geometry->reserved) {
    if (value != 0) return fail(ShmPageErrc::kBadReserved, 0, value);
  }
  const uint64_t region =
      static_cast<uint64_t>(RegionBytes(static_cast<std::size_t>(stride)));
  const uint64_t declared = header->page_bytes.load(std::memory_order_relaxed);
  if (declared != region) {
    return fail(ShmPageErrc::kBadPageBytes, region, declared);
  }
  if (mapped_bytes < region) {
    return fail(ShmPageErrc::kTooSmall, region, mapped_bytes);
  }
  return ShmPageErrc::kOk;
}

// SHM page wrapper, layout v3: the 64 B described header, the fixed 64 B
// geometry block and the unchanged in-process body. Only the page carries the
// magic/version/layout identity, so a v2 region (a bare LatestSlot<T>) can
// never validate as a page. In-process users construct LatestSlot<T> directly
// and never pay for the header or geometry; the wrapper adds no indirection to
// BeginWrite/CommitWrite.
template <typename T>
struct LatestSlotPage {
  ShmSuperHeader header;
  LatestSlotPageGeometry geometry;
  LatestSlot<T> slot;
};

namespace detail {
// Permanent compile-time lock on the in-process LatestSlot body (layout v2,
// RT-03) and the legacy v2 page size. The body offsets are probe ABI
// (src/probe/page_reader.cpp derives published_idx_/seq_ from the cell
// stride) and the generated kGraphShmSlotSizes still sizes the body. The v3
// SHM page wraps this body in a ShmSuperHeader; its geometry lives in
// LatestSlotLayoutModel<T> below. Instantiating this struct for a T is what
// runs the checks; the test seam and tests/test_abi_layout.cpp do exactly
// that.
template <typename T>
struct LatestSlotLayout {
  using Slot = LatestSlot<T>;
  using Cell = typename Slot::Cell;
  static_assert(sizeof(Cell) % 64 == 0, "Cell stride must be a 64 multiple");
  static_assert(sizeof(Cell) == ((sizeof(T) + 63u) / 64u) * 64u,
                "Cell stride must equal RoundUp64(sizeof(T))");
  static_assert(offsetof(Slot, slot_) == 0, "slot_ must lead the body");
  static_assert(offsetof(Slot, published_idx_) == 2 * sizeof(Cell),
                "published_idx_ offset is probe ABI");
  static_assert(offsetof(Slot, reserved_) == 2 * sizeof(Cell) + 4,
                "reserved_ pad keeps seq_ 8-aligned (layout v2)");
  static_assert(offsetof(Slot, seq_) == 2 * sizeof(Cell) + 8,
                "seq_ offset is probe ABI (u64 since RT-03)");
  static_assert(offsetof(Slot, tap_page_) == 2 * sizeof(Cell) + 16,
                "process-local tap pointer lives in the body tail");
  static_assert(offsetof(Slot, tap_struct_id_) == 2 * sizeof(Cell) + 24,
                "tap struct id lives in the body tail");
  static_assert(sizeof(Slot) == 2 * sizeof(Cell) + 64,
                "body size (legacy kGraphShmSlotSizes) must stay stable");

  static constexpr std::size_t kCellBytes = sizeof(Cell);
  // Exported so LatestSlotLayoutModel<T> is the only place that adds the page
  // header; nothing else re-derives these offsets.
  static constexpr std::size_t kBodyBytes = sizeof(Slot);
  static constexpr std::size_t kPublishedIdxOffset =
      offsetof(Slot, published_idx_);
  static constexpr std::size_t kSeqOffset = offsetof(Slot, seq_);
  static constexpr std::size_t kControlBytes =
      sizeof(Slot) - 2 * sizeof(Cell);
  // Legacy v2 page size (body only). Kept for the ABI tests that still pin it.
  static constexpr std::size_t kPageBytes = sizeof(Slot);
};

// Test-only seam (RT-03): wraparound tests construct seq_ states near the
// u64 boundary that no feasible run can reach by writing. Never use in
// production — writing seq_ outside the Begin/Commit/Abort protocol breaks
// the seqlock contract. The layout asserts live in LatestSlotLayout above.
template <typename T>
struct LatestSlotTestAccess : LatestSlotLayout<T> {
  using Slot = typename LatestSlotLayout<T>::Slot;

  static void SetSeq(Slot* s, uint64_t seq) {
    s->seq_.store(seq, std::memory_order_relaxed);
  }
  static uint64_t Seq(const Slot* s) {
    return s->seq_.load(std::memory_order_relaxed);
  }
  static uint32_t PublishedIdx(const Slot* s) {
    return s->published_idx_.load(std::memory_order_relaxed);
  }
};
}  // namespace detail

// Identity of one LatestSlot<T> page: the ShmPageSpec handed to the described
// region API plus the payload identity that was mixed into its layout_hash.
// Keeping the two together means a caller can never hold the combined hash
// without the payload_layout_hash it was built from.
struct LatestSlotPageSpec {
  ShmPageSpec shm;
  uint64_t payload_layout_hash = 0;
};

// The single source of truth for a LatestSlot<T> SHM page: header at offset 0,
// the two payload cells at kSlotsOffset, the published_idx_/seq_ control tail
// at kPublishedIdxOffset/kSeqOffset, the cell stride, and the exact region
// size. The generator's slot table, the supervisor, the probe and the record
// tap must all derive their numbers from here instead of re-deriving
// 2*RoundUp64(sizeof(T)) + 64 independently.
//
// The body layout stays locked by detail::LatestSlotLayout<T>; the model only
// adds the described page header and the payload identity.
template <typename T>
struct LatestSlotLayoutModel {
  using Body = detail::LatestSlotLayout<T>;
  using Page = LatestSlotPage<T>;

  static constexpr uint64_t kMagic = kLatestSlotPageMagic;
  static constexpr uint32_t kAbiVersion = kLatestSlotPageAbiVersion;
  static constexpr std::size_t kHeaderBytes =
      latest_slot_page_layout::kHeaderBytes;
  static constexpr std::size_t kGeometryOffset =
      latest_slot_page_layout::kGeometryOffset;
  static constexpr std::size_t kGeometryBytes =
      latest_slot_page_layout::kGeometryBytes;
  static constexpr std::size_t kPayloadBytes = sizeof(T);
  static constexpr std::size_t kSlotCount =
      latest_slot_page_layout::kSlotCount;
  static constexpr std::size_t kCellStride = Body::kCellBytes;
  static constexpr std::size_t kSlotsOffset =
      latest_slot_page_layout::kSlotsOffset;
  static constexpr std::size_t kSlotsBytes = kSlotCount * kCellStride;
  static constexpr std::size_t kPublishedIdxOffset =
      kSlotsOffset + Body::kPublishedIdxOffset;
  static constexpr std::size_t kSeqOffset = kSlotsOffset + Body::kSeqOffset;
  static constexpr std::size_t kControlBytes = Body::kControlBytes;
  static constexpr std::size_t kRegionBytes =
      latest_slot_page_layout::RegionBytes(kCellStride);

  static_assert(kHeaderBytes == 64, "ShmSuperHeader is one cache line");
  static_assert(kGeometryBytes == 64, "geometry block is one cache line");
  static_assert(kGeometryOffset == kHeaderBytes,
                "geometry follows the super-header");
  static_assert(kSlotsOffset == kGeometryOffset + kGeometryBytes,
                "payload cells follow the geometry block");
  static_assert(kSlotsOffset % 64 == 0, "payload cells start on a line");
  static_assert(kCellStride % 64 == 0, "cell stride is RoundUp64(sizeof(T))");
  static_assert(kCellStride == ((kPayloadBytes + 63u) / 64u) * 64u,
                "cell stride must be RoundUp64(sizeof(T))");
  static_assert(offsetof(Page, header) == 0, "header leads the page");
  static_assert(offsetof(Page, geometry) == kGeometryOffset,
                "geometry offset is page ABI");
  static_assert(offsetof(Page, slot) == kSlotsOffset,
                "payload body offset is page ABI");
  static_assert(sizeof(Page) == kRegionBytes,
                "page is header + geometry + body with no tail padding");
  // alignof(LatestSlot<T>) is 64 (Cell's alignas), so a 64-byte geometry block
  // after the 64-byte header leaves no padding: the body starts at 128.
  static_assert(alignof(LatestSlot<T>) <= latest_slot_page_layout::kGeometryBytes &&
                    latest_slot_page_layout::kGeometryBytes %
                            alignof(LatestSlot<T>) ==
                        0,
                "geometry block must end on a body-aligned offset");
  static_assert(kPublishedIdxOffset == kSlotsOffset + kSlotsBytes,
                "published_idx_ follows the two payload cells");
  static_assert(kSeqOffset == kPublishedIdxOffset + 8,
                "seq_ is u64 and 8-aligned after published_idx_");
  static_assert(kRegionBytes == kPublishedIdxOffset + kControlBytes,
                "region is header + geometry + cells + control tail");
  static_assert(kRegionBytes % 64 == 0, "region is cache-line sized");

  // Builds the only valid PageSpec for this payload. payload_layout_hash is
  // the payload's wire/metadata identity supplied by the caller; 0 is
  // rejected because it would make every same-size layout interchangeable.
  // The combined layout_hash covers both the slot geometry above and the
  // payload identity.
  static constexpr LatestSlotPageSpec PageSpec(
      uint64_t payload_layout_hash, uint64_t feature_bits = 0);

  // True only for a spec produced by PageSpec() (or one field-for-field equal
  // to it): payload hash present, page magic/version/size exact, and the
  // combined layout hash re-derived from the payload hash.
  static constexpr bool IsSpecValid(const LatestSlotPageSpec& spec);
};

// Combined page layout hash for a LatestSlot<T> page: slot geometry plus the
// caller-supplied payload identity, mixed with the shared ShmLayoutHash
// convention from shm_page.h. Exposed so generated code can pin it.
template <typename T>
constexpr uint64_t LatestSlotPageLayoutHash(uint64_t payload_layout_hash) {
  using Model = LatestSlotLayoutModel<T>;
  const uint64_t hash =
      ShmLayoutHash(kLatestSlotPageMagic, Model::kAbiVersion,
                    Model::kHeaderBytes, Model::kGeometryOffset,
                    Model::kGeometryBytes, Model::kPayloadBytes,
                    Model::kCellStride, Model::kRegionBytes,
                    Model::kSlotsOffset, Model::kSlotsBytes,
                    Model::kPublishedIdxOffset, Model::kSeqOffset,
                    Model::kSlotCount, payload_layout_hash);
  // 0 is the "absent identity" sentinel and never a valid layout hash.
  return hash == 0 ? 1 : hash;
}

template <typename T>
constexpr LatestSlotPageSpec LatestSlotLayoutModel<T>::PageSpec(
    uint64_t payload_layout_hash, uint64_t feature_bits) {
  LatestSlotPageSpec spec{};
  spec.payload_layout_hash = payload_layout_hash;
  spec.shm.magic = kMagic;
  spec.shm.abi_version = kAbiVersion;
  spec.shm.page_bytes = kRegionBytes;
  spec.shm.feature_bits = feature_bits;
  spec.shm.layout_hash = payload_layout_hash == 0
                             ? 0
                             : LatestSlotPageLayoutHash<T>(payload_layout_hash);
  return spec;
}

template <typename T>
constexpr bool LatestSlotLayoutModel<T>::IsSpecValid(
    const LatestSlotPageSpec& spec) {
  return spec.payload_layout_hash != 0 && spec.shm.magic == kMagic &&
         spec.shm.abi_version == kAbiVersion &&
         spec.shm.page_bytes == kRegionBytes && spec.shm.layout_hash != 0 &&
         spec.shm.layout_hash ==
             LatestSlotPageLayoutHash<T>(spec.payload_layout_hash);
}

// Validates an already-mapped LatestSlot<T> page against the model: spec
// consistency, then the available mapping length, then the 64 B super-header
// (magic, version, combined layout hash, page bytes, feature bits). Callers
// must not touch the body until this returns kOk; geometry is always taken
// from the model, never from a previously mapped page.
template <typename T>
ShmPageErrc ValidateLatestSlotPage(const void* base, std::size_t mapped_bytes,
                                   const LatestSlotPageSpec& spec,
                                   ShmPageError* error = nullptr) {
  using Model = LatestSlotLayoutModel<T>;
  if (error != nullptr) *error = ShmPageError{};
  if (!Model::IsSpecValid(spec)) {
    if (error != nullptr) {
      *error = ShmPageError{ShmPageErrc::kInvalidSpec, 0, 0};
    }
    return ShmPageErrc::kInvalidSpec;
  }
  if (base == nullptr || mapped_bytes < Model::kRegionBytes) {
    if (error != nullptr) {
      *error = ShmPageError{ShmPageErrc::kTooSmall, Model::kRegionBytes,
                            mapped_bytes};
    }
    return ShmPageErrc::kTooSmall;
  }
  const ShmPageErrc header_code =
      ValidateShmSuperHeader(base, mapped_bytes, spec.shm, error);
  if (header_code != ShmPageErrc::kOk) return header_code;
  const ShmPageErrc geometry_code =
      ValidateLatestSlotPageGeometry(base, mapped_bytes, error);
  if (geometry_code != ShmPageErrc::kOk) return geometry_code;
  // The page is self-consistent but may describe another payload layout. The
  // exact payload size and stride are part of this T's identity.
  const auto* geometry = reinterpret_cast<const LatestSlotPageGeometry*>(
      static_cast<const unsigned char*>(base) + Model::kGeometryOffset);
  const uint64_t payload_bytes =
      geometry->payload_bytes.load(std::memory_order_relaxed);
  const uint64_t stride = geometry->cell_stride.load(std::memory_order_relaxed);
  if (payload_bytes != Model::kPayloadBytes || stride != Model::kCellStride) {
    if (error != nullptr) {
      *error = ShmPageError{ShmPageErrc::kBadGeometry, Model::kCellStride,
                            stride};
    }
    return ShmPageErrc::kBadGeometry;
  }
  return ShmPageErrc::kOk;
}

}  // namespace thyme
