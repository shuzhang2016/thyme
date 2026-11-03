#pragma once
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/waitable.h"
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <pthread.h>

namespace thyme {
// Single-producer / single-consumer ring of packed (index, generation) words.
//
// Full-ring policy: push_latest EVICTS the oldest entry (drop-oldest) and
// returns its index (plus generation via dropped_gen); drop_count() counts
// evictions. A lagging consumer may therefore skip entries, and may observe
// the same "latest" value duplicated or delivered out of production order —
// try_pop yields entries in survival order, not production order. Producers
// that need every item consumed must size Cap for the worst-case lag.
//
// Wake protocol and memory ordering (RT-06). A publish is two steps — the
// head store, then Notify's waiter probe — and a wait is register-then-check:
//
//   producer: head_.store(release)         publish
//             atomic_thread_fence(seq_cst) store-load edge
//             waiters_.load(relaxed)       Notify: wake, or skip when 0
//   consumer: waiters_.fetch_add(seq_cst)  register
//             atomic_thread_fence(seq_cst) store-load edge
//             head_.load(seq_cst)          readiness re-check (EmptySeqCst)
//
// The probe's answer is only meaningful if it is ordered after the publish, so
// Notify keeps one store-load edge in front of it. With that edge the two
// halves form the store-buffering (SB) litmus, which a seq_cst total order
// forbids: "the publish landed but the probe read 0 waiters" and "the consumer
// registered but its readiness check read an older head" cannot both hold. That
// is what makes the no-waiter fast path sound — a consumer whose registration
// the probe missed observes the publish in its own re-check instead of
// sleeping — and it is also why the edge cannot be dropped, i.e. it is priced
// per publish, not per waiter. A wake that is *not* skipped needs no edge: the
// mutex protocol already orders the broadcast against the waiter's re-check.
//
// RT-06 measurements (bench_core spsc_ring_notify_no_waiter,
// spsc_ring_wake_one_waiter, ref_publish_*):
//   * The probe load is relaxed on purpose: the fence already orders it after
//     the publish, so a seq_cst load there bought nothing (and the relaxed
//     form is a plain ldr rather than an ldar on weakly ordered targets).
//   * Folding the edge into the publish (seq_cst store, no fence) is sound in
//     the model and needs no fence on AArch64 (stlr), but on x86_64 the locked
//     read-modify-write serialises the producer's next head access and measured
//     slower than the fence, so this header keeps the fence.
//   * The other two wake paths reach the same invariant with no fence at all,
//     because their publish is already a seq_cst bump on a dedicated wake word:
//     ShmSpscRing::CommitWrite (epoch bump, then epoch_waiters probe) and
//     EventThread::WakeHub::Notify (wake-word bump, then sleeper probe). Here
//     the publish word is the ring head, where a seq_cst store is the more
//     expensive encoding on x86_64, hence the fence.
//
// Payload ordering is independent of all this: a slot's release store (or the
// caller's plain writes before CommitWrite) is published by the head store and
// read after an acquire head load, so RT-19's atomic-payload rules apply as
// before.
/** \brief In-process SPSC ring of packed (index, generation) words.
 *  \note Drop-oldest: push_latest evicts the oldest entry when full, so the
 *        consumer may skip entries. try_pop hands out an (index, generation)
 *        pair only; the payload lives in the caller's pool and the caller
 *        must release it. One producer and one consumer thread at most. */
template <std::size_t Cap>
class SpscRing : public Waitable {
  static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "Cap power of two");

 public:
  static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

  SpscRing() { InitWait(); }
  ~SpscRing() override {
    pthread_cond_destroy(&cv_);
    pthread_mutex_destroy(&mu_);
  }
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Optional record tap (triggered acquisition, design doc 4.1): when armed,
  // every successful push also appends one sample to the process-local mirror
  // ring. This ring carries (index, generation) words, not payloads; the
  // payload normally lives in a slot pool the ring indexes into. When
  // payload_base/payload_stride describe that pool, the tap copies
  // payload_base + idx * payload_stride (payload_stride bytes per sample);
  // otherwise it records the 8-byte packed (index, generation) word itself.
  // nullptr page (default) = no recording, zero overhead. Tap failures are
  // counted in the page and never affect push semantics.
  void SetRecordTap(RecordMirrorRingPage* page, uint16_t struct_id,
                    const void* payload_base = nullptr,
                    uint32_t payload_stride = 0) {
    tap_page_ = page;
    tap_struct_id_ = struct_id;
    tap_payload_base_ = payload_base;
    tap_payload_stride_ = payload_stride;
  }

  // Writer thread only. Also drops the payload-pool description.
  void ClearRecordTap() { SetRecordTap(nullptr, 0); }

  bool record_tap_armed() const { return tap_page_ != nullptr; }
  uint16_t record_tap_struct_id() const { return tap_struct_id_; }

  uint32_t push_latest(uint32_t idx, uint32_t gen,
                       uint32_t* dropped_gen = nullptr) {
    const uint64_t packed = Pack(idx, gen);
    const uint32_t h = head_.load(std::memory_order_relaxed);
    uint32_t t = tail_.load(std::memory_order_acquire);
    uint32_t dropped = kInvalidIndex;
    while (h - t == Cap) {
      uint32_t expected = t;
      if (tail_.compare_exchange_weak(expected, t + 1,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
        const uint64_t evicted =
            buf_[t & (Cap - 1)].load(std::memory_order_acquire);
        dropped = UnpackIdx(evicted);
        if (dropped_gen != nullptr) *dropped_gen = UnpackGen(evicted);
        drop_cnt_.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      t = expected;  // consumer advanced; re-evaluate fullness
    }
    buf_[h & (Cap - 1)].store(packed, std::memory_order_release);
    head_.store(h + 1, std::memory_order_release);
    TapRecord(idx, gen);
    Notify();
    return dropped;
  }

  // Non-overwriting push: returns false when the ring is full, leaving all
  // entries intact (moodycamel-style try_enqueue semantics). On success the
  // entry is appended and waiters are notified, exactly like push_latest.
  bool try_push(uint32_t idx, uint32_t gen) {
    const uint32_t h = head_.load(std::memory_order_relaxed);
    const uint32_t t = tail_.load(std::memory_order_acquire);
    if (h - t == Cap) return false;
    buf_[h & (Cap - 1)].store(Pack(idx, gen), std::memory_order_release);
    head_.store(h + 1, std::memory_order_release);
    TapRecord(idx, gen);
    Notify();
    return true;
  }

  bool try_pop(uint32_t& idx, uint32_t& gen) {
    for (;;) {
      const uint32_t t = tail_.load(std::memory_order_acquire);
      const uint32_t h = head_.load(std::memory_order_acquire);
      if (t == h) return false;
      const uint64_t packed =
          buf_[t & (Cap - 1)].load(std::memory_order_acquire);
      uint32_t expected = t;
      if (!tail_.compare_exchange_strong(expected, t + 1,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        continue;  // producer evicted/advanced tail; re-read the new head
      }
      idx = UnpackIdx(packed);
      gen = UnpackGen(packed);
      return true;
    }
  }

  bool empty() const override {
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    const uint32_t h = head_.load(std::memory_order_acquire);
    return t == h;
  }

  // Pending entries (head - tail). SPSC snapshot: may be off by one against
  // a concurrent push/pop; intended for low-frequency depth sampling.
  uint64_t size() const {
    const uint32_t h = head_.load(std::memory_order_acquire);
    const uint32_t t = tail_.load(std::memory_order_acquire);
    return static_cast<uint64_t>(h - t);
  }

  uint64_t Depth() const override { return size(); }

  uint32_t drop_count() const {
    return drop_cnt_.load(std::memory_order_relaxed);
  }

  // Registered WaitUntil callers (RT-06 observability, the in-process twin of
  // EventThread::wake_waiter_count): 0 means Notify takes the skip path. The
  // count is advisory only — the wake contract rests on the seq_cst SB
  // exclusion documented in the class comment, not on this read.
  uint32_t waiter_count() const {
    return waiters_.load(std::memory_order_seq_cst);
  }

  void Notify() override {
    // Store-load edge, then the waiter probe (see the class comment). One
    // fence per publish makes the probe's zero answer trustworthy; the probe
    // itself is relaxed because the fence has already ordered it after the
    // publish.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (waiters_.load(std::memory_order_relaxed) == 0) return;
    pthread_mutex_lock(&mu_);
    pthread_cond_broadcast(&cv_);
    pthread_mutex_unlock(&mu_);
  }

  void WaitUntil(uint64_t deadline_ns) override {
    // Register before checking readiness: a producer that probes after this
    // fetch_add sees the registration and broadcasts; one whose probe ran
    // before it published with a release store, whose fence ordered it before
    // that probe. This fence and the one in Notify are the two halves of the
    // SB pattern; the readiness re-check below is the consumer's half of the
    // pair itself (see the class comment).
    waiters_.fetch_add(1, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    pthread_mutex_lock(&mu_);
    if (EmptySeqCst()) {
      struct timespec now {};
      clock_gettime(CLOCK_MONOTONIC, &now);
      const uint64_t now_ns =
          static_cast<uint64_t>(now.tv_sec) * 1000000000ULL +
          static_cast<uint64_t>(now.tv_nsec);
      if (now_ns < deadline_ns) {
#ifdef __APPLE__
        const uint64_t rem = deadline_ns - now_ns;
        struct timespec rel {};
        rel.tv_sec = static_cast<time_t>(rem / 1000000000ULL);
        rel.tv_nsec = static_cast<long>(rem % 1000000000ULL);
        pthread_cond_timedwait_relative_np(&cv_, &mu_, &rel);
#else
        struct timespec abs {};
        abs.tv_sec = static_cast<time_t>(deadline_ns / 1000000000ULL);
        abs.tv_nsec = static_cast<long>(deadline_ns % 1000000000ULL);
        pthread_cond_timedwait(&cv_, &mu_, &abs);
#endif
      }
    }
    pthread_mutex_unlock(&mu_);
    waiters_.fetch_sub(1, std::memory_order_release);
  }

 private:
  // RT-10: head_ and tail_ each own a line so the producer's head store and
  // the consumer's tail store never false-share (docs/reference/cacheline-audit.md);
  // buf_ and the condvar state follow.
  alignas(64) std::atomic<uint32_t> head_{0};
  alignas(64) std::atomic<uint32_t> tail_{0};

  // WaitUntil's readiness check. Same predicate as empty(), but the head load
  // is seq_cst: it is the consumer half of the SB pair (register, then read a
  // head that must show any publish the producer's probe did not see). empty()
  // keeps the cheaper acquire load for depth/idle sampling, where no wake
  // decision depends on it.
  bool EmptySeqCst() const {
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    const uint32_t h = head_.load(std::memory_order_seq_cst);
    return t == h;
  }

  void TapRecord(uint32_t idx, uint32_t gen) {
    if (tap_page_ == nullptr) return;
    const uint64_t packed = Pack(idx, gen);
    const void* data = &packed;
    uint32_t size = static_cast<uint32_t>(sizeof(packed));
    if (tap_payload_base_ != nullptr) {
      data = static_cast<const uint8_t*>(tap_payload_base_) +
             static_cast<uint64_t>(idx) * tap_payload_stride_;
      size = tap_payload_stride_;
    }
    tap_page_->TryWriteRing(tap_struct_id_, data, size, MonoNs(), gen);
  }

  static uint64_t MonoNs() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
  }

  static uint64_t Pack(uint32_t idx, uint32_t gen) {
    return (static_cast<uint64_t>(gen) << 32) | idx;
  }
  static uint32_t UnpackIdx(uint64_t packed) {
    return static_cast<uint32_t>(packed);
  }
  static uint32_t UnpackGen(uint64_t packed) {
    return static_cast<uint32_t>(packed >> 32);
  }

  std::atomic<uint64_t> buf_[Cap]{};
  std::atomic<uint32_t> drop_cnt_{0};
  std::atomic<uint32_t> waiters_{0};
  // Process-local record tap (see SetRecordTap).
  RecordMirrorRingPage* tap_page_{nullptr};
  const void* tap_payload_base_{nullptr};
  uint32_t tap_payload_stride_{0};
  uint16_t tap_struct_id_{0};
  pthread_mutex_t mu_{};
  pthread_cond_t cv_{};
  void InitWait() {
    pthread_mutex_init(&mu_, nullptr);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#ifdef __linux__
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&cv_, &attr);
    pthread_condattr_destroy(&attr);
  }
};

}  // namespace thyme
