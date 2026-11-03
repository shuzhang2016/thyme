#pragma once
#include "thyme/runtime/spsc_ring.h"
#include "thyme/runtime/waitable.h"
#include "thyme/runtime/write_section.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>

namespace thyme {

// Cooperative backpressure for replay injectors only (not real-time Push).
struct PushWaitGate {
  bool (*aborted)(void* ctx) = nullptr;
  void* ctx = nullptr;
  uint64_t slice_ns = 100000000ULL;  // wait slice; re-check abort between slices
};

// In-process SPSC full-delivery queue implementing Waitable.
//
// Unlike SpscRing's drop-oldest push_latest, Push never overwrites: it uses
// try_push semantics and returns false when full. Live producers count each
// rejected push on their HealthSlot via PublishAndFork (M4.2); replay uses
// PushOrWait and does not drop. The consumer drains with TryPop. Slots are
// recycled through a free-list ring, so no allocation happens after construction.
// WaitUntil / Notify reuse SpscRing's condvar notification (see spsc_ring.h).
/** \brief In-process SPSC full-delivery queue implementing Waitable.
 *  \note Push never overwrites: it returns false when full (the producer
 *        counts the drop on its HealthSlot). TryPop moves the front payload
 *        out; no slot borrow escapes the call. The pipe owns its ring slots
 *        and free-list; the consumer side is single-threaded. */
template <typename T, std::size_t Cap>
class NotifPipe : public Waitable {
 public:
  NotifPipe() {
    for (uint32_t i = 0; i < Cap; ++i) free_.push_latest(i, 0);
  }
  NotifPipe(const NotifPipe&) = delete;
  NotifPipe& operator=(const NotifPipe&) = delete;

  // Optional process-local ring tap. Null (the default) keeps recording off;
  // only successfully committed payloads are copied, never failed/aborted writes.
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

  // Appends v; returns false when full (caller credits HealthSlot::AddDrops).
  // Deprecated for new code (RT-16, docs/reference/coding-rules.md CR-02): prefer the
  // BeginWrite/CommitWrite in-place path below. Kept as a thin wrapper so
  // both paths share ONE publish sequence (BeginWrite slot pick -> payload
  // stores -> CommitWrite publish + notify).
  bool Push(const T& v) {
    WriteSection<NotifPipe> section(*this);
    if (!section) return false;
    *section = v;
    section.commit();
    return true;
  }

  // RT-16: in-place publish. Pops a free-list slot and returns it for the
  // caller to fill with plain writes; the single producer owns the slot until
  // CommitWrite. Returns nullptr when the pipe is full — exactly the failed
  // Push case, so the caller credits HealthSlot::AddDrops and must NOT
  // spin-retry (backpressure is the contract).
  //
  // Abort semantics: the slot leaves the free list at BeginWrite but is never
  // linked into ready_ before CommitWrite, so a consumer cannot observe a
  // half-filled payload; an abandoned BeginWrite just parks the slot outside
  // both rings until AbortWrite() recycles it. Single producer: never
  // interleave two BeginWrite/CommitWrite pairs.
  T* BeginWrite() {
    uint32_t idx = 0, gen = 0;
    if (!free_.try_pop(idx, gen)) return nullptr;
    pending_idx_ = idx;
    return &slots_[idx];
  }

  // Publishes the slot filled through BeginWrite. Notification is the
  // protocol Push always used: the slot is visible (try_push release-stored
  // the head) before the extra target is notified, so its waiters re-checking
  // readiness see this push.
  void CommitWrite() {
    const uint32_t idx = pending_idx_;
    pending_idx_ = kNoPendingSlot;
    // Cannot fail: a free slot implies ready_ holds at most Cap-1 entries.
    if (!ready_.try_push(idx, 0)) {
      free_.push_latest(idx, 0);  // unreachable; keep the invariant anyway
      return;
    }
    if (tap_page_ != nullptr) {
      tap_page_->TryWriteRing(tap_struct_id_, &slots_[idx], sizeof(T), MonoNs(),
                              publish_sequence_);
    }
    ++publish_sequence_;
    Waitable* target = notify_target_.load(std::memory_order_acquire);
    if (target != nullptr) target->Notify();
  }

  // Abort class 4/4 (write_section.h): recycle unpublished slot onto the free
  // list without publishing. See BeginWrite.
  void AbortWrite() {
    if (pending_idx_ == kNoPendingSlot) return;
    free_.push_latest(pending_idx_, 0);
    pending_idx_ = kNoPendingSlot;
  }

  // Blocks until Push succeeds or gate.aborted returns true. Waits on the free
  // list (consumer recycles slots) in slice_ns steps so pause/stop can abort.
  bool PushOrWait(const T& v, const PushWaitGate& gate) {
    while (true) {
      if (gate.aborted != nullptr && gate.aborted(gate.ctx)) return false;
      if (Push(v)) return true;
      uint64_t slice = gate.slice_ns != 0 ? gate.slice_ns : 100000000ULL;
      struct timespec now {};
      clock_gettime(CLOCK_MONOTONIC, &now);
      const uint64_t now_ns =
          static_cast<uint64_t>(now.tv_sec) * 1000000000ULL +
          static_cast<uint64_t>(now.tv_nsec);
      free_.WaitUntil(now_ns + slice);
    }
  }

  // Waitable::SetNotify override: every successful Push notifies target in
  // addition to this pipe's own waiter (EventThread wires each input pipe to
  // its shared wake hub through this). Atomic so a consumer detaching during
  // teardown never races a producer into a dangling read; attach before
  // producers start, detach after they stop. nullptr detaches.
  void SetNotify(Waitable* target) override {
    notify_target_.store(target, std::memory_order_release);
  }

  bool TryPop(T& out) {
    uint32_t idx = 0, gen = 0;
    if (!ready_.try_pop(idx, gen)) return false;
    out = slots_[idx];
    free_.push_latest(idx, 0);
    return true;
  }

  bool empty() const override { return ready_.empty(); }
  // Pending (published, not yet popped) messages — the queue-depth probe
  // value for this pipe.
  uint64_t Depth() const override { return ready_.size(); }
  // Number of consumers currently registered in WaitUntil (the ring's sleeper
  // count). Diagnostic only: a registered waiter re-checks readiness, so a
  // missed publish is harmless, exactly like SpscRing::waiter_count. Tests use
  // it to prove a waiter has blocked before measuring a wake, instead of
  // sleeping.
  uint32_t waiter_count() const { return ready_.waiter_count(); }
  // Registered PushOrWait producers (the free-list ring's sleeper count).
  // Same advisory contract as waiter_count(); used to prove a blocked producer
  // without a settle sleep.
  uint32_t slot_waiter_count() const { return free_.waiter_count(); }
  void WaitUntil(uint64_t deadline_ns) override { ready_.WaitUntil(deadline_ns); }
  void Notify() override { ready_.Notify(); }

 private:
  static constexpr uint32_t kNoPendingSlot = ~0u;

  static uint64_t MonoNs() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
  }

  SpscRing<Cap> free_;
  SpscRing<Cap> ready_;
  T slots_[Cap]{};
  std::atomic<Waitable*> notify_target_{nullptr};
  RecordMirrorRingPage* tap_page_{nullptr};
  uint32_t publish_sequence_{0};
  uint16_t tap_struct_id_{0};
  // Slot handed out by BeginWrite and not yet committed/aborted (producer
  // thread only; kNoPendingSlot = no in-place publish in flight).
  uint32_t pending_idx_{kNoPendingSlot};
};

}  // namespace thyme
