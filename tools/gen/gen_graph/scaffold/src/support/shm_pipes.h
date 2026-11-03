#pragma once
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/write_section.h"
#include "support/index_pipe.h"
#include <atomic>
#include <cstdint>

template <typename T, typename PoolT>
struct ShmLatestSink final : IndexPipe {
  ShmLatestSink(thyme::LatestSlot<T>& slot, PoolT& pool) : slot_(&slot), pool_(&pool) {}
  uint32_t push_latest(uint32_t idx, uint32_t gen,
                       uint32_t* dropped_gen = nullptr) override {
    (void)dropped_gen;
    // ARCH-12: WriteSection owns abort if copy/commit cannot finish.
    thyme::WriteSection<thyme::LatestSlot<T>> section(*slot_);
    *section = pool_->at(idx);
    section.commit();
    pool_->release(idx, gen);
    return 0xFFFFFFFFu;
  }
  bool try_pop(uint32_t&, uint32_t&) override { return false; }
  bool empty() const override { return true; }
  uint32_t drop_count() const override { return 0; }
 private:
  thyme::LatestSlot<T>* slot_;
  PoolT* pool_;
};

template <typename T, typename PoolT>
struct ShmLatestSource final : IndexPipe {
  ShmLatestSource(thyme::LatestSlot<T>& slot, PoolT& pool,
                 std::atomic<uint64_t>* frames = nullptr)
      : slot_(&slot), pool_(&pool), frames_(frames) {}
  uint32_t push_latest(uint32_t, uint32_t,
                       uint32_t* dropped_gen = nullptr) override {
    (void)dropped_gen;
    return 0xFFFFFFFFu;
  }
  bool try_pop(uint32_t& out_idx, uint32_t& out_gen) override {
    uint32_t idx = pool_->acquire();
    if (idx == PoolT::kInvalidIndex) return false;
    const uint32_t gen = pool_->generation(idx);
    uint64_t seen = seen_;
    if (!slot_->ReadIfNew(seen, pool_->at(idx))) {
      pool_->abort_write(idx);
      return false;
    }
    pool_->publish(idx);
    seen_ = seen;
    out_idx = idx;
    out_gen = gen;
    if (frames_ != nullptr) frames_->fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  bool empty() const override { return !slot_->HasNew(seen_); }
  uint32_t drop_count() const override { return 0; }
 private:
  thyme::LatestSlot<T>* slot_;
  PoolT* pool_;
  uint64_t seen_{0};
  std::atomic<uint64_t>* frames_{nullptr};
};
