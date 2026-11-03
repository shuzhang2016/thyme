// TST-04: property/invariant tests for the in-process context primitives.
//
// A fixed-seed splitmix64 generator drives a deterministic operation schedule
// against a reference model and asserts the primitive's invariants after every
// step:
//   - no duplication: a value is delivered at most once (and in order for the
//     FIFO forms),
//   - no loss unless declared: produced == delivered + queued + declared
//     drops/refusals, with the primitive's own drop counter matching,
//   - boundedness: depth never exceeds capacity and the depth probes agree
//     with the model,
//   - abort semantics: an uncommitted BeginWrite never becomes visible.
// The concurrent cases use fixed iteration counts and completion handshakes
// (never wall-clock assertions), and stay inside the mr_gate budget.
#include "thyme/check.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/object_pool.h"
#include "thyme/runtime/spsc_ring.h"

#include "fuzz_harness.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <thread>
#include <vector>

using namespace thyme;

namespace {

using thyme_fuzz::SplitMix64;

enum Fate : uint8_t { kUnseen = 0, kDelivered = 1, kDropped = 2 };

void MarkOnce(std::vector<uint8_t>& fate, uint64_t value, Fate want) {
  CHECK(value < fate.size());
  const std::size_t i = static_cast<std::size_t>(value);
  CHECK(fate[i] == kUnseen);
  fate[i] = want;
}

uint32_t GenOf(uint32_t value) { return value * 3u + 1u; }

// ---------------------------------------------------------------------------
// SpscRing: drop-oldest push_latest.
// ---------------------------------------------------------------------------
void SpscRingDropOldest() {
  constexpr uint32_t kCap = 8;
  constexpr uint32_t kProduced = 3000;
  SpscRing<kCap> q;
  std::deque<uint32_t> model;
  std::vector<uint8_t> fate(kProduced + 1, kUnseen);
  uint32_t produced = 0;
  uint32_t delivered = 0;
  uint32_t dropped = 0;
  SplitMix64 rng(0x53505253303401ull);  // "SPRS04"

  while (produced < kProduced) {
    if (rng.Below(100) < 55) {
      const uint32_t value = ++produced;
      uint32_t dropped_gen = 0;
      const uint32_t evicted =
          q.push_latest(value, GenOf(value), &dropped_gen);
      if (model.size() == kCap) {
        CHECK(evicted == model.front());
        CHECK(dropped_gen == GenOf(model.front()));
        MarkOnce(fate, model.front(), kDropped);
        ++dropped;
        model.pop_front();
        CHECK(q.drop_count() == dropped);
      } else {
        CHECK(evicted == SpscRing<kCap>::kInvalidIndex);
      }
      model.push_back(value);
    } else {
      uint32_t value = 0;
      uint32_t gen = 0;
      const bool ok = q.try_pop(value, gen);
      CHECK(ok == !model.empty());
      if (ok) {
        CHECK(value == model.front());
        CHECK(gen == GenOf(value));
        MarkOnce(fate, value, kDelivered);
        ++delivered;
        model.pop_front();
      }
    }
    CHECK(q.size() == static_cast<uint64_t>(model.size()));
    CHECK(q.size() <= kCap);
    CHECK(q.Depth() == q.size());
    CHECK(q.empty() == model.empty());
    CHECK(produced == delivered + dropped +
                         static_cast<uint32_t>(model.size()));
    CHECK(q.drop_count() == dropped);
  }

  while (!model.empty()) {
    uint32_t value = 0;
    uint32_t gen = 0;
    CHECK(q.try_pop(value, gen));
    CHECK(value == model.front());
    CHECK(gen == GenOf(value));
    MarkOnce(fate, value, kDelivered);
    ++delivered;
    model.pop_front();
  }
  CHECK(q.empty());
  CHECK(q.drop_count() == dropped);
  CHECK(produced == delivered + dropped);
  for (uint32_t v = 1; v <= kProduced; ++v) {
    CHECK(fate[v] != kUnseen);
  }
}

// SpscRing: non-overwriting try_push must lose nothing and reject only when
// full (the refusal is the declared loss).
void SpscRingTryPush() {
  constexpr uint32_t kCap = 4;
  constexpr uint32_t kAttempts = 3000;
  SpscRing<kCap> q;
  std::deque<uint32_t> model;
  std::vector<uint8_t> fate(kAttempts + 1, kUnseen);
  uint32_t attempted = 0;
  uint32_t accepted = 0;
  uint32_t refused = 0;
  uint32_t delivered = 0;
  SplitMix64 rng(0x54525950533401ull);  // "TRYS04"

  while (attempted < kAttempts) {
    if (rng.Below(100) < 60) {
      const uint32_t value = ++attempted;
      const bool full = model.size() == kCap;
      const bool ok = q.try_push(value, GenOf(value));
      CHECK(ok == !full);
      if (ok) {
        model.push_back(value);
        ++accepted;
      } else {
        ++refused;
      }
    } else {
      uint32_t value = 0;
      uint32_t gen = 0;
      const bool ok = q.try_pop(value, gen);
      CHECK(ok == !model.empty());
      if (ok) {
        CHECK(value == model.front());
        CHECK(gen == GenOf(value));
        MarkOnce(fate, value, kDelivered);
        ++delivered;
        model.pop_front();
      }
    }
    CHECK(q.size() == static_cast<uint64_t>(model.size()));
    CHECK(q.size() <= kCap);
    CHECK(q.Depth() == q.size());
    CHECK(attempted == accepted + refused);
    CHECK(accepted == delivered + static_cast<uint32_t>(model.size()));
  }

  while (!model.empty()) {
    uint32_t value = 0;
    uint32_t gen = 0;
    CHECK(q.try_pop(value, gen));
    CHECK(value == model.front());
    MarkOnce(fate, value, kDelivered);
    ++delivered;
    model.pop_front();
  }
  CHECK(q.empty());
  CHECK(attempted == accepted + refused);
  CHECK(accepted == delivered);  // every accepted value arrived exactly once
  CHECK(q.drop_count() == 0);    // try_push never evicts
}

// ---------------------------------------------------------------------------
// NotifPipe: full-delivery queue with an explicit refused-push counter and an
// in-place BeginWrite/CommitWrite/AbortWrite path.
// ---------------------------------------------------------------------------
void NotifPipeSchedule() {
  constexpr uint32_t kCap = 8;
  constexpr uint32_t kAttempts = 3000;
  constexpr uint32_t kGarbage = 0xDEADBEEFu;
  NotifPipe<uint32_t, kCap> pipe;
  std::deque<uint32_t> model;
  std::vector<uint8_t> fate(kAttempts + 1, kUnseen);
  uint32_t attempted = 0;
  uint32_t accepted = 0;
  uint32_t refused = 0;
  uint32_t delivered = 0;
  SplitMix64 rng(0x4E4F5449463034ull);  // "NOTIF04"

  while (attempted < kAttempts) {
    const uint64_t op = rng.Below(100);
    if (op < 45) {
      const uint32_t value = ++attempted;
      const bool full = model.size() == kCap;
      const bool ok = pipe.Push(value);
      CHECK(ok == !full);
      if (ok) {
        model.push_back(value);
        ++accepted;
      } else {
        ++refused;
      }
    } else if (op < 60) {
      const uint32_t value = ++attempted;
      const bool full = model.size() == kCap;
      uint32_t* slot = pipe.BeginWrite();
      CHECK((slot == nullptr) == full);
      if (slot != nullptr) {
        *slot = value;
        pipe.CommitWrite();
        model.push_back(value);
        ++accepted;
      } else {
        ++refused;
      }
    } else if (op < 72) {
      // Aborted in-place publish: must stay invisible and recover capacity.
      if (model.size() < kCap) {
        uint32_t* slot = pipe.BeginWrite();
        CHECK(slot != nullptr);
        *slot = kGarbage;
        pipe.AbortWrite();
      }
    } else {
      uint32_t value = 0;
      const bool ok = pipe.TryPop(value);
      CHECK(ok == !model.empty());
      if (ok) {
        CHECK(value == model.front());
        MarkOnce(fate, value, kDelivered);
        ++delivered;
        model.pop_front();
      }
    }
    CHECK(pipe.Depth() == static_cast<uint64_t>(model.size()));
    CHECK(pipe.Depth() <= kCap);
    CHECK(pipe.empty() == model.empty());
    CHECK(attempted == accepted + refused);
    CHECK(accepted == delivered + static_cast<uint32_t>(model.size()));
  }

  while (!model.empty()) {
    uint32_t value = 0;
    CHECK(pipe.TryPop(value));
    CHECK(value == model.front());
    MarkOnce(fate, value, kDelivered);
    ++delivered;
    model.pop_front();
  }
  CHECK(pipe.empty());
  CHECK(attempted == accepted + refused);
  CHECK(accepted == delivered);  // MarkOnce rejected any duplicate delivery
}

// Explicit abort semantics: the slot leaves the free list at BeginWrite but is
// linked into ready_ only at CommitWrite, so a consumer can never observe a
// half-filled payload and the capacity is fully recovered.
void NotifPipeAbort() {
  NotifPipe<uint32_t, 2> pipe;
  uint32_t out = 0;
  uint32_t* slot = pipe.BeginWrite();
  CHECK(slot != nullptr);
  *slot = 0xDEADBEEFu;
  CHECK(pipe.empty());
  CHECK(!pipe.TryPop(out));
  pipe.AbortWrite();
  CHECK(pipe.empty());
  CHECK(pipe.Depth() == 0);

  slot = pipe.BeginWrite();
  CHECK(slot != nullptr);
  *slot = 7;
  pipe.CommitWrite();
  CHECK(pipe.BeginWrite() != nullptr);  // still has the second slot
  CHECK(pipe.TryPop(out) && out == 7);
  CHECK(pipe.Depth() == 0);
}

// ---------------------------------------------------------------------------
// ObjectPool: acquire/publish/release generation protocol.
// ---------------------------------------------------------------------------
struct alignas(64) PoolCell {
  uint64_t x;
};

enum PoolModel : uint8_t { kFree = 0, kWriting = 1, kReady = 2 };

void ObjectPoolSchedule() {
  constexpr uint32_t kN = 8;
  ObjectPool<PoolCell, kN> pool;
  PoolModel state[kN];
  uint32_t gen[kN];
  for (uint32_t i = 0; i < kN; ++i) {
    state[i] = kFree;
    gen[i] = 0;
  }
  SplitMix64 rng(0x504F4F4C303401ull);  // "POOL04"

  for (int step = 0; step < 4000; ++step) {
    const uint64_t op = rng.Below(100);
    uint32_t free_count = 0;
    for (uint32_t i = 0; i < kN; ++i) {
      if (state[i] == kFree) ++free_count;
    }

    if (op < 35) {
      const uint32_t idx = pool.acquire();
      if (free_count == 0) {
        CHECK((idx == ObjectPool<PoolCell, kN>::kInvalidIndex));
      } else {
        CHECK((idx != ObjectPool<PoolCell, kN>::kInvalidIndex));
        CHECK(state[idx] == kFree);  // no double hand-out
        const uint32_t next_gen = gen[idx] + 1u;
        CHECK(pool.generation(idx) == next_gen);
        gen[idx] = next_gen;
        state[idx] = kWriting;
        pool.at(idx).x = next_gen;
        if (rng.Below(100) < 70) {
          pool.publish(idx);
          state[idx] = kReady;
        }
      }
    } else if (op < 50) {
      const uint32_t idx = static_cast<uint32_t>(rng.Below(kN));
      if (state[idx] == kWriting) {
        pool.publish(idx);
        state[idx] = kReady;
      }
    } else if (op < 65) {
      const uint32_t idx = static_cast<uint32_t>(rng.Below(kN));
      if (state[idx] == kWriting) {
        pool.abort_write(idx);
        state[idx] = kFree;  // generation is not rolled back
      }
    } else if (op < 85) {
      const uint32_t idx = static_cast<uint32_t>(rng.Below(kN));
      if (state[idx] == kReady) {
        CHECK(pool.release(idx, gen[idx]));
        state[idx] = kFree;
      }
    } else if (op < 95) {
      // Stale generation: rejected and leaves the slot untouched.
      const uint32_t idx = static_cast<uint32_t>(rng.Below(kN));
      if (state[idx] == kReady) {
        CHECK(!pool.release(idx, gen[idx] - 1u));
        CHECK(pool.generation(idx) == gen[idx]);
        CHECK(state[idx] == kReady);
      }
    } else {
      const uint32_t idx = static_cast<uint32_t>(rng.Below(kN));
      if (state[idx] == kFree) CHECK(!pool.release(idx, gen[idx]));
    }
  }

  // Every slot must be reclaimable at the end (no leaked ownership).
  for (uint32_t i = 0; i < kN; ++i) {
    if (state[i] == kWriting) {
      pool.abort_write(i);
      state[i] = kFree;
    } else if (state[i] == kReady) {
      CHECK(pool.release(i, gen[i]));
      state[i] = kFree;
    }
    CHECK(state[i] == kFree);
  }
}

void ObjectPoolSingleSlot() {
  ObjectPool<PoolCell, 1> pool;
  CHECK(pool.acquire() == 0);
  // The slot is kWriting, not acquirable again, so it cannot be handed out
  // twice; abort_write recycles it.
  CHECK((pool.acquire() == ObjectPool<PoolCell, 1>::kInvalidIndex));
  pool.publish(0);
  CHECK(pool.release(0, 1));
  CHECK(!pool.release(0, 1));  // already free

  CHECK(pool.acquire() == 0);
  CHECK(pool.generation(0) == 2);
  pool.publish(0);
  // A stale generation (the previous owner's) must be rejected even though the
  // slot is ready for release.
  CHECK(!pool.release(0, 1));
  CHECK(pool.generation(0) == 2);
  CHECK(pool.release(0, 2));

  CHECK(pool.acquire() == 0);
  CHECK(pool.generation(0) == 3);
  pool.abort_write(0);
  CHECK(pool.acquire() == 0);
  CHECK(pool.generation(0) == 4);
  pool.abort_write(0);
}

// ---------------------------------------------------------------------------
// LatestSlot: newest-only seqlock. Reads never expose a torn or unpublished
// value, including across BeginWrite/AbortWrite.
// ---------------------------------------------------------------------------
struct Wide {
  uint64_t w[8];
};

Wide MakeWide(uint64_t gen) {
  Wide p{};
  for (uint64_t& word : p.w) word = gen;
  return p;
}

void FillWide(Wide* p, uint64_t gen, std::size_t words) {
  for (std::size_t i = 0; i < words; ++i) p->w[i] = gen;
}

bool Consistent(const Wide& p) {
  for (const uint64_t word : p.w) {
    if (word != p.w[0]) return false;
  }
  return true;
}

void CheckLatest(const LatestSlot<Wide>& slot, uint64_t published) {
  Wide out{};
  if (published == 0) {
    CHECK(!slot.Read(out));
    return;
  }
  CHECK(slot.Read(out));
  CHECK(Consistent(out));
  CHECK(out.w[0] == published);
}

void LatestSlotSchedule() {
  LatestSlot<Wide> slot;
  uint64_t published = 0;
  uint64_t next_value = 0;
  uint64_t ops = 0;
  SplitMix64 rng(0x4C41544553303401ull);  // "LATES04"

  for (int step = 0; step < 1500; ++step) {
    const uint64_t op = rng.Below(100);
    if (op < 45) {
      Wide* p = slot.BeginWrite();
      FillWide(p, ++next_value, 8);
      slot.CommitWrite();
      published = next_value;
      ++ops;
      CheckLatest(slot, published);
    } else if (op < 70) {
      // Abort a fully filled in-place write: nothing is published and the
      // previous value stays current.
      Wide* p = slot.BeginWrite();
      FillWide(p, 0xDEADBEEFDEADBEEFull, 8);
      slot.AbortWrite();
      ++ops;
      CheckLatest(slot, published);
    } else if (op < 85) {
      // Half-filled section: readers must fail instead of returning a torn
      // value, and AbortWrite restores the previous value.
      Wide* p = slot.BeginWrite();
      FillWide(p, 0xAA55AA55AA55AA55ull, 4);
      Wide out{};
      CHECK(!slot.Read(out));
      slot.AbortWrite();
      ++ops;
      CheckLatest(slot, published);
    } else {
      slot.Write(MakeWide(++next_value));
      published = next_value;
      ++ops;
      CheckLatest(slot, published);
    }
  }

  // Every completed BeginWrite/CommitWrite/AbortWrite/Write advances seq by 2.
  CHECK(thyme::detail::LatestSlotTestAccess<Wide>::Seq(&slot) == ops * 2u);
  CHECK((thyme::detail::LatestSlotTestAccess<Wide>::Seq(&slot) & 1u) == 0u);
}

// ---------------------------------------------------------------------------
// Concurrent cases: fixed iteration counts plus completion handshakes.
// ---------------------------------------------------------------------------
constexpr uint32_t kConcurrentValues = 5000;

struct RingRun {
  SpscRing<8> q;
  std::atomic<uint32_t> produced{0};
  std::atomic<uint64_t> delivered{0};
  std::atomic<uint64_t> replayed{0};
  std::atomic<uint64_t> torn{0};
};

void RingProducer(RingRun* run) {
  for (uint32_t i = 1; i <= kConcurrentValues; ++i) {
    run->q.push_latest(i, GenOf(i));
    run->produced.store(i, std::memory_order_release);
  }
}

void RingConsumer(RingRun* run) {
  uint32_t last = 0;
  uint64_t delivered = 0;
  for (;;) {
    uint32_t value = 0;
    uint32_t gen = 0;
    if (run->q.try_pop(value, gen)) {
      if (value <= last) {
        run->replayed.fetch_add(1, std::memory_order_relaxed);
      }
      if (gen != GenOf(value)) {
        run->torn.fetch_add(1, std::memory_order_relaxed);
      }
      last = value;
      ++delivered;
      continue;
    }
    if (run->produced.load(std::memory_order_acquire) != kConcurrentValues) {
      continue;
    }
    // The failed pop above may have raced the producer's eviction window even
    // though every push is complete now. Re-check the queue after the
    // completion edge before declaring the drain finished.
    if (!run->q.try_pop(value, gen)) break;
    if (value <= last) {
      run->replayed.fetch_add(1, std::memory_order_relaxed);
    }
    if (gen != GenOf(value)) {
      run->torn.fetch_add(1, std::memory_order_relaxed);
    }
    last = value;
    ++delivered;
  }
  run->delivered.store(delivered, std::memory_order_relaxed);
}

void SpscRingConcurrent() {
  RingRun run;
  std::thread producer(RingProducer, &run);
  std::thread consumer(RingConsumer, &run);
  producer.join();
  consumer.join();
  CHECK(run.replayed.load(std::memory_order_relaxed) == 0);
  CHECK(run.torn.load(std::memory_order_relaxed) == 0);
  const uint64_t delivered = run.delivered.load(std::memory_order_relaxed);
  CHECK(run.q.empty());
  CHECK(delivered + run.q.drop_count() == kConcurrentValues);
}

struct PipeRun {
  NotifPipe<uint32_t, 8> q;
  std::atomic<uint32_t> attempted{0};
  std::atomic<uint64_t> refused{0};
  std::atomic<uint64_t> delivered{0};
  std::atomic<uint64_t> replayed{0};
};

void PipeProducer(PipeRun* run) {
  for (uint32_t i = 1; i <= kConcurrentValues; ++i) {
    if (!run->q.Push(i)) {
      run->refused.fetch_add(1, std::memory_order_relaxed);
    }
    run->attempted.store(i, std::memory_order_release);
  }
}

void PipeConsumer(PipeRun* run) {
  uint32_t last = 0;
  uint64_t delivered = 0;
  for (;;) {
    uint32_t value = 0;
    if (run->q.TryPop(value)) {
      if (value <= last) {
        run->replayed.fetch_add(1, std::memory_order_relaxed);
      }
      last = value;
      ++delivered;
      continue;
    }
    if (run->attempted.load(std::memory_order_acquire) != kConcurrentValues) {
      continue;
    }
    // Re-check after the completion edge: the failed pop above may have
    // preceded the producer's final (successful) push.
    if (!run->q.TryPop(value)) break;
    if (value <= last) {
      run->replayed.fetch_add(1, std::memory_order_relaxed);
    }
    last = value;
    ++delivered;
  }
  run->delivered.store(delivered, std::memory_order_relaxed);
}

void NotifPipeConcurrent() {
  PipeRun run;
  std::thread producer(PipeProducer, &run);
  std::thread consumer(PipeConsumer, &run);
  producer.join();
  consumer.join();
  CHECK(run.replayed.load(std::memory_order_relaxed) == 0);
  const uint64_t delivered = run.delivered.load(std::memory_order_relaxed);
  const uint64_t refused = run.refused.load(std::memory_order_relaxed);
  CHECK(run.q.empty());
  CHECK(delivered + refused == kConcurrentValues);
}

struct PoolRun {
  ObjectPool<PoolCell, 8> pool;
  std::atomic<int> in_use[8]{};
  std::atomic<uint64_t> double_handout{0};
  std::atomic<uint64_t> bad_generation{0};
  std::atomic<uint64_t> completed{0};
};

void PoolWorker(PoolRun* run, uint64_t iterations) {
  for (uint64_t i = 0; i < iterations; ++i) {
    const uint32_t idx = run->pool.acquire();
    if (idx == ObjectPool<PoolCell, 8>::kInvalidIndex) {
      std::this_thread::yield();
      continue;
    }
    // Exactly one owner may observe the slot as idle. A double hand-out would
    // make this fetch_add return a non-zero previous value.
    if (run->in_use[idx].fetch_add(1, std::memory_order_acq_rel) != 0) {
      run->double_handout.fetch_add(1, std::memory_order_relaxed);
    }
    const uint32_t gen = run->pool.generation(idx);
    run->pool.at(idx).x = gen;
    run->pool.publish(idx);
    // Drop the ownership marker BEFORE release: release makes the slot
    // acquirable, so clearing it afterwards would let the next owner observe
    // this thread's stale marker and look like a double hand-out.
    run->in_use[idx].fetch_sub(1, std::memory_order_acq_rel);
    if (!run->pool.release(idx, gen)) {
      run->bad_generation.fetch_add(1, std::memory_order_relaxed);
    }
    run->completed.fetch_add(1, std::memory_order_relaxed);
  }
}

void ObjectPoolConcurrent() {
  PoolRun run;
  std::thread a(PoolWorker, &run, 5000);
  std::thread b(PoolWorker, &run, 5000);
  a.join();
  b.join();
  CHECK(run.double_handout.load(std::memory_order_relaxed) == 0);
  CHECK(run.bad_generation.load(std::memory_order_relaxed) == 0);
  CHECK(run.completed.load(std::memory_order_relaxed) == 10000);
}

void LatestSlotConcurrent() {
  constexpr uint64_t kCommits = 10000;
  LatestSlot<Wide> slot;
  std::atomic<bool> reader_ready{false};
  std::atomic<bool> writer_done{false};
  std::atomic<bool> torn{false};
  std::atomic<uint64_t> reads{0};

  std::thread reader([&] {
    Wide out{};
    reader_ready.store(true, std::memory_order_release);
    while (!writer_done.load(std::memory_order_acquire)) {
      if (slot.Read(out)) {
        if (!Consistent(out)) torn.store(true, std::memory_order_relaxed);
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  // Handshake: do not race ahead of the reader's first successful read, or a
  // fast writer could finish all commits before the reader is scheduled.
  while (!reader_ready.load(std::memory_order_acquire)) {
  }
  for (uint64_t i = 1; i <= kCommits; ++i) {
    Wide* p = slot.BeginWrite();
    FillWide(p, i, 8);
    slot.CommitWrite();
    if (i == 1) {
      while (reads.load(std::memory_order_relaxed) == 0) {
      }
    }
  }
  writer_done.store(true, std::memory_order_release);
  reader.join();

  CHECK(!torn.load(std::memory_order_relaxed));
  Wide last{};
  CHECK(slot.Read(last));
  CHECK(Consistent(last));
  CHECK(last.w[0] == kCommits);
}

}  // namespace

int main() {
  SpscRingDropOldest();
  SpscRingTryPush();
  NotifPipeSchedule();
  NotifPipeAbort();
  ObjectPoolSchedule();
  ObjectPoolSingleSlot();
  LatestSlotSchedule();
  SpscRingConcurrent();
  NotifPipeConcurrent();
  ObjectPoolConcurrent();
  LatestSlotConcurrent();
  return 0;
}
