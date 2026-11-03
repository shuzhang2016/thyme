// Code generated for the event fixture graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
#pragma once
#include "dummy/contracts/stub_types.h"
#include "thyme/runtime/object_pool.h"
#include "thyme/runtime/spsc_ring.h"
inline constexpr std::size_t kRingCap = {{RING_CAP}};
inline constexpr std::size_t kPoolN = {{POOL_N}};

template <typename PoolT>
using Ring = thyme::SpscRing<kRingCap>;
