#include "thyme/runtime/pinned_spmc_ring.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <type_traits>

#define CHECK(c) do { if (!(c)) { \
  std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #c); \
  std::abort(); \
} } while (0)

namespace {

struct Value {
  uint64_t sequence;
  uint64_t mirror;
  uint64_t payload[30];
};

using Ring = thyme::PinnedSpmcRing<Value, 4, 2>;

Value Make(uint64_t sequence) {
  Value value{};
  value.sequence = sequence;
  value.mirror = sequence;
  for (auto& item : value.payload) item = sequence;
  return value;
}

void CheckValue(const Value& value, uint64_t sequence) {
  if (value.sequence != sequence || value.mirror != sequence) {
    std::fprintf(stderr, "value mismatch: sequence=%llu mirror=%llu expected=%llu\n",
                 static_cast<unsigned long long>(value.sequence),
                 static_cast<unsigned long long>(value.mirror),
                 static_cast<unsigned long long>(sequence));
    std::abort();
  }
  for (const auto item : value.payload) CHECK(item == sequence);
}

}  // namespace

int main() {
  static_assert(!std::is_polymorphic<Ring>::value, "no runtime polymorphism");
  Ring ring;
  Ring::Reader first;
  Ring::Reader second;
  Ring::Reader rejected;
  CHECK(first.Bind(&ring));
  CHECK(second.Bind(&ring));
  CHECK(!rejected.Bind(&ring));

  CHECK(ring.Publish(Make(1)));
  CHECK(ring.Publish(Make(2)));
  Ring::Borrowed a;
  Ring::Borrowed b;
  CHECK(first.TryRead(&a));
  CheckValue(*a, 1);
  CHECK(second.TryRead(&b));
  CheckValue(*b, 1);
  CHECK(a.still_valid() && b.still_valid());

  CHECK(ring.Publish(Make(3)));
  CHECK(ring.Publish(Make(4)));
  CHECK(!ring.Publish(Make(5)));  // frame 1 is still pinned in slot 0
  CHECK(ring.pinned_skip_count() == 1);
  CHECK(ring.Publish(Make(6)));
  CHECK(ring.pinned_skip_count() == 1);
  CheckValue(*a, 1);
  CheckValue(*b, 1);

  a.Reset();
  b.Reset();
  CHECK(first.TryRead(&a));
  CheckValue(*a, 3);  // head=6, Cap=4: follow resumes at 6-4=2
  CHECK(first.TryRead(&a));
  CheckValue(*a, 4);
  CHECK(first.TryRead(&a));
  CheckValue(*a, 6);
  CHECK(!first.TryRead(&a));

  CHECK(second.TryRead(&b, thyme::PinnedReadMode::kLatest));
  CheckValue(*b, 6);
  CHECK(second.drop_count() == 0);
  b.Reset();

  {
    auto* slot = ring.BeginWrite();
    CHECK(slot != nullptr);
    *slot = Make(7);
    ring.AbortWrite();
  }
  CHECK(!first.TryRead(&a));
  CHECK(first.drop_count() == 3);  // overrun + pinned-skip hole + aborted publication
  CHECK(ring.Publish(Make(8)));
  CHECK(first.TryRead(&a));
  CheckValue(*a, 8);
  a.Reset();

  Ring concurrent;
  Ring::Reader follow;
  Ring::Reader latest;
  CHECK(follow.Bind(&concurrent));
  CHECK(latest.Bind(&concurrent));
  std::atomic<bool> start{false};
  std::atomic<bool> done{false};
  std::atomic<uint64_t> follow_reads{0};
  std::atomic<uint64_t> latest_reads{0};
  std::thread producer([&]() {
    while (!start.load(std::memory_order_acquire)) {}
    for (uint64_t i = 1; i <= 2000; ++i) {
      while (!concurrent.Publish(Make(i))) std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
  });
  std::thread follower([&]() {
    while (!start.load(std::memory_order_acquire)) {}
    Ring::Borrowed view;
    while (!done.load(std::memory_order_acquire) || follow_reads.load() < 2000) {
      if (!follow.TryRead(&view)) {
        if (done.load(std::memory_order_acquire)) break;
        std::this_thread::yield();
        continue;
      }
      CHECK(view.still_valid());
      const uint64_t sequence = view->sequence;
      CheckValue(*view, sequence);
      CHECK(view.still_valid());
      ++follow_reads;
      view.Reset();
    }
  });
  std::thread latest_reader([&]() {
    while (!start.load(std::memory_order_acquire)) {}
    Ring::Borrowed view;
    while (!done.load(std::memory_order_acquire) ||
           latest_reads.load(std::memory_order_relaxed) == 0) {
      if (latest.TryRead(&view, thyme::PinnedReadMode::kLatest)) {
        CHECK(view.still_valid());
      const uint64_t sequence = view->sequence;
      CheckValue(*view, sequence);
      CHECK(view.still_valid());
        ++latest_reads;
        view.Reset();
      } else {
        std::this_thread::yield();
      }
    }
  });
  start.store(true, std::memory_order_release);
  producer.join();
  follower.join();
  latest_reader.join();
  CHECK(follow_reads.load() > 0);
  CHECK(latest_reads.load() > 0);

  std::printf("test_pinned_spmc_ring ok\n");
  return 0;
}
