#pragma once
#include "thyme/runtime/object_pool.h"
#include "thyme/runtime/spsc_ring.h"
#include "thyme/runtime/waitable.h"

#include <atomic>
#include <cstdint>

namespace thyme {

// Process-local index-handoff queue: the pool owns payload slots, the ring
// carries packed (index, generation) words. Publish and read copy no payload —
// only the index moves — and the slot returns to the pool when the reader
// releases it (or immediately when the writer aborts).
//
// This is the single call shape for cyclic queue edges: BeginWrite fills the
// pool slot in place, CommitWrite hands the index over, TryRead/Release is the
// consumer side. It wraps ObjectPool + SpscRing instead of replacing them, so
// the generation/ABA contract stays in the pool.
template <typename Pool, std::size_t Cap>
class LocalIndexQueue : public Waitable {
 public:
  using Slot = typename Pool::value_type;
  using Ring = SpscRing<Cap>;
  static constexpr uint32_t kInvalidIndex = Pool::kInvalidIndex;

  struct Write {
    Slot* slot = nullptr;
    uint32_t index = kInvalidIndex;
    uint32_t generation = 0;
    bool ok() const { return slot != nullptr; }
  };

  struct Read {
    const Slot* slot = nullptr;
    uint32_t index = kInvalidIndex;
    uint32_t generation = 0;
    bool ok() const { return slot != nullptr; }
  };

  LocalIndexQueue() = default;
  LocalIndexQueue(Pool* pool, Ring* ring) { Bind(pool, ring); }

  void Bind(Pool* pool, Ring* ring) {
    pool_ = pool;
    ring_ = ring;
  }
  bool wired() const { return pool_ != nullptr && ring_ != nullptr; }
  // Escape hatches for tools that need the raw pair (replay injection, taps,
  // depth sampling). Data-plane code should stay on the five calls above.
  Pool* pool() const { return pool_; }
  Ring* ring() const { return ring_; }

  // Triggered acquisition sees the payload the ring points at: the tap copies
  // pool cells, so it needs the cell base and stride, not the queue.
  void SetRecordTap(RecordMirrorRingPage* page, uint16_t struct_id) {
    if (ring_ != nullptr) {
      ring_->SetRecordTap(page, struct_id,
                          pool_ != nullptr ? &pool_->at(0) : nullptr,
                          static_cast<uint32_t>(sizeof(typename Pool::Cell)));
    }
  }
  void ClearRecordTap() {
    if (ring_ != nullptr) ring_->ClearRecordTap();
  }
  bool record_tap_armed() const {
    return ring_ != nullptr && ring_->record_tap_armed();
  }

  Write BeginWrite() {
    Write w;
    if (!wired()) return w;
    const uint32_t idx = pool_->acquire();
    if (idx == kInvalidIndex) return w;
    w.index = idx;
    w.generation = pool_->generation(idx);
    w.slot = &pool_->at(idx);
    return w;
  }

  // The payload (and its Ready state) is published before the index becomes
  // visible; a full ring releases the slot and reports failure so the caller
  // decides whether to retry or drop.
  bool CommitWrite(Write& w) {
    if (!wired() || w.slot == nullptr) return false;
    pool_->publish(w.index);
    if (!ring_->try_push(w.index, w.generation)) {
      (void)pool_->release(w.index, w.generation);
      w = Write{};
      return false;
    }
    w = Write{};
    return true;
  }

  void AbortWrite(Write& w) {
    if (wired() && w.slot != nullptr) pool_->abort_write(w.index);
    w = Write{};
  }

  bool TryRead(Read& r) {
    if (!wired()) return false;
    uint32_t idx = 0;
    uint32_t gen = 0;
    if (!ring_->try_pop(idx, gen)) return false;
    // A slot can be re-acquired after a writer abort; the generation mismatch
    // means the ring entry is stale and the payload no longer belongs to it.
    if (pool_->generation(idx) != gen) return false;
    r.index = idx;
    r.generation = gen;
    r.slot = &pool_->at(idx);
    return true;
  }

  void Release(Read& r) {
    if (wired() && r.slot != nullptr) {
      (void)pool_->release(r.index, r.generation);
    }
    r = Read{};
  }

  bool empty() const override { return ring_ == nullptr || ring_->empty(); }
  void Notify() override {}
  void SetNotify(Waitable* target) override {
    if (ring_ != nullptr) ring_->SetNotify(target);
  }
  void WaitUntil(uint64_t deadline_ns) override {
    if (ring_ != nullptr) ring_->WaitUntil(deadline_ns);
  }
  std::atomic<uint32_t>* NotifyWakeWord() const override {
    return ring_ != nullptr ? ring_->NotifyWakeWord() : nullptr;
  }
  uint64_t Depth() const override { return ring_ != nullptr ? ring_->Depth() : 0; }

  // Ring-side counters. A full ring is reported through CommitWrite's false
  // return; drop_count only grows when a caller uses push_latest-style paths
  // on the same ring.
  uint32_t drop_count() const {
    return ring_ != nullptr ? ring_->drop_count() : 0;
  }
  uint32_t waiter_count() const {
    return ring_ != nullptr ? ring_->waiter_count() : 0;
  }

 private:
  Pool* pool_ = nullptr;
  Ring* ring_ = nullptr;
};

}  // namespace thyme
