// Code generated for the broadcast fixture graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
#pragma once
#include "dummy/contracts/stub_types.h"
#include "thyme/runtime/object_pool.h"
#include "thyme/runtime/spsc_ring.h"
inline constexpr std::size_t kRingCap = {{RING_CAP}};
inline constexpr std::size_t kPoolN = {{POOL_N}};

// Tick is the trivially copyable Frame<> payload (see stub_types.h).
static_assert(sizeof(Tick) > 0, "Tick payload required for Frame<Tick>");

template <typename PoolT>
using Ring = thyme::SpscRing<kRingCap>;
