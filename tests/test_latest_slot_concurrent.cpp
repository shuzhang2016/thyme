#include "thyme/check.h"
#include "thyme/runtime/latest_slot.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

using namespace thyme;

// This file is also compiled as test_latest_slot_concurrent_tsan with
// -fsanitize=thread when THYME_ENABLE_TSAN=ON (see tests/CMakeLists.txt).
// The reader may copy a slot the writer is about to overwrite; a torn copy is
// caught by the seq_ re-check and retried. LatestSlot accesses the payload
// bytes atomically on both sides (RT-19), so the overlap is race-free -- no
// TSan annotations are involved. This test must stay green under TSan.


struct Pod {
  uint32_t v;
};

int main() {
  LatestSlot<Pod> slot;
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> last_written{0};
  std::atomic<uint64_t> read_ok{0};
  std::atomic<uint64_t> read_fail{0};

  std::thread writer([&] {
    for (uint32_t i = 1; !stop.load(std::memory_order_relaxed); ++i) {
      slot.Write(Pod{i});
      last_written.store(i, std::memory_order_release);
    }
  });

  std::thread reader([&] {
    Pod out{};
    uint64_t seen = 0;
    uint32_t max_seen = 0;
    while (!stop.load(std::memory_order_acquire)) {
      if (slot.ReadIfNew(seen, out)) {
        CHECK(out.v > 0);
        if (out.v < max_seen) {
          CHECK(false);  // latest-value: reader never goes backwards
        }
        max_seen = out.v;
        read_ok.fetch_add(1, std::memory_order_relaxed);
      } else if (slot.Read(out)) {
        CHECK(out.v > 0);
        read_ok.fetch_add(1, std::memory_order_relaxed);
      } else {
        read_fail.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true, std::memory_order_release);
  writer.join();
  reader.join();

  CHECK(last_written.load(std::memory_order_acquire) > 1000);
  CHECK(read_ok.load(std::memory_order_relaxed) > 100);
  return 0;
}
