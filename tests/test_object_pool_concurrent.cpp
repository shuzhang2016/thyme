#include "thyme/check.h"
#include "thyme/runtime/object_pool.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace thyme;

namespace {

struct alignas(64) Cell {
  uint64_t x;
};

// Multiple threads hammer acquire/publish/release concurrently. The pool's
// internal cursor and the free-list head must not race (checked under TSan in
// the test_object_pool_concurrent_tsan variant, which builds this file too).
// The in_use marker catches a slot being handed to two owners at once; it is
// cleared before release so the next owner never observes a stale marker.
template <bool kFreeList>
void Hammer() {
  using Pool = ObjectPool<Cell, 8, kFreeList>;

  Pool pool;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> acquired{0};
  std::atomic<uint64_t> double_handout{0};
  std::atomic<uint64_t> bad_release{0};
  std::atomic<int> in_use[8]{};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        const uint32_t idx = pool.acquire();
        if (idx == Pool::kInvalidIndex) continue;
        if (in_use[idx].fetch_add(1, std::memory_order_acq_rel) != 0) {
          double_handout.fetch_add(1, std::memory_order_relaxed);
        }
        const uint32_t gen = pool.generation(idx);
        pool.at(idx).x += 1;  // exclusive: acquire won this slot
        pool.publish(idx);
        in_use[idx].fetch_sub(1, std::memory_order_acq_rel);
        if (!pool.release(idx, gen)) {
          bad_release.fetch_add(1, std::memory_order_relaxed);
        }
        acquired.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stop.store(true, std::memory_order_relaxed);
  for (auto& th : threads) th.join();
  CHECK(acquired.load(std::memory_order_relaxed) > 0);
  CHECK(double_handout.load(std::memory_order_relaxed) == 0);
  CHECK(bad_release.load(std::memory_order_relaxed) == 0);
}

}  // namespace

int main() {
  Hammer<false>();  // cursor + scan
  Hammer<true>();   // opt-in free list
  return 0;
}
