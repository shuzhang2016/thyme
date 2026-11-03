// Index-handoff queue: pool slots + (index, generation) ring, no payload copy.
#include "thyme/check.h"
#include "thyme/runtime/local_index_queue.h"

#include <cstdint>
#include <cstdio>

namespace {

struct Msg {
  uint64_t seq = 0;
  uint64_t mirror = 0;
};

// Pool deeper than the ring, so ring-full and pool-empty are distinct cases.
using Pool = thyme::ObjectPool<Msg, 8>;
using Queue = thyme::LocalIndexQueue<Pool, 4>;

}  // namespace

int main() {
  Pool pool;
  Queue::Ring ring;
  Queue queue(&pool, &ring);
  CHECK(queue.wired());
  CHECK(queue.empty());

  // Unbound queue reports failure instead of touching memory.
  Queue unbound;
  CHECK(!unbound.wired());
  auto none = unbound.BeginWrite();
  CHECK(!none.ok());
  Queue::Read unbound_read;
  CHECK(!unbound.TryRead(unbound_read));

  // In-place publish hands out the pool slot and moves only the index.
  auto write = queue.BeginWrite();
  CHECK(write.ok());
  write.slot->seq = 7;
  write.slot->mirror = ~uint64_t{7};
  CHECK(queue.CommitWrite(write));
  CHECK(!write.ok());
  CHECK(!queue.empty());
  CHECK(queue.Depth() == 1);

  Queue::Read read;
  CHECK(queue.TryRead(read));
  CHECK(read.ok());
  CHECK(read.slot->seq == 7 && read.slot->mirror == ~uint64_t{7});
  queue.Release(read);
  CHECK(!read.ok());
  CHECK(queue.empty());

  // Abort returns the slot without publishing an index.
  auto aborted = queue.BeginWrite();
  CHECK(aborted.ok());
  aborted.slot->seq = 99;
  queue.AbortWrite(aborted);
  CHECK(queue.empty());
  Queue::Read stale;
  CHECK(!queue.TryRead(stale));

  // Ring full: the commit fails and returns the slot, so a retry works after
  // the consumer drains one entry.
  for (int i = 0; i < 4; ++i) {
    auto w = queue.BeginWrite();
    CHECK(w.ok());
    w.slot->seq = static_cast<uint64_t>(i);
    CHECK(queue.CommitWrite(w));
  }
  auto overflow = queue.BeginWrite();
  CHECK(overflow.ok());
  CHECK(!queue.CommitWrite(overflow));
  CHECK(!overflow.ok());

  Queue::Read drain;
  CHECK(queue.TryRead(drain));
  queue.Release(drain);
  auto retry = queue.BeginWrite();
  CHECK(retry.ok());
  retry.slot->seq = 42;
  CHECK(queue.CommitWrite(retry));
  for (int i = 0; i < 4; ++i) {
    Queue::Read r;
    CHECK(queue.TryRead(r));
    queue.Release(r);
  }
  CHECK(queue.empty());

  // Pool exhaustion is a separate failure from a full ring: hold every slot
  // without committing, then BeginWrite must stop handing them out.
  Pool small_pool;
  Queue::Ring small_ring;
  Queue small(&small_pool, &small_ring);
  Queue::Write held[8];
  for (auto& w : held) {
    w = small.BeginWrite();
    CHECK(w.ok());
  }
  CHECK(!small.BeginWrite().ok());
  for (auto& w : held) small.AbortWrite(w);
  CHECK(small.BeginWrite().ok());

  std::printf("test_local_index_queue ok\n");
  return 0;
}
