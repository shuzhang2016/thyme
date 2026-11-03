#include "thyme/check.h"
#include "thyme/runtime/record_mirror.h"
#include "thyme/runtime/shm_region.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <pthread.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace thyme;


static int CaptureStderrToFile(const char* path) {
  int saved = dup(2);
  if (saved < 0) return -1;
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    close(saved);
    return -1;
  }
  dup2(fd, 2);
  close(fd);
  return saved;
}

static void RestoreStderr(int saved) {
  if (saved < 0) return;
  fflush(stderr);
  dup2(saved, 2);
  close(saved);
}

static int CountOccurrences(const char* path, const char* needle) {
  FILE* f = std::fopen(path, "rb");
  if (f == nullptr) return -1;
  std::string body;
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
  std::fclose(f);
  int count = 0;
  std::size_t pos = 0;
  while ((pos = body.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += 1;
  }
  return count;
}


// Payload large enough that a memcpy lasts long enough for two racing writers
// to interleave (widens the torn-read window so a check-then-act regression is
// detectable). kTapPayload must be <= kRecordMirrorMaxPayload.
inline constexpr uint32_t kTapPayload = 16384;
// 240 KB variant (64-aligned, matches test_record_large_payload.cpp) — stresses
// the same torn-read detection with a much longer memcpy window.
inline constexpr uint32_t kLargeTapPayload = 240000;

struct TapArg {
  RecordMirrorPage* page;
  uint8_t pattern;
  int iters;
  uint32_t payload_size;
};
static void* Tapper(void* p) {
  auto* a = static_cast<TapArg*>(p);
  std::vector<uint8_t> buf(a->payload_size, a->pattern);
  for (int i = 0; i < a->iters; ++i) {
    a->page->TryWriteSlot(0, buf.data(), a->payload_size,
                          1000 + static_cast<uint64_t>(i), a->pattern);
  }
  return nullptr;
}

struct TapReadArg {
  RecordMirrorPage* page;
  const std::atomic<bool>* stop;
  std::atomic<int>* mixed;  // reads that observed a torn (mixed) payload
  uint32_t payload_size;
};
static void* TapReader(void* p) {
  auto* a = static_cast<TapReadArg*>(p);
  std::vector<uint8_t> buf(a->payload_size);
  while (!a->stop->load(std::memory_order_relaxed)) {
    uint32_t sz = 0;
    uint64_t mono = 0;
    uint32_t seq = 0;
    if (!a->page->ReadSlot(0, buf.data(), a->payload_size, sz, mono, seq)) {
      continue;
    }
    if (sz != a->payload_size) continue;
    if (buf[0] != 0xAA && buf[0] != 0x55) continue;
    for (uint32_t i = 1; i < sz; ++i) {
      if (buf[i] != buf[0]) {
        a->mixed->fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
  return nullptr;
}

// Two writers race on slot 0 with disjoint payload patterns (0xAA vs 0x55)
// while a reader continuously samples. The CAS seqlock guarantees only one
// writer enters the write-critical-section at a time, so the reader must NEVER
// observe a mixed (torn) payload. Runs under any payload size (16 KB and
// 240 KB both exercised) so the torn-read window scales with the memcpy cost.
static void RunTapContention(RecordMirrorPage* page, uint32_t payload_size) {
  std::atomic<bool> rstop{false};
  std::atomic<int> mixed{0};
  TapArg ta{page, 0xAA, 4000, payload_size};
  TapArg tb{page, 0x55, 4000, payload_size};
  TapReadArg ra{page, &rstop, &mixed, payload_size};
  pthread_t wa{}, wb{}, rd{};
  CHECK(pthread_create(&rd, nullptr, TapReader, &ra) == 0);
  CHECK(pthread_create(&wa, nullptr, Tapper, &ta) == 0);
  CHECK(pthread_create(&wb, nullptr, Tapper, &tb) == 0);
  CHECK(pthread_join(wa, nullptr) == 0);
  CHECK(pthread_join(wb, nullptr) == 0);
  rstop.store(true, std::memory_order_relaxed);
  CHECK(pthread_join(rd, nullptr) == 0);
  CHECK(mixed.load(std::memory_order_relaxed) == 0);
  CHECK((page->slot_headers[0].seq.load(std::memory_order_acquire) & 1u) == 0);
}

int main() {
  // SHM seqlock contract: lock-free atomics are asserted at compile time in
  // record_mirror.h; keep a runtime witness too.
  CHECK(std::atomic<uint32_t>::is_always_lock_free);
  CHECK(std::atomic<uint64_t>::is_always_lock_free);

  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  // Capacity is now derived from the contract size: the tap test below writes
  // kTapPayload bytes, so the channel must be sized to accept them.
  desc.payload_size = kTapPayload;
  std::snprintf(desc.name, sizeof(desc.name), "TestStruct");

  char name[32];
  std::snprintf(name, sizeof(name), "/c9rm_%d", static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);

  ShmRegion a;
  CHECK(a.Create(name, sizeof(RecordMirrorPage)));
  auto* page = static_cast<RecordMirrorPage*>(a.data());
  CHECK(page->Init(1, &desc, 1));
  CHECK(page->Validate());

  uint8_t in[64];
  for (int i = 0; i < 64; ++i) in[i] = static_cast<uint8_t>(i);
  CHECK(page->WriteSlot(0, in, 64, 1000, 7));

  uint8_t out[64]{};
  uint32_t sz = 0;
  uint64_t mono = 0;
  uint32_t seq = 0;
  CHECK(page->ReadSlot(0, out, sizeof(out), sz, mono, seq));
  CHECK(sz == 64);
  CHECK(mono == 1000);
  CHECK(seq == 7);
  CHECK(std::memcmp(in, out, 64) == 0);

  // TryWriteSlot contends when seqlock is odd (writer in progress).
  page->slot_headers[0].seq.store(1, std::memory_order_release);
  CHECK(!page->TryWriteSlot(0, in, 64, 4000, 8));
  CHECK(page->mirror_odd_skip_count() == 1);
  page->slot_headers[0].seq.store(2, std::memory_order_release);
  CHECK(page->TryWriteSlot(0, in, 64, 4000, 8));

  // Lock the two disciplines' differing semantics: with seq odd (a tap
  // mid-write), the tap path drops while the single-producer WriteSlot bumps
  // unconditionally — entering Write mid-tap leaves the seqlock broken (odd),
  // which is exactly why the two paths must never share a slot.
  page->slot_headers[0].seq.store(5, std::memory_order_release);
  CHECK(!page->TryWriteSlot(0, in, 64, 5000, 9));
  CHECK(page->mirror_odd_skip_count() == 2);
  CHECK(page->WriteSlot(0, in, 64, 5001, 10));
  CHECK((page->slot_headers[0].seq.load(std::memory_order_acquire) & 1u) == 1u);
  page->slot_headers[0].seq.store(8, std::memory_order_release);  // restore even

  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    ShmRegion ro;
    CHECK(ro.OpenRetry(name, sizeof(RecordMirrorPage), 1000000000ULL, true));
    auto* rp = static_cast<const RecordMirrorPage*>(ro.data());
    CHECK(rp->Validate());
    uint8_t buf[64]{};
    uint32_t rsz = 0;
    uint64_t rmono = 0;
    uint32_t rseq = 0;
    CHECK(rp->ReadSlot(0, buf, sizeof(buf), rsz, rmono, rseq));
    CHECK(rsz == 64);
    CHECK(std::memcmp(in, buf, 64) == 0);
    _exit(0);
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  // Concurrent TryWriteSlot contention with a live reader: two writers race on
  // slot 0 with disjoint payload patterns (0xAA vs 0x55) while a reader
  // continuously samples. The CAS seqlock guarantees only one writer enters
  // the write-critical-section at a time, so the reader must NEVER observe a
  // mixed (torn) payload. Checking only the final state is not enough: the
  // last complete memcpy overwrites any mid-flight mix, so the detection must
  // happen concurrently with the writes.
  RunTapContention(page, kTapPayload);

  // Mixed-size channels: each gets exactly its contract's capacity in the
  // shared arena, laid out back-to-back with no overlap.
  {
    auto mixedPage = std::make_unique<RecordMirrorPage>();
    ThymeStructDesc mdesc[2]{};
    mdesc[0].struct_id = 0;
    mdesc[0].contract_version = 1;
    mdesc[0].payload_size = 100;  // round_up -> 128
    std::snprintf(mdesc[0].name, sizeof(mdesc[0].name), "Small");
    mdesc[1].struct_id = 1;
    mdesc[1].contract_version = 1;
    mdesc[1].payload_size = 240000;  // ~240 KB, 64-aligned
    std::snprintf(mdesc[1].name, sizeof(mdesc[1].name), "Large");
    CHECK(mixedPage->Init(2, mdesc, 2));
    CHECK(mixedPage->Validate());
    CHECK(mixedPage->SlotCapacity(0) == 128);
    CHECK(mixedPage->SlotCapacity(1) == 240000);
    CHECK(mixedPage->PayloadPtr(1) - mixedPage->PayloadPtr(0) == 128);
    CHECK(mixedPage->slot_headers[0].payload_offset + mixedPage->slot_headers[0].payload_capacity <=
          mixedPage->slot_headers[1].payload_offset);

    std::vector<uint8_t> small(100);
    for (int i = 0; i < 100; ++i) small[i] = static_cast<uint8_t>(i * 3);
    std::vector<uint8_t> large(240000);
    for (int i = 0; i < 240000; ++i) large[i] = static_cast<uint8_t>(i % 251);
    CHECK(mixedPage->WriteSlot(0, small.data(), 100, 1000, 1));
    CHECK(mixedPage->WriteSlot(1, large.data(), 240000, 1000, 2));

    std::vector<uint8_t> rout(100);
    uint32_t rsz = 0;
    uint64_t rmono = 0;
    uint32_t rseq = 0;
    CHECK(mixedPage->ReadSlot(0, rout.data(), 100, rsz, rmono, rseq));
    CHECK(rsz == 100);
    CHECK(std::memcmp(small.data(), rout.data(), 100) == 0);

    std::vector<uint8_t> lout(240000);
    CHECK(mixedPage->ReadSlot(1, lout.data(), 240000, rsz, rmono, rseq));
    CHECK(rsz == 240000);
    CHECK(std::memcmp(large.data(), lout.data(), 240000) == 0);
  }

  // Arena-overflow and per-channel-overflow Init failures: Validate() false.
  {
    auto over = std::make_unique<RecordMirrorPage>();
    ThymeStructDesc odesc[5]{};
    for (int i = 0; i < 5; ++i) {
      odesc[i].struct_id = static_cast<uint16_t>(i);
      odesc[i].contract_version = 1;
      odesc[i].payload_size = kRecordMirrorMaxPayload;  // 5 × 512 KB > arena
      std::snprintf(odesc[i].name, sizeof(odesc[i].name), "Big%d", i);
    }
    CHECK(!over->Init(3, odesc, 5));
    CHECK(!over->Validate());

    auto tooBig = std::make_unique<RecordMirrorPage>();
    ThymeStructDesc bdesc{};
    bdesc.struct_id = 0;
    bdesc.contract_version = 1;
    bdesc.payload_size = kRecordMirrorMaxPayload + 1;  // exceeds per-channel cap
    std::snprintf(bdesc.name, sizeof(bdesc.name), "Huge");
    CHECK(!tooBig->Init(3, &bdesc, 1));
    CHECK(!tooBig->Validate());
  }

  // Oversize drop: size > per-slot capacity drops, counts in oversize_drop_cnt,
  // and the stderr log fires exactly once (one-shot).
  {
    auto drop = std::make_unique<RecordMirrorPage>();
    ThymeStructDesc ddesc{};
    ddesc.struct_id = 0;
    ddesc.contract_version = 1;
    ddesc.payload_size = 64;
    std::snprintf(ddesc.name, sizeof(ddesc.name), "SmallCap");
    CHECK(drop->Init(4, &ddesc, 1));
    CHECK(drop->oversize_drop_count() == 0);

    char cap_path[64];
    std::snprintf(cap_path, sizeof(cap_path), "/tmp/thyme_rm_oversize_%d.log",
                  static_cast<int>(getpid()));
    std::vector<uint8_t> big(128, 0xAB);

    int saved = CaptureStderrToFile(cap_path);
    CHECK(saved >= 0);
    CHECK(!drop->WriteSlot(0, big.data(), 128, 2000, 1));
    CHECK(!drop->TryWriteSlot(0, big.data(), 128, 2000, 2));
    RestoreStderr(saved);

    CHECK(drop->oversize_drop_count() == 2);
    CHECK(CountOccurrences(cap_path, "oversized payload dropped") == 1);
    std::remove(cap_path);
  }

  // Exact-capacity boundary: size == cap succeeds, size == cap + 1 drops
  // (proves the write path uses `>` not `>=`). cap = RoundUpCapacity(100) = 128.
  {
    auto bound = std::make_unique<RecordMirrorPage>();
    ThymeStructDesc bdesc{};
    bdesc.struct_id = 0;
    bdesc.contract_version = 1;
    bdesc.payload_size = 100;  // round_up -> 128
    std::snprintf(bdesc.name, sizeof(bdesc.name), "Boundary");
    CHECK(bound->Init(5, &bdesc, 1));
    CHECK(bound->SlotCapacity(0) == 128);

    std::vector<uint8_t> exact(128, 0x11);
    CHECK(bound->WriteSlot(0, exact.data(), 128, 2000, 1));
    CHECK(bound->oversize_drop_count() == 0);

    std::vector<uint8_t> over(129, 0x22);
    CHECK(!bound->WriteSlot(0, over.data(), 129, 2000, 2));
    CHECK(bound->oversize_drop_count() == 1);

    std::vector<uint8_t> back(128);
    uint32_t bsz = 0;
    uint64_t bmono = 0;
    uint32_t bseq = 0;
    CHECK(bound->ReadSlot(0, back.data(), 128, bsz, bmono, bseq));
    CHECK(bsz == 128);
    CHECK(std::memcmp(exact.data(), back.data(), 128) == 0);
  }

  // Adjacent-slot canary: filling slot 0 to full capacity must not pollute
  // slot 1's arena region (offset arithmetic alone is not enough — assert the
  // actual bytes at slot 1's offset and its readback).
  {
    auto canary = std::make_unique<RecordMirrorPage>();
    ThymeStructDesc cdesc[2]{};
    cdesc[0].struct_id = 0;
    cdesc[0].contract_version = 1;
    cdesc[0].payload_size = 64;  // cap 64
    std::snprintf(cdesc[0].name, sizeof(cdesc[0].name), "Slot0");
    cdesc[1].struct_id = 1;
    cdesc[1].contract_version = 1;
    cdesc[1].payload_size = 64;  // cap 64
    std::snprintf(cdesc[1].name, sizeof(cdesc[1].name), "Slot1");
    CHECK(canary->Init(6, cdesc, 2));

    std::vector<uint8_t> slot1(64, 0x5A);
    CHECK(canary->WriteSlot(1, slot1.data(), 64, 1000, 1));

    std::vector<uint8_t> slot0(64, 0x33);
    CHECK(canary->WriteSlot(0, slot0.data(), 64, 1000, 2));

    // Slot 1's first arena byte sits right after slot 0's 64-byte region and
    // must still hold slot 1's pattern (not slot 0's 0x33 spill).
    CHECK(canary->slot_headers[0].payload_offset + 64 ==
          canary->slot_headers[1].payload_offset);
    CHECK(canary->PayloadPtr(1)[0] == 0x5A);

    std::vector<uint8_t> rout(64);
    uint32_t rsz = 0;
    uint64_t rmono = 0;
    uint32_t rseq = 0;
    CHECK(canary->ReadSlot(1, rout.data(), 64, rsz, rmono, rseq));
    CHECK(rsz == 64);
    CHECK(std::memcmp(slot1.data(), rout.data(), 64) == 0);
  }

  // Concurrent-reader torn-read stress under a 240 KB payload (the large
  // memcpy widens the torn-read window; the existing 16 KB stress lives above).
  {
    auto big = std::make_unique<RecordMirrorPage>();
    ThymeStructDesc bdesc{};
    bdesc.struct_id = 0;
    bdesc.contract_version = 1;
    bdesc.payload_size = kLargeTapPayload;
    std::snprintf(bdesc.name, sizeof(bdesc.name), "LargeTap");
    CHECK(big->Init(7, &bdesc, 1));
    RunTapContention(big.get(), kLargeTapPayload);
  }

  a.Unmap();
  ShmRegion::UnlinkName(name);
  return 0;
}
