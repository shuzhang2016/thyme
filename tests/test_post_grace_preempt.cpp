// test_post_grace_preempt.cpp — R4: a sample stamped inside the post window
// but landing on the ring after the wall-clock deadline (thread preempted
// between the tap's MonoNs and the head store) must still be collected while
// the grace window is open. post_grace_ns default is now 20 ms.
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <vector>

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
  const int me = static_cast<int>(getpid());
  char page_name[96], out_dir[96];
  std::snprintf(page_name, sizeof(page_name), "/c9grace_page_%d", me);
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/c9grace_%d", me);
  ShmRegion::UnlinkName(page_name);

  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = 8;
  std::snprintf(desc.name, sizeof(desc.name), "Pod8");
  RecordMirrorRingSpec spec{8, 64};
  record_mirror::RingClient mirror;
  CHECK(mirror.Create(page_name, &desc, &spec, 1, 1));

  triggered::Config ecfg;
  ecfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name,
                   ""}};
  ecfg.out_dir = out_dir;
  // R4: the DEFAULT post_grace_ns (now 20 ms) is what this test exercises.
  CHECK(ecfg.post_grace_ns == 20000000ULL);
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(ecfg));

  // One in-window sample before the trigger.
  uint64_t v0 = 0xA0;
  CHECK(mirror.Page()->TryWriteRing(0, &v0, sizeof(v0), NowNs(), 0));

  const uint64_t t0 = NowNs();
  CHECK(rec.Trigger(1, 5000000ULL, 30000000ULL, t0));  // before 5ms, after 30ms
  const uint64_t deadline_wall = t0 + 30000000ULL;

  // Ride the post window; at deadline + 10 ms (inside the 20 ms grace) write
  // a sample stamped at deadline - 5 ms — a tap preempted 15 ms.
  const uint64_t inject_at = deadline_wall + 10000000ULL;
  bool injected = false;
  const uint64_t giveup = NowNs() + 10000000000ULL;
  while (rec.phase() != triggered::Phase::kArmed) {
    if (!injected && NowNs() >= inject_at) {
      uint64_t v1 = 0xB1;
      CHECK(mirror.Page()->TryWriteRing(0, &v1, sizeof(v1),
                                        deadline_wall - 5000000ULL, 1));
      injected = true;
    }
    rec.Tick();
    CHECK(NowNs() < giveup);
    SleepNs(200000);
  }
  CHECK(injected);

  // The package must contain BOTH samples: the pre-window one and the
  // grace-window straggler (it would be lost with the old 2 ms grace).
  CHECK(triggered::TriggeredRecorder::PackageComplete(
      rec.last_package_dir()));
  ThymeBinReader rd((rec.last_package_dir() + "/small.tbin").c_str());
  CHECK(rd.ok());
  uint64_t seen = 0;
  bool saw_straggler = false;
  ThymeBinDataEntry e;
  while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
    CHECK(e.type == ThymeBinEntryType::kData);
    uint64_t v = 0;
    std::memcpy(&v, e.payload.data(), sizeof(v));
    if (v == 0xB1) {
      saw_straggler = true;
      CHECK(e.mono_ns == deadline_wall - 5000000ULL);
    }
    ++seen;
  }
  CHECK(seen == 2);
  CHECK(saw_straggler);

  mirror.Shutdown(true);
  rec.Shutdown();
  return 0;
}
