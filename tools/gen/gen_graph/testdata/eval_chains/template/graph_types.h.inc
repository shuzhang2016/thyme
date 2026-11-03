#pragma once
#include "dummy/contracts/stub_types.h"
#include "thyme/runtime/object_pool.h"
#include "thyme/runtime/spsc_ring.h"

inline constexpr std::size_t kRingCap = {{RING_CAP}};
inline constexpr std::size_t kPoolN = {{POOL_N}};

template <typename PoolT>
using Ring = thyme::SpscRing<kRingCap>;

using Hop1Pool = thyme::ObjectPool<thyme::Frame<Reading>, kPoolN>;
using Hop2Pool = thyme::ObjectPool<thyme::Frame<Reading>, kPoolN>;
