#pragma once
#include "generated/graph_types.h"
#include <cstdint>

struct IndexPipe {
  virtual uint32_t push_latest(uint32_t idx, uint32_t gen,
                               uint32_t* dropped_gen = nullptr) = 0;
  virtual bool try_pop(uint32_t& idx, uint32_t& gen) = 0;
  virtual bool empty() const = 0;
  virtual uint32_t drop_count() const = 0;
  virtual ~IndexPipe() = default;
};

struct RingIndexPipe final : IndexPipe {
  explicit RingIndexPipe(thyme::SpscRing<kRingCap>& r) : r_(&r) {}
  uint32_t push_latest(uint32_t idx, uint32_t gen,
                       uint32_t* dropped_gen = nullptr) override {
    return r_->push_latest(idx, gen, dropped_gen);
  }
  bool try_pop(uint32_t& idx, uint32_t& gen) override {
    return r_->try_pop(idx, gen);
  }
  bool empty() const override { return r_->empty(); }
  uint32_t drop_count() const override { return r_->drop_count(); }
 private:
  thyme::SpscRing<kRingCap>* r_;
};
