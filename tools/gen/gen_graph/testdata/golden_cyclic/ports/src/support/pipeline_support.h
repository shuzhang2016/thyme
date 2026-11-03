#pragma once
#include "generated/graph_types.h"
#include <ctime>
#include <cstdint>

inline uint64_t NowMonoNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

template <typename RingT, typename PoolT>
inline bool DrainKeepLast(RingT& ring, PoolT& pool, uint32_t& out_idx,
                          uint32_t& out_gen) {
  bool got = false;
  uint32_t idx = 0, gen = 0, prev = 0, prev_gen = 0;
  while (ring.try_pop(idx, gen)) {
    if (got) pool.release(prev, prev_gen);
    prev = idx;
    prev_gen = gen;
    got = true;
  }
  if (got) {
    out_idx = prev;
    out_gen = prev_gen;
  }
  return got;
}

template <typename PipeT, typename PoolT>
inline void PushLatestReleaseDrop(PipeT& pipe, PoolT& pool, uint32_t idx) {
  const uint32_t gen = pool.generation(idx);
  uint32_t dropped_gen = 0;
  const uint32_t dropped = pipe.push_latest(idx, gen, &dropped_gen);
  if (dropped != 0xFFFFFFFFu) {
    (void)pool.release(dropped, dropped_gen);
  }
}
