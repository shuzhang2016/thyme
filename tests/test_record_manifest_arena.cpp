// test_record_manifest_arena.cpp — GEN-12/TST-11 arena half.
//
// gen_graph sizes the runtime record table from the manifest
// (`GetThymeRecordTable().count == kRecordRegistryCount`), and
// RecordMirrorRingPage sizes its arena solely from that count. This test pins
// the runtime invariant the generated shrink relies on: dropping one manifest
// channel drops exactly one channel's ring bytes, and an empty manifest has no
// arena.
#include "thyme/check.h"
#include "thyme/runtime/record_mirror_ring.h"

#include <cstdint>

using namespace thyme;

int main() {
  constexpr uint32_t kPayload = 256;
  constexpr uint32_t kDepth = 2;
  ThymeStructDesc descs[3]{};
  RecordMirrorRingSpec specs[3]{};
  for (int i = 0; i < 3; ++i) {
    descs[i].struct_id = static_cast<uint16_t>(i);
    descs[i].payload_size = kPayload;
    specs[i].payload_size = kPayload;
    specs[i].ring_depth = kDepth;
  }

  const uint64_t all = RecordMirrorRingPage::ComputeArenaBytes(descs, specs, 3);
  const uint64_t recorded =
      RecordMirrorRingPage::ComputeArenaBytes(descs, specs, 2);
  const uint64_t one_slot =
      (sizeof(RecordMirrorRingSlotHeader) +
       RecordMirrorRingPage::RoundUpCapacity(kPayload)) *
      kDepth;

  CHECK(all > 0);
  CHECK(recorded > 0);
  CHECK(all - recorded == one_slot);
  CHECK(RecordMirrorRingPage::ComputeArenaBytes(descs, specs, 0) == 0);

  return 0;
}
