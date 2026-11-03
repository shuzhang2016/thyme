#pragma once
#include "thyme/runtime/waitable.h"
#include "thyme/runtime/write_section.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <type_traits>

namespace thyme {

// Process-local, zero-copy latest-value store for large payloads with one
// writer and many readers.
//
// Unlike LatestSlot (seqlock + copy-out), this store pins the slot a reader is
// inspecting: the writer may only fill an un-pinned slot other than the
// currently published one. Readers get a move-only Borrowed<T> (RAII release)
// and never copy the payload. If every slot is pinned or writer-owned, the
// publish is dropped and counted; the store never overwrites live data.
//
// Lifetime: slots are inline; construct one store at Prepare()/setup time and
// do not copy or move it while readers are active. Payload stores happen
// before the release-store of `latest_`; a reader that observes it also sees
// the payload. The single writer must serialize BeginWrite/CommitWrite.
template <typename T, std::size_t Slots = 4>
class PinnedLatestStore : public Waitable {
  static_assert(Slots >= 2, "PinnedLatestStore needs two slots to avoid rewriting latest");
  static_assert(std::is_trivially_copyable<T>::value,
                "PinnedLatestStore payload must be trivially copyable");
  static_assert(std::is_standard_layout<T>::value,
                "PinnedLatestStore payload must be standard layout");
  static_assert(std::atomic<uint32_t>::is_always_lock_free,
                "PinnedLatestStore requires lock-free uint32 atomics");

 public:
  static constexpr uint32_t kWriterBit = 1u << 31;
  static constexpr uint32_t kPinMask = ~kWriterBit;
  static constexpr int kReadRetries = 16;
  static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

  class Borrowed {
   public:
    Borrowed() = default;
    ~Borrowed() { Reset(); }
    Borrowed(const Borrowed&) = delete;
    Borrowed& operator=(const Borrowed&) = delete;
    Borrowed(Borrowed&& o) noexcept : store_(o.store_), idx_(o.idx_) {
      o.store_ = nullptr;
      o.idx_ = kInvalid;
    }
    Borrowed& operator=(Borrowed&& o) noexcept {
      if (this != &o) {
        Reset();
        store_ = o.store_;
        idx_ = o.idx_;
        o.store_ = nullptr;
        o.idx_ = kInvalid;
      }
      return *this;
    }

    explicit operator bool() const { return store_ != nullptr; }
    const T* get() const {
      return store_ != nullptr ? &store_->slots_[idx_].value : nullptr;
    }
    const T& operator*() const { return *get(); }
    const T* operator->() const { return get(); }

    void Reset() {
      if (store_ != nullptr) {
        store_->slots_[idx_].state.fetch_sub(1, std::memory_order_release);
        store_ = nullptr;
        idx_ = kInvalid;
      }
    }

   private:
    friend class PinnedLatestStore;
    PinnedLatestStore* store_ = nullptr;
    uint32_t idx_ = kInvalid;
  };

  PinnedLatestStore() = default;
  ~PinnedLatestStore() = default;
  PinnedLatestStore(const PinnedLatestStore&) = delete;
  PinnedLatestStore& operator=(const PinnedLatestStore&) = delete;

  // Returns the next writable slot, or nullptr when every slot is pinned or
  // the only free slot is the currently published one. The slot remains
  // writer-owned until CommitWrite/AbortWrite.
  T* BeginWrite() {
    const uint32_t latest_idx = LatestIndex();
    for (std::size_t attempt = 0; attempt < Slots; ++attempt) {
      const uint32_t idx = next_++ % static_cast<uint32_t>(Slots);
      if (idx == latest_idx) continue;
      uint32_t expected = 0;
      if (slots_[idx].state.compare_exchange_strong(
              expected, kWriterBit, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        write_idx_ = idx;
        return &slots_[idx].value;
      }
    }
    drops_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }

  // Publishes the slot filled through BeginWrite. The payload writes are
  // sequenced before the release-store of `latest_`.
  void CommitWrite() {
    if (write_idx_ == kInvalid) return;
    Slot& slot = slots_[write_idx_];
    uint32_t gen = slot.gen.load(std::memory_order_relaxed) + 1;
    if (gen == 0) gen = 1;  // 0 is reserved for "never published"
    slot.gen.store(gen, std::memory_order_relaxed);
    const uint64_t snap = (static_cast<uint64_t>(gen) << 32) | write_idx_;
    latest_.store(snap, std::memory_order_release);
    slot.state.store(0, std::memory_order_release);
    if (notify_ != nullptr) notify_->Notify();
    write_idx_ = kInvalid;
  }

  // Abort class 4/4 (write_section.h): recycle unpublished slot (clear the
  // writer bit) without changing the published value.
  void AbortWrite() {
    if (write_idx_ == kInvalid) return;
    slots_[write_idx_].state.store(0, std::memory_order_release);
    write_idx_ = kInvalid;
  }

  bool Publish(const T& value) {
    WriteSection<PinnedLatestStore> section(*this);
    if (!section) return false;
    *section = value;
    section.commit();
    return true;
  }

  bool empty() const override { return !has_value(); }
  void Notify() override {}
  void SetNotify(Waitable* target) { notify_ = target; }
  std::atomic<uint32_t>* NotifyWakeWord() const override {
    return notify_ != nullptr ? notify_->NotifyWakeWord() : nullptr;
  }
  void WaitUntil(uint64_t deadline_ns) override {
    while (empty()) {
      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      const uint64_t now_ns = static_cast<uint64_t>(now.tv_sec) * 1000000000ULL +
                              static_cast<uint64_t>(now.tv_nsec);
      if (now_ns >= deadline_ns) return;
      const uint64_t remaining = deadline_ns - now_ns;
      timespec req{static_cast<time_t>(remaining / 1000000000ULL),
                   static_cast<long>(remaining % 1000000000ULL)};
      nanosleep(&req, nullptr);
    }
  }

  // Pins the latest slot and returns a view. Returns false when there is no
  // published value or the retry budget is exhausted; the payload is never
  // copied and `out` owns the pin until Reset/destruction.
  bool TryRead(Borrowed* out) {
    if (out == nullptr) return false;
    out->Reset();
    for (int attempt = 0; attempt < kReadRetries; ++attempt) {
      const uint64_t snap = latest_.load(std::memory_order_acquire);
      if (snap == 0) return false;
      const uint32_t idx = static_cast<uint32_t>(snap);
      const uint32_t gen = static_cast<uint32_t>(snap >> 32);
      if (idx >= Slots) return false;
      Slot& slot = slots_[idx];
      uint32_t state = slot.state.load(std::memory_order_acquire);
      if ((state & kWriterBit) != 0 || (state & kPinMask) == kPinMask) {
        continue;
      }
      if (!slot.state.compare_exchange_weak(state, state + 1,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        continue;
      }
      // The writer may have published a newer value before this pin landed.
      // Re-check both the published snapshot and the slot generation so a
      // recycled slot cannot be mistaken for the value we loaded.
      if (latest_.load(std::memory_order_acquire) != snap ||
          slot.gen.load(std::memory_order_relaxed) != gen) {
        slot.state.fetch_sub(1, std::memory_order_release);
        continue;
      }
      out->store_ = this;
      out->idx_ = idx;
      return true;
    }
    retries_.fetch_add(kReadRetries, std::memory_order_relaxed);
    return false;
  }

  uint64_t dropped_publishes() const {
    return drops_.load(std::memory_order_relaxed);
  }
  uint64_t read_retries() const {
    return retries_.load(std::memory_order_relaxed);
  }
  bool has_value() const {
    return latest_.load(std::memory_order_acquire) != 0;
  }

 private:
  struct alignas(64) Slot {
    std::atomic<uint32_t> state{0};
    std::atomic<uint32_t> gen{0};
    alignas(64) T value{};
  };

  uint32_t LatestIndex() const {
    const uint64_t snap = latest_.load(std::memory_order_acquire);
    return snap == 0 ? kInvalid : static_cast<uint32_t>(snap);
  }

  std::atomic<uint64_t> latest_{0};
  std::atomic<uint64_t> drops_{0};
  std::atomic<uint64_t> retries_{0};
  Slot slots_[Slots];
  Waitable* notify_ = nullptr;
  uint32_t next_ = 0;      // single writer
  uint32_t write_idx_ = kInvalid;
};

}  // namespace thyme
