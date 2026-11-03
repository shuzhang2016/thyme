#pragma once
#include "thyme/contracts/abi.h"
#include "thyme/runtime/detail/atomic_payload.h"
#ifndef NDEBUG
#include "thyme/runtime/detail/single_writer_guard.h"
#endif
#include "thyme/runtime/futex.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/waitable.h"
#include "thyme/runtime/write_section.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <type_traits>

namespace thyme {

// RT-09: bounded retry budget of SpmcReader::TryPop/ReadLatest (one greppable
// constant for the retry-loop audit; exhaustion returns false and never hands
// out a torn payload).
inline constexpr int kSpmcReadRetries = 16;

// "THYMSPM1" — distinct from kShmSpscRingMagic ("THYMSSR1").
// Version 2 adds the shared `epoch_waiters` count so a producer wakes only
// the readers that are actually asleep (PERF-14); v1 always issued a wake-all.
// Version 3 adds per-reader drop slots and a shared slot-allocation bitmap.
// Version 4 (ARCH-11) moves the page behind a 64 B `ShmSuperHeader`; v3 pages
// fail the described-open instead of being read at shifted offsets.
inline constexpr uint64_t kShmSpmcRingMagic = 0x5448594D53504D31ULL;
inline constexpr uint32_t kShmSpmcRingVersion = 4;
inline constexpr std::size_t kShmSpmcReaderSlotMaskOffset = 208;
inline constexpr std::size_t kShmSpmcReaderDropSlotsOffset = 256;
inline constexpr std::size_t kShmSpmcPayloadOffset = 320;
static_assert(kShmSpmcRingMagic != 0x5448594D53535231ULL,
              "SPMC magic must be distinct from kShmSpscRingMagic");

// The per-page initialisation (cap, drop slots, super-header, magic last) is a
// templated static member on ShmSpmcRing because the layout hash depends on
// both T and Cap; a type-erased raw helper could not publish a complete spec.

enum class SpmcRead { Follow, Latest };

struct PushResult {
  bool ok;
  // Old slots overwritten by this publish (0/1); not a per-reader loss count.
  uint64_t overwritten;
};

template <typename T, std::size_t Cap>
class SpmcReader;

namespace detail {
// RT-23 test seam: lets a test set the reader's drop counter above 2^32
// without billions of TryPop iterations. Never used by production code.
template <typename T, std::size_t Cap>
struct SpmcReaderTestAccess;
}  // namespace detail

// Cross-process SPMC broadcast ring living in shared memory.
//
// Single writer, many readers. Push never fails: the oldest slot is
// overwritten. Each consumer holds its own cursor in SpmcReader (follow or
// latest). Notification is a futex on an epoch word inside the region;
// a process-local notify_targets_ array (not in SHM) is a same-process test
// hook for EventThread hubs. Create/Open follow the magic-last protocol.
//
// Notify registry (RT-08): the two fields live on the producer's notify path
// but are NOT setup-only — SpmcReader::Bind / Waitable::SetNotify can register
// a target while the ring is already being published (late attach). What is
// guaranteed:
//   * n_notify_ is an atomic slot counter; a registration reserves a slot with
//     a CAS and then publishes the target with a release store into
//     notify_targets_[i]. Notify loads the count with acquire and each slot
//     with acquire, so it either sees a fully initialised target or skips a
//     null (not-yet-published) slot. A publish that races a registration may
//     miss that one target; the next publish includes it.
//   * registrations are bounded by kAbiMaxSpmcReaders; the (N+1)th returns
//     false without taking a slot.
//   * `notifying_` is a re-entrancy guard for the Notify -> target->Notify()
//     fan-out (a reader forwarding back into the ring must not recurse), not a
//     lock. It is relaxed-atomic: overlapping Notify calls from two threads
//     stay well-defined and may both fan out (duplicate wakeups are benign),
//     but the single-producer contract still calls Notify from one thread in
//     normal operation.
template <typename T, std::size_t Cap>
class ShmSpmcRing {
  static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "Cap power of two");
  static_assert(std::is_trivially_copyable<T>::value,
                "ShmSpmcRing payload must be trivially copyable for shm use");
  static_assert(std::atomic<uint64_t>::is_always_lock_free,
                "ShmSpmcRing requires lock-free uint64 atomics");
  static_assert(std::atomic<uint32_t>::is_always_lock_free,
                "ShmSpmcRing requires lock-free uint32 atomics");

 public:
  // RT-10: page alignment comes from the first alignas(64) member plus the
  // page-aligned shm mapping. Line assignment: super-header, cap, head,
  // epoch+epoch_waiters+reader_drops+reader_slot_mask (one line on purpose:
  // the futex word and the sleeper count are read together by the writer's
  // wake decision), per-reader drop slots, then one line per payload Cell.
  // Keep new hot words line-aligned; see docs/reference/cacheline-audit.md.
  struct Layout {
    static constexpr uint64_t kMagic = kShmSpmcRingMagic;
    static constexpr uint32_t kVersion = kShmSpmcRingVersion;

    ShmSuperHeader header;
    alignas(64) std::atomic<uint32_t> cap{0};
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint32_t> epoch{0};
    // Readers currently blocked in WaitUntil, one slot per reader process.
    // The writer wakes exactly this many sleepers (broadcast epoch: never
    // wake-one, or a slower reader would be stranded) and skips the syscall
    // when it is zero.
    std::atomic<uint32_t> epoch_waiters{0};
    // Aggregate drops observed by all readers; uses v2 reserved padding.
    std::atomic<uint64_t> reader_drops{0};
    // Shared across wrappers/processes. A set bit owns one per-reader counter;
    // successful Bind never reuses it during this page's lifetime.
    std::atomic<uint32_t> reader_slot_mask{0};
    alignas(64) std::atomic<uint64_t> reader_drop_slots[kAbiMaxSpmcReaders]{};
    struct alignas(64) Cell {
      std::atomic<uint64_t> seq{0};
      T value;
    };
    Cell slots[Cap];

    // Full ARCH-11 page description; see ShmSpscRing::Layout::LayoutHash for
    // why every size/offset participates.
    static constexpr uint64_t LayoutHash() {
      return ShmLayoutHash(
          kMagic, kVersion, static_cast<uint64_t>(sizeof(Layout)),
          static_cast<uint64_t>(alignof(Layout)),
          static_cast<uint64_t>(sizeof(T)), static_cast<uint64_t>(alignof(T)),
          static_cast<uint64_t>(Cap), kAbiMaxSpmcReaders,
          static_cast<uint64_t>(offsetof(Layout, cap)),
          static_cast<uint64_t>(offsetof(Layout, head)),
          static_cast<uint64_t>(offsetof(Layout, epoch)),
          static_cast<uint64_t>(offsetof(Layout, epoch_waiters)),
          static_cast<uint64_t>(offsetof(Layout, reader_drops)),
          static_cast<uint64_t>(offsetof(Layout, reader_slot_mask)),
          static_cast<uint64_t>(offsetof(Layout, reader_drop_slots)),
          static_cast<uint64_t>(offsetof(Layout, slots)),
          static_cast<uint64_t>(sizeof(typename Layout::Cell)));
    }

    static constexpr ShmPageSpec Spec() {
      return ShmPageSpec{kMagic, kVersion, LayoutHash(),
                         static_cast<uint64_t>(sizeof(Layout)), 0};
    }
  };
  static_assert(alignof(T) <= 64, "ShmSpmcRing cells provide 64-byte alignment");
  // SHM page layout, kShmSpmcRingVersion == 4 (ARCH-11 super-header);
  // docs/reference/abi-compatibility.md. Changing any of these offsets REQUIRES a
  // version bump: the probe (src/probe/page_reader.cpp) reads
  // head/epoch/slots via offsetof, and RegionBytes() (= sizeof(Layout)) is
  // recorded in the manifest slot_bytes.
  static_assert(offsetof(Layout, header) == 0, "super-header leads the page");
  static_assert(offsetof(Layout, cap) == 64, "cap owns the second cell");
  static_assert(offsetof(Layout, head) == 128, "head owns the third cell");
  static_assert(offsetof(Layout, epoch) == 192,
                "epoch futex word is probe ABI");
  static_assert(offsetof(Layout, epoch_waiters) == 196,
                "epoch_waiters is v2 ABI (PERF-14)");
  static_assert(offsetof(Layout, reader_drops) == 200,
                "reader_drops uses v2 reserved padding");
  static_assert(offsetof(Layout, reader_slot_mask) == kShmSpmcReaderSlotMaskOffset,
                "reader slot bitmap is v3 ABI");
  static_assert(offsetof(Layout, reader_drop_slots) == kShmSpmcReaderDropSlotsOffset,
                "per-reader drops are v3 ABI");
  static_assert(offsetof(Layout, slots) == kShmSpmcPayloadOffset,
                "payload cells start after the 5-cell header");
  static_assert(offsetof(typename Layout::Cell, value) ==
                    ((sizeof(std::atomic<uint64_t>) + alignof(T) - 1u) /
                     alignof(T)) * alignof(T),
                "payload follows the seqlock at its natural alignment");
  static_assert(sizeof(typename Layout::Cell) ==
                    ((offsetof(typename Layout::Cell, value) + sizeof(T) + 63u) /
                     64u) * 64u,
                "slot stride must be RoundUp64(payload end)");
  static_assert(sizeof(Layout) == kShmSpmcPayloadOffset +
                                   Cap * sizeof(typename Layout::Cell),
                "RegionBytes() must stay 5 header cells + Cap slot cells");

  ShmSpmcRing() = default;
  ~ShmSpmcRing() { Close(); }
  ShmSpmcRing(const ShmSpmcRing&) = delete;
  ShmSpmcRing& operator=(const ShmSpmcRing&) = delete;

  static constexpr std::size_t RegionBytes() { return sizeof(Layout); }

  static bool Prepare(const char* name) {
    ShmRegion::UnlinkName(name);
    ShmRegion region;
    if (!region.Create(name, RegionBytes())) return false;
    auto* layout = new (region.data()) Layout;
    layout->cap.store(static_cast<uint32_t>(Cap), std::memory_order_relaxed);
    InitShmSuperHeader(&layout->header, Layout::Spec());
    ShmPageError error;
    if (ValidateShmSuperHeader(region.data(), region.size(),
                               Layout::Spec(), &error) != ShmPageErrc::kOk) {
      ShmRegion::UnlinkName(name);
      return false;
    }
    return true;
  }

  bool Create(const char* name) {
    if (!region_.Create(name, RegionBytes())) return false;
    layout_ = new (region_.data()) Layout;
    layout_->cap.store(static_cast<uint32_t>(Cap), std::memory_order_relaxed);
    layout_->reader_slot_mask.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kAbiMaxSpmcReaders; ++i) {
      layout_->reader_drop_slots[i].store(0, std::memory_order_relaxed);
    }
    InitShmSuperHeader(&layout_->header, Layout::Spec());
    ShmPageError error;
    if (ValidateShmSuperHeader(region_.data(), region_.size(),
                               Layout::Spec(), &error) != ShmPageErrc::kOk) {
      std::fprintf(stderr, "ShmSpmcRing: %s create validation failed (%s)\n",
                   name, ShmPageErrorName(error.code));
      Close();
      ShmRegion::UnlinkName(name);
      return false;
    }
    return true;
  }

  bool Open(const char* name, uint64_t timeout_ns) {
    if (!region_.OpenDescribed(name, Layout::Spec(), timeout_ns)) return false;
    Layout* l = static_cast<Layout*>(region_.data());
    if (l->cap.load(std::memory_order_acquire) != static_cast<uint32_t>(Cap)) {
      std::fprintf(stderr, "ShmSpmcRing: %s capacity mismatch\n", name);
      region_.Unmap();
      return false;
    }
    layout_ = l;
    return true;
  }

  void Close() {
    layout_ = nullptr;
    region_.Unmap();
  }

  // Mapping accessors used by SpmcReader (same process as this wrapper).
  Layout* layout() const { return layout_; }
  std::atomic<uint64_t>* head() const {
    return layout_ != nullptr ? &layout_->head : nullptr;
  }
  std::atomic<uint32_t>* epoch() const {
    return layout_ != nullptr ? &layout_->epoch : nullptr;
  }

  // Optional record tap (triggered acquisition, design doc 4.1): when armed,
  // every successful Push also appends the value to the process-local mirror
  // ring. Process-local state (like notify_targets_): never stored in the shm
  // Layout. The assembly layer injects it; nullptr (default) = no recording,
  // zero overhead. Tap failures are counted in the page and never affect Push.
  void SetRecordTap(RecordMirrorRingPage* page, uint16_t struct_id) {
    tap_page_ = page;
    tap_struct_id_ = struct_id;
  }

  // Writer thread only, and not from inside BeginWrite/CommitWrite.
  void ClearRecordTap() {
    tap_page_ = nullptr;
    tap_struct_id_ = 0;
  }

  bool record_tap_armed() const { return tap_page_ != nullptr; }
  uint16_t record_tap_struct_id() const { return tap_struct_id_; }

  // Thin wrapper over the RT-14 in-place path so both paths share ONE publish
  // sequence (BeginWrite opens the slot, CommitWrite publishes it). The copy
  // from the caller buffer uses atomic bytes: a reader may be copying the slot
  // this Push overwrites, and per docs/design/adr/0001 the overlap is race-free only
  // when both sides access the payload atomically (RT-19).
  // Deprecated for new code (RT-16, docs/reference/coding-rules.md CR-02): call
  // BeginWrite/CommitWrite directly; this wrapper stays for existing callers
  // and replay/injection paths that already hold a materialized value.
  PushResult PushWithResult(const T& v) {
    WriteSection<ShmSpmcRing> section(*this);
    detail::AtomicPayloadCopyIn(section.get(), &v, sizeof(T));
    const PushResult result{true, pending_head_ >= Cap ? 1u : 0u};
    section.commit();
    return result;
  }

  void Push(const T& v) { (void)PushWithResult(v); }

  // RT-14: in-place publish. Marks the head slot write-in-progress (seq=0)
  // and returns its payload for the caller to fill with PLAIN writes: the
  // single writer owns the slot between BeginWrite and CommitWrite, so no
  // atomic byte copy is needed for the in-place fill itself (ADR 0001
  // writer-side rule). Overwrite semantics are unchanged: the slot may hold
  // the oldest frame, which is replaced.
  //
  // Abort semantics: an un-committed slot keeps seq=0 — readers treat it as
  // write-in-progress and head never covers it, so the half-filled payload is
  // never observed. AbortWrite() has no shared-state rollback: the frame the
  // slot previously held is sacrificed (a follow reader that had not consumed it
  // counts a drop, never a torn read), because restoring its seq after the
  // payload was touched would resurrect torn bytes. Single writer: never
  // interleave two BeginWrite/CommitWrite pairs.
  T* BeginWrite() {
    pending_head_ = layout_->head.load(std::memory_order_relaxed);
    auto& slot = layout_->slots[pending_head_ & (Cap - 1)];
#ifndef NDEBUG
    detail::SingleWriterGuard::Begin("ShmSpmcRing", layout_, &slot.value,
                                     pending_head_ + 1u);
#endif
    slot.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    return &slot.value;
  }

  // Publishes the slot filled through BeginWrite. Ordering is exactly the
  // protocol Push always used: seq (release) -> head (release) -> record tap
  // -> Notify (epoch bump + wake of the registered sleepers, PERF-14).
  void CommitWrite() {
    auto& slot = layout_->slots[pending_head_ & (Cap - 1)];
#ifndef NDEBUG
    detail::SingleWriterGuard::Check("CommitWrite", "ShmSpmcRing", layout_,
                                     &slot.value, pending_head_ + 1u);
#endif
    slot.seq.store(pending_head_ + 1, std::memory_order_release);
    layout_->head.store(pending_head_ + 1, std::memory_order_release);
    if (tap_page_ != nullptr) {
      tap_page_->TryWriteRing(tap_struct_id_, &slot.value, sizeof(T), MonoNs(),
                              static_cast<uint32_t>(pending_head_));
    }
    Notify();
#ifndef NDEBUG
    detail::SingleWriterGuard::End("CommitWrite", "ShmSpmcRing", layout_,
                                   &slot.value, pending_head_ + 1u);
#endif
  }

  // Abort class 3/4 (write_section.h): slot allocated then sacrificed. seq=0
  // hole; head is not advanced until CommitWrite. See BeginWrite.
#ifndef NDEBUG
  void AbortWrite() {
    auto& slot = layout_->slots[pending_head_ & (Cap - 1)];
    detail::SingleWriterGuard::Check("AbortWrite", "ShmSpmcRing", layout_,
                                     &slot.value, pending_head_ + 1u);
    detail::SingleWriterGuard::End("AbortWrite", "ShmSpmcRing", layout_,
                                   &slot.value, pending_head_ + 1u);
  }
#else
  void AbortWrite() {}
#endif

  // Registers a Waitable that Notify() forwards to (same-process test hook /
  // EventThread wake hub). Safe at any time, including late attach while the
  // producer is publishing — see the class comment (RT-08). Returns false when
  // the registry is full or the target is null.
  bool AddNotifyTarget(Waitable* t) {
    if (t == nullptr) return false;
    uint32_t i = n_notify_.load(std::memory_order_relaxed);
    for (;;) {
      if (i >= kAbiMaxSpmcReaders) return false;
      if (n_notify_.compare_exchange_weak(i, i + 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
        // Publish the pointer AFTER the slot count: a racing Notify that
        // already sees this slot reads null and skips it (next publish
        // includes the target), never a torn pointer.
        notify_targets_[i].store(t, std::memory_order_release);
        return true;
      }
    }
  }

  void Notify() {
    if (layout_ == nullptr) return;
    // Bind registers the reader itself as a notify target; SpmcReader::Notify
    // forwards here. The guard must come BEFORE the bump: otherwise every
    // Push re-bumped/woke the epoch once per bound reader (reader.Notify ->
    // ring.Notify -> bump + wake -> guard), i.e. N+1 wake syscalls for one
    // publish (PERF-14). RT-08: relaxed loads/stores keep overlapping Notify
    // calls well-defined; the guard is recursion suppression, not mutual
    // exclusion (two concurrent fan-outs are harmless duplicates).
    if (notifying_.load(std::memory_order_relaxed)) return;
    layout_->epoch.fetch_add(1, std::memory_order_seq_cst);
    // Readers share one epoch: wake every registered sleeper, so slow readers
    // are never stranded. With none registered this is a no-op and the epoch
    // bump still lets a later waiter observe the publish.
    FutexWakeWaiters(
        &layout_->epoch,
        layout_->epoch_waiters.load(std::memory_order_seq_cst));
    notifying_.store(true, std::memory_order_relaxed);
    const uint32_t n_notify = n_notify_.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n_notify; ++i) {
      Waitable* target = notify_targets_[i].load(std::memory_order_acquire);
      if (target != nullptr) target->Notify();
    }
    notifying_.store(false, std::memory_order_relaxed);
  }

 private:
  friend class SpmcReader<T, Cap>;

  uint32_t ClaimReaderDropSlot() {
    uint32_t mask = layout_->reader_slot_mask.load(std::memory_order_relaxed);
    for (;;) {
      uint32_t index = 0;
      while (index < kAbiMaxSpmcReaders && (mask & (1u << index)) != 0) {
        ++index;
      }
      if (index == kAbiMaxSpmcReaders) return kAbiMaxSpmcReaders;
      if (layout_->reader_slot_mask.compare_exchange_weak(
              mask, mask | (1u << index), std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        return index;
      }
    }
  }

  void ReleaseReaderDropSlot(uint32_t index) {
    if (index >= kAbiMaxSpmcReaders) return;
    layout_->reader_drop_slots[index].store(0, std::memory_order_relaxed);
    layout_->reader_slot_mask.fetch_and(~(1u << index),
                                        std::memory_order_acq_rel);
  }

  static uint64_t MonoNs() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
  }

  ShmRegion region_;
  Layout* layout_{nullptr};
  // Process-local notify registry (never stored in the shm layout). RT-08:
  // registration can run while the producer is publishing (late attach), so
  // the slot count and the slots themselves are atomics — see AddNotifyTarget
  // and the class comment for the publication protocol. Capacity is the ABI
  // reader ceiling; Bind and SetNotify both register here.
  std::atomic<Waitable*> notify_targets_[kAbiMaxSpmcReaders]{};
  std::atomic<uint32_t> n_notify_{0};
  // RT-08: re-entrancy guard for the Notify -> target->Notify() fan-out.
  // Relaxed atomic (no hot-path cost on x86; a plain mov on AArch64 too), so
  // overlapping Notify calls are well-defined rather than a race. It is a
  // recursion suppressor, NOT a lock. Nothing re-enters between the epoch bump
  // and the store above, so the guard covers the fan-out only.
  std::atomic<bool> notifying_{false};
  // Process-local record tap (see SetRecordTap).
  RecordMirrorRingPage* tap_page_{nullptr};
  uint16_t tap_struct_id_{0};
  // Process-local pending BeginWrite state (RT-14): the publish index the
  // open write targets. Never stored in the Layout.
  uint64_t pending_head_{0};
};

template <typename T, std::size_t Cap>
class SpmcReader final : public Waitable {
  static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "Cap power of two");
  static_assert(std::is_trivially_copyable<T>::value,
                "SpmcReader payload must be trivially copyable for shm use");

 public:
  SpmcReader() = default;
  SpmcReader(const SpmcReader&) = delete;
  SpmcReader& operator=(const SpmcReader&) = delete;

  // RT-08: publish the reader state BEFORE registering the reader as a notify
  // target. AddNotifyTarget release-stores the target pointer and the fan-out
  // acquire-loads it, so this ordering makes ring_/mode_/cursors visible to a
  // concurrent Notify() that reaches this reader. Registering first would let
  // the fan-out call Notify() on a reader whose ring_ is still null (or racing
  // the store) — that is the late-attach race the audit found.
  bool Bind(ShmSpmcRing<T, Cap>* ring, SpmcRead mode) {
    if (ring == nullptr || ring->layout() == nullptr) return false;
    if (ring_ != nullptr) return false;
    drop_slot_ = ring->ClaimReaderDropSlot();
    if (drop_slot_ == kAbiMaxSpmcReaders) return false;
    ring_ = ring;
    mode_ = mode;
    next_ = 0;
    seen_head_ = 0;
    drops_ = 0;
    retries_ = 0;
    torn_ = 0;
    if (!ring->AddNotifyTarget(this)) {
      ring_ = nullptr;
      ring->ReleaseReaderDropSlot(drop_slot_);
      drop_slot_ = kAbiMaxSpmcReaders;
      return false;
    }
    return true;
  }

  bool TryPop(T& out, uint64_t* dropped_since_last = nullptr) {
    if (mode_ == SpmcRead::Latest) {
      return ReadLatest(out, dropped_since_last);
    }
    if (dropped_since_last == nullptr) return TryPopFollow(out);
    const uint64_t before = drops_;
    const bool ok = TryPopFollow(out);
    *dropped_since_last = drops_ - before;
    return ok;
  }

  bool ReadLatest(T& out, uint64_t* dropped_since_last = nullptr) {
    if (dropped_since_last == nullptr) return ReadLatestImpl(out);
    const uint64_t before = drops_;
    const bool ok = ReadLatestImpl(out);
    *dropped_since_last = drops_ - before;
    return ok;
  }

  uint64_t drop_count() const { return drops_; }
  uint32_t reader_slot() const { return drop_slot_; }
  // Registered sleepers across all readers of this ring (the shared
  // epoch_waiters word). Diagnostic only: a waiter that misses the probe
  // re-checks readiness after registering, so the count is advisory. Tests use
  // it to prove both readers have blocked before measuring wake latency.
  uint32_t waiter_count() const {
    if (ring_ == nullptr || ring_->layout_ == nullptr) return 0;
    return ring_->layout_->epoch_waiters.load(std::memory_order_seq_cst);
  }
  // RT-09: bounded-retry accounting for this reader (process-local and
  // non-atomic like drop_count(): the cursor and counters belong to the one
  // thread that reads this reader). retry_count() counts loop re-reads;
  // torn_count() the subset where a publish invalidated the payload copy.
  // Nothing is touched on the success path.
  uint64_t retry_count() const { return retries_; }
  uint64_t torn_count() const { return torn_; }

  bool empty() const override {
    if (ring_ == nullptr || ring_->layout_ == nullptr) return true;
    const uint64_t h = ring_->layout_->head.load(std::memory_order_acquire);
    if (mode_ == SpmcRead::Latest) return seen_head_ == h;
    return next_ >= h;
  }

  void WaitUntil(uint64_t deadline_ns) override {
    if (ring_ == nullptr || ring_->layout_ == nullptr) return;
    // Register this reader as a sleeper before re-checking readiness; the
    // writer wakes exactly the registered count and skips the syscall when it
    // is zero. Register-then-check keeps a skipped wake harmless.
    ring_->layout_->epoch_waiters.fetch_add(1, std::memory_order_seq_cst);
    for (;;) {
      if (!empty()) break;
      const uint64_t now = MonoNs();
      if (now >= deadline_ns) break;
      const uint32_t snap =
          ring_->layout_->epoch.load(std::memory_order_seq_cst);
      if (!empty()) break;
      const uint64_t rem = deadline_ns - now;
      struct timespec rel {};
      rel.tv_sec = static_cast<time_t>(rem / 1000000000ULL);
      rel.tv_nsec = static_cast<long>(rem % 1000000000ULL);
      FutexWait(&ring_->layout_->epoch, snap, &rel);
#ifndef __linux__
      struct timespec req {0, 1000000};
      nanosleep(&req, nullptr);
#endif
    }
    ring_->layout_->epoch_waiters.fetch_sub(1, std::memory_order_seq_cst);
  }

  void Notify() override {
    if (ring_ != nullptr) ring_->Notify();
  }

  // Registers an additional notify target. Safe at any time (RT-08): the
  // registry uses atomic publication, so a late attach never races the
  // producer; a publish already in flight may miss the new target.
  void SetNotify(Waitable* target) override {
    if (ring_ == nullptr || target == nullptr) return;
    ring_->AddNotifyTarget(target);
  }

 private:
  friend struct detail::SpmcReaderTestAccess<T, Cap>;

  void NoteDrops(uint64_t n) {
    drops_ += n;
    if (ring_ != nullptr && ring_->layout_ != nullptr) {
      if (drop_slot_ < kAbiMaxSpmcReaders) {
        ring_->layout_->reader_drop_slots[drop_slot_].fetch_add(
            n, std::memory_order_relaxed);
      }
      ring_->layout_->reader_drops.fetch_add(n, std::memory_order_relaxed);
    }
  }

  bool TryPopFollow(T& out) {
    if (ring_ == nullptr || ring_->layout_ == nullptr) return false;
    typename ShmSpmcRing<T, Cap>::Layout* l = ring_->layout_;
    constexpr uint64_t mask = Cap - 1;
    for (int attempt = 0; attempt < kSpmcReadRetries; ++attempt) {
      const uint64_t h = l->head.load(std::memory_order_acquire);
      if (next_ >= h) return false;
      if (h - next_ > Cap) {
        NoteDrops(h - Cap - next_);
        next_ = h - Cap;
      }
      auto& slot = l->slots[next_ & mask];
      const uint64_t s0 = slot.seq.load(std::memory_order_acquire);
      if (s0 != next_ + 1) {
        retries_ += 1;
        const uint64_t h2 = l->head.load(std::memory_order_acquire);
        if (h2 - next_ > Cap) {
          NoteDrops(h2 - Cap - next_);
          next_ = h2 - Cap;
        }
        continue;
      }
      CopyPayload(out, slot.value);
      const uint64_t s1 = slot.seq.load(std::memory_order_acquire);
      if (s1 != s0) {
        retries_ += 1;
        torn_ += 1;
        NoteDrops(1);
        next_ += 1;
        continue;
      }
      next_ += 1;
      return true;
    }
    return false;
  }

  bool ReadLatestImpl(T& out) {
    if (ring_ == nullptr || ring_->layout_ == nullptr) return false;
    typename ShmSpmcRing<T, Cap>::Layout* l = ring_->layout_;
    constexpr uint64_t mask = Cap - 1;
    for (int i = 0; i < kSpmcReadRetries; ++i) {
      const uint64_t h = l->head.load(std::memory_order_acquire);
      if (h == 0 || h == seen_head_) return false;
      auto& slot = l->slots[(h - 1) & mask];
      const uint64_t s0 = slot.seq.load(std::memory_order_acquire);
      if (s0 != h) {
        retries_ += 1;
        continue;
      }
      CopyPayload(out, slot.value);
      const uint64_t s1 = slot.seq.load(std::memory_order_acquire);
      if (s1 != s0) {
        retries_ += 1;
        torn_ += 1;
        continue;
      }
      next_ = seen_head_ = h;
      return true;
    }
    return false;
  }

  static uint64_t MonoNs() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
  }

  static void CopyPayload(T& out, const T& src) {
    // Atomic payload bytes -- see Push() above (RT-19).
    detail::AtomicPayloadCopyOut(&out, &src, sizeof(T));
  }

  ShmSpmcRing<T, Cap>* ring_{nullptr};
  SpmcRead mode_{SpmcRead::Follow};
  uint64_t next_{0};
  uint64_t seen_head_{0};
  uint64_t drops_{0};
  uint32_t drop_slot_{kAbiMaxSpmcReaders};
  // RT-09 retry accounting (see retry_count()/torn_count()); single-reader
  // state, non-atomic like the cursor above.
  uint64_t retries_{0};
  uint64_t torn_{0};
};

template <typename T, std::size_t Cap>
inline void CreditSpmcReaderDrops(const SpmcReader<T, Cap>& reader,
                                  HealthSlot* health, uint64_t& seen) {
  if (health == nullptr) return;
  const uint64_t current = reader.drop_count();
  // RT-23: pass the full 64-bit delta through. HealthSlot::drop_cnt is 64-bit
  // on the v3 page; truncating or saturating here would lose reader drops
  // above 2^32 at the last step.
  health->AddDrops(current - seen);
  seen = current;
}

namespace detail {
template <typename T, std::size_t Cap>
struct SpmcReaderTestAccess {
  static void SetDropCount(SpmcReader<T, Cap>& reader, uint64_t drops) {
    reader.drops_ = drops;
  }
};
}  // namespace detail

}  // namespace thyme
