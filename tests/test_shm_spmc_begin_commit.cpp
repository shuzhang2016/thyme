// RT-14: ShmSpmcRing BeginWrite/CommitWrite in-place publish path — the
// seq -> head -> epoch publish ordering is exactly Push's, overwrite
// semantics are unchanged, an aborted BeginWrite leaves the slot unpublished
// (seq=0; the frame it held is sacrificed, counted as a drop), and concurrent
// readers never observe a half-written payload.
#include "thyme/check.h"
#include "thyme/runtime/shm_spmc_ring.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <unistd.h>

using namespace thyme;

namespace {

struct Payload {
  uint64_t a;
  uint32_t b;
};

struct Wide {
  uint64_t w[8];
};

Wide MakeWide(uint64_t gen) {
  Wide p{};
  for (uint64_t& word : p.w) word = gen;
  return p;
}

bool Consistent(const Wide& p) {
  for (const uint64_t word : p.w) {
    if (word != p.w[0]) return false;
  }
  return true;
}

}  // namespace

int main() {
  char name[64];
  std::snprintf(name, sizeof(name), "/c9spbc_%d", static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);

  constexpr std::size_t kCap = 4;

  // 1. In-place publish: seq/head/epoch advance exactly like Push, both
  // reader modes observe the values.
  {
    ShmSpmcRing<Payload, kCap> producer;
    CHECK(producer.Create(name));
    ShmSpmcRing<Payload, kCap> consumer;
    CHECK(consumer.Open(name, 1000000000ULL));
    SpmcReader<Payload, kCap> follow;
    SpmcReader<Payload, kCap> latest;
    CHECK(follow.Bind(&consumer, SpmcRead::Follow));
    CHECK(latest.Bind(&consumer, SpmcRead::Latest));

    for (uint64_t i = 0; i < 3; ++i) {
      const uint64_t head_before =
          producer.layout()->head.load(std::memory_order_relaxed);
      const uint32_t epoch_before =
          producer.layout()->epoch.load(std::memory_order_relaxed);
      Payload* slot = producer.BeginWrite();
      CHECK(slot != nullptr);
      slot->a = i;
      slot->b = static_cast<uint32_t>(i * 10);
      producer.CommitWrite();
      // Publish ordering: slot seq = head index + 1, head +1, epoch +1.
      const auto& cell = producer.layout()->slots[i & (kCap - 1)];
      CHECK(cell.seq.load(std::memory_order_relaxed) == i + 1);
      CHECK(producer.layout()->head.load(std::memory_order_relaxed) ==
            head_before + 1);
      CHECK(producer.layout()->epoch.load(std::memory_order_relaxed) ==
            epoch_before + 1);
    }

    for (uint64_t i = 0; i < 3; ++i) {
      Payload p{};
      CHECK(follow.TryPop(p));
      CHECK(p.a == i && p.b == static_cast<uint32_t>(i * 10));
    }
    Payload last{};
    CHECK(latest.ReadLatest(last) && last.a == 2 && last.b == 20);
    CHECK(follow.empty());

    producer.Close();
    consumer.Close();
  }

  // 2. Overwrite unchanged: publishing Cap + 3 in-place drops the oldest for
  // a follow reader, exactly like Push.
  {
    ShmRegion::UnlinkName(name);
    ShmSpmcRing<Payload, kCap> producer;
    CHECK(producer.Create(name));
    ShmSpmcRing<Payload, kCap> consumer;
    CHECK(consumer.Open(name, 1000000000ULL));
    SpmcReader<Payload, kCap> follow;
    CHECK(follow.Bind(&consumer, SpmcRead::Follow));

    for (uint64_t i = 0; i < kCap + 3; ++i) {
      Payload* slot = producer.BeginWrite();
      slot->a = i;
      slot->b = static_cast<uint32_t>(i);
      producer.CommitWrite();
    }
    Payload p{};
    std::size_t got = 0;
    uint64_t prev = 0;
    while (follow.TryPop(p)) {
      if (got > 0) CHECK(p.a == prev + 1);
      prev = p.a;
      ++got;
    }
    CHECK(got == kCap);
    CHECK(prev == kCap + 3 - 1);
    CHECK(follow.drop_count() == 3);

    producer.Close();
    consumer.Close();
  }

  // 3. AbortWrite: no shared state is rolled back (head was never advanced,
  // the slot stays at seq=0) and no epoch bump/wake is issued for the aborted
  // write. The frame the slot previously held is sacrificed: a follow reader
  // counts it as a drop, never a torn read.
  {
    ShmRegion::UnlinkName(name);
    ShmSpmcRing<Payload, kCap> producer;
    CHECK(producer.Create(name));
    ShmSpmcRing<Payload, kCap> consumer;
    CHECK(consumer.Open(name, 1000000000ULL));

    for (uint64_t i = 0; i < 3; ++i) {
      producer.Push(Payload{i, static_cast<uint32_t>(i)});
    }

    const uint64_t head_before =
        producer.layout()->head.load(std::memory_order_relaxed);
    const uint32_t epoch_before =
        producer.layout()->epoch.load(std::memory_order_relaxed);
    Payload* slot = producer.BeginWrite();  // targets slot 3 (frame 3)
    slot->a = 0xDEAD;
    slot->b = 0xDEAD;
    producer.AbortWrite();
    CHECK(producer.layout()->head.load(std::memory_order_relaxed) ==
          head_before);
    CHECK(producer.layout()->epoch.load(std::memory_order_relaxed) ==
          epoch_before);
    // Slot 3 stays write-in-progress (invalid) until the next BeginWrite.
    CHECK(producer.layout()->slots[3].seq.load(std::memory_order_relaxed) ==
          0);

    // Abort a write that targeted an OCCUPIED slot: Push frame 3 first (head
    // reaches 4), so the next BeginWrite overwrites slot 0 (frame 0).
    producer.Push(Payload{3, 3});  // frame 3 -> slot 3
    Payload* s = producer.BeginWrite();  // frame 4 -> slot 0 (holds frame 0)
    s->a = 0xBEEF;
    producer.AbortWrite();
    CHECK(producer.layout()->slots[0].seq.load(std::memory_order_relaxed) ==
          0);  // sacrificed, not resurrected half-written
    CHECK(producer.layout()->head.load(std::memory_order_relaxed) == 4);

    // A follow reader whose cursor sits on the sacrificed slot sees a
    // permanent hole (seq=0) and cannot skip it until the slot is rewritten
    // by the next commit — the aborted frame is simply never delivered.
    SpmcReader<Payload, kCap> stuck;
    CHECK(stuck.Bind(&consumer, SpmcRead::Follow));
    Payload sp{};
    CHECK(!stuck.TryPop(sp));  // hole at frame 0: no data, no tear

    // Commit the frame the aborted write was filling: the hole closes and the
    // reader recovers, counting exactly one drop for the sacrificed frame 0.
    Payload* fix = producer.BeginWrite();  // same slot 0 again
    CHECK(fix == s);
    fix->a = 4;
    fix->b = 4;
    producer.CommitWrite();
    SpmcReader<Payload, kCap> r;
    CHECK(r.Bind(&consumer, SpmcRead::Follow));
    for (uint64_t i = 1; i < 5; ++i) {
      Payload p{};
      CHECK(r.TryPop(p));
      CHECK(p.a == i && p.b == static_cast<uint32_t>(i));
    }
    CHECK(r.empty());
    CHECK(r.drop_count() == 1);
    CHECK(stuck.TryPop(sp) && sp.a == 1);  // the stuck reader recovered too

    producer.Close();
    consumer.Close();
  }

  // 4. Abort after a partial fill keeps the slot at seq=0 and head/epoch
  // unchanged; the next BeginWrite reuses the same slot.
  {
    ShmRegion::UnlinkName(name);
    ShmSpmcRing<Payload, kCap> producer;
    CHECK(producer.Create(name));
    ShmSpmcRing<Payload, kCap> consumer;
    CHECK(consumer.Open(name, 1000000000ULL));
    SpmcReader<Payload, kCap> follow;
    CHECK(follow.Bind(&consumer, SpmcRead::Follow));

    producer.Push(Payload{1, 1});
    const uint64_t head_before =
        producer.layout()->head.load(std::memory_order_relaxed);
    const uint32_t epoch_before =
        producer.layout()->epoch.load(std::memory_order_relaxed);
    Payload* slot = producer.BeginWrite();
    slot->a = 0xDEAD;
    producer.AbortWrite();
    CHECK(producer.layout()->slots[1].seq.load(std::memory_order_relaxed) ==
          0);
    CHECK(producer.layout()->head.load(std::memory_order_relaxed) ==
          head_before);
    CHECK(producer.layout()->epoch.load(std::memory_order_relaxed) ==
          epoch_before);
    Payload p{};
    CHECK(follow.TryPop(p) && p.a == 1);
    CHECK(follow.empty());  // the un-committed write is invisible

    Payload* again = producer.BeginWrite();
    CHECK(again == slot);
    again->a = 2;
    again->b = 2;
    producer.CommitWrite();
    CHECK(follow.TryPop(p) && p.a == 2 && p.b == 2);

    producer.Close();
    consumer.Close();
  }

  // 5. Concurrent writer + multiple readers: every successful read is
  // internally consistent (a half-written payload can never be observed) and
  // the follow reader sees a strictly increasing run.
  {
    char cname[64];
    std::snprintf(cname, sizeof(cname), "/c9spbcc_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(cname);
    ShmSpmcRing<Wide, 64> producer;
    CHECK(producer.Create(cname));
    ShmSpmcRing<Wide, 64> consumer;
    CHECK(consumer.Open(cname, 1000000000ULL));

    constexpr uint64_t kN = 20000;
    std::atomic<bool> bad{false};
    std::atomic<uint64_t> follow_got{0};
    std::atomic<uint64_t> latest_got{0};

    std::thread follow_thread([&] {
      SpmcReader<Wide, 64> r;
      CHECK(r.Bind(&consumer, SpmcRead::Follow));
      Wide p{};
      uint64_t got = 0;
      // Pop until caught up with a finished writer (drops are expected with
      // kN >> Cap; they surface through drop_count, not as torn reads).
      for (;;) {
        if (r.TryPop(p)) {
          if (!Consistent(p)) bad.store(true, std::memory_order_relaxed);
          ++got;
          continue;
        }
        if (producer.layout()->head.load(std::memory_order_acquire) >= kN) {
          break;
        }
      }
      while (r.TryPop(p)) {  // final drain
        if (!Consistent(p)) bad.store(true, std::memory_order_relaxed);
        ++got;
      }
      follow_got.store(got, std::memory_order_relaxed);
    });
    std::thread latest_thread([&] {
      SpmcReader<Wide, 64> r;
      CHECK(r.Bind(&consumer, SpmcRead::Latest));
      Wide p{};
      uint64_t got = 0;
      // Drain until the writer is done and no new frame shows up for a grace
      // period of idle polls.
      uint64_t idle = 0;
      for (;;) {
        if (r.ReadLatest(p)) {
          if (!Consistent(p)) bad.store(true, std::memory_order_relaxed);
          ++got;
          idle = 0;
          continue;
        }
        const bool writer_done =
            producer.layout()->head.load(std::memory_order_acquire) >= kN;
        if (writer_done && ++idle > 100000) break;
      }
      latest_got.store(got, std::memory_order_relaxed);
    });
    for (uint64_t i = 1; i <= kN; ++i) {
      Wide* s = producer.BeginWrite();
      const Wide v = MakeWide(i);
      for (int k = 0; k < 4; ++k) s->w[k] = v.w[k];
      for (int k = 4; k < 8; ++k) s->w[k] = v.w[k];
      producer.CommitWrite();
    }
    follow_thread.join();
    latest_thread.join();
    CHECK(!bad.load(std::memory_order_relaxed));
    CHECK(follow_got.load(std::memory_order_relaxed) > 0);
    CHECK(latest_got.load(std::memory_order_relaxed) > 0);

    producer.Close();
    consumer.Close();
    ShmRegion::UnlinkName(cname);
  }

  ShmRegion::UnlinkName(name);
  return 0;
}
