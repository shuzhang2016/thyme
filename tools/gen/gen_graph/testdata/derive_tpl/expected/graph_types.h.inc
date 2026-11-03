// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
#pragma once
#include "contracts/types.h"
#include "support/pipeline_io.h"
#include "thyme/runtime/object_pool.h"
#include "thyme/runtime/spsc_ring.h"

inline constexpr std::size_t kRingCap = {{RING_CAP}};
inline constexpr std::size_t kPoolN = {{POOL_N}};
inline constexpr uint64_t kSessionId = 1;

using FooPool = thyme::ObjectPool<thyme::Frame<Foo>, kPoolN>;
using BarPool = thyme::ObjectPool<thyme::Frame<Bar>, kPoolN>;
using BazPool = thyme::ObjectPool<thyme::Frame<Baz>, kPoolN>;

template <typename PoolT>
using Ring = thyme::SpscRing<kRingCap>;
