// RT-13: ShmSpscRing BeginWrite/CommitWrite in-place publish path — results
// identical to Push, full ring returns nullptr (no caller spin), an aborted
// BeginWrite leaves the slot unpublished, and a concurrent consumer never
// observes a half-written payload.
#include "thyme/check.h"
#include "thyme/runtime/shm_spsc_ring.h"

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

// Internally consistent payload for the half-written detection: every word
// carries the sequence number, so a torn read shows mixed generations.
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
  std::snprintf(name, sizeof(name), "/c9ssbc_%d", static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);

  ShmSpscRing<Payload, 4> producer;
  CHECK(producer.Create(name));
  ShmSpscRing<Payload, 4> consumer;
  CHECK(consumer.Open(name, 1000000000ULL));

  // In-place publish: fill the slot through the BeginWrite pointer, commit,
  // and the consumer pops exactly what was written.
  Payload* slot = producer.BeginWrite();
  CHECK(slot != nullptr);
  slot->a = 11;
  slot->b = 22;
  producer.CommitWrite();
  Payload out{};
  CHECK(consumer.TryPop(out));
  CHECK(out.a == 11 && out.b == 22);
  CHECK(consumer.empty());

  // Interleaved with Push: one publish path, FIFO order across both APIs.
  for (uint64_t i = 0; i < 4; ++i) {
    if ((i & 1u) == 0) {
      CHECK(producer.Push(Payload{i, static_cast<uint32_t>(i)}));
    } else {
      Payload* s = producer.BeginWrite();
      CHECK(s != nullptr);
      s->a = i;
      s->b = static_cast<uint32_t>(i);
      producer.CommitWrite();
    }
  }
  for (uint64_t i = 0; i < 4; ++i) {
    CHECK(consumer.TryPop(out));
    CHECK(out.a == i && out.b == static_cast<uint32_t>(i));
  }
  CHECK(consumer.empty());

  // Full ring: BeginWrite returns nullptr and counts push_fail exactly like
  // Push. The caller must not spin; popping frees a slot for the next Begin.
  for (uint32_t i = 0; i < 4; ++i) CHECK(producer.Push(Payload{i, i}));
  CHECK(producer.BeginWrite() == nullptr);
  CHECK(producer.push_fail_count() == 1);
  CHECK(!producer.Push(Payload{9, 9}));
  CHECK(producer.push_fail_count() == 2);
  Payload d{};
  CHECK(consumer.TryPop(d));
  slot = producer.BeginWrite();
  CHECK(slot != nullptr);
  slot->a = 5;
  slot->b = 5;
  producer.AbortWrite();  // no-op for SPSC: nothing was shared yet
  CHECK(consumer.empty() == false);  // 3 older items remain
  // The aborted slot is handed out again by the next BeginWrite.
  slot = producer.BeginWrite();
  CHECK(slot != nullptr);
  slot->a = 6;
  slot->b = 6;
  producer.CommitWrite();
  for (int i = 0; i < 3; ++i) CHECK(consumer.TryPop(d));
  CHECK(consumer.TryPop(d) && d.a == 6 && d.b == 6);
  CHECK(consumer.empty());

  // Abort after a partial fill leaves the slot unpublished — head and epoch
  // do not move, the consumer stays empty, and the next BeginWrite reuses the
  // same slot address.
  const uint32_t epoch0 = producer.epoch();
  slot = producer.BeginWrite();
  CHECK(slot != nullptr);
  slot->a = 0xDEAD;
  producer.AbortWrite();
  CHECK(consumer.empty());
  CHECK(producer.epoch() == epoch0);
  Payload* again = producer.BeginWrite();
  CHECK(again == slot);
  again->a = 77;
  again->b = 77;
  producer.CommitWrite();
  CHECK(producer.epoch() == epoch0 + 1);
  CHECK(consumer.TryPop(out) && out.a == 77 && out.b == 77);
  CHECK(consumer.empty());

  producer.Close();
  consumer.Close();
  ShmRegion::UnlinkName(name);

  // Concurrent consumer never sees a half-written payload: the writer fills
  // the wide payload in two halves (a tear would be observable mid-fill if
  // the slot were visible before CommitWrite).
  {
    char cname[64];
    std::snprintf(cname, sizeof(cname), "/c9ssbcc_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(cname);
    ShmSpscRing<Wide, 64> prod;
    CHECK(prod.Create(cname));
    ShmSpscRing<Wide, 64> cons;
    CHECK(cons.Open(cname, 1000000000ULL));

    constexpr uint64_t kN = 50000;
    std::atomic<uint64_t> popped{0};
    std::atomic<bool> bad{false};
    std::thread reader([&] {
      Wide p{};
      while (popped.load(std::memory_order_relaxed) < kN) {
        if (cons.TryPop(p)) {
          if (!Consistent(p)) bad.store(true, std::memory_order_relaxed);
          popped.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
    for (uint64_t i = 1; i <= kN; ++i) {
      Wide* s = prod.BeginWrite();
      if (s == nullptr) {
        // Full: BeginWrite never spins internally; the caller owns the
        // backpressure policy (here: wait for the consumer to drain).
        --i;
        struct timespec req {0, 1000};
        nanosleep(&req, nullptr);
        continue;
      }
      const Wide v = MakeWide(i);
      for (int k = 0; k < 4; ++k) s->w[k] = v.w[k];
      struct timespec req {0, 500};  // widen the pre-commit window
      nanosleep(&req, nullptr);
      for (int k = 4; k < 8; ++k) s->w[k] = v.w[k];
      prod.CommitWrite();
    }
    reader.join();
    CHECK(!bad.load(std::memory_order_relaxed));
    CHECK(popped.load(std::memory_order_relaxed) == kN);
    CHECK(cons.empty());

    prod.Close();
    cons.Close();
    ShmRegion::UnlinkName(cname);
  }
  return 0;
}
