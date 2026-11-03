// RT-19: seqlock payload access is atomic on both sides, so a reader copying a
// slot the writer is concurrently overwriting is a permitted relaxed atomic
// overlap instead of a C++ data race. This test proves the observable contract
// (a successful read is never torn) and exercises the byte-copy helper's
// alignment/length handling. It must stay green under TSan.

#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/record_mirror_ring.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

using namespace thyme;

namespace {

struct Payload64 {
  uint64_t w[8];
};

Payload64 MakePayload(uint64_t gen) {
  Payload64 p{};
  for (uint64_t& word : p.w) word = gen;
  return p;
}

bool InternallyConsistent(const Payload64& p) {
  for (const uint64_t word : p.w) {
    if (word != p.w[0]) return false;
  }
  return p.w[0] != 0;
}

// The helper must copy exactly n bytes for every alignment/short-length
// combination and must not write outside dst.
void CheckCopyHelper() {
  uint8_t src[128];
  for (size_t i = 0; i < sizeof(src); ++i) {
    src[i] = static_cast<uint8_t>(i * 7 + 3);
  }
  for (size_t n = 0; n <= 40; ++n) {
    for (size_t src_off = 0; src_off < 8; ++src_off) {
      for (size_t dst_off = 0; dst_off < 8; ++dst_off) {
        uint8_t out[96];
        std::memset(out, 0xA5, sizeof(out));
        detail::AtomicPayloadCopyOut(out + dst_off, src + src_off, n);
        CHECK(std::memcmp(out + dst_off, src + src_off, n) == 0);
        for (size_t i = 0; i < dst_off; ++i) CHECK(out[i] == 0xA5);
        for (size_t i = dst_off + n; i < sizeof(out); ++i) CHECK(out[i] == 0xA5);

        uint8_t in[96];
        std::memset(in, 0xA5, sizeof(in));
        detail::AtomicPayloadCopyIn(in + dst_off, src + src_off, n);
        CHECK(std::memcmp(in + dst_off, src + src_off, n) == 0);
        for (size_t i = 0; i < dst_off; ++i) CHECK(in[i] == 0xA5);
        for (size_t i = dst_off + n; i < sizeof(in); ++i) CHECK(in[i] == 0xA5);
      }
    }
  }
}

void CheckLatestSlotStress() {
  constexpr uint32_t kWrites = 200000;
  LatestSlot<Payload64> slot;
  std::atomic<bool> go{false};
  std::atomic<bool> done{false};
  std::atomic<int> ready{0};
  std::atomic<uint64_t> read_ok{0};
  std::atomic<uint64_t> torn{0};
  std::atomic<uint64_t> exhausted{0};

  std::thread writer([&] {
    // Let the readers reach their spin loop first; otherwise a fast writer can
    // finish before either reader is scheduled and the stress is vacuous.
    while (ready.load(std::memory_order_acquire) < 2) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (uint32_t i = 1; i <= kWrites; ++i) slot.Write(MakePayload(i));
    // Bounded wait: prove the readers actually observed published samples
    // while the test was running, not just the final value afterwards.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (read_ok.load(std::memory_order_acquire) < 2 &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
  });

  auto reader = [&](bool use_if_new) {
    Payload64 out{};
    uint64_t seen = 0;
    uint32_t max_seen = 0;
    ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    while (!done.load(std::memory_order_acquire)) {
      bool got = false;
      if (use_if_new) {
        got = slot.ReadIfNew(seen, out);
      } else {
        got = slot.Read(out);
      }
      if (!got) {
        // Retry exhaustion is a documented outcome (writer in progress).
        exhausted.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (!InternallyConsistent(out) || out.w[0] > kWrites) {
        torn.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if (use_if_new) {
        // latest-value discipline: a newer generation never goes backwards.
        if (out.w[0] < max_seen) {
          torn.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        max_seen = static_cast<uint32_t>(out.w[0]);
      }
      read_ok.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::thread r1([&] { reader(false); });
  std::thread r2([&] { reader(true); });
  writer.join();
  r1.join();
  r2.join();

  CHECK(torn.load() == 0);
  const uint64_t total = read_ok.load() + exhausted.load();
  CHECK(read_ok.load() > 0);
  CHECK(total > 0);
  std::printf(
      "latest_slot: ok=%llu exhausted=%llu (reads=%llu)\n",
      static_cast<unsigned long long>(read_ok.load()),
      static_cast<unsigned long long>(exhausted.load()),
      static_cast<unsigned long long>(total));

  Payload64 final{};
  CHECK(slot.Read(final));
  CHECK(final.w[0] == kWrites);
  CHECK(InternallyConsistent(final));
}

// Optional (cheap) coverage of the ring tap reader path: the recorder reads
// samples from a slot the writer may be overwriting, exactly like LatestSlot.
void CheckRecordMirrorRingStress() {
  char name[64];
  std::snprintf(name, sizeof(name), "/rt19ring_%d", static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);

  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = sizeof(Payload64);
  std::snprintf(desc.name, sizeof(desc.name), "Payload64");
  RecordMirrorRingSpec spec{sizeof(Payload64), 8};

  record_mirror::RingClient client;
  CHECK(client.Create(name, &desc, &spec, 1, 7));
  RecordMirrorRingPage* page = client.Page();
  CHECK(page != nullptr && page->Validate());

  constexpr uint32_t kWrites = 40000;
  std::atomic<bool> go{false};
  std::atomic<bool> done{false};
  std::atomic<int> ready{0};
  std::atomic<uint64_t> ok{0};
  std::atomic<uint64_t> torn{0};
  std::atomic<uint64_t> skipped{0};

  std::thread writer([&] {
    while (ready.load(std::memory_order_acquire) < 1) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (uint32_t i = 1; i <= kWrites; ++i) {
      const Payload64 p = MakePayload(i);
      (void)page->TryWriteRing(0, &p, sizeof(p), i, i);
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (ok.load(std::memory_order_acquire) < 1 &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
  });

  std::thread reader([&] {
    Payload64 out{};
    ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    while (!done.load(std::memory_order_acquire)) {
      const uint64_t head = page->RingHead(0);
      if (head == 0) continue;
      uint32_t sz = 0;
      uint64_t mono = 0;
      uint32_t src_seq = 0;
      uint32_t stable = 0;
      if (!page->ReadRingSample(0, head - 1,
                                reinterpret_cast<uint8_t*>(&out), sizeof(out),
                                sz, mono, src_seq, &stable)) {
        skipped.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (sz != sizeof(out) || !InternallyConsistent(out) || out.w[0] > kWrites) {
        torn.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      ok.fetch_add(1, std::memory_order_relaxed);
    }
  });

  writer.join();
  reader.join();

  CHECK(torn.load() == 0);
  CHECK(ok.load() > 0);
  std::printf("ring: ok=%llu skipped=%llu\n",
              static_cast<unsigned long long>(ok.load()),
              static_cast<unsigned long long>(skipped.load()));
  ShmRegion::UnlinkName(name);
}

}  // namespace

int main() {
  CheckCopyHelper();
  CheckLatestSlotStress();
  CheckRecordMirrorRingStress();
  return 0;
}
