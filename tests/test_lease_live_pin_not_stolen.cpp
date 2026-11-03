// test_lease_live_pin_not_stolen.cpp — BF-15: a successfully acquired live
// lease must not be stolen just because the writer reaches its slot while the
// reader is between the epoch stamp and the final seq re-check. The test uses
// the BF-17 epoch-stamp seam to run the writer Begin at exactly that point;
// the writer must see the fresh epoch and skip (pinned_skip), never steal.
#include "thyme/check.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

using namespace thyme;

namespace {

constexpr uint32_t kLen = 128;

struct HookCtx {
  PinnedSlotWriter* writer = nullptr;
  bool called = false;
  bool skipped = false;
};

void WriterBeginInStampWindow(void* p) {
  auto* c = static_cast<HookCtx*>(p);
  c->called = true;
  PinnedSlotWriter::BeginView bv = c->writer->Begin(kLen);
  c->skipped = (bv.payload == nullptr) && c->writer->pending_skipped();
  c->writer->Commit();  // no-op after a skip
}

}  // namespace

int main() {
  char name[96];
  std::snprintf(name, sizeof(name), "/c9livepin_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());

  PinnedSlotWriter w;
  CHECK(w.Create(name, kLen, 2, 0));
  PinnedSlotReader r;
  CHECK(r.Open(name, 1000000000ULL));

  // head=2: frame 2 is the next write and maps back to slot 0 (frame 0's slot).
  for (uint8_t f = 0; f < 2; ++f) {
    PinnedSlotWriter::BeginView bv = w.Begin(kLen);
    CHECK(bv.payload != nullptr);
    std::memset(bv.payload, f, kLen);
    w.Commit();
  }
  CHECK(w.head() == 2);

  HookCtx ctx;
  ctx.writer = &w;
  r.set_epoch_stamp_test_hook(&WriterBeginInStampWindow, &ctx);

  PinnedSlotReader::ReadResult status = PinnedSlotReader::ReadResult::kTorn;
  PinnedSlotReader::Lease live = r.Acquire(0, &status);
  r.set_epoch_stamp_test_hook(nullptr, nullptr);

  CHECK(ctx.called);
  CHECK(ctx.skipped);                 // writer saw pin + fresh epoch -> skip
  CHECK(status == PinnedSlotReader::ReadResult::kOk);
  CHECK(live.valid());
  CHECK(live.still_valid());          // live lease was not stolen
  CHECK(w.lease()->lease_steal.load(std::memory_order_acquire) == 0);
  CHECK(w.pinned_skip_count() == 1);
  CHECK(live.view().payload[0] == 0);
  CHECK(live.Release() == LeaseReleaseResult::kOk);

  r.Close();
  w.Close();
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  return 0;
}
