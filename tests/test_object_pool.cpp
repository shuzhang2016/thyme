#include "thyme/check.h"
#include "thyme/runtime/object_pool.h"

using namespace thyme;


struct alignas(64) Cell { uint32_t x; };

int main() {
  ObjectPool<Cell, 4> pool;
  const uint32_t a = pool.acquire();
  CHECK((a != ObjectPool<Cell, 4>::kInvalidIndex));
  const uint32_t a_gen = pool.generation(a);
  pool.at(a).x = 42;
  pool.publish(a);
  CHECK(pool.at(a).x == 42);
  CHECK(pool.release(a, a_gen));
  uint32_t ids[4];
  uint32_t gens[4];
  for (int i = 0; i < 4; ++i) {
    ids[i] = pool.acquire();
    CHECK((ids[i] != ObjectPool<Cell, 4>::kInvalidIndex));
    gens[i] = pool.generation(ids[i]);
  }
  CHECK((pool.acquire() == ObjectPool<Cell, 4>::kInvalidIndex));
  pool.publish(ids[0]);
  CHECK(pool.release(ids[0], gens[0]));
  CHECK(!pool.release(ids[0], gens[0]));
  CHECK((pool.acquire() != ObjectPool<Cell, 4>::kInvalidIndex));

  const uint32_t stale_idx = ids[0];
  const uint32_t stale_gen = gens[0];
  pool.publish(stale_idx);
  const uint32_t new_gen = pool.generation(stale_idx);
  CHECK(new_gen != stale_gen);
  CHECK(!pool.release(stale_idx, stale_gen));
  CHECK(!pool.release(stale_idx, stale_gen));
  CHECK(pool.release(stale_idx, new_gen));

  ObjectPool<Cell, 1> reuse;
  const uint32_t r0 = reuse.acquire();
  CHECK((r0 == 0));
  const uint32_t g_old = reuse.generation(r0);
  reuse.publish(r0);
  CHECK(reuse.release(r0, g_old));
  const uint32_t r1 = reuse.acquire();
  CHECK((r1 == 0));
  const uint32_t g_new = reuse.generation(r1);
  CHECK(g_new != g_old);
  reuse.publish(r1);
  CHECK(!reuse.release(r0, g_old));
  CHECK(reuse.release(r1, g_new));

  ObjectPool<Cell, 4> rollback;
  uint32_t rb[4];
  uint32_t rb_gen[4];
  for (int i = 0; i < 4; ++i) {
    rb[i] = rollback.acquire();
    CHECK((rb[i] != ObjectPool<Cell, 4>::kInvalidIndex));
    rb_gen[i] = rollback.generation(rb[i]);
  }
  CHECK((rollback.acquire() == ObjectPool<Cell, 4>::kInvalidIndex));
  CHECK(!rollback.release(rb[0], rb_gen[0]));
  CHECK(!rollback.release(rb[1], rb_gen[1]));
  CHECK((rollback.acquire() == ObjectPool<Cell, 4>::kInvalidIndex));
  rollback.abort_write(rb[0]);
  rollback.abort_write(rb[1]);
  rollback.abort_write(rb[2]);
  rollback.abort_write(rb[3]);
  for (int i = 0; i < 4; ++i) {
    CHECK((rollback.acquire() != ObjectPool<Cell, 4>::kInvalidIndex));
  }
  return 0;
}
