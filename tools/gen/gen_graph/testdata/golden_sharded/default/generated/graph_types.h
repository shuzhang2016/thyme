// Code generated for the event fixture graph. DO NOT EDIT.
// gen_graph 3.11.1 (output format 3)
// Hand logic belongs in src/nodes/.
#pragma once
#include "dummy/contracts/stub_types.h"
#include "thyme/runtime/object_pool.h"
#include "thyme/runtime/spsc_ring.h"
inline constexpr std::size_t kRingCap = 8;
inline constexpr std::size_t kPoolN = 4;

template <typename PoolT>
using Ring = thyme::SpscRing<kRingCap>;
