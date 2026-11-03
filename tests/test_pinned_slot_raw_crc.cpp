// test_pinned_slot_raw_crc.cpp — R2: every frame index record carries
// payload_crc over the raw segment as written; a replay-side check recomputes
// from .raw and a single flipped byte must be reported as corruption (with
// the offset), not silently pass.
#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
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

// Replay-side verification: recompute each segment's crc from .raw; returns
// the offset of the first corrupt frame, or -1.
int64_t VerifyRawCrc(const std::string& pkg) {
  ThymeBinReader rd((pkg + "/frames/cloud_1.index.tbin").c_str());
  if (!rd.ok()) return -2;
  FILE* raw = std::fopen((pkg + "/frames/cloud_1.raw").c_str(), "rb");
  if (raw == nullptr) return -2;
  int64_t bad = -1;
  ThymeBinDataEntry e;
  while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
    if (e.type != ThymeBinEntryType::kData) continue;
    triggered::FrameIndexRecord ir{};
    std::memcpy(&ir, e.payload.data(), sizeof(ir));
    std::vector<uint8_t> seg(ir.len);
    if (fseeko(raw, static_cast<off_t>(ir.offset), SEEK_SET) != 0 ||
        std::fread(seg.data(), 1, ir.len, raw) != ir.len ||
        Crc32(seg.data(), ir.len) != ir.payload_crc) {
      bad = static_cast<int64_t>(ir.offset);
      break;
    }
  }
  std::fclose(raw);
  return bad;
}

}  // namespace

int main() {
  const int me = static_cast<int>(getpid());
  char page[96], chan[96], out[96];
  std::snprintf(page, sizeof(page), "/c9rawcrc_page_%d", me);
  std::snprintf(chan, sizeof(chan), "/c9rawcrc_chan_%d", me);
  std::snprintf(out, sizeof(out), "/tmp/c9rawcrc_%d", me);
  ShmRegion::UnlinkName(page);
  ShmRegion::UnlinkName(chan);

  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = 16;
  std::snprintf(desc.name, sizeof(desc.name), "Pod16");
  RecordMirrorRingSpec spec{16, 64};
  record_mirror::RingClient mirror;
  CHECK(mirror.Create(page, &desc, &spec, 1, 1));
  PinnedSlotWriter w;
  CHECK(w.Create(chan, 1000, 16, 0));
  for (uint64_t f = 0; f < 6; ++f) {
    PinnedSlotWriter::BeginView bv = w.Begin(1000);
    std::memset(bv.payload, static_cast<int>(0x10 + f), 1000);
    w.Commit(NowNs());
    SleepNs(500000);
  }

  triggered::Config ecfg;
  ecfg.sources = {{triggered::SourceConfig::Kind::kPinnedSlot, chan, "cloud_1"}};
  ecfg.out_dir = out;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(ecfg));
  CHECK(rec.Trigger(1, 60000000000ULL, 0));
  const uint64_t deadline = NowNs() + 10000000000ULL;
  while (rec.phase() != triggered::Phase::kArmed) {
    rec.Tick();
    CHECK(NowNs() < deadline);
    SleepNs(100000);
  }
  const std::string pkg = rec.last_package_dir();
  CHECK(triggered::TriggeredRecorder::PackageComplete(pkg));

  // Healthy: every segment verifies.
  CHECK(VerifyRawCrc(pkg) == -1);

  // Flip one byte inside the second segment -> reported with its offset.
  {
    // REC-26/27: frame 0 starts at 128 (channel header), so frame 1 starts
    // at 128 + 1000.
    const long kFrame1 = 128 + 1000;
    FILE* raw = std::fopen((pkg + "/frames/cloud_1.raw").c_str(), "rb+");
    CHECK(raw != nullptr);
    CHECK(fseeko(raw, kFrame1 + 17, SEEK_SET) == 0);  // second frame, byte 17
    int c = fgetc(raw);
    CHECK(c >= 0);
    CHECK(fseeko(raw, kFrame1 + 17, SEEK_SET) == 0);
    CHECK(fputc(c ^ 0xFF, raw) != EOF);
    CHECK(std::fclose(raw) == 0);
  }
  CHECK(VerifyRawCrc(pkg) == 128 + 1000);  // damaged segment offset

  mirror.Shutdown(true);
  w.Close();
  ShmRegion::UnlinkName(chan);
  rec.Shutdown();
  return 0;
}
