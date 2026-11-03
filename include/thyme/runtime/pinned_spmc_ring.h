#pragma once

#include "thyme/runtime/waitable.h"
#include "thyme/runtime/write_section.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <type_traits>

namespace thyme {

enum class PinnedReadMode : uint8_t {
  kFollow = 0,
  kLatest = 1,
};

// Process-local single-writer/multi-reader ring with borrowed, RAII-pinned
// payloads. The writer never overwrites a pinned value; a pinned target is
// skipped and counted. Follow readers report overwritten holes, while Latest
// readers intentionally skip older values without counting them as drops.
template <typename T, std::size_t Slots, std::size_t MaxReaders = 8>
class PinnedSpmcRing {
  static_assert(Slots >= 2, "PinnedSpmcRing needs at least two slots");
  static_assert(MaxReaders > 0, "PinnedSpmcRing needs a reader slot");
  static_assert(std::is_trivially_copyable<T>::value,
                "PinnedSpmcRing payload must be trivially copyable");
  static_assert(std::is_standard_layout<T>::value,
                "PinnedSpmcRing payload must be standard layout");

 public:
  static constexpr uint64_t kInvalidFrame = ~static_cast<uint64_t>(0);
  static constexpr int kReadRetries = 16;
  static constexpr uint32_t kWriterBit = 1u << 31;
  static constexpr uint32_t kPinMask = ~kWriterBit;

  class Reader;

  class Borrowed {
   public:
    Borrowed() = default;
    ~Borrowed() { Reset(); }
    Borrowed(const Borrowed&) = delete;
    Borrowed& operator=(const Borrowed&) = delete;
    Borrowed(Borrowed&& other) noexcept { MoveFrom(other); }
    Borrowed& operator=(Borrowed&& other) noexcept {
      if (this != &other) {
        Reset();
        MoveFrom(other);
      }
      return *this;
    }

    const T* get() const {
      return ring_ != nullptr ? &ring_->slots_[frame_ % Slots].value : nullptr;
    }
    const T& operator*() const { return *get(); }
    const T* operator->() const { return get(); }
    explicit operator bool() const { return ring_ != nullptr; }
    uint64_t frame_index() const { return frame_; }
    uint64_t generation() const { return generation_; }

    bool still_valid() const {
      return ring_ != nullptr &&
             ring_->slots_[frame_ % Slots].seq.load(std::memory_order_acquire) ==
                 generation_ &&
             (ring_->slots_[frame_ % Slots].state.load(std::memory_order_acquire) &
              kPinMask) > 0;
    }

    void Reset() {
      if (ring_ != nullptr) {
        ring_->slots_[frame_ % Slots].state.fetch_sub(1, std::memory_order_release);
      }
      ring_ = nullptr;
      frame_ = kInvalidFrame;
      generation_ = 0;
    }

   private:
    friend class PinnedSpmcRing;
    void MoveFrom(Borrowed& other) {
      ring_ = other.ring_;
      frame_ = other.frame_;
      generation_ = other.generation_;
      other.ring_ = nullptr;
      other.frame_ = kInvalidFrame;
      other.generation_ = 0;
    }

    PinnedSpmcRing* ring_ = nullptr;
    uint64_t frame_ = kInvalidFrame;
    uint64_t generation_ = 0;
  };

  class Reader : public Waitable {
   public:
    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    bool Bind(PinnedSpmcRing* ring) {
      if (ring == nullptr || ring_ != nullptr) return false;
      const uint32_t id = ring->reader_count_.fetch_add(1, std::memory_order_acq_rel);
      if (id >= MaxReaders) {
        ring->reader_count_.fetch_sub(1, std::memory_order_acq_rel);
        return false;
      }
      ring_ = ring;
      reader_id_ = id;
      return true;
    }

    bool empty() const override { return ring_ == nullptr || next_ >= ring_->head(); }
    void Notify() override {}
    std::atomic<uint32_t>* NotifyWakeWord() const override {
      return ring_ != nullptr ? ring_->NotifyWakeWord() : nullptr;
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

    bool TryRead(Borrowed* out, PinnedReadMode mode = PinnedReadMode::kFollow) {
      if (out == nullptr || ring_ == nullptr) return false;
      out->Reset();
      for (int attempt = 0; attempt < PinnedSpmcRing::kReadRetries; ++attempt) {
        const uint64_t h = ring_->head_.load(std::memory_order_acquire);
        if (next_ >= h) return false;
        if (h - next_ > Slots) {
          if (mode == PinnedReadMode::kFollow) drops_ += h - Slots - next_;
          next_ = h - Slots;
        }
        if (mode == PinnedReadMode::kLatest) next_ = h - 1;

        const uint64_t frame = next_;
        auto& slot = ring_->slots_[frame % Slots];
        const uint64_t s0 = slot.seq.load(std::memory_order_acquire);
        if (s0 != frame + 1) {
          const uint64_t h2 = ring_->head_.load(std::memory_order_acquire);
          if (h2 - next_ > Slots) {
            if (mode == PinnedReadMode::kFollow) drops_ += h2 - Slots - next_;
            next_ = h2 - Slots;
            continue;
          }
          if (h2 == next_) return false;
          ++next_;
          if (mode == PinnedReadMode::kFollow) ++drops_;
          continue;
        }

        uint32_t state = slot.state.load(std::memory_order_acquire);
        if ((state & kWriterBit) != 0 || (state & kPinMask) == kPinMask) {
          continue;
        }
        if (!slot.state.compare_exchange_weak(state, state + 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
          continue;
        }
        if (slot.seq.load(std::memory_order_acquire) != s0) {
          slot.state.fetch_sub(1, std::memory_order_release);
          continue;
        }
        out->ring_ = ring_;
        out->frame_ = frame;
        out->generation_ = s0;
        ++next_;
        return true;
      }
      retries_.fetch_add(PinnedSpmcRing::kReadRetries, std::memory_order_relaxed);
      return false;
    }

    uint64_t drop_count() const { return drops_; }
    uint64_t retry_count() const { return retries_.load(std::memory_order_relaxed); }
    uint32_t reader_id() const { return reader_id_; }

   private:
    PinnedSpmcRing* ring_ = nullptr;
    uint64_t next_ = 0;
    uint64_t drops_ = 0;
    std::atomic<uint64_t> retries_{0};
    uint32_t reader_id_ = 0;
  };

  PinnedSpmcRing() = default;
  PinnedSpmcRing(const PinnedSpmcRing&) = delete;
  PinnedSpmcRing& operator=(const PinnedSpmcRing&) = delete;

  void SetNotify(Waitable* target) { notify_ = target; }
  std::atomic<uint32_t>* NotifyWakeWord() const {
    return notify_ != nullptr ? notify_->NotifyWakeWord() : nullptr;
  }

  T* BeginWrite() {
    const uint64_t h = head_.load(std::memory_order_relaxed);
    auto& slot = slots_[h % Slots];
    uint32_t expected = 0;
    if (!slot.state.compare_exchange_strong(expected, kWriterBit,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
      head_.store(h + 1, std::memory_order_release);
      pinned_skips_.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    slot.seq.store(0, std::memory_order_release);
    pending_ = h;
    return &slot.value;
  }

  void CommitWrite() {
    if (pending_ == kInvalidFrame) return;
    auto& slot = slots_[pending_ % Slots];
    slot.seq.store(pending_ + 1, std::memory_order_release);
    slot.state.store(0, std::memory_order_release);
    head_.store(pending_ + 1, std::memory_order_release);
    if (notify_ != nullptr) notify_->Notify();
    pending_ = kInvalidFrame;
  }

  void AbortWrite() {
    if (pending_ == kInvalidFrame) return;
    // The old value is sacrificed; readers treat this slot as a hole.
    slots_[pending_ % Slots].state.store(0, std::memory_order_release);
    head_.store(pending_ + 1, std::memory_order_release);
    pending_ = kInvalidFrame;
  }

  bool Publish(const T& value) {
    WriteSection<PinnedSpmcRing> section(*this);
    if (!section) return false;
    *section = value;
    section.commit();
    return true;
  }

  uint64_t pinned_skip_count() const {
    return pinned_skips_.load(std::memory_order_relaxed);
  }
  uint64_t head() const { return head_.load(std::memory_order_acquire); }

 private:
  struct alignas(64) Slot {
    std::atomic<uint64_t> seq{0};
    std::atomic<uint32_t> state{0};
    alignas(64) T value{};
  };

  Slot slots_[Slots];
  std::atomic<uint64_t> head_{0};
  std::atomic<uint64_t> pinned_skips_{0};
  std::atomic<uint32_t> reader_count_{0};
  Waitable* notify_ = nullptr;
  uint64_t pending_ = kInvalidFrame;
};

}  // namespace thyme
