#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace thyme {
enum class SlotState : uint32_t { kFree = 0, kWriting = 1, kReady = 2 };

namespace detail {
template <typename T, std::size_t N, bool kFreeList>
struct ObjectPoolTestAccess;

// Free-list storage is a separate (empty) base so the default scan variant
// keeps exactly its pre-RT-04 object layout. Enabling the option is per
// instantiation and process-local: ObjectPool lives on the heap side
// (slots_ is new[]-allocated), never in an SHM page, so this is not an ABI
// surface. A pool that is ever embedded in an SHM struct must keep the
// default (kFreeList == false).
template <std::size_t N, bool kFreeList>
struct ObjectPoolFreeList;

template <std::size_t N>
struct ObjectPoolFreeList<N, false> {};

template <std::size_t N>
struct ObjectPoolFreeList<N, true> {
  // Treiber stack: low 32 bits = slot index (0xFFFFFFFF == empty), high 32
  // bits = push tag. The tag is bumped on every push so a stale CAS (pop or
  // push) after a pop/push cycle observes a different head value.
  std::atomic<uint64_t> head_{0x00000000FFFFFFFFULL};
  // Successor of each free slot; only touched while the slot is owned by the
  // stack (written before a release-push, read while it is the head).
  std::atomic<uint32_t> next_[N]{};
};
}  // namespace detail

// Fixed-capacity slot pool with FREE -> WRITING -> READY -> FREE
// lifecycle. acquire() hands out an exclusive index (state CAS, or a
// free-list pop); the owner fills the value, publish()es it, and the consumer
// release()s it with the generation observed at acquire time. A generation
// mismatch rejects stale releases (double release, release after abort,
// release by a previous owner).
//
// Generation width (RT-04): generation_[i] is uint32 and is incremented once
// per acquire() (abort_write does not roll it back). The counter wraps with
// well-defined unsigned arithmetic; release() compares full values, so no
// ordering arithmetic exists. A full 2^32 wrap of one slot aliases the
// previous generation and can accept a stale release again. At the design
// rate of 10-50 acquisitions/s for the same slot that is 2.72 years at 50 Hz
// (2^32 / 50 / 86400 / 365 = 2.72) and 13.6 years at 10 Hz of continuous
// reuse, far beyond process lifetime / rebuild intervals, so no u64 widening
// or 0-skip is warranted (skipping a value does not remove the ABA either).
// See docs/reference/object-pool-aba.md for the release/ABA rationale.
//
// acquire() has two strategies:
//   * default (kFreeList == false): scan up to N slots from cursor_ and claim
//     the first FREE one with a CAS. O(N) worst case (e.g. one free slot
//     behind N-1 held slots), O(1) when the next cursor position is free.
//   * opt-in (kFreeList == true): pop a free index from an O(1) tagged LIFO
//     stack; release()/abort_write() push it back. Same state machine and
//     generation contract, no scan. Costs one extra CAS per release and a
//     per-slot successor word plus a head word. Bench numbers in
//     docs/reference/object-pool-aba.md; the default stays OFF because the 10-50 Hz
//     acquire rate does not justify the extra release-side work.
/** \brief Fixed-capacity pool of cacheline-isolated slots.
 *  \note The pool owns its (heap-allocated) storage. acquire()/begin_write()
 *        hand out a slot index that is only valid until release()/
 *        abort_write(); the returned index is a borrow, never a pointer to
 *        keep. Exactly one consumer may hold a given slot. */
template <typename T, std::size_t N, bool kFreeList = false>
class ObjectPool : private detail::ObjectPoolFreeList<N, kFreeList> {
 public:
  using value_type = T;
  static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
  static_assert(N < kInvalidIndex, "N must fit the uint32 slot index");

  // Each slot lives in its own 64-byte-aligned cacheline cell: contract
  // payload types are natural-layout (byte-compatible with RTE/SDK types),
  // so cacheline isolation / false-sharing protection is the container's
  // job, not the type's. Cell stride is round_up(sizeof(T), 64).
  struct alignas(64) Cell {
    T value;
  };
  static_assert(alignof(T) <= 64, "ObjectPool cells provide 64-byte alignment");

  // Slots are heap-allocated: T can be a multi-MB message type, so
  // keeping the backing array inline would bloat every ObjectPool-containing
  // value type (Pipeline / RoleLocals) to ~9 MB and overflow the main-thread
  // stack at entry. Per-process pools are only ever referenced by pointer, so
  // this is invisible to consumers. Never copied/moved (slots_ is a raw array).
  ObjectPool() : slots_(new Cell[N]()) { InitializeFreeList(); }
  ~ObjectPool() { delete[] slots_; }
  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;
  ObjectPool(ObjectPool&&) = delete;
  ObjectPool& operator=(ObjectPool&&) = delete;

  uint32_t acquire() {
    if constexpr (kFreeList) {
      uint32_t idx = kInvalidIndex;
      if (!FreeListPop(&idx)) return kInvalidIndex;
      // The popped slot is exclusively ours; no CAS is needed to move it out
      // of FREE. The release CAS in release()/abort_write() is what makes the
      // slot visible on the free list, and pop's acquire orders its payload.
      state_[idx].store(SlotState::kWriting, std::memory_order_relaxed);
      generation_[idx].fetch_add(1, std::memory_order_acq_rel);
      return idx;
    } else {
      // cursor_ is only a fairness hint: relaxed access, every acquire() call
      // still scans all N slots and claims one via CAS.
      const std::size_t base = cursor_.load(std::memory_order_relaxed);
      for (std::size_t i = 0; i < N; ++i) {
        const std::size_t idx = (base + i) % N;
        SlotState expected = SlotState::kFree;
        if (state_[idx].compare_exchange_strong(
                expected, SlotState::kWriting, std::memory_order_acquire,
                std::memory_order_relaxed)) {
          generation_[idx].fetch_add(1, std::memory_order_acq_rel);
          cursor_.store((idx + 1) % N, std::memory_order_relaxed);
          return static_cast<uint32_t>(idx);
        }
      }
      return kInvalidIndex;
    }
  }

  void publish(uint32_t idx) {
    state_[idx].store(SlotState::kReady, std::memory_order_release);
  }
  void abort_write(uint32_t idx) {
    SlotState expected = SlotState::kWriting;
    if (state_[idx].compare_exchange_strong(expected, SlotState::kFree,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
      if constexpr (kFreeList) FreeListPush(idx);
    }
  }
  uint32_t generation(uint32_t idx) const {
    return generation_[idx].load(std::memory_order_acquire);
  }
  bool release(uint32_t idx, uint32_t gen) {
    for (;;) {
      const uint32_t g = generation_[idx].load(std::memory_order_acquire);
      if (g != gen) return false;
      SlotState expected = SlotState::kReady;
      if (state_[idx].compare_exchange_weak(expected, SlotState::kFree,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
        if constexpr (kFreeList) FreeListPush(idx);
        return true;
      }
      if (expected != SlotState::kReady) return false;
    }
  }
  T& at(uint32_t idx) { return slots_[idx].value; }
  const T& at(uint32_t idx) const { return slots_[idx].value; }

 private:
  friend struct detail::ObjectPoolTestAccess<T, N, kFreeList>;

  void InitializeFreeList() {
    if constexpr (kFreeList) {
      // Push in reverse so acquire() pops low indices first (cosmetic only;
      // hand-out order is not part of the contract).
      for (std::size_t i = N; i-- > 0;) FreeListPush(static_cast<uint32_t>(i));
    }
  }

  static constexpr uint32_t FreeListTag(uint64_t head) {
    return static_cast<uint32_t>(head >> 32);
  }

  void FreeListPush(uint32_t idx) {
    uint64_t old = this->head_.load(std::memory_order_relaxed);
    for (;;) {
      // The tag is recomputed from the current head on every retry: it must
      // be strictly newer than the value we replace, otherwise a stale pop
      // that observed the old {idx, tag} could alias this push.
      const uint64_t desired =
          (static_cast<uint64_t>(FreeListTag(old) + 1u) << 32) |
          static_cast<uint64_t>(idx);
      this->next_[idx].store(static_cast<uint32_t>(old),
                             std::memory_order_relaxed);
      if (this->head_.compare_exchange_weak(old, desired,
                                            std::memory_order_release,
                                            std::memory_order_relaxed)) {
        return;
      }
    }
  }

  bool FreeListPop(uint32_t* out) {
    uint64_t old = this->head_.load(std::memory_order_acquire);
    for (;;) {
      const uint32_t idx = static_cast<uint32_t>(old);
      if (idx == kInvalidIndex) return false;
      // Safe to read before the CAS: next_[idx] is only modified by the owner
      // of idx. If another thread pops idx first, its later push changes the
      // head tag and this CAS fails; the value read here is then discarded.
      const uint32_t next = this->next_[idx].load(std::memory_order_relaxed);
      const uint64_t desired =
          (static_cast<uint64_t>(FreeListTag(old)) << 32) |
          static_cast<uint64_t>(next);
      // Failure also uses acquire: when the retry's `old` comes from the
      // failed CAS, it must synchronize with the release-push that wrote that
      // head so the next loop's relaxed next_[idx] read sees the successor.
      if (this->head_.compare_exchange_weak(old, desired,
                                            std::memory_order_acquire,
                                            std::memory_order_acquire)) {
        *out = idx;
        return true;
      }
    }
  }

  Cell* slots_;
  std::atomic<SlotState> state_[N]{};
  std::atomic<uint32_t> generation_[N]{};
  std::atomic<std::size_t> cursor_{0};
};

namespace detail {
// RT-04 test seam: set a slot's generation to reach the u32 boundary without
// 2^32 real acquire cycles. Only the test binary instantiates this; it touches
// private state and must never be used by product code.
template <typename T, std::size_t N, bool kFreeList>
struct ObjectPoolTestAccess {
  using Pool = ObjectPool<T, N, kFreeList>;

  static void SetGeneration(Pool* pool, uint32_t idx, uint32_t gen) {
    pool->generation_[idx].store(gen, std::memory_order_relaxed);
  }
};
}  // namespace detail

}  // namespace thyme
