// RT-09: bounded seqlock retry loops expose their retry/torn counts.
// This in-process half covers the containers that need no named SHM region:
// LatestSlot (caller-owned stats) and the mirror pages (caller-owned
// torn_drops). The SHM half (SpmcReader / PinnedSlotReader) lives in
// test_retry_accounting_shm.cpp.
//
// Every case forces the retry deterministically by leaving the slot in its
// "writer in progress" state (odd seq / seq==0) through the public test seams
// or a direct store into a process-local page, then asserts the budget was
// consumed exactly once. A successful read must leave the counters untouched.
//
// Scope note: the tear field (stats.torn / torn_count()) can only increment
// when a publish lands *between* the seqlock load and the copy re-check, which
// cannot be forced from one thread without a hot-path hook. Those paths are
// covered by the concurrent stress tests; here we pin the retry counters and
// the "clean read leaves them at zero" invariant.
#include "thyme/check.h"
#include "thyme/contracts/struct_desc.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/record_mirror.h"
#include "thyme/runtime/record_mirror_ring.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

using namespace thyme;

namespace {

struct Pod {
  uint32_t v;
};

ThymeStructDesc MakeDesc(uint16_t id, uint32_t payload, const char* name) {
  ThymeStructDesc d{};
  d.struct_id = id;
  d.contract_version = 1;
  d.payload_size = payload;
  std::snprintf(d.name, sizeof(d.name), "%s", name);
  return d;
}

// 64-byte aligned scratch for the ring page + its trailing arena (the page is
// alignas(64) and is normally mapped in SHM).
struct AlignedBuf {
  void* p;
  explicit AlignedBuf(std::size_t bytes)
      : p(::operator new(bytes, std::align_val_t(64))) {}
  ~AlignedBuf() { ::operator delete(p, std::align_val_t(64)); }
  AlignedBuf(const AlignedBuf&) = delete;
  AlignedBuf& operator=(const AlignedBuf&) = delete;
  template <typename T>
  T* as() {
    return static_cast<T*>(p);
  }
};

}  // namespace

int main() {
  // ---- LatestSlot: Read/ReadIfNew budget + caller-owned stats. ----
  {
    LatestSlot<Pod> slot;
    Pod out{0};
    LatestSlotReadStats stats;

    // Never written: no retry, no payload.
    CHECK(!slot.Read(out, &stats));
    CHECK(stats.retries == 0 && stats.torn == 0);

    slot.Write(Pod{7});
    CHECK(slot.Read(out, &stats) && out.v == 7);
    CHECK(stats.retries == 0 && stats.torn == 0);

    // Writer in progress (odd seq): Read burns the whole budget and reports
    // it; the published value is never handed out.
    detail::LatestSlotTestAccess<Pod>::SetSeq(&slot, 3u);
    LatestSlotReadStats busy;
    CHECK(!slot.Read(out, &busy));
    CHECK(busy.retries == kLatestSlotReadRetries);
    CHECK(busy.torn == 0);
    CHECK(out.v == 7);  // untouched

    // Same for ReadIfNew; a fresh `seen` must not advance on failure.
    uint64_t seen = 0;
    LatestSlotReadStats busy_new;
    CHECK(!slot.ReadIfNew(seen, out, &busy_new));
    CHECK(busy_new.retries == kLatestSlotReadRetries);
    CHECK(seen == 0);

    // Closing the section makes the read succeed without further increments.
    detail::LatestSlotTestAccess<Pod>::SetSeq(&slot, 2u);
    CHECK(slot.ReadIfNew(seen, out, &busy_new));
    CHECK(out.v == 7 && seen == 2);
    CHECK(busy_new.retries == kLatestSlotReadRetries);
  }

  // ---- RecordMirrorPage::ReadSlot: caller-owned torn_drops. ----
  {
    auto page = std::make_unique<RecordMirrorPage>();
    ThymeStructDesc desc = MakeDesc(0, 8, "Pod");
    CHECK(page->Init(1, &desc, 1));

    Pod v{42};
    CHECK(page->WriteSlot(0, &v, sizeof(v), 123, 1));
    Pod out{0};
    uint32_t sz = 0;
    uint64_t mono = 0, torn = 0;
    uint32_t src = 0;
    CHECK(page->ReadSlot(0, reinterpret_cast<uint8_t*>(&out), sizeof(out),
                         sz, mono, src, nullptr, &torn));
    CHECK(out.v == 42 && sz == sizeof(v) && torn == 0);

    // Poison the seqlock to "write in progress": the read must exhaust the
    // budget and count exactly one torn drop (not one per retry).
    page->slot_headers[0].seq.store(1, std::memory_order_relaxed);
    CHECK(!page->ReadSlot(0, reinterpret_cast<uint8_t*>(&out), sizeof(out),
                          sz, mono, src, nullptr, &torn));
    CHECK(torn == 1);

    // Budget is a named constant (RT-09 audit): 16 loops, one count.
    CHECK(kMirrorReadRetries == 16);
  }

  // ---- RecordMirrorRingPage::ReadRingSample/PeekRingSample: same path. ----
  {
    ThymeStructDesc descs[1] = {MakeDesc(0, 8, "Pod")};
    RecordMirrorRingSpec specs[1] = {{8, 2}};
    const std::size_t bytes =
        static_cast<std::size_t>(RecordMirrorRingPage::RequiredBytes(
            descs, specs, 1));
    CHECK(bytes > sizeof(RecordMirrorRingPage));
    AlignedBuf buf(bytes);
    auto* page = buf.as<RecordMirrorRingPage>();
    new (page) RecordMirrorRingPage();
    CHECK(page->Init(1, descs, specs, 1));

    Pod v{9};
    CHECK(page->TryWriteRing(0, &v, sizeof(v), 100, 1));

    RecordMirrorRingSampleDesc desc;
    uint64_t torn = 0;
    CHECK(page->PeekRingSample(0, 0, desc, &torn));
    CHECK(desc.publish_index == 0 && torn == 0);

    // The ring's private slot header is reachable through the public geometry
    // (ArenaBase + slots_offset), so the test can force the mid-write state.
    auto* slot = reinterpret_cast<RecordMirrorRingSlotHeader*>(
        page->ArenaBase() + page->ring_descs[0].slots_offset);
    slot->seq.store(1, std::memory_order_relaxed);

    CHECK(!page->PeekRingSample(0, 0, desc, &torn));
    CHECK(torn == 1);  // one count per exhausted budget, not per retry

    Pod out{0};
    uint32_t sz = 0;
    uint64_t mono = 0;
    uint32_t src = 0;
    CHECK(!page->ReadRingSample(0, 0, reinterpret_cast<uint8_t*>(&out),
                                sizeof(out), sz, mono, src, nullptr, &torn));
    CHECK(torn == 2);
    CHECK(kMirrorSampleReadRetries == 16);

    // Republishing (even seq) reads clean again and leaves the counter alone.
    slot->seq.store(2u, std::memory_order_release);
    CHECK(page->PeekRingSample(0, 0, desc, &torn));
    CHECK(torn == 2);

    page->~RecordMirrorRingPage();
  }

  std::printf("test_retry_accounting: ok\n");
  return 0;
}
