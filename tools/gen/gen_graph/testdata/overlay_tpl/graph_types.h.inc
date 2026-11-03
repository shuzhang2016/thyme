// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
#pragma once
#include "thyme/runtime/object_pool.h"
#include "thyme/runtime/spsc_ring.h"
inline constexpr std::size_t kRingCap = {{RING_CAP}};
inline constexpr std::size_t kPoolN = {{POOL_N}};

// Overlay vocabulary: extras.txt maps mybuf -> MyCustomBuffer (read by tick).
struct MyCustomBuffer {
  uint32_t value{};
};
