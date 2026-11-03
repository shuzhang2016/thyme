// replay_mode: global replay switches plus the seqlock-guarded inject-stage
// buffer. Readers must never observe a torn stage name while the replay
// runner (the single writer) updates it.
#include "thyme/check.h"
#include "thyme/app/replay_mode.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

int main() {
  using namespace thyme::app::replay_mode;

  CHECK(!Active());
  CHECK(!FullInject());
  CHECK(InjectStage() == nullptr);

  SetActive(true);
  CHECK(Active());
  SetActive(false);
  CHECK(!Active());

  SetFullInject(true);
  CHECK(FullInject());
  SetFullInject(false);
  CHECK(!FullInject());

  // Stage set/get round-trip.
  SetInjectStage("fusion");
  CHECK(InjectStage() != nullptr);
  CHECK(std::strcmp(InjectStage(), "fusion") == 0);
  SetInjectStage("decision");
  CHECK(std::strcmp(InjectStage(), "decision") == 0);
  SetInjectStage(nullptr);
  CHECK(InjectStage() == nullptr);
  SetInjectStage("");
  CHECK(InjectStage() == nullptr);

  SetPaused(true);
  CHECK(Paused());
  SetPaused(false);
  CHECK(!Paused());

  RequestStop();
  CHECK(StopRequested());
  ClearStop();
  CHECK(!StopRequested());

  // Concurrent stress: one writer flips stage names while readers copy under
  // the seqlock; every read must yield one of the writer's names (or none),
  // never a torn mixture.
  const char* kNames[] = {"fusion", "decision", "planning", "arcomm", ""};
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    for (int i = 0; i < 20000 && !stop.load(std::memory_order_acquire); ++i) {
      SetInjectStage(kNames[i % 4]);
      SetActive((i & 1) != 0);
    }
  });
  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_acquire)) {
        const char* s = InjectStage();
        if (s == nullptr) continue;
        bool ok = false;
        for (const char* n : kNames) {
          if (std::strcmp(s, n) == 0) { ok = true; break; }
        }
        CHECK(ok);  // any other value is a torn read
      }
    });
  }
  for (int i = 0; i < 500; ++i) std::this_thread::yield();
  stop.store(true, std::memory_order_release);
  writer.join();
  for (auto& t : readers) t.join();

  // Replay pause race: producer threads spin on pause predicates while the
  // replay runner toggles Active / FullInject / InjectStage (same switches
  // ReplayProducerShouldPause reads). Must not crash or observe torn stages.
  auto should_pause_prod = []() -> bool {
    if (!Active()) return false;
    if (FullInject()) return true;  // "prod" is the only data producer here
    const char* stage = InjectStage();
    if (stage != nullptr) return std::strcmp(stage, "fusion") == 0;
    return false;
  };
  stop.store(false, std::memory_order_release);
  std::atomic<uint64_t> pause_checks{0};
  std::thread replay_writer([&] {
    for (int i = 0; i < 30000 && !stop.load(std::memory_order_acquire); ++i) {
      SetActive((i & 3) != 0);
      SetFullInject((i & 7) == 0);
      SetInjectStage(kNames[i % 4]);
    }
  });
  std::vector<std::thread> producers;
  for (int r = 0; r < 4; ++r) {
    producers.emplace_back([&] {
      while (!stop.load(std::memory_order_acquire)) {
        (void)should_pause_prod();
        pause_checks.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  // Wait until at least one producer has actually run before stopping, so the
  // assertion below is deterministic under load instead of racing a fixed
  // number of yields (producers may be starved when tests run in parallel).
  for (int i = 0; i < 1000000 &&
                  pause_checks.load(std::memory_order_relaxed) == 0; ++i) {
    std::this_thread::yield();
  }
  stop.store(true, std::memory_order_release);
  replay_writer.join();
  for (auto& t : producers) t.join();
  CHECK(pause_checks.load(std::memory_order_relaxed) > 0);

  return 0;
}
