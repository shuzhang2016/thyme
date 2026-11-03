// TST-04: property/invariant tests for the shared-memory context primitives.
//
// The same invariant set as test_primitive_properties.cpp, but over two
// mappings of a named SHM region so the producer and consumer really go
// through the cross-mapping path (head/tail/seq/epoch in shared memory):
//   - no duplication: a delivered value is delivered at most once and in
//     production order,
//   - no loss unless declared: produced == delivered + queued + refused, and
//     SPMC delivered + drop_count == committed head,
//   - abort semantics: an uncommitted BeginWrite is never visible; the SPMC
//     seq=0 hole blocks (never tears) a follow reader until the slot is
//     rewritten,
//   - concurrency: 1 producer x 1 consumer (SPSC) and 1 producer x 2 readers
//     (SPMC follow + latest), driven by fixed counts and completion
//     handshakes.
// Every region is unlinked at the end so /dev/shm returns to its baseline.
#include "thyme/check.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"

#include "fuzz_harness.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

using thyme_fuzz::SplitMix64;

constexpr uint64_t kGarbage = 0xDEADBEEFDEADBEEFULL;

struct Payload {
  uint64_t value;
};

struct Wide {
  uint64_t w[8];
};

void FillWide(Wide* p, uint64_t gen) {
  for (uint64_t& word : p->w) word = gen;
}

bool Consistent(const Wide& p) {
  for (const uint64_t word : p.w) {
    if (word != p.w[0]) return false;
  }
  return true;
}

uint64_t MonoNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

void MakeName(char* out, std::size_t size, const char* tag, int instance) {
  std::snprintf(out, size, "/t04_%s_%d_%d", tag, static_cast<int>(getpid()),
                instance);
  ShmRegion::UnlinkName(out);
}

// ---------------------------------------------------------------------------
// ShmSpscRing: full-delivery Push (failure == declared refusal) mixed with the
// in-place BeginWrite/CommitWrite and abort paths.
// ---------------------------------------------------------------------------
void ShmSpscSchedule() {
  constexpr std::size_t kCap = 8;
  constexpr uint64_t kAttempts = 2500;
  char name[64];
  MakeName(name, sizeof(name), "spsc", 0);

  ShmSpscRing<Payload, kCap> writer;
  CHECK(writer.Create(name));
  ShmSpscRing<Payload, kCap> view;
  CHECK(view.Open(name, 1000000000ULL));

  std::deque<uint64_t> model;
  uint64_t attempted = 0;
  uint64_t delivered = 0;
  uint64_t refused = 0;
  SplitMix64 rng(0x5348535053433031ull);  // "SHSPSC01"
  uint64_t delivered_last = 0;
  bool have_last = false;

  while (attempted < kAttempts) {
    const uint64_t op = rng.Below(100);
    if (op < 50) {
      const uint64_t value = ++attempted;
      const bool full = model.size() == kCap;
      const bool ok = writer.Push(Payload{value});
      CHECK(ok == !full);
      if (ok) {
        model.push_back(value);
      } else {
        ++refused;
      }
    } else if (op < 62) {
      const uint64_t value = ++attempted;
      const bool full = model.size() == kCap;
      Payload* slot = writer.BeginWrite();
      CHECK((slot == nullptr) == full);
      if (slot != nullptr) {
        slot->value = value;
        writer.CommitWrite();
        model.push_back(value);
      } else {
        ++refused;
      }
    } else if (op < 72) {
      // Abandoned in-place write: capacity and depth are unchanged and the
      // half-filled slot is never visible.
      if (model.size() < kCap) {
        Payload* slot = writer.BeginWrite();
        CHECK(slot != nullptr);
        const uint64_t depth_before = writer.Depth();
        slot->value = kGarbage;
        writer.AbortWrite();
        CHECK(writer.Depth() == depth_before);
      }
    } else {
      Payload out{};
      const bool ok = view.TryPop(out);
      CHECK(ok == !model.empty());
      if (ok) {
        CHECK(out.value == model.front());
        CHECK(!have_last || out.value > delivered_last);
        delivered_last = out.value;
        have_last = true;
        model.pop_front();
        ++delivered;
      }
    }
    CHECK(writer.push_fail_count() == refused);
    CHECK(writer.Depth() == static_cast<uint64_t>(model.size()));
    CHECK(view.Depth() == writer.Depth());
    CHECK(writer.Depth() <= kCap);
    CHECK(view.empty() == model.empty());
    CHECK(attempted == delivered + refused +
                         static_cast<uint64_t>(model.size()));
  }

  while (!model.empty()) {
    Payload out{};
    CHECK(view.TryPop(out));
    CHECK(out.value == model.front());
    model.pop_front();
    ++delivered;
  }
  CHECK(view.empty());
  CHECK(attempted == delivered + refused);
  CHECK(writer.push_fail_count() == refused);

  writer.Close();
  view.Close();
  ShmRegion::UnlinkName(name);
}

// Abort never publishes; a full BeginWrite is refused exactly like Push; the
// slot handed out after an abort is reused by the next commit.
void ShmSpscAbort() {
  constexpr std::size_t kCap = 4;
  char name[64];
  MakeName(name, sizeof(name), "spscabort", 0);

  ShmSpscRing<Payload, kCap> writer;
  CHECK(writer.Create(name));
  ShmSpscRing<Payload, kCap> view;
  CHECK(view.Open(name, 1000000000ULL));

  for (uint64_t i = 1; i <= 3; ++i) CHECK(writer.Push(Payload{i}));
  Payload* slot = writer.BeginWrite();
  CHECK(slot != nullptr);
  slot->value = kGarbage;
  writer.AbortWrite();
  CHECK(writer.Depth() == 3);  // head never advanced
  Payload out{};
  for (uint64_t i = 1; i <= 3; ++i) {
    CHECK(view.TryPop(out));
    CHECK(out.value == i);
  }
  CHECK(!view.TryPop(out));  // the aborted write is invisible

  // Fill, then verify BeginWrite refuses on a full ring and counts push_fail.
  for (uint64_t i = 4; i <= 7; ++i) CHECK(writer.Push(Payload{i}));
  CHECK(writer.Depth() == kCap);
  CHECK(writer.BeginWrite() == nullptr);
  CHECK(writer.push_fail_count() == 1);
  CHECK(view.TryPop(out) && out.value == 4);

  slot = writer.BeginWrite();  // head 7 -> slot 3, whose old frame was popped
  CHECK(slot != nullptr);
  const uint64_t depth_before = writer.Depth();
  slot->value = kGarbage;
  writer.AbortWrite();
  CHECK(writer.Depth() == depth_before);
  Payload* again = writer.BeginWrite();
  CHECK(again == slot);
  again->value = 8;
  writer.CommitWrite();
  for (uint64_t i = 5; i <= 8; ++i) {
    CHECK(view.TryPop(out));
    CHECK(out.value == i);
  }
  CHECK(!view.TryPop(out));
  CHECK(view.empty());

  writer.Close();
  view.Close();
  ShmRegion::UnlinkName(name);
}

// 1 producer x 1 consumer across two mappings. Fixed count, no timing
// assertions: every one of the N values must arrive once, in order.
void ShmSpscConcurrent() {
  constexpr std::size_t kCap = 64;
  constexpr uint64_t kValues = 20000;
  char name[64];
  MakeName(name, sizeof(name), "spscconc", 0);

  ShmSpscRing<Payload, kCap> writer;
  CHECK(writer.Create(name));
  ShmSpscRing<Payload, kCap> view;
  CHECK(view.Open(name, 1000000000ULL));

  std::thread producer([&] {
    for (uint64_t i = 1; i <= kValues; ++i) {
      const Payload p{i};
      // Backpressure: retry until the consumer makes room (full-delivery
      // contract; no value may be lost).
      while (!writer.Push(p)) {
      }
    }
  });

  uint64_t got = 0;
  uint64_t last = 0;
  for (;;) {
    Payload out{};
    if (view.TryPop(out)) {
      CHECK(out.value == last + 1);
      last = out.value;
      ++got;
      if (got == kValues) break;
      continue;
    }
    view.WaitUntil(MonoNs() + 1000000ULL);
  }
  producer.join();
  CHECK(view.empty());
  CHECK(writer.Depth() == 0);
  CHECK(got == kValues);

  writer.Close();
  view.Close();
  ShmRegion::UnlinkName(name);
}

// ---------------------------------------------------------------------------
// ShmSpmcRing: overwrite (broadcast) semantics. Aborts sacrifice the frame
// the overwritten slot held, which a follow reader must count as a drop and
// never deliver as a torn/garbage value.
// ---------------------------------------------------------------------------
void ShmSpmcAbortSchedule() {
  constexpr std::size_t kCap = 8;
  constexpr int kOps = 1200;
  char name[64];
  MakeName(name, sizeof(name), "spmc", 0);

  ShmSpmcRing<Payload, kCap> writer;
  CHECK(writer.Create(name));
  ShmSpmcRing<Payload, kCap> view;
  CHECK(view.Open(name, 1000000000ULL));
  SpmcReader<Payload, kCap> reader;
  CHECK(reader.Bind(&view, SpmcRead::Follow));

  std::vector<uint64_t> commits;
  commits.reserve(kOps + 2);
  std::size_t candidate = 0;
  uint64_t accounted_drops = 0;
  uint64_t last = 0;
  bool have_last = false;

  // Every delivered value must be a committed value, strictly increasing, and
  // the frames skipped before it are exactly the accounted drops.
  auto drain = [&] {
    Payload out{};
    while (reader.TryPop(out)) {
      CHECK(out.value != kGarbage);
      CHECK(!have_last || out.value > last);
      while (candidate < commits.size() && commits[candidate] != out.value) {
        ++candidate;
        ++accounted_drops;
      }
      CHECK(candidate < commits.size());
      ++candidate;
      last = out.value;
      have_last = true;
    }
  };

  SplitMix64 rng(0x53484D43533031ull);  // "SHMCS01"
  uint64_t next_value = 0;
  bool last_was_abort = false;
  for (int step = 0; step < kOps; ++step) {
    if (rng.Below(100) < 70) {
      Payload* slot = writer.BeginWrite();
      CHECK(slot != nullptr);
      slot->value = ++next_value;
      writer.CommitWrite();
      commits.push_back(next_value);
      last_was_abort = false;
    } else {
      Payload* slot = writer.BeginWrite();
      CHECK(slot != nullptr);
      slot->value = kGarbage;
      writer.AbortWrite();
      last_was_abort = true;
    }
    if (step % 7 == 0) drain();
  }
  // A trailing abort leaves the head slot at seq=0; one commit closes the hole
  // (the aborted attempt itself never counts as a committed frame).
  if (last_was_abort) {
    Payload* slot = writer.BeginWrite();
    CHECK(slot != nullptr);
    slot->value = ++next_value;
    writer.CommitWrite();
    commits.push_back(next_value);
  }
  drain();

  CHECK(candidate == commits.size());
  CHECK(reader.drop_count() == accounted_drops);
  CHECK(reader.empty());
  CHECK(writer.layout()->head.load(std::memory_order_acquire) ==
        commits.size());

  writer.Close();
  view.Close();
  ShmRegion::UnlinkName(name);
}

// Deterministic seq=0 hole: an aborted overwrite of an occupied slot blocks a
// follow reader at that frame (no torn read, no data), and the next commit
// closes the hole while the sacrificed frame is counted as exactly one drop.
void ShmSpmcHole() {
  constexpr std::size_t kCap = 4;
  char name[64];
  MakeName(name, sizeof(name), "spmchole", 0);

  ShmSpmcRing<Payload, 4> writer;
  CHECK(writer.Create(name));
  ShmSpmcRing<Payload, 4> view;
  CHECK(view.Open(name, 1000000000ULL));

  for (uint64_t i = 0; i < kCap; ++i) writer.Push(Payload{i});
  Payload* sacrificed = writer.BeginWrite();  // frame 4 -> slot 0 (frame 0)
  CHECK(sacrificed != nullptr);
  CHECK(sacrificed == &writer.layout()->slots[0].value);
  sacrificed->value = kGarbage;
  writer.AbortWrite();
  CHECK(writer.layout()->slots[0].seq.load(std::memory_order_relaxed) == 0);
  CHECK(writer.layout()->head.load(std::memory_order_relaxed) == kCap);

  SpmcReader<Payload, kCap> stuck;
  CHECK(stuck.Bind(&view, SpmcRead::Follow));
  Payload out{};
  CHECK(!stuck.TryPop(out));  // hole at frame 0: no data and no tear

  Payload* fix = writer.BeginWrite();
  CHECK(fix == sacrificed);
  fix->value = 4;
  writer.CommitWrite();

  // Frames 1..4 arrive; frame 0 was sacrificed (one drop), never frame 0's
  // garbage.
  for (uint64_t i = 1; i <= 4; ++i) {
    CHECK(stuck.TryPop(out));
    CHECK(out.value == i);
  }
  CHECK(!stuck.TryPop(out));
  CHECK(stuck.drop_count() == 1);
  CHECK(stuck.empty());

  writer.Close();
  view.Close();
  ShmRegion::UnlinkName(name);
}

// 1 producer x 2 readers: follow and latest. Fixed commit count; both readers
// assert consistency and monotonicity. Only follow accounts skipped frames as
// drops.
void ShmSpmcConcurrent() {
  constexpr std::size_t kCap = 64;
  constexpr uint64_t kValues = 20000;
  char name[64];
  MakeName(name, sizeof(name), "spmcconc", 0);

  ShmSpmcRing<Wide, kCap> writer;
  CHECK(writer.Create(name));
  ShmSpmcRing<Wide, kCap> view;
  CHECK(view.Open(name, 1000000000ULL));
  SpmcReader<Wide, kCap> follow;
  SpmcReader<Wide, kCap> latest;
  CHECK(follow.Bind(&view, SpmcRead::Follow));
  CHECK(latest.Bind(&view, SpmcRead::Latest));

  std::atomic<uint64_t> torn{0};
  std::atomic<uint64_t> follow_got{0};
  std::atomic<uint64_t> latest_got{0};
  std::atomic<uint64_t> follow_last{0};
  std::atomic<uint64_t> latest_last{0};

  std::thread follow_thread([&] {
    Wide p{};
    uint64_t got = 0;
    uint64_t last = 0;
    bool have_last = false;
    for (;;) {
      if (follow.TryPop(p)) {
        if (!Consistent(p) || (have_last && p.w[0] <= last)) {
          torn.fetch_add(1, std::memory_order_relaxed);
        }
        last = p.w[0];
        have_last = true;
        ++got;
        continue;
      }
      if (writer.layout()->head.load(std::memory_order_acquire) >= kValues) {
        while (follow.TryPop(p)) {  // final drain
          if (!Consistent(p) || (have_last && p.w[0] <= last)) {
            torn.fetch_add(1, std::memory_order_relaxed);
          }
          last = p.w[0];
          have_last = true;
          ++got;
        }
        break;
      }
      std::this_thread::yield();
    }
    follow_got.store(got, std::memory_order_relaxed);
    follow_last.store(last, std::memory_order_relaxed);
  });

  std::thread latest_thread([&] {
    Wide p{};
    uint64_t got = 0;
    uint64_t last = 0;
    bool have_last = false;
    for (;;) {
      if (latest.ReadLatest(p)) {
        if (!Consistent(p) || (have_last && p.w[0] <= last)) {
          torn.fetch_add(1, std::memory_order_relaxed);
        }
        last = p.w[0];
        have_last = true;
        ++got;
        continue;
      }
      if (writer.layout()->head.load(std::memory_order_acquire) >= kValues) {
        while (latest.ReadLatest(p)) {  // final drain
          if (!Consistent(p) || (have_last && p.w[0] <= last)) {
            torn.fetch_add(1, std::memory_order_relaxed);
          }
          last = p.w[0];
          have_last = true;
          ++got;
        }
        break;
      }
      std::this_thread::yield();
    }
    latest_got.store(got, std::memory_order_relaxed);
    latest_last.store(last, std::memory_order_relaxed);
  });

  for (uint64_t i = 1; i <= kValues; ++i) {
    Wide* slot = writer.BeginWrite();
    FillWide(slot, i);
    writer.CommitWrite();
  }
  follow_thread.join();
  latest_thread.join();

  CHECK(torn.load(std::memory_order_relaxed) == 0);
  const uint64_t fgot = follow_got.load(std::memory_order_relaxed);
  const uint64_t lgot = latest_got.load(std::memory_order_relaxed);
  CHECK(fgot > 0);
  CHECK(lgot > 0);
  CHECK(follow_last.load(std::memory_order_relaxed) == kValues);
  CHECK(latest_last.load(std::memory_order_relaxed) == kValues);
  CHECK(fgot + follow.drop_count() == kValues);
  CHECK(lgot <= kValues);
  CHECK(latest.drop_count() == 0);

  writer.Close();
  view.Close();
  ShmRegion::UnlinkName(name);
}

}  // namespace

int main() {
  ShmSpscSchedule();
  ShmSpscAbort();
  ShmSpscConcurrent();
  ShmSpmcAbortSchedule();
  ShmSpmcHole();
  ShmSpmcConcurrent();
  return 0;
}
