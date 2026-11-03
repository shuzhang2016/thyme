// Code generated for the ports fixture graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/wiring.h"
#include "generated/record_registry.h"

#include "thyme/app/replay_mode.h"
#include "thyme/runtime/notif_pipe.h"

#include <cstring>

inline bool ReplayInjectOne(Pipeline& p, uint16_t struct_id,
                            const void* data, uint32_t size) {
  switch (struct_id) {
    case 0: {
      if (size != sizeof(thyme::Frame<Foo>)) return false;
      auto& pool = p.foo_pool;
      auto& ring = p.foo_ring;
      uint32_t idx = pool.acquire();
      if (idx == FooPool::kInvalidIndex) return false;
      std::memcpy(&pool.at(idx), data, size);
      pool.publish(idx);
      const uint32_t gen = pool.generation(idx);
      uint32_t dropped_gen = 0;
      uint32_t dropped = ring.push_latest(idx, gen, &dropped_gen);
      if (dropped != 0xFFFFFFFFu) pool.release(dropped, dropped_gen);
      return true;
    }
    default: return false;
  }
}
