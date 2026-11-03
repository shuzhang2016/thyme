#pragma once
#ifndef NDEBUG
#include "thyme/runtime/detail/single_writer_guard.h"
#endif
#include "thyme/runtime/futex.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/waitable.h"
#include "thyme/runtime/write_section.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <type_traits>

namespace thyme {

// Magic/version shared by every ShmSpscRing instantiation (kept in sync with
// Layout below). Version 2: payload slots became cacheline-isolated cells
// (alignas(64) Cell wrapping a natural-layout T); v1 stored a packed T array.
// Version 3: the layout carries `epoch_waiters`, the cross-process sleeper
// count that lets a producer skip the futex wake when nobody is waiting
// (PERF-14). Version 4 (ARCH-11): the page starts with a 64 B
// `ShmSuperHeader` and the ring fields move one cell down, so attach can
// validate magic/version/layout hash/page size/features before touching the
// payload. No dual-read: a v3 page fails the v4 described-open.
inline constexpr uint64_t kShmSpscRingMagic = 0x5448594D53535231ULL;
inline constexpr uint32_t kShmSpscRingVersion = 4;

// Cross-process SPSC full-delivery queue living in shared memory.
//
// Unlike LatestSlot (latest-wins) and SpscRing (drop-oldest), Push never
// overwrites: a full ring returns false and bumps push_fail_count, leaving
// the drop policy to the caller. Values are stored by value in the region,
// so T must be trivially copyable. Notification is a futex on an epoch word
// inside the region (Linux). Waiters publish themselves in a shared counter
// before re-checking the epoch (register-then-check), so Push can skip the
// futex syscall when nobody is sleeping and still cannot lose a wakeup: the
// epoch bump precedes the counter read, and a later waiter either observes
// the bumped epoch or finds data ready. Non-Linux falls back to a 1ms poll.
//
// Create/Open follow the SharedHealthPage magic-gate pattern: the creator
// zero-fills via ShmRegion, then release-stores the magic last; the opener
// spins on the magic before touching anything else.
template <typename T, std::size_t Cap>
class ShmSpscRing : public Waitable {
  static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "Cap power of two");
  static_assert(std::is_trivially_copyable<T>::value,
                "ShmSpscRing payload must be trivially copyable for shm use");
  static_assert(std::atomic<uint32_t>::is_always_lock_free,
                "ShmSpscRing requires lock-free uint32 atomics");

 public:
  // RT-10: the page is 64-byte aligned because its first member is
  // alignas(64) and shm regions are page-aligned; each field group below owns
  // exactly one cache line (head/tail/epoch+waiters), and every payload Cell
  // is one line. Do not add unaligned hot words between them.
  struct Layout {
    static constexpr uint64_t kMagic = kShmSpscRingMagic;
    static constexpr uint32_t kVersion = kShmSpscRingVersion;

    ShmSuperHeader header;
    alignas(64) std::atomic<uint32_t> cap{0};
    alignas(64) std::atomic<uint32_t> head{0};
    alignas(64) std::atomic<uint32_t> tail{0};
    alignas(64) std::atomic<uint32_t> epoch{0};
    // Registered epoch sleepers (incremented by WaitUntil before it re-checks
    // readiness). Lives in shm so a producer in another process can decide
    // between wake-one and no wake at all (PERF-14).
    std::atomic<uint32_t> epoch_waiters{0};
    std::atomic<uint32_t> push_fail{0};
    // Each payload slot owns a 64-byte-aligned cacheline cell (stride
    // round_up(sizeof(T), 64)): payloads are natural-layout contract types,
    // so cacheline isolation is the container's job.
    struct alignas(64) Cell {
      T value;
    };
    Cell slots[Cap];

    // Full ARCH-11 page description. The layout hash binds every
    // ABI-relevant value: magic/version, page and payload type sizes, the
    // capacity, and each field/slot offset. A same-size page built with a
    // different T or Cap therefore fails attach instead of misreading slots.
    static constexpr uint64_t LayoutHash() {
      return ShmLayoutHash(
          kMagic, kVersion, static_cast<uint64_t>(sizeof(Layout)),
          static_cast<uint64_t>(alignof(Layout)),
          static_cast<uint64_t>(sizeof(T)), static_cast<uint64_t>(alignof(T)),
          static_cast<uint64_t>(Cap),
          static_cast<uint64_t>(offsetof(Layout, cap)),
          static_cast<uint64_t>(offsetof(Layout, head)),
          static_cast<uint64_t>(offsetof(Layout, tail)),
          static_cast<uint64_t>(offsetof(Layout, epoch)),
          static_cast<uint64_t>(offsetof(Layout, epoch_waiters)),
          static_cast<uint64_t>(offsetof(Layout, push_fail)),
          static_cast<uint64_t>(offsetof(Layout, slots)),
          static_cast<uint64_t>(sizeof(typename Layout::Cell)));
    }

    static constexpr ShmPageSpec Spec() {
      return ShmPageSpec{kMagic, kVersion, LayoutHash(),
                         static_cast<uint64_t>(sizeof(Layout)), 0};
    }
  };
  static_assert(alignof(T) <= 64, "ShmSpscRing cells provide 64-byte alignment");
  // SHM page layout, kShmSpscRingVersion == 4 (ARCH-11 super-header);
  // docs/reference/abi-compatibility.md. Changing any of these offsets REQUIRES a
  // version bump: the probe (src/probe/page_reader.cpp) reads
  // head/tail/epoch/push_fail/slots via offsetof, and RegionBytes()
  // (= sizeof(Layout)) is recorded in the manifest slot_bytes.
  static_assert(offsetof(Layout, header) == 0, "super-header leads the page");
  static_assert(offsetof(Layout, cap) == 64, "cap owns the second cell");
  static_assert(offsetof(Layout, head) == 128, "head owns the third cell");
  static_assert(offsetof(Layout, tail) == 192, "tail offsets are probe ABI");
  static_assert(offsetof(Layout, epoch) == 256,
                "epoch futex word is probe ABI");
  static_assert(offsetof(Layout, epoch_waiters) == 260,
                "epoch_waiters is v3 ABI (PERF-14)");
  static_assert(offsetof(Layout, push_fail) == 264,
                "push_fail offset is probe ABI");
  static_assert(offsetof(Layout, slots) == 320,
                "payload cells start after the 5-cell header");
  static_assert(sizeof(typename Layout::Cell) ==
                    ((sizeof(T) + 63u) / 64u) * 64u,
                "slot stride must be RoundUp64(sizeof(T))");
  static_assert(sizeof(Layout) == 320 + Cap * sizeof(typename Layout::Cell),
                "RegionBytes() must stay 5 header cells + Cap slot cells");

  ShmSpscRing() = default;
  ~ShmSpscRing() override { Close(); }
  ShmSpscRing(const ShmSpscRing&) = delete;
  ShmSpscRing& operator=(const ShmSpscRing&) = delete;

  static constexpr std::size_t RegionBytes() { return sizeof(Layout); }

  // Supervisor / Prepare path: Unlink any leftover name, create a described
  // page, initialise the ring header, publish magic last, then Unmap. The
  // named object remains for subsequent Open by producers and consumers (no
  // role Create).
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
    // Ring fields are zero from the mapping; publish magic last.
    InitShmSuperHeader(&layout_->header, Layout::Spec());
    ShmPageError error;
    if (ValidateShmSuperHeader(region_.data(), region_.size(),
                               Layout::Spec(), &error) != ShmPageErrc::kOk) {
      std::fprintf(stderr, "ShmSpscRing: %s create validation failed (%s)\n",
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
    if (l->cap.load(std::memory_order_acquire) != Cap) {
      std::fprintf(stderr, "ShmSpscRing: %s capacity mismatch\n", name);
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

  // Producer-side wake word: after every successful Push the word is bumped
  // so a consumer EventThread waiting on the SAME shared word (SetWakeWord)
  // is woken across processes. When the local notify target already bumps
  // that same word (EventThread's WakeHub), the direct bump/wake is skipped,
  // so one publish wakes each word at most once. nullptr disables.
  void SetWakeWord(std::atomic<uint32_t>* w) {
    wake_word_.store(w, std::memory_order_release);
  }

  // Appends v; returns false when full (caller decides the drop policy).
  // Thin wrapper over the RT-13 in-place path so both paths share ONE publish
  // sequence (BeginWrite slot pick -> payload stores -> CommitWrite release).
  // Deprecated for new code (RT-16, docs/reference/coding-rules.md CR-02): call
  // BeginWrite/CommitWrite directly; this wrapper stays for existing callers
  // and replay/injection paths that already hold a materialized value.
  // Process-local record tap. Not stored in Layout. Writer thread only, and
  // not from inside BeginWrite/CommitWrite. Null page means disarmed.
  void SetRecordTap(RecordMirrorRingPage* page, uint16_t struct_id) {
    tap_page_ = page;
    tap_struct_id_ = struct_id;
  }

  void ClearRecordTap() {
    tap_page_ = nullptr;
    tap_struct_id_ = 0;
  }

  bool record_tap_armed() const { return tap_page_ != nullptr; }
  uint16_t record_tap_struct_id() const { return tap_struct_id_; }

  bool Push(const T& v) {
    WriteSection<ShmSpscRing> section(*this);
    if (!section) return false;
    *section = v;
    section.commit();
    return true;
  }

  // RT-13: in-place publish. Returns the payload of the slot the next publish
  // will occupy, for the caller to fill with plain writes (the single
  // producer owns the slot until CommitWrite; a consumer only ever reads
  // slots below head). Returns nullptr when the ring is full — push_fail is
  // counted exactly like a failed Push, and the caller must NOT spin-retry
  // (backpressure is the contract).
  //
  // Abort semantics: head is not advanced until CommitWrite, so simply not
  // committing leaves the slot unpublished — a consumer never observes the
  // half-filled payload, and the next BeginWrite hands out the same slot
  // again. AbortWrite() has no shared-state rollback, keeping the three
  // primitives' in-place APIs uniform. Single producer: never interleave two
  // BeginWrite/CommitWrite pairs.
  T* BeginWrite() {
    const uint32_t h = layout_->head.load(std::memory_order_relaxed);
    const uint32_t t = layout_->tail.load(std::memory_order_acquire);
    if (h - t == Cap) {
      layout_->push_fail.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
#ifndef NDEBUG
    T* const slot = &layout_->slots[h & (Cap - 1)].value;
    detail::SingleWriterGuard::Begin("ShmSpscRing", layout_, slot, h + 1u);
    return slot;
#else
    return &layout_->slots[h & (Cap - 1)].value;
#endif
  }

  // Publishes the slot filled through BeginWrite. Memory ordering is the
  // protocol Push always used: the plain payload stores are sequenced before
  // the release-store of head, so a consumer that acquire-loads the new head
  // observes the complete payload. The epoch bump / wake discipline is
  // unchanged (PERF-14: exactly one epoch bump per publish, no extra wakes).
  void CommitWrite() {
    const uint32_t h = layout_->head.load(std::memory_order_relaxed);
#ifndef NDEBUG
    detail::SingleWriterGuard::Check(
        "CommitWrite", "ShmSpscRing", layout_,
        &layout_->slots[h & (Cap - 1)].value, h + 1u);
#endif
    layout_->head.store(h + 1, std::memory_order_release);
    if (tap_page_ != nullptr) {
      tap_page_->TryWriteRing(tap_struct_id_,
                              &layout_->slots[h & (Cap - 1)].value, sizeof(T),
                              MonoNs(), h);
    }
    Waitable* target = notify_target_.load(std::memory_order_acquire);
    // One publish => exactly one epoch bump. If the attached target forwards
    // its wake to this same epoch word (EventThread hub with SetWakeWord to
    // the ring epoch), let its Notify own the bump/wake; otherwise bump here.
    const bool target_bumps_epoch =
        target != nullptr && target->NotifyWakeWord() == &layout_->epoch;
    if (!target_bumps_epoch) {
      layout_->epoch.fetch_add(1, std::memory_order_seq_cst);
      WakeEpoch();
    }
    if (target != nullptr) target->Notify();
    // Cross-process wake word: on a mapping with no local target, the shared
    // word is bumped and woken here. When a local target is attached and it
    // bumps the same word (the EventThread hub), that Notify already covers
    // it; bumping again would issue a second futex wake for one message.
    // The SPSC contract has one consumer node per word, so wake-one cannot
    // strand a second waiter; a waiter that is about to sleep either sees the
    // bump or is inside FUTEX_WAIT and gets picked.
    std::atomic<uint32_t>* ww = wake_word_.load(std::memory_order_acquire);
    if (ww != nullptr && ww != &layout_->epoch &&
        (target == nullptr || target->NotifyWakeWord() != ww)) {
      ww->fetch_add(1, std::memory_order_seq_cst);
      FutexWakeOne(ww);
    }
#ifndef NDEBUG
    detail::SingleWriterGuard::End(
        "CommitWrite", "ShmSpscRing", layout_,
        &layout_->slots[h & (Cap - 1)].value, h + 1u);
#endif
  }

  // Abort class 2/4 (write_section.h): head not advanced / no-op. BeginWrite
  // touched no shared head, so release AbortWrite is empty. See BeginWrite.
#ifndef NDEBUG
  void AbortWrite() {
    const uint32_t h = layout_->head.load(std::memory_order_relaxed);
    T* const slot = &layout_->slots[h & (Cap - 1)].value;
    detail::SingleWriterGuard::Check("AbortWrite", "ShmSpscRing", layout_,
                                     slot, h + 1u);
    detail::SingleWriterGuard::End("AbortWrite", "ShmSpscRing", layout_, slot,
                                   h + 1u);
  }
#else
  void AbortWrite() {}
#endif

  bool TryPop(T& out) {
    const uint32_t t = layout_->tail.load(std::memory_order_relaxed);
    const uint32_t h = layout_->head.load(std::memory_order_acquire);
    if (t == h) return false;
    out = layout_->slots[t & (Cap - 1)].value;
    layout_->tail.store(t + 1, std::memory_order_release);
    return true;
  }

  bool empty() const override {
    const uint32_t t = layout_->tail.load(std::memory_order_relaxed);
    const uint32_t h = layout_->head.load(std::memory_order_acquire);
    return t == h;
  }

  // Ring fill (head - tail) for queue-depth probes; kDepthUnknown while the
  // region is not mapped. SPSC snapshot, may race a concurrent push/pop.
  uint64_t Depth() const override {
    if (layout_ == nullptr) return kDepthUnknown;
    const uint32_t h = layout_->head.load(std::memory_order_acquire);
    const uint32_t t = layout_->tail.load(std::memory_order_acquire);
    return static_cast<uint64_t>(h - t);
  }

  uint32_t push_fail_count() const {
    return layout_->push_fail.load(std::memory_order_relaxed);
  }

  // Wake-path observability (tests / perf evidence): epoch bumps observed by
  // this mapping and sleepers currently registered for the epoch futex.
  uint32_t epoch() const {
    return layout_->epoch.load(std::memory_order_seq_cst);
  }
  uint32_t epoch_waiters() const {
    return layout_->epoch_waiters.load(std::memory_order_seq_cst);
  }

  // Waitable::SetNotify override: producer-side local hook — Push runs on
  // the producer's mapping, so the target is notified only when attached to
  // the wrapper Push is called through. Cross-process consumer wake-up is
  // the futex (WaitUntil), not this hook.
  void SetNotify(Waitable* target) override {
    notify_target_.store(target, std::memory_order_release);
  }

  void Notify() override {
    layout_->epoch.fetch_add(1, std::memory_order_seq_cst);
    WakeEpoch();
  }

  void WaitUntil(uint64_t deadline_ns) override {
    // Register before the first readiness check. A producer that reads the
    // count after this add sees us; one that read it before is ordered before
    // this add, hence before the epoch snapshot and re-check below (all
    // seq_cst), so its bump is either observed (EAGAIN) or the ready check
    // wins. Register-then-check is what makes the waiter count advisory-only
    // and keeps a skipped wake provably harmless.
    layout_->epoch_waiters.fetch_add(1, std::memory_order_seq_cst);
    for (;;) {
      if (!empty()) break;
      const uint64_t now = MonoNs();
      if (now >= deadline_ns) break;
      const uint32_t snap = layout_->epoch.load(std::memory_order_seq_cst);
      if (!empty()) break;
      const uint64_t rem = deadline_ns - now;
      struct timespec rel {};
      rel.tv_sec = static_cast<time_t>(rem / 1000000000ULL);
      rel.tv_nsec = static_cast<long>(rem % 1000000000ULL);
      // EINTR/EAGAIN (epoch moved between the empty check and the wait) both
      // fall through to the loop re-check. Off Linux FutexWait is a no-op,
      // so poll at 1ms.
      FutexWait(&layout_->epoch, snap, &rel);
#ifndef __linux__
      struct timespec req {0, 1000000};
      nanosleep(&req, nullptr);
#endif
    }
    layout_->epoch_waiters.fetch_sub(1, std::memory_order_seq_cst);
  }

 private:
  // Wakes exactly the registered sleepers. With none registered the futex
  // syscall is skipped; the epoch bump already happened, so a waiter that
  // registers later snapshots the bumped epoch or re-checks readiness and
  // cannot miss this publish.
  void WakeEpoch() {
    const uint32_t waiters =
        layout_->epoch_waiters.load(std::memory_order_seq_cst);
    FutexWakeWaiters(&layout_->epoch, waiters);
  }

  static uint64_t MonoNs() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
  }

  ShmRegion region_;
  Layout* layout_{nullptr};
  // In-process pointers (never stored in the shm layout): notify_target_ is
  // the Waitable hook, wake_word_ the shared futex word for cross-process
  // consumer wake-up (SetWakeWord).
  std::atomic<Waitable*> notify_target_{nullptr};
  std::atomic<std::atomic<uint32_t>*> wake_word_{nullptr};
  RecordMirrorRingPage* tap_page_{nullptr};
  uint16_t tap_struct_id_ = 0;
};

}  // namespace thyme
