// Process-local zero-copy latest store: large payload, one writer, N readers.
#include "thyme/check.h"
#include "thyme/runtime/pinned_latest_store.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

using namespace thyme;

namespace {

struct Payload {
  uint64_t seq = 0;
  uint64_t check = 0;
  uint8_t data[4096] = {};
};

Payload MakePayload(uint64_t seq) {
  Payload p;
  p.seq = seq;
  p.check = ~seq;
  for (std::size_t i = 0; i < sizeof(p.data); ++i) {
    p.data[i] = static_cast<uint8_t>(seq + i);
  }
  return p;
}

bool PayloadOk(const Payload& p) { return p.check == ~p.seq; }

}  // namespace

int main() {
  static_assert(!std::is_copy_constructible<
                    PinnedLatestStore<Payload, 4>::Borrowed>::value,
                "Borrowed is a move-only pin");
  static_assert(std::is_move_constructible<
                    PinnedLatestStore<Payload, 4>::Borrowed>::value,
                "Borrowed must be movable");

  PinnedLatestStore<Payload, 4> store;
  CHECK(!store.has_value());
  {
    PinnedLatestStore<Payload, 4>::Borrowed view;
    CHECK(!store.TryRead(&view));
  }

  CHECK(store.Publish(MakePayload(1)));
  {
    PinnedLatestStore<Payload, 4>::Borrowed view;
    CHECK(store.TryRead(&view));
    CHECK(view);
    CHECK(view->seq == 1 && PayloadOk(*view));
    CHECK(view.get() == &*view);
  }

  // Multiple readers can pin the same latest slot; writer uses another slot.
  {
    PinnedLatestStore<Payload, 2> two;
    CHECK(two.Publish(MakePayload(1)));
    PinnedLatestStore<Payload, 2>::Borrowed a;
    CHECK(two.TryRead(&a));
    CHECK(two.Publish(MakePayload(2)));
    PinnedLatestStore<Payload, 2>::Borrowed b;
    CHECK(two.TryRead(&b));
    CHECK(a->seq == 1 && b->seq == 2);
    // Both slots pinned: the next publish must be dropped, never overwrite.
    CHECK(!two.Publish(MakePayload(3)));
    CHECK(two.dropped_publishes() == 1);
    a.Reset();
    CHECK(two.Publish(MakePayload(3)));
    PinnedLatestStore<Payload, 2>::Borrowed c;
    CHECK(two.TryRead(&c));
    CHECK(c->seq == 3);
  }

  // One writer, several readers: no reader observes a torn payload.
  {
    PinnedLatestStore<Payload, 4> shared;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> bad{0};
    uint64_t last_published = 0;
    uint64_t dropped = 0;
    constexpr int kReaders = 4;
    std::vector<std::thread> readers;
    for (int i = 0; i < kReaders; ++i) {
      readers.emplace_back([&] {
        while (!stop.load(std::memory_order_acquire)) {
          PinnedLatestStore<Payload, 4>::Borrowed view;
          if (!shared.TryRead(&view)) continue;
          if (!PayloadOk(*view)) bad.fetch_add(1, std::memory_order_relaxed);
          reads.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
    CHECK(shared.Publish(MakePayload(1)));
    last_published = 1;
    // Condition-driven, not sleep-based: a heavily loaded host can starve the
    // readers past the whole publish loop, so wait until one read is observed
    // (bounded) instead of asserting on scheduling luck.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (reads.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    for (uint64_t seq = 2; seq <= 20000; ++seq) {
      if (shared.Publish(MakePayload(seq))) last_published = seq;
      else ++dropped;
    }
    stop.store(true, std::memory_order_release);
    for (auto& t : readers) t.join();
    CHECK(bad.load() == 0);
    CHECK(reads.load() > 0);
    // Under load a reader can stay pinned across a scheduling gap, so slots may
    // legitimately run out and the publish is dropped. That is the documented
    // behaviour, not a torn read: every drop must be accounted for and the last
    // available value must be the last publish that succeeded.
    CHECK(last_published > 0);
    CHECK(dropped == shared.dropped_publishes());
    PinnedLatestStore<Payload, 4>::Borrowed final;
    CHECK(shared.TryRead(&final));
    CHECK(final->seq == last_published && PayloadOk(*final));
  }

  // Abort must not publish a half-written payload.
  {
    PinnedLatestStore<Payload, 2> aborted;
    CHECK(aborted.Publish(MakePayload(7)));
    Payload* slot = aborted.BeginWrite();
    CHECK(slot != nullptr);
    slot->seq = 99;
    aborted.AbortWrite();
    PinnedLatestStore<Payload, 2>::Borrowed view;
    CHECK(aborted.TryRead(&view));
    CHECK(view->seq == 7 && PayloadOk(*view));
  }

  return 0;
}
