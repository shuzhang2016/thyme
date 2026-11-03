// Cross-process SPMC broadcast ring in shared memory (no backpressure).
#include "thyme/check.h"
#include "thyme/contracts/abi.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"
#include "thyme/runtime/waitable.h"
#include "test_wait.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

using namespace thyme;

namespace {

struct Payload {
  uint64_t a;
  uint32_t b;
};

constexpr std::size_t kCap = 4;

uint64_t MonoNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

struct CountingTarget : Waitable {
  bool empty() const override { return true; }
  void WaitUntil(uint64_t) override {}
  void Notify() override { count.fetch_add(1, std::memory_order_relaxed); }
  std::atomic<int> count{0};
};

struct UnlinkName {
  char name[64]{};
  explicit UnlinkName(const char* n) {
    std::snprintf(name, sizeof(name), "%s", n);
    ShmRegion::UnlinkName(name);
  }
  ~UnlinkName() { ShmRegion::UnlinkName(name); }
  UnlinkName(const UnlinkName&) = delete;
  UnlinkName& operator=(const UnlinkName&) = delete;
};

}  // namespace

int main() {
  CHECK(kShmSpmcRingMagic != kShmSpscRingMagic);
  // v4: ARCH-11 super-header; v3 added per-reader drop slots/bitmap.
  CHECK(kShmSpmcRingVersion == 4);

  char name[64];
  std::snprintf(name, sizeof(name), "/c9spmc_%d", static_cast<int>(getpid()));
  UnlinkName cleaner(name);

  CHECK((ShmSpmcRing<Payload, kCap>::RegionBytes() ==
         sizeof(typename ShmSpmcRing<Payload, kCap>::Layout)));

  // 1. Same process, two mappings: Push 3, two follow readers each see 0,1,2.
  {
    ShmSpmcRing<Payload, kCap> producer;
    CHECK(producer.Create(name));
    ShmSpmcRing<Payload, kCap> consumer;
    CHECK(consumer.Open(name, 1000000000ULL));

    // ARCH-11 v4: different T/Cap => different layout hash => refusal.
    {
      ShmSpmcRing<uint64_t, kCap> wrong_payload;
      CHECK(!wrong_payload.Open(name, 0));
      ShmSpmcRing<Payload, kCap * 2> wrong_cap;
      CHECK(!wrong_cap.Open(name, 0));
    }

    SpmcReader<Payload, kCap> r1;
    SpmcReader<Payload, kCap> r2;
    CHECK(r1.Bind(&consumer, SpmcRead::Follow));
    CHECK(r2.Bind(&consumer, SpmcRead::Follow));

    for (uint64_t i = 0; i < 3; ++i) {
      const PushResult result =
          producer.PushWithResult(Payload{i, static_cast<uint32_t>(i)});
      CHECK(result.ok && result.overwritten == 0);
    }
    for (uint64_t i = 0; i < 3; ++i) {
      Payload a{};
      Payload b{};
      CHECK(r1.TryPop(a));
      CHECK(r2.TryPop(b));
      CHECK(a.a == i && a.b == static_cast<uint32_t>(i));
      CHECK(b.a == i && b.b == static_cast<uint32_t>(i));
    }
    CHECK(r1.empty());
    CHECK(r2.empty());

    producer.Close();
    consumer.Close();
  }

  // 2. One latest + one follow: Push 0..N; latest equals last; follow gets a
  // prefix then gap-adjusted remainder; drop_count() > 0 when N > Cap.
  {
    ShmRegion::UnlinkName(name);
    ShmSpmcRing<Payload, kCap> producer;
    CHECK(producer.Create(name));
    ShmSpmcRing<Payload, kCap> consumer;
    CHECK(consumer.Open(name, 1000000000ULL));

    SpmcReader<Payload, kCap> follow;
    SpmcReader<Payload, kCap> latest;
    CHECK(follow.Bind(&consumer, SpmcRead::Follow));
    CHECK(latest.Bind(&consumer, SpmcRead::Latest));

    for (uint64_t i = 0; i < 2; ++i) {
      producer.Push(Payload{i, static_cast<uint32_t>(i)});
      Payload p{};
      CHECK(follow.TryPop(p) && p.a == i);
    }
    const uint64_t N = 10;  // N > Cap
    for (uint64_t i = 2; i <= N; ++i) {
      producer.Push(Payload{i, static_cast<uint32_t>(i)});
    }

    Payload last{};
    CHECK(latest.ReadLatest(last) && last.a == N);

    Payload p{};
    CHECK(follow.TryPop(p));
    uint64_t prev = p.a;
    std::size_t got = 1;
    while (follow.TryPop(p)) {
      CHECK(p.a == prev + 1);
      prev = p.a;
      ++got;
    }
    CHECK(got > 0);
    CHECK(prev == N);
    CHECK(follow.drop_count() > 0);

    producer.Close();
    consumer.Close();
  }

  // 3. Burst: Push Cap+5 with no consumer; Push still returns; follow
  // drop_count() increases; Push is not blocked.
  {
    ShmRegion::UnlinkName(name);
    ShmSpmcRing<Payload, kCap> producer;
    CHECK(producer.Create(name));
    ShmSpmcRing<Payload, kCap> consumer;
    CHECK(consumer.Open(name, 1000000000ULL));

    const uint64_t t0 = MonoNs();
    uint64_t producer_overwritten = 0;
    for (uint64_t i = 0; i < kCap + 5; ++i) {
      const PushResult result =
          producer.PushWithResult(Payload{i, static_cast<uint32_t>(i)});
      CHECK(result.ok);
      producer_overwritten += result.overwritten;
    }
    CHECK(producer_overwritten == 5);
    CHECK(MonoNs() - t0 < 50000000ULL);

    SpmcReader<Payload, kCap> follow;
    CHECK(follow.Bind(&consumer, SpmcRead::Follow));
    Payload p{};
    uint64_t dropped = 0;
    CHECK(follow.TryPop(p, &dropped));
    CHECK(dropped == 5);
    std::size_t got = 1;
    while (follow.TryPop(p, &dropped)) {
      CHECK(dropped == 0);
      ++got;
    }
    CHECK(dropped == 0);
    CHECK(got == kCap);
    CHECK(follow.drop_count() == 5);
    CHECK(consumer.layout()->reader_drops.load(std::memory_order_acquire) == 5);
    HealthSlot health{};
    uint64_t seen = 0;
    CreditSpmcReaderDrops(follow, &health, seen);
    CHECK(health.drop_cnt.load() == 5);
    CreditSpmcReaderDrops(follow, &health, seen);
    CHECK(health.drop_cnt.load() == 5);

    producer.Close();
    consumer.Close();
  }

  // 4. Two threads WaitUntil on two follow readers; one Push; both return
  // before 50ms (epoch wake-all).
  {
    ShmRegion::UnlinkName(name);
    ShmSpmcRing<Payload, kCap> producer;
    CHECK(producer.Create(name));
    ShmSpmcRing<Payload, kCap> consumer;
    CHECK(consumer.Open(name, 1000000000ULL));

    // ARCH-11 v4: different T/Cap => different layout hash => refusal.
    {
      ShmSpmcRing<uint64_t, kCap> wrong_payload;
      CHECK(!wrong_payload.Open(name, 0));
      ShmSpmcRing<Payload, kCap * 2> wrong_cap;
      CHECK(!wrong_cap.Open(name, 0));
    }

    SpmcReader<Payload, kCap> r1;
    SpmcReader<Payload, kCap> r2;
    CHECK(r1.Bind(&consumer, SpmcRead::Follow));
    CHECK(r2.Bind(&consumer, SpmcRead::Follow));
    CHECK(r1.empty());
    CHECK(r2.empty());

    std::atomic<uint64_t> lat1_ms{9999};
    std::atomic<uint64_t> lat2_ms{9999};
    std::thread t1([&] {
      const uint64_t start = MonoNs();
      r1.WaitUntil(start + 2000000000ULL);
      lat1_ms.store((MonoNs() - start) / 1000000ULL, std::memory_order_relaxed);
    });
    std::thread t2([&] {
      const uint64_t start = MonoNs();
      r2.WaitUntil(start + 2000000000ULL);
      lat2_ms.store((MonoNs() - start) / 1000000ULL, std::memory_order_relaxed);
    });
    CHECK(thyme_test::WaitFor([&] { return r1.waiter_count() == 2; }));
    producer.Push(Payload{42, 42});
    t1.join();
    t2.join();
    CHECK(lat1_ms.load(std::memory_order_relaxed) < 50);
    CHECK(lat2_ms.load(std::memory_order_relaxed) < 50);

    // True cross-process: child pushes, parent follow WaitUntil wakes.
    Payload drain{};
    CHECK(r1.TryPop(drain) && drain.a == 42);
    CHECK(r2.TryPop(drain) && drain.a == 42);
    CHECK(r1.empty());
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      producer.Push(Payload{77, 77});
      _exit(0);
    }
    const uint64_t w0 = MonoNs();
    r1.WaitUntil(w0 + 2000000000ULL);
    const uint64_t cross_ms = (MonoNs() - w0) / 1000000ULL;
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(cross_ms < 500);
    CHECK(r1.TryPop(drain) && drain.a == 77);

    producer.Close();
    consumer.Close();
  }

  // 5. Two CountingTarget Waitables via SetNotify; Push increments both.
  {
    ShmRegion::UnlinkName(name);
    ShmSpmcRing<Payload, kCap> producer;
    CHECK(producer.Create(name));

    SpmcReader<Payload, kCap> r1;
    SpmcReader<Payload, kCap> r2;
    CHECK(r1.Bind(&producer, SpmcRead::Follow));
    CHECK(r2.Bind(&producer, SpmcRead::Follow));

    CountingTarget t1;
    CountingTarget t2;
    r1.SetNotify(&t1);
    r2.SetNotify(&t2);
    producer.Push(Payload{1, 1});
    CHECK(t1.count.load(std::memory_order_relaxed) == 1);
    CHECK(t2.count.load(std::memory_order_relaxed) == 1);

    producer.Close();
  }

  // 6. Ninth AddNotifyTarget / Bind returns false.
  {
    ShmRegion::UnlinkName(name);
    ShmSpmcRing<Payload, kCap> producer;
    CHECK(producer.Create(name));

    CountingTarget dummy[kAbiMaxSpmcReaders + 1];
    for (std::size_t i = 0; i < kAbiMaxSpmcReaders; ++i) {
      CHECK(producer.AddNotifyTarget(&dummy[i]));
    }
    CHECK(!producer.AddNotifyTarget(&dummy[kAbiMaxSpmcReaders]));

    SpmcReader<Payload, kCap> extra;
    CHECK(!extra.Bind(&producer, SpmcRead::Follow));

    producer.Close();
  }

  // Supervisor Prepare path: Open-only fails; Prepare then Open succeeds.
  {
    char life[64];
    std::snprintf(life, sizeof(life), "/c9spmclife_%d",
                  static_cast<int>(getpid()));
    UnlinkName life_cleaner(life);

    ShmSpmcRing<Payload, kCap> orphan;
    CHECK(!orphan.Open(life, 50000000ULL));
    CHECK((ShmSpmcRing<Payload, kCap>::Prepare(life)));

    ShmSpmcRing<Payload, kCap> prep_prod;
    ShmSpmcRing<Payload, kCap> prep_cons;
    CHECK(prep_prod.Open(life, 1000000000ULL));
    CHECK(prep_cons.Open(life, 1000000000ULL));
    prep_prod.Push(Payload{3, 3});
    SpmcReader<Payload, kCap> r;
    CHECK(r.Bind(&prep_cons, SpmcRead::Follow));
    Payload got{};
    CHECK(r.TryPop(got) && got.a == 3 && got.b == 3);
    prep_prod.Close();
    prep_cons.Close();
  }

  // Latest intentionally skips older values; that policy is not a drop.
  {
    ShmRegion::UnlinkName(name);
    constexpr std::size_t kDropCap = 8;
    ShmSpmcRing<Payload, kDropCap> producer;
    CHECK(producer.Create(name));
    ShmSpmcRing<Payload, kDropCap> consumer;
    CHECK(consumer.Open(name, 1000000000ULL));

    SpmcReader<Payload, kDropCap> latest;
    CHECK(latest.Bind(&consumer, SpmcRead::Latest));

    for (uint64_t i = 0; i < 5; ++i) {
      producer.Push(Payload{i, static_cast<uint32_t>(i)});
    }
    Payload last{};
    uint64_t dropped = 99;
    CHECK(latest.TryPop(last, &dropped) && last.a == 4);
    CHECK(dropped == 0);
    CHECK(latest.drop_count() == 0);
    CHECK(consumer.layout()->reader_drops.load(std::memory_order_acquire) == 0);
    CHECK(!latest.TryPop(last, &dropped));
    CHECK(dropped == 0);

    producer.Close();
    consumer.Close();
  }

  // Producer overwrite count and per-reader loss are distinct ledgers.
  {
    ShmRegion::UnlinkName(name);
    ShmSpmcRing<Payload, 2> producer;
    CHECK(producer.Create(name));
    ShmSpmcRing<Payload, 2> consumer;
    ShmSpmcRing<Payload, 2> consumer2;
    CHECK(consumer.Open(name, 1000000000ULL));
    CHECK(consumer2.Open(name, 1000000000ULL));
    SpmcReader<Payload, 2> first;
    SpmcReader<Payload, 2> second;
    CHECK(first.Bind(&consumer, SpmcRead::Follow));
    CHECK(second.Bind(&consumer2, SpmcRead::Follow));
    PushResult result{};
    for (uint64_t i = 0; i < 3; ++i) {
      result = producer.PushWithResult(Payload{i, static_cast<uint32_t>(i)});
      CHECK(result.ok);
    }
    CHECK(result.overwritten == 1);
    Payload out{};
    uint64_t first_drop = 0;
    uint64_t second_drop = 0;
    CHECK(first.TryPop(out, &first_drop) && first_drop == 1);
    CHECK(second.TryPop(out, &second_drop) && second_drop == 1);
    CHECK(first.reader_slot() != second.reader_slot());
    CHECK(consumer.layout()->reader_drop_slots[first.reader_slot()].load(
              std::memory_order_acquire) == 1);
    CHECK(consumer.layout()->reader_drop_slots[second.reader_slot()].load(
              std::memory_order_acquire) == 1);
    CHECK(consumer.layout()->reader_drops.load(std::memory_order_acquire) == 2);
    producer.Close();
    consumer.Close();
    consumer2.Close();
  }

  // A full ring overwrites even when every reader is caught up; that is not
  // reader loss. Follow cursors and sequence numbers stay exact.
  {
    ShmRegion::UnlinkName(name);
    ShmSpmcRing<Payload, 2> producer;
    CHECK(producer.Create(name));
    ShmSpmcRing<Payload, 2> consumer;
    CHECK(consumer.Open(name, 1000000000ULL));
    SpmcReader<Payload, 2> follow;
    CHECK(follow.Bind(&consumer, SpmcRead::Follow));
    Payload out{};
    for (uint64_t i = 0; i < 4; ++i) {
      const PushResult result =
          producer.PushWithResult(Payload{i, static_cast<uint32_t>(i)});
      CHECK(result.ok);
      CHECK(result.overwritten == (i >= 2 ? 1u : 0u));
      uint64_t dropped = 99;
      CHECK(follow.TryPop(out, &dropped));
      CHECK(dropped == 0);
      CHECK(out.a == i);
    }
    CHECK(follow.drop_count() == 0);
    CHECK(consumer.layout()->reader_drops.load(std::memory_order_acquire) == 0);
    producer.Close();
    consumer.Close();
  }

  HealthSlot health{};
  // RT-23: exact 64-bit arithmetic. 0xFFFFFFFE + 2 crosses 2^32; the old
  // saturating u32 code would have pinned the counter at UINT32_MAX.
  health.AddDrops(UINT64_C(0xFFFFFFFE));
  health.AddDrops(2);
  CHECK(health.drop_cnt.load() == UINT64_C(0x100000000));
  health.AddDrops(1);
  CHECK(health.drop_cnt.load() == UINT64_C(0x100000001));

  // Test seam: a reader delta above 2^32 must reach the health slot exactly,
  // with no u32 truncation or saturation at the CreditSpmcReaderDrops boundary.
  SpmcReader<Payload, kCap> reader;
  uint64_t seen = 0;
  detail::SpmcReaderTestAccess<Payload, kCap>::SetDropCount(
      reader, UINT64_C(0x100000007));
  CreditSpmcReaderDrops(reader, &health, seen);
  CHECK(seen == UINT64_C(0x100000007));
  CHECK(health.drop_cnt.load() == UINT64_C(0x200000008));
  detail::SpmcReaderTestAccess<Payload, kCap>::SetDropCount(
      reader, UINT64_C(0x10000000A));
  CreditSpmcReaderDrops(reader, &health, seen);
  CHECK(health.drop_cnt.load() == UINT64_C(0x20000000B));
  return 0;
}
