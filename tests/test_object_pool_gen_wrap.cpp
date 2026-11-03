// RT-04: ObjectPool::generation is uint32 and increments once per acquire().
// No feasible run can perform 2^32 acquisitions of the same slot, so the
// boundary states are reached through the documented test seam
// (detail::ObjectPoolTestAccess) and the full lifecycle is exercised there.
//
// The assertions answer two questions:
//   * does the unsigned wrap break the stale-release guard? (no: release
//     compares full 32-bit generations, and a pre-wrap generation no longer
//     matches 0 after 0xFFFFFFFF + 1)
//   * can acquire() hand out an in-use slot while generations wrap? (no: the
//     FREE -> WRITING gate is the state CAS/LIFO pop, independent of the
//     generation value).
//
// The same checks run for the default scan path and the opt-in free-list
// path so the two acquire strategies cannot drift on generation semantics.
#include "thyme/check.h"
#include "thyme/runtime/object_pool.h"

#include <cstdint>
#include <type_traits>
#include <utility>

using namespace thyme;

namespace {

struct alignas(64) Cell {
  uint64_t x;
};

using Gen = decltype(std::declval<const ObjectPool<Cell, 1>&>().generation(0u));
static_assert(std::is_same<Gen, uint32_t>::value,
              "RT-04 assumes a 32-bit generation; widen the wrap math and docs "
              "together if this changes");

// One-slot pool so every acquire() necessarily returns slot 0 on both
// strategies (the scan path's cursor is irrelevant).
template <bool kFreeList>
void RunWrapCycle() {
  using Pool = ObjectPool<Cell, 1, kFreeList>;
  using Access = detail::ObjectPoolTestAccess<Cell, 1, kFreeList>;

  Pool pool;
  const uint32_t first = pool.acquire();
  CHECK(first != Pool::kInvalidIndex);
  pool.abort_write(first);

  Access::SetGeneration(&pool, first, 0xFFFFFFFEu);
  const uint32_t second = pool.acquire();
  CHECK(second == first);
  CHECK(pool.generation(second) == 0xFFFFFFFFu);
  pool.publish(second);
  // The pre-acquire generation is stale even though the slot is READY.
  CHECK(!pool.release(second, 0xFFFFFFFEu));
  CHECK(pool.generation(second) == 0xFFFFFFFFu);
  CHECK(pool.release(second, 0xFFFFFFFFu));

  const uint32_t third = pool.acquire();
  CHECK(third == first);
  CHECK(pool.generation(third) == 0u);  // 0xFFFFFFFF + 1 wraps to 0
  pool.publish(third);
  // The immediately preceding generation is rejected across the wrap.
  CHECK(!pool.release(third, 0xFFFFFFFFu));
  CHECK(pool.generation(third) == 0u);
  CHECK(pool.release(third, 0u));
}

// ---- One slot stays held while another is recycled across the wrap ----
template <bool kFreeList>
void RunWrapWithHeldSlot() {
  using Pool = ObjectPool<Cell, 2, kFreeList>;
  using Access = detail::ObjectPoolTestAccess<Cell, 2, kFreeList>;

  {
    Pool pool;
    const uint32_t held = pool.acquire();
    const uint32_t recycled = pool.acquire();
    CHECK(held != recycled);
    CHECK(pool.acquire() == Pool::kInvalidIndex);

    Access::SetGeneration(&pool, recycled, 0xFFFFFFFFu);
    pool.publish(recycled);
    CHECK(!pool.release(recycled, 0xFFFFFFFEu));
    CHECK(pool.acquire() == Pool::kInvalidIndex);  // held + ready, none free
    CHECK(pool.release(recycled, 0xFFFFFFFFu));

    const uint32_t wrapped = pool.acquire();
    CHECK(wrapped == recycled);   // the only free slot
    CHECK(wrapped != held);       // never hands out the held slot
    CHECK(pool.generation(wrapped) == 0u);
    CHECK(pool.acquire() == Pool::kInvalidIndex);

    // abort_write across the wrap recycles without publishing; the generation
    // is not rolled back (documented contract).
    pool.abort_write(wrapped);
    const uint32_t after_abort = pool.acquire();
    CHECK(after_abort == recycled);
    CHECK(pool.generation(after_abort) == 1u);
    pool.abort_write(after_abort);
    pool.abort_write(held);

    const uint32_t a = pool.acquire();
    const uint32_t b = pool.acquire();
    CHECK(a != b);
    CHECK(pool.acquire() == Pool::kInvalidIndex);
    pool.abort_write(a);
    pool.abort_write(b);
  }
}

// ABA rejection paths shared by both acquire strategies. These are the
// single-owner cases: a slot that was never published, one abandoned via
// abort_write, and a stale/double release.
template <bool kFreeList>
void RunAbaRejections() {
  using Pool = ObjectPool<Cell, 1, kFreeList>;

  Pool pool;
  const uint32_t idx = pool.acquire();
  const uint32_t gen = pool.generation(idx);

  // Still WRITING: a release without publish must be rejected.
  CHECK(!pool.release(idx, gen));
  CHECK(pool.generation(idx) == gen);

  // Release after abandon (abort_write): the state gate rejects it even
  // though the generation still matches.
  pool.abort_write(idx);
  CHECK(!pool.release(idx, gen));
  CHECK(pool.generation(idx) == gen);

  // A new owner increments the generation; the old owner's release is stale.
  const uint32_t again = pool.acquire();
  CHECK(again == idx);
  CHECK(pool.generation(again) == gen + 1u);
  pool.publish(again);
  CHECK(!pool.release(idx, gen));  // stale generation
  CHECK(pool.generation(again) == gen + 1u);
  CHECK(pool.release(again, gen + 1u));
  CHECK(!pool.release(again, gen + 1u));  // double release
}

}  // namespace

int main() {
  RunWrapCycle<false>();
  RunWrapCycle<true>();
  RunWrapWithHeldSlot<false>();
  RunWrapWithHeldSlot<true>();
  RunAbaRejections<false>();
  RunAbaRejections<true>();
  return 0;
}
