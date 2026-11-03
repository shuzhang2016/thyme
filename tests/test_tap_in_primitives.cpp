// test_tap_in_primitives.cpp — regression test for "node-oblivious recording"
// (design doc 4.1): no test here calls any recording API to publish; samples
// reach the mirror ring purely as a side effect of ShmSpmcRing::Push,
// SpscRing::push_latest/try_push, and LatestSlot::Write with an injected tap.
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"
#include "thyme/runtime/spsc_ring.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

using namespace thyme;

namespace {

struct Pod {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};
static_assert(std::is_trivially_copyable<Pod>::value, "shm payload POD");

bool ReadPod(RecordMirrorRingPage* page, uint16_t sid, uint64_t index,
             Pod& out, uint64_t* mono = nullptr, uint32_t* sseq = nullptr) {
  uint32_t sz = 0;
  uint64_t m = 0;
  uint32_t s = 0;
  if (!page->ReadRingSample(sid, index, reinterpret_cast<uint8_t*>(&out),
                            sizeof(out), sz, m, s)) {
    return false;
  }
  if (sz != sizeof(Pod)) return false;
  if (mono != nullptr) *mono = m;
  if (sseq != nullptr) *sseq = s;
  return true;
}

}  // namespace

int main() {
  // Mirror ring page: struct 0 (Pod, depth 4), struct 1 (Pod, depth 2),
  // struct 2 (packed SpscRing word, depth 4).
  ThymeStructDesc descs[3]{};
  for (uint16_t i = 0; i < 3; ++i) {
    descs[i].struct_id = i;
    descs[i].contract_version = 1;
    descs[i].payload_size = i == 2 ? 8 : static_cast<uint32_t>(sizeof(Pod));
    std::snprintf(descs[i].name, sizeof(descs[i].name), "Tap%u", i);
  }
  RecordMirrorRingSpec specs[3] = {
      {static_cast<uint32_t>(sizeof(Pod)), 4},
      {static_cast<uint32_t>(sizeof(Pod)), 2},
      {8, 4}};

  char page_name[64];
  std::snprintf(page_name, sizeof(page_name), "/c9tap_page_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(page_name);
  record_mirror::RingClient mirror;
  CHECK(mirror.Create(page_name, descs, specs, 3, 1));
  RecordMirrorRingPage* page = mirror.Page();
  CHECK(page != nullptr && page->Validate());

  // --- ShmSpmcRing tap: cross-process small-struct channel. ---
  char ring_name[64];
  std::snprintf(ring_name, sizeof(ring_name), "/c9tap_spmc_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(ring_name);
  {
    ShmSpmcRing<Pod, 8> ring;
    CHECK(ring.Create(ring_name));
    // No tap yet: pushes must not touch the mirror.
    Pod p{1, 2, 3};
    ring.Push(p);
    CHECK(page->RingHead(0) == 0);

    ring.SetRecordTap(page, 0);
    for (uint64_t i = 1; i <= 6; ++i) {
      Pod v{i, static_cast<uint32_t>(i * 10), static_cast<uint32_t>(i * 100)};
      ring.Push(v);
    }
    // Mirror depth 4 with 6 tapped pushes -> retained publish indexes [2, 6).
    // Mirror index j carries the (j+1)-th push; the SPMC publish index h is
    // tapped as source_sequence (one untapped Push already made h = j+1).
    CHECK(page->RingHead(0) == 6);
    Pod got{};
    uint32_t sseq = 0;
    CHECK(!ReadPod(page, 0, 1, got));  // evicted
    for (uint64_t j = 2; j < 6; ++j) {
      CHECK(ReadPod(page, 0, j, got, nullptr, &sseq));
      CHECK(got.a == j + 1);
      CHECK(got.b == (j + 1) * 10);
      CHECK(sseq == static_cast<uint32_t>(j + 1));  // SPMC publish index
    }
    // Time-window harvest: every tapped sample is inside [t0, now].
    RecordMirrorRingSampleDesc harvest[8];
    const uint32_t n = page->HarvestWindow(0, 0, ~0ULL, harvest, 8);
    CHECK(n == 4);

    CHECK(ring.record_tap_armed());
    CHECK(ring.record_tap_struct_id() == 0);
    // Disarming restores zero-recording behavior.
    ring.ClearRecordTap();
    CHECK(!ring.record_tap_armed());
    CHECK(ring.record_tap_struct_id() == 0);
    Pod extra{99, 0, 0};
    ring.Push(extra);
    CHECK(page->RingHead(0) == 6);
    ring.Close();
  }
  ShmRegion::UnlinkName(ring_name);

  // --- SpscRing tap over a payload pool (index ring, payload stays in pool).
  {
    SpscRing<8> ring;
    Pod pool[4]{};
    ring.SetRecordTap(page, 1, pool, static_cast<uint32_t>(sizeof(Pod)));
    for (uint32_t i = 0; i < 3; ++i) {
      pool[i].a = 0xA0 + i;
      pool[i].b = i;
      pool[i].c = 0;
      CHECK(ring.push_latest(i, i) == SpscRing<8>::kInvalidIndex);
    }
    CHECK(page->RingHead(1) == 3);
    // Depth 2 -> only publish indexes 1,2 retained.
    Pod evicted{};
    CHECK(!ReadPod(page, 1, 0, evicted));
    for (uint64_t i = 1; i < 3; ++i) {
      Pod got{};
      uint32_t gen = 0;
      CHECK(ReadPod(page, 1, i, got, nullptr, &gen));
      CHECK(got.a == 0xA0 + i);
      CHECK(gen == static_cast<uint32_t>(i));  // generation as source_sequence
    }
    // try_push success taps too; failed try_push (not provoked here) must not.
    pool[3].a = 0xFF;
    CHECK(ring.try_push(3, 3));
    CHECK(page->RingHead(1) == 4);
    // Depth 2 -> only indexes 2,3 retained.
    Pod got{};
    CHECK(!ReadPod(page, 1, 1, got));
    CHECK(ReadPod(page, 1, 3, got));
    CHECK(got.a == 0xFF);
  }

  // --- SpscRing tap without a payload pool records the packed (idx, gen). ---
  {
    SpscRing<4> ring;
    ring.SetRecordTap(page, 2);
    ring.push_latest(0xDEADBEEF, 7);
    CHECK(page->RingHead(2) == 1);
    uint64_t packed = 0;
    uint32_t sz = 0;
    uint64_t m = 0;
    uint32_t s = 0;
    CHECK(page->ReadRingSample(2, 0, reinterpret_cast<uint8_t*>(&packed),
                               sizeof(packed), sz, m, s));
    CHECK(sz == 8);
    CHECK(static_cast<uint32_t>(packed) == 0xDEADBEEF);
    CHECK(static_cast<uint32_t>(packed >> 32) == 7);
    CHECK(s == 7);
    ring.ClearRecordTap();
    CHECK(!ring.record_tap_armed());
    ring.push_latest(1, 1);
    CHECK(page->RingHead(2) == 1);
  }

  // --- ShmSpscRing tap: full-delivery queue copies only committed pushes. ---
  {
    char n3[64];
    std::snprintf(n3, sizeof(n3), "/c9tap_spsc_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(n3);
    ShmSpscRing<Pod, 4> ring;
    CHECK(ring.Create(n3));
    CHECK(!ring.record_tap_armed());
    Pod skipped{7, 0, 0};
    CHECK(ring.Push(skipped));
    const uint64_t before = page->RingHead(1);
    ring.SetRecordTap(page, 1);
    CHECK(ring.record_tap_armed());
    CHECK(ring.record_tap_struct_id() == 1);
    Pod v{8, 1, 0};
    CHECK(ring.Push(v));
    CHECK(page->RingHead(1) == before + 1);
    ring.ClearRecordTap();
    CHECK(!ring.record_tap_armed());
    Pod after{9, 0, 0};
    CHECK(ring.Push(after));
    CHECK(page->RingHead(1) == before + 1);
    ring.Close();
    ShmRegion::UnlinkName(n3);
  }

  // --- NotifPipe tap: in-process queue, clear stops the copy. ---
  {
    NotifPipe<Pod, 4> pipe;
    const uint64_t before = page->RingHead(0);
    pipe.SetRecordTap(page, 0);
    Pod v{3, 4, 5};
    CHECK(pipe.Push(v));
    CHECK(page->RingHead(0) == before + 1);
    pipe.ClearRecordTap();
    CHECK(!pipe.record_tap_armed());
    Pod extra{4, 0, 0};
    CHECK(pipe.Push(extra));
    CHECK(page->RingHead(0) == before + 1);
  }

  // --- LatestSlot tap: in-process latest-value channel. ---
  {
    LatestSlot<Pod> slot;
    Pod p{5, 6, 7};
    slot.Write(p);
    CHECK(page->RingHead(0) == 7);  // untouched without tap
    slot.SetRecordTap(page, 0);
    CHECK(slot.record_tap_armed());
    for (uint64_t i = 0; i < 3; ++i) {
      Pod v{0xC0 + i, static_cast<uint32_t>(i), 0};
      slot.Write(v);
    }
    CHECK(page->RingHead(0) == 10);
    Pod got{};
    CHECK(ReadPod(page, 0, 9, got));
    CHECK(got.a == 0xC2);
    slot.ClearRecordTap();
    CHECK(!slot.record_tap_armed());
    slot.Write(p);
    CHECK(page->RingHead(0) == 10);
  }

  // --- Tap failure never blocks nor corrupts the primitive's semantics:
  // arm a tap whose struct capacity is smaller than the payload (oversize)
  // on an otherwise healthy ring; Push still succeeds and consumers still
  // read the value. ---
  {
    char n2[64];
    std::snprintf(n2, sizeof(n2), "/c9tap_spmc2_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(n2);
    // 128-byte payload > struct 2's 64-byte capacity -> every tap
    // oversize-drops while Push stays healthy.
    struct BigPod {
      uint8_t data[128];
    };
    ShmSpmcRing<BigPod, 4> ring;
    CHECK(ring.Create(n2));
    ring.SetRecordTap(page, 2);
    BigPod v{};
    v.data[0] = 0x77;
    v.data[127] = 0x66;
    ring.Push(v);
    CHECK(page->oversize_drop_count() >= 1);
    SpmcReader<BigPod, 4> reader;
    CHECK(reader.Bind(&ring, SpmcRead::Follow));
    BigPod out{};
    CHECK(reader.TryPop(out));
    CHECK(out.data[0] == 0x77 && out.data[127] == 0x66);
    ring.Close();
    ShmRegion::UnlinkName(n2);
  }

  mirror.Shutdown(true);
  return 0;
}
