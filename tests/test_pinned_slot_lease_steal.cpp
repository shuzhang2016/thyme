// test_pinned_slot_lease_steal.cpp — a lease older than steal_timeout_ns
// belongs to a dead reader; the writer steals the slot (lease_steal counts)
// and publishes normally instead of skipping forever. A fresh pin is never
// stolen.
#include "thyme/check.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <unistd.h>

using namespace thyme;

namespace {

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

void SleepNs(uint64_t ns) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
  nanosleep(&ts, nullptr);
}

}  // namespace

int main() {
  char name[96];
  std::snprintf(name, sizeof(name), "/c9lease_steal_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());

  PinnedSlotWriter w;
  CHECK(w.Create(name, 128, 2, 0, 50000000ULL));  // steal timeout 50 ms
  PinnedSlotReader r;
  CHECK(r.Open(name, 1000000000ULL));

  // Two frames; pin frame 0's slot.
  for (uint8_t f = 0; f < 2; ++f) {
    PinnedSlotWriter::BeginView bv = w.Begin(128);
    std::memset(bv.payload, f, 128);
    w.Commit();
  }
  {
    PinnedSlotReader::Lease hold = r.Acquire(0);
    CHECK(hold.valid());

    // Fresh pin: wrap once -> slot of frame 0 is pinned -> skip (no steal).
    PinnedSlotWriter::BeginView bv = w.Begin(128);  // frame 2 -> slot 0
    CHECK(bv.payload == nullptr);
    CHECK(w.pending_skipped());
    w.Commit();
    CHECK(w.pinned_skip_count() == 1);
    CHECK(w.lease()->lease_steal.load(std::memory_order_acquire) == 0);

    // Simulate the reader dying while holding the pin: poll with writes until
    // the steal timeout expires and the writer steals the slot (deterministic
    // condition, no fixed sleeps).
    uint64_t spins = 0;
    {
      bv = w.Begin(128);  // frame 3 -> slot 1
      CHECK(bv.payload != nullptr);
      std::memset(bv.payload, 3, 128);
      w.Commit();
      bool stole = false;
      const uint64_t deadline = NowNs() + 3000000000ULL;
      while (NowNs() < deadline) {
        bv = w.Begin(128);
        if (bv.payload != nullptr) {
          std::memset(bv.payload, static_cast<uint8_t>(4 + spins), 128);
          w.Commit();
          if (w.lease()->lease_steal.load(std::memory_order_acquire) > 0) {
            stole = true;  // frame landed in slot 0: the expired pin was stolen
            break;
          }
        } else {
          w.Commit();  // an occasional pin on slot 1? No — only slot 0 is pinned
        }
        ++spins;
        SleepNs(5000000ULL);  // 5 ms
      }
      CHECK(stole);
      // While waiting, every even write to slot 0 is a fresh-pin skip: the count
      // grows but steal happens once; the pin==0 assert is in the next section.
      CHECK(w.lease()->lease_steal.load(std::memory_order_acquire) == 1);
      CHECK(w.pinned_skip_count() >= 1);
    }

    // After a steal advances the generation (gen+1), the old guard's release
    // skips on generation mismatch — pin is 0, never underflows, and does not
    // touch later readers' pins.
    hold.Reset();
    {
      const uint64_t word =
          reinterpret_cast<const PinnedSlotLeaseCell*>(r.lease() + 1)[0]
              .pin_gen.load(std::memory_order_acquire);
      CHECK(LeasePinOf(word) == 0);   // no underflow
      CHECK(LeaseGenOf(word) == 1);   // steal advanced the generation
    }
    // After the steal a new reader's acquire/pin is unaffected by the old
    // lease: lease the frame in the stolen slot.
    const uint64_t stolen_frame = w.head() - 1;  // frame written by the stealing write
    PinnedSlotReader::View v;
    uint32_t gen = 0;
    CHECK(r.AcquireLease(stolen_frame, v, &gen) ==
          PinnedSlotReader::ReadResult::kOk);
    CHECK(v.payload[0] == static_cast<uint8_t>(4 + spins));
    CHECK(gen == 1);  // new lease after the steal's generation change
    r.ReleaseLease(stolen_frame, gen);
    {
      const uint64_t word =
          reinterpret_cast<const PinnedSlotLeaseCell*>(r.lease() + 1)[0]
              .pin_gen.load(std::memory_order_acquire);
      CHECK(LeasePinOf(word) == 0);
    }
  }

  // Concurrency: two reader threads acquire/release at high frequency while the
  // writer keeps writing under a very short steal timeout — pins never underflow
  // (pin==0xFFFFFFFF never appears) and all return to zero at the end.
  {
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
    PinnedSlotWriter w2;
    CHECK(w2.Create(name, 128, 4, 0, 1000000ULL));  // 1 ms steal timeout
    PinnedSlotReader r2;
    PinnedSlotReader r2b;
    CHECK(r2.Open(name, 1000000000ULL));
    CHECK(r2b.Open(name, 1000000000ULL));

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> bad{0};
    pthread_t writer, ra, rb;
    struct WArg {
      PinnedSlotWriter* w;
      std::atomic<bool>* stop;
    };
    auto writer_fn = [](void* p) -> void* {
      auto* a = static_cast<WArg*>(p);
      uint8_t f = 0;
      while (!a->stop->load(std::memory_order_relaxed)) {
        PinnedSlotWriter::BeginView bv = a->w->Begin(128);
        if (bv.payload != nullptr) {
          std::memset(bv.payload, f++, 128);
        }
        a->w->Commit();
      }
      return nullptr;
    };
    struct RArg {
      PinnedSlotReader* r;
      std::atomic<bool>* stop;
      std::atomic<uint64_t>* bad;
      uint64_t base;
    };
    auto reader_fn = [](void* p) -> void* {
      auto* a = static_cast<RArg*>(p);
      while (!a->stop->load(std::memory_order_relaxed)) {
        const uint64_t head = a->r->head();
        if (head == 0) continue;
        PinnedSlotReader::View v;
        uint32_t gen = 0;
        const uint64_t idx = a->base + (head - 1);
        if (a->r->AcquireLease(idx, v, &gen) == PinnedSlotReader::ReadResult::kOk) {
          a->r->ReleaseLease(idx, gen);
        }
        // Dead-reader simulation: occasionally pin without releasing (let the writer steal).
        if ((idx & 31) == 0) {
          PinnedSlotReader::View vv;
          uint32_t g2 = 0;
          a->r->AcquireLease(idx, vv, &g2);  // deliberately not released
        }
      }
      return nullptr;
    };
    WArg wa{&w2, &stop};
    RArg ra1{&r2, &stop, &bad, 0}, ra2{&r2b, &stop, &bad, 1};
    CHECK(pthread_create(&writer, nullptr, writer_fn, &wa) == 0);
    CHECK(pthread_create(&ra, nullptr, reader_fn, &ra1) == 0);
    CHECK(pthread_create(&rb, nullptr, reader_fn, &ra2) == 0);
    SleepNs(500000000ULL);  // 500 ms contention
    stop.store(true, std::memory_order_relaxed);
    CHECK(pthread_join(writer, nullptr) == 0);
    CHECK(pthread_join(ra, nullptr) == 0);
    CHECK(pthread_join(rb, nullptr) == 0);
    // The readers intentionally leak pins on every 32nd acquisition, so a
    // small per-slot upper bound is not an invariant. Only the 31-bit wrap
    // range proves underflow here; live pins are drained deterministically below.
    for (uint32_t s = 0; s < 4; ++s) {
      const uint64_t word =
          reinterpret_cast<const PinnedSlotLeaseCell*>(r2.lease() + 1)[s]
              .pin_gen.load(std::memory_order_acquire);
      CHECK(LeasePinOf(word) < 0x7FFFFF00ULL);
    }
    CHECK(w2.lease()->lease_steal.load(std::memory_order_acquire) > 0);

    const uint64_t drain_deadline = NowNs() + 3000000000ULL;
    bool drained = false;
    while (NowNs() < drain_deadline) {
      PinnedSlotWriter::BeginView bv = w2.Begin(128);
      if (bv.payload != nullptr) {
        std::memset(bv.payload, 0, 128);
      }
      w2.Commit();
      drained = true;
      for (uint32_t s = 0; s < 4; ++s) {
        const uint64_t word =
            reinterpret_cast<const PinnedSlotLeaseCell*>(r2.lease() + 1)[s]
                .pin_gen.load(std::memory_order_acquire);
        if (LeasePinOf(word) != 0) {
          drained = false;
          break;
        }
      }
      if (drained) {
        break;
      }
      SleepNs(1000000ULL);
    }
    CHECK(drained);
    r2.Close();
    w2.Close();
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  }

  r.Close();
  w.Close();
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  return 0;
}
