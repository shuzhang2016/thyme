// SharedHealthPage multi-worker concurrent read/write stress. Workers each own
// a slot and update it continuously; readers repeatedly Validate() the page
// and sample worker slots. Per-worker counters are monotone, so a reader's
// consecutive observations of the same slot must never go backwards. A
// cross-process leg puts the page in POSIX SHM and forks two children that
// hammer their own slots while the parent validates and reads.
#include "thyme/check.h"
#include "thyme/runtime/shared_health.h"
#include "thyme/runtime/shm_region.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

using namespace thyme;

namespace {
constexpr uint32_t kWorkers = 6;
constexpr uint32_t kSession = 4242;
}  // namespace

int main() {
  SharedHealthPage page;
  CHECK(page.Init(kSession, kWorkers));
  CHECK(page.Validate());
  CHECK(page.worker_count() == kWorkers);
  CHECK(page.session_id() == kSession);

  // ---- Part A: in-process worker/reader threads ----
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> monotone_violations{0};
  {
    std::vector<std::thread> workers;
    for (uint32_t w = 0; w < kWorkers; ++w) {
      workers.emplace_back([&, w] {
        HealthSlot& s = page.slots[w];
        uint64_t t = 1;
        while (!stop.load(std::memory_order_relaxed)) {
          s.MarkAlive(t);  // bump last_finish_mono_ns then alive_counter
          s.state.store(static_cast<uint32_t>(t % 4), std::memory_order_relaxed);
          s.output_sequence.fetch_add(1, std::memory_order_release);
          ++t;
        }
      });
    }

    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r) {
      readers.emplace_back([&] {
        uint64_t prev[kWorkers] = {};
        while (!stop.load(std::memory_order_acquire)) {
          if (!page.Validate()) {
            CHECK(false);  // header is immutable after Init; never invalid
          }
          for (uint32_t w = 0; w < kWorkers; ++w) {
            const uint64_t alive =
                page.slots[w].alive_counter.load(std::memory_order_acquire);
            if (alive < prev[w]) {
              monotone_violations.fetch_add(1, std::memory_order_relaxed);
            }
            prev[w] = alive;
          }
        }
      });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop.store(true, std::memory_order_release);
    for (auto& t : workers) t.join();
    for (auto& t : readers) t.join();

    for (uint32_t w = 0; w < kWorkers; ++w) {
      CHECK(page.slots[w].alive_counter.load(std::memory_order_acquire) > 10000);
      CHECK(page.slots[w].output_sequence.load(std::memory_order_acquire) >
            10000);
    }
    CHECK(monotone_violations.load(std::memory_order_relaxed) == 0);
  }

  // ---- Part B: cross-process writers hammer their own slots ----
  // The page must live in POSIX SHM: a fork() COW copy is private per process,
  // so children writing a stack-allocated page would never reach the parent.
  // The stop flag must live in SHM for the same reason -- a stack atomic is
  // COW-copied and the children would never observe the parent's store.
  {
    char name[32];
    std::snprintf(name, sizeof(name), "/c9shs_%d", static_cast<int>(getpid()));
    char stop_name[40];
    std::snprintf(stop_name, sizeof(stop_name), "/c9shs_stop_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(stop_name);

    ShmRegion region;
    CHECK(region.Create(name, sizeof(SharedHealthPage)));
    auto* shm_page = static_cast<SharedHealthPage*>(region.data());
    CHECK(shm_page->Init(kSession, kWorkers));
    CHECK(shm_page->Validate());

    ShmRegion stop_region;
    auto* stop_flag = stop_region.Create<std::atomic<bool>>(
        stop_name, sizeof(std::atomic<bool>), false);
    CHECK(stop_flag != nullptr);

    pid_t p1 = fork();
    CHECK(p1 >= 0);
    if (p1 == 0) {
      HealthSlot& s = shm_page->slots[0];
      while (!stop_flag->load(std::memory_order_relaxed)) {
        s.MarkAlive(1);
        s.output_sequence.fetch_add(1, std::memory_order_relaxed);
      }
      _exit(0);
    }
    pid_t p2 = fork();
    CHECK(p2 >= 0);
    if (p2 == 0) {
      HealthSlot& s = shm_page->slots[1];
      while (!stop_flag->load(std::memory_order_relaxed)) {
        s.MarkAlive(1);
        s.output_sequence.fetch_add(1, std::memory_order_relaxed);
      }
      _exit(0);
    }

    const uint64_t a0_before =
        shm_page->slots[0].alive_counter.load(std::memory_order_acquire);
    const uint64_t a1_before =
        shm_page->slots[1].alive_counter.load(std::memory_order_acquire);
    uint64_t samples = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < deadline) {
      CHECK(shm_page->Validate());
      (void)shm_page->slots[0].output_sequence.load(std::memory_order_acquire);
      (void)shm_page->slots[1].output_sequence.load(std::memory_order_acquire);
      ++samples;
    }

    stop_flag->store(true, std::memory_order_release);
    int st = 0;
    CHECK(waitpid(p1, &st, 0) == p1);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(waitpid(p2, &st, 0) == p2);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    CHECK(shm_page->slots[0].alive_counter.load(std::memory_order_acquire) >
          a0_before);
    CHECK(shm_page->slots[1].alive_counter.load(std::memory_order_acquire) >
          a1_before);
    CHECK(samples > 0);

    region.Unmap();
    stop_region.Unmap();
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(stop_name);
  }

  return 0;
}
