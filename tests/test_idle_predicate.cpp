// test_idle_predicate.cpp — T3: the chassis-mirror idle predicate evaluated
// through IdleDetector: hold timing, unreadable-source fallback (both modes),
// and the quota-pressure override.
#include "test_shm.h"
#include "thyme/check.h"
#include "thyme/record/idle_detector.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

using namespace thyme;

namespace {

// Fake chassis struct: [uint8 park_brk_status @0][float vehicle_speed @4].
void WriteChassis(RecordMirrorRingPage* page, uint8_t epb, float speed) {
  uint8_t buf[8] = {};
  buf[0] = epb;
  std::memcpy(buf + 4, &speed, sizeof(speed));
  CHECK(page->TryWriteRing(0, buf, sizeof(buf), 0, 0));
}

}  // namespace

int main() {
  char page_name[64];
  std::snprintf(page_name, sizeof(page_name), "/c9idle_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(page_name);

  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = 8;
  std::snprintf(desc.name, sizeof(desc.name), "ChassisPod");
  RecordMirrorRingSpec spec{8, 16};
  record_mirror::RingClient writer;
  CHECK(writer.Create(page_name, &desc, &spec, 1, 1));

  idle::ChassisSpec cspec;
  cspec.page_shm = page_name;
  cspec.struct_id = 0;
  cspec.epb_offset = 0;
  cspec.speed_offset = 4;
  cspec.speed_threshold = 0.1;
  idle::ChassisMirrorPredicate pred;
  CHECK(pred.Init(cspec));

  idle::Config cfg;
  cfg.idle_hold_ns = 1000000000ULL;  // 1 s
  idle::IdleDetector det(cfg, idle::ChassisMirrorPredicate::Pred, &pred);

  // Busy (EPB off): never flushes.
  WriteChassis(writer.Page(), 0, 0.0f);
  CHECK(!det.ShouldFlush(1000, 0.0));
  CHECK(!det.ShouldFlush(1000 + 5 * cfg.idle_hold_ns, 0.0));
  CHECK(det.condition_since() == 0);

  // EPB engaged + speed ~0, sustained: flush only after the hold.
  WriteChassis(writer.Page(), 1, 0.0f);
  CHECK(!det.ShouldFlush(10000, 0.0));
  CHECK(det.condition_since() == 10000);
  CHECK(!det.ShouldFlush(10000 + cfg.idle_hold_ns / 2, 0.0));
  CHECK(det.ShouldFlush(10000 + cfg.idle_hold_ns, 0.0));

  // Speed above threshold breaks the condition.
  WriteChassis(writer.Page(), 1, 5.0f);
  CHECK(!det.ShouldFlush(10000 + 2 * cfg.idle_hold_ns, 0.0));
  CHECK(det.condition_since() == 0);

  // EPB off with zero speed is NOT idle.
  WriteChassis(writer.Page(), 0, 0.0f);
  CHECK(!det.ShouldFlush(10000 + 3 * cfg.idle_hold_ns, 0.0));

  // Pressure override: busy chassis but pending ratio >= 0.8 -> flush.
  WriteChassis(writer.Page(), 0, 0.0f);  // EPB off = busy
  CHECK(!det.ShouldFlush(10000 + 3 * cfg.idle_hold_ns, 0.79));
  CHECK(det.ShouldFlush(10000 + 3 * cfg.idle_hold_ns, 0.80));

  // Unreadable source: fallback always_busy (false) vs assume_idle (true).
  // A predicate that failed Init (page never existed) reports source_ok=false.
  {
    idle::ChassisSpec gone_spec = cspec;
    gone_spec.page_shm = test::ShmTestName("gone_nonexistent");
    idle::ChassisMirrorPredicate gone;
    CHECK(!gone.Init(gone_spec, 100000000ULL));
    idle::IdleDetector det_busy(cfg, idle::ChassisMirrorPredicate::Pred,
                                &gone);
    CHECK(!det_busy.ShouldFlush(1, 0.0));  // always_busy
    idle::Config cfg2 = cfg;
    cfg2.fallback_assume_idle = true;
    idle::IdleDetector det_idle(cfg2, idle::ChassisMirrorPredicate::Pred,
                                &gone);
    CHECK(det_idle.ShouldFlush(1, 0.0));  // assume_idle
  }

  writer.Shutdown(true);
  return 0;
}
