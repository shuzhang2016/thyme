// RT-04: the opt-in O(1) free-list acquire path must be contract-equivalent
// to the default cursor+scan path. The tests below run the same deterministic
// operation schedules against a reference model for both variants:
//   * hand-out set: acquiring every slot yields each index exactly once and
//     never one that is already in use;
//   * generation protocol: acquire increments by one, publish/abort/release
//     transitions match the model, stale and double releases are rejected;
//   * double-free: releasing a slot twice must not put it on the free list
//     twice (a duplicate entry would hand it out while still owned).
#include "thyme/check.h"
#include "thyme/runtime/object_pool.h"

#include "fuzz_harness.h"

#include <cstdint>

using namespace thyme;
using thyme_fuzz::SplitMix64;

namespace {

struct alignas(64) Cell {
  uint64_t x;
};

enum ModelState : uint8_t { kFreeM = 0, kWritingM = 1, kReadyM = 2 };

template <bool kFreeList>
using Pool = ObjectPool<Cell, 8, kFreeList>;

// Acquire all slots, check distinctness and that the hand-out set is exactly
// the index space; recycle and repeat.
template <bool kFreeList>
void RunFullHandOut() {
  constexpr uint32_t kN = 8;
  Pool<kFreeList> pool;
  bool seen[kN] = {};

  uint32_t idx[kN];
  uint32_t gen[kN];
  uint32_t gen_by_idx[kN] = {};
  for (uint32_t i = 0; i < kN; ++i) {
    idx[i] = pool.acquire();
    CHECK(idx[i] != Pool<kFreeList>::kInvalidIndex);
    CHECK(!seen[idx[i]]);  // no double hand-out
    seen[idx[i]] = true;
    gen[i] = pool.generation(idx[i]);
    gen_by_idx[idx[i]] = gen[i];
  }
  for (uint32_t i = 0; i < kN; ++i) CHECK(seen[i]);
  CHECK(pool.acquire() == Pool<kFreeList>::kInvalidIndex);

  for (uint32_t i = 0; i < kN; ++i) {
    CHECK(!pool.release(idx[i], gen[i]));  // still WRITING
    pool.publish(idx[i]);
    CHECK(pool.release(idx[i], gen[i]));
    CHECK(!pool.release(idx[i], gen[i]));  // double release
  }

  uint32_t again_idx[kN];
  bool seen_again[kN] = {};
  for (uint32_t i = 0; i < kN; ++i) {
    const uint32_t again = pool.acquire();
    CHECK(again != Pool<kFreeList>::kInvalidIndex);
    CHECK(!seen_again[again]);
    seen_again[again] = true;
    // Every slot was reused exactly once since its initial acquire.
    CHECK(pool.generation(again) == gen_by_idx[again] + 1u);
    again_idx[i] = again;
  }
  CHECK(pool.acquire() == Pool<kFreeList>::kInvalidIndex);
  for (uint32_t i = 0; i < kN; ++i) CHECK(seen_again[i]);
  for (uint32_t i = 0; i < kN; ++i) pool.abort_write(again_idx[i]);
}

// Deterministic schedule with a reference model, mirroring TST-04's
// ObjectPoolSchedule but parameterized over the acquire strategy.
template <bool kFreeList>
void RunSchedule(uint64_t seed) {
  constexpr uint32_t kN = 8;
  Pool<kFreeList> pool;
  ModelState state[kN];
  uint32_t gen[kN];
  for (uint32_t i = 0; i < kN; ++i) {
    state[i] = kFreeM;
    gen[i] = 0;
  }
  SplitMix64 rng(seed);

  for (int step = 0; step < 4000; ++step) {
    const uint64_t op = rng.Below(100);
    uint32_t free_count = 0;
    for (uint32_t i = 0; i < kN; ++i) {
      if (state[i] == kFreeM) ++free_count;
    }

    if (op < 35) {
      const uint32_t idx = pool.acquire();
      if (free_count == 0) {
        CHECK(idx == Pool<kFreeList>::kInvalidIndex);
      } else {
        CHECK(idx != Pool<kFreeList>::kInvalidIndex);
        CHECK(idx < kN);
        CHECK(state[idx] == kFreeM);  // never a slot already in use
        const uint32_t next_gen = gen[idx] + 1u;
        CHECK(pool.generation(idx) == next_gen);
        gen[idx] = next_gen;
        state[idx] = kWritingM;
        pool.at(idx).x = next_gen;
        if (rng.Below(100) < 70) {
          pool.publish(idx);
          state[idx] = kReadyM;
        }
      }
    } else if (op < 50) {
      const uint32_t idx = static_cast<uint32_t>(rng.Below(kN));
      if (state[idx] == kWritingM) {
        pool.publish(idx);
        state[idx] = kReadyM;
      }
    } else if (op < 65) {
      const uint32_t idx = static_cast<uint32_t>(rng.Below(kN));
      if (state[idx] == kWritingM) {
        pool.abort_write(idx);
        state[idx] = kFreeM;  // generation is not rolled back
        CHECK(pool.generation(idx) == gen[idx]);
      }
    } else if (op < 85) {
      const uint32_t idx = static_cast<uint32_t>(rng.Below(kN));
      if (state[idx] == kReadyM) {
        CHECK(pool.release(idx, gen[idx]));
        state[idx] = kFreeM;
      }
    } else if (op < 95) {
      const uint32_t idx = static_cast<uint32_t>(rng.Below(kN));
      if (state[idx] == kReadyM) {
        CHECK(!pool.release(idx, gen[idx] - 1u));  // stale generation
        CHECK(pool.generation(idx) == gen[idx]);
        CHECK(state[idx] == kReadyM);
      }
    } else {
      const uint32_t idx = static_cast<uint32_t>(rng.Below(kN));
      if (state[idx] == kFreeM) CHECK(!pool.release(idx, gen[idx]));
    }
  }

  for (uint32_t i = 0; i < kN; ++i) {
    if (state[i] == kWritingM) {
      pool.abort_write(i);
      state[i] = kFreeM;
    } else if (state[i] == kReadyM) {
      CHECK(pool.release(i, gen[i]));
      state[i] = kFreeM;
    }
  }
  // All slots must be back on the free list: acquire each exactly once, then
  // recycle them together (aborting inside the loop would make the slot
  // immediately acquirable again).
  uint32_t held[kN];
  bool seen[kN] = {};
  for (uint32_t i = 0; i < kN; ++i) {
    const uint32_t idx = pool.acquire();
    CHECK(idx != Pool<kFreeList>::kInvalidIndex);
    CHECK(!seen[idx]);
    seen[idx] = true;
    held[i] = idx;
  }
  CHECK(pool.acquire() == Pool<kFreeList>::kInvalidIndex);
  for (uint32_t i = 0; i < kN; ++i) CHECK(seen[i]);
  for (uint32_t i = 0; i < kN; ++i) pool.abort_write(held[i]);
}

// A double release must not duplicate the free-list entry. With two slots,
// after releasing slot A twice, the two acquires must return A and B (in some
// order), not A twice.
template <bool kFreeList>
void RunDoubleFree() {
  ObjectPool<Cell, 2, kFreeList> pool;
  const uint32_t a = pool.acquire();
  const uint32_t b = pool.acquire();
  CHECK(a != b);
  const uint32_t a_gen = pool.generation(a);
  const uint32_t b_gen = pool.generation(b);
  pool.publish(a);
  pool.publish(b);
  CHECK(pool.release(a, a_gen));
  CHECK(!pool.release(a, a_gen));        // double free
  CHECK(!pool.release(b, a_gen + 1u));   // wrong generation
  CHECK(pool.release(b, b_gen));

  const uint32_t first = pool.acquire();
  const uint32_t second = pool.acquire();
  CHECK(first != second);
  CHECK((pool.acquire() ==
         ObjectPool<Cell, 2, kFreeList>::kInvalidIndex));
  CHECK((first == a && second == b) || (first == b && second == a));
  pool.abort_write(first);
  pool.abort_write(second);
}

}  // namespace

int main() {
  RunFullHandOut<false>();
  RunFullHandOut<true>();
  RunSchedule<false>(0x504F4F4C303401ull);  // "POOL04"
  RunSchedule<true>(0x504F4F4C303401ull);
  RunDoubleFree<false>();
  RunDoubleFree<true>();
  return 0;
}
