// GEN-08/ARCH-11 layout fixture: pool aliases and ring template used by the
// generated shm_slots/mp_role_locals headers.
#pragma once
#include "dummy/contracts/stub_types.h"
#include "support/pipeline_io.h"
#include "thyme/runtime/object_pool.h"
#include "thyme/runtime/spsc_ring.h"

#include <cstddef>
#include <cstdint>

inline constexpr std::size_t kRingCap = {{RING_CAP}};
inline constexpr std::size_t kPoolN = {{POOL_N}};
inline constexpr uint64_t kSessionId = 1;

using ReadingPool = thyme::ObjectPool<thyme::Frame<Reading>, kPoolN>;

template <typename PoolT>
using Ring = thyme::SpscRing<kRingCap>;
