// demo_producer.cpp — minimal tap-wired producer for trigger_center bring-up
// and smoke tests: creates a mirror ring page (one Pod16 struct), an SPMC
// ring with the record tap armed, and optionally a PinnedSlotStore; publishes
// until SIGTERM. Business code fills the ring slot in place
// (BeginWrite/CommitWrite, RT-16) / channel Begin+Commit — recording is
// entirely a side effect of the tap.
//
// Usage: demo_producer <page_shm> <ring_shm> [channel_shm] [rate_hz]

#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/triggered_recorder.h"  // triggered::kH264FlagIsIdr
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/write_section.h"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

volatile sig_atomic_t g_stop = 0;
void OnTerm(int) { g_stop = 1; }

struct Pod16 {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: demo_producer <page_shm> <ring_shm> [channel_shm] "
                 "[rate_hz]\n");
    return 2;
  }
  const double rate = argc > 4 ? std::atof(argv[4]) : 100.0;

  thyme::ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = sizeof(Pod16);
  std::snprintf(desc.name, sizeof(desc.name), "Pod16");
  thyme::RecordMirrorRingSpec spec{sizeof(Pod16), 4096};

  thyme::record_mirror::RingClient mirror;
  if (!mirror.Create(argv[1], &desc, &spec, 1, 1)) return 1;
  thyme::ShmSpmcRing<Pod16, 16> ring;
  if (!ring.Create(argv[2])) return 1;
  ring.SetRecordTap(mirror.Page(), 0);

  thyme::PinnedSlotWriter frames;
  const bool with_frames = argc > 3 && argv[3][0] != '\0' &&
                           std::strcmp(argv[3], "-") != 0;
  if (with_frames && !frames.Create(argv[3], 4096, 128, 0)) return 1;

  std::signal(SIGTERM, OnTerm);
  std::signal(SIGINT, OnTerm);
  const uint64_t period_ns = static_cast<uint64_t>(1e9 / rate);
  std::fprintf(stderr, "demo_producer: page=%s ring=%s frames=%s\n", argv[1],
               argv[2], with_frames ? argv[3] : "(none)");
  for (uint64_t i = 0; g_stop == 0; ++i) {
    thyme::WriteSection<decltype(ring)> section(ring);
    section->a = i;
    section->b = static_cast<uint32_t>(i * 2);
    section->c = static_cast<uint32_t>(i * 3);
    section.commit();
    if (with_frames) {
      thyme::PinnedSlotWriter::BeginView bv = frames.Begin(1000);
      if (bv.payload != nullptr) {
        std::memset(bv.payload, static_cast<int>(i & 0xFF), 1000);
        // h264 emulation: one IDR every 10 frames (GOP-aligned slicing relies
        // on the is_idr bit in flags).
        bv.header->flags.store(i % 10 == 0 ? thyme::triggered::kH264FlagIsIdr
                                           : 0,
                               std::memory_order_relaxed);
        frames.Commit();
      } else if (bv.status ==
                 thyme::PinnedSlotWriter::BeginStatus::kGated) {
        // REP-10: a replay owns this channel; say so once instead of silently
        // spinning on a gated channel.
        static bool warned = false;
        if (!warned) {
          std::fprintf(stderr,
                       "demo_producer: %s is gated by a replay; real "
                       "producer writes are blocked\n",
                       argv[3]);
          warned = true;
        }
      }
    }
    struct timespec ts {};
    ts.tv_sec = static_cast<time_t>(period_ns / 1000000000ULL);
    ts.tv_nsec = static_cast<long>(period_ns % 1000000000ULL);
    nanosleep(&ts, nullptr);
  }
  return 0;
}
