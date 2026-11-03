// test_crc_policy.cpp — REC-21 per-channel CRC policy (crc=package|frame|off).
//
// Covers: policy parsing per channel kind + explicit overrides (incl. the
// dump-config round trip), the ABI-12 no-CRC frame seal (reserved[0] bit0 +
// crc32=0, old-format frames still verified), and the drain-side behavior of
// each policy on a pinned_slot channel (package: skip per-frame verify, manifest
// still verifies; frame: corrupt transport CRC -> GAP; off: payload_crc=0).
#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/package_integrity.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/trigger_config.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
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

constexpr uint32_t kTestMagic = 0x54535448u;  // "TSTH"
constexpr uint16_t kTestVersion = 7;
constexpr uint32_t kSlotBytes = 1000;
constexpr uint32_t kSlotCount = 16;
constexpr uint32_t kFrameCount = 6;

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

// ---- config parsing --------------------------------------------------------

const char* kPolicyBase = R"(
[trigger_center]
allow_empty_sources = true

[recorder]
output_dir = /tmp/thyme_crc_policy

[channel./cloud_1]
kind = pinned_slot
name = cloud_1

[channel./h264_front]
kind = h264_stream
name = h264_front
bitrate_kbps = 4000

[channel./thyme_recr_100]
kind = mirror_ring
)";

using CrcPolicy = triggered::SourceConfig::CrcPolicy;

const triggered::SourceConfig& Channel(const trigger_cfg::CenterConfig& cfg,
                                       const char* shm) {
  for (const auto& sc : cfg.recorder_sources) {
    if (sc.shm_name == shm) return sc;
  }
  CHECK(false);  // channel missing from the parsed config
  return cfg.recorder_sources.back();
}

void TestConfigParsing() {
  // Defaults by kind: bulk pinned_slot/h264 -> package, mirror ring -> frame.
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(trigger_cfg::ParseCenterConfig(kPolicyBase, &cfg, &err));
    CHECK(Channel(cfg, "/cloud_1").crc == CrcPolicy::kPackage);
    CHECK(Channel(cfg, "/h264_front").crc == CrcPolicy::kPackage);
    CHECK(Channel(cfg, "/thyme_recr_100").crc == CrcPolicy::kFrame);
    CHECK(triggered::EffectiveCrcPolicy(Channel(cfg, "/cloud_1")) ==
          CrcPolicy::kPackage);
    CHECK(triggered::EffectiveCrcPolicy(Channel(cfg, "/thyme_recr_100")) ==
          CrcPolicy::kFrame);
  }
  // Explicit overrides win over the kind default.
  {
    const std::string text = std::string(kPolicyBase) + R"(
[channel./cloud_2]
kind = pinned_slot
name = cloud_2
crc = frame

[channel./thyme_recr_200]
kind = mirror_ring
crc = off
)";
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(trigger_cfg::ParseCenterConfig(text, &cfg, &err));
    CHECK(Channel(cfg, "/cloud_2").crc == CrcPolicy::kFrame);
    CHECK(Channel(cfg, "/thyme_recr_200").crc == CrcPolicy::kOff);
  }
  // crc declared BEFORE kind in the same section still resolves correctly.
  {
    const std::string text = std::string(kPolicyBase) + R"(
[channel./cloud_3]
crc = off
kind = pinned_slot
name = cloud_3
)";
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(trigger_cfg::ParseCenterConfig(text, &cfg, &err));
    CHECK(Channel(cfg, "/cloud_3").crc == CrcPolicy::kOff);
  }
  // Unknown value is a parse error naming the key.
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(!trigger_cfg::ParseCenterConfig(
        "[channel./cam]\nkind = pinned_slot\ncrc = sometimes\n", &cfg, &err));
    CHECK(err.find("crc") != std::string::npos);
  }
  // crc=package on a small-struct channel would be a silent no-op (.tbin
  // rows always carry per-record CRC): rejected like any unimplementable key.
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(!trigger_cfg::ParseCenterConfig(
        "[trigger_center]\nallow_empty_sources = true\n"
        "[recorder]\noutput_dir = /tmp/thyme_crc_policy\n"
        "[channel./ring]\nkind = mirror_ring\ncrc = package\n",
        &cfg, &err));
    CHECK(err.find("crc=package") != std::string::npos);
  }
  // --dump-config materialises the effective policy and reparses to it.
  {
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(trigger_cfg::ParseCenterConfig(kPolicyBase, &cfg, &err));
    const std::string dump = trigger_cfg::DumpConfig(cfg);
    CHECK(dump.find("crc=package") != std::string::npos);
    CHECK(dump.find("crc=frame") != std::string::npos);
    trigger_cfg::CenterConfig cfg2;
    CHECK(trigger_cfg::ParseCenterConfig(dump, &cfg2, &err));
    CHECK(cfg2.recorder_sources.size() == cfg.recorder_sources.size());
    for (std::size_t i = 0; i < cfg.recorder_sources.size(); ++i) {
      CHECK(cfg2.recorder_sources[i].crc == cfg.recorder_sources[i].crc);
    }
  }
  // The engine-side resolver maps kDefault for programmatic configs.
  {
    triggered::SourceConfig bf;
    bf.kind = triggered::SourceConfig::Kind::kPinnedSlot;
    CHECK(bf.crc == CrcPolicy::kDefault);
    CHECK(triggered::EffectiveCrcPolicy(bf) == CrcPolicy::kPackage);
    triggered::SourceConfig mr;
    CHECK(triggered::EffectiveCrcPolicy(mr) == CrcPolicy::kFrame);
  }
}

// ---- ABI-12 frame seal (small Frame<T> crc=off semantics) -------------------

void TestFrameHeaderPolicy() {
  FrameHeader h{};
  std::memset(&h, 0, sizeof(h));
  h.size = sizeof(FrameHeader) + 4;
  const uint8_t* payload = reinterpret_cast<const uint8_t*>("abcd");

  // crc=off seal: reserved[0] bit0 set, crc32 = 0, readers skip verification.
  FrameHeaderSealNoCrc(h, kTestMagic, kTestVersion);
  CHECK((h.reserved[0] & kFrameFlagCrcDisabled) != 0);
  CHECK(h.crc32 == 0);
  CHECK(FrameCrcDisabled(h));
  CHECK(FrameHeaderOk(h, payload, 4, kTestMagic, kTestVersion));

  // Old-format frame (pre-ABI-12, flag clear, real CRC) still verifies as
  // before — crc-present semantics are unchanged by the new bit.
  FrameHeaderSeal(h, payload, 4, kTestMagic, kTestVersion);
  CHECK(!FrameCrcDisabled(h));
  CHECK(h.crc32 != 0);
  CHECK(FrameHeaderOk(h, payload, 4, kTestMagic, kTestVersion));

  // Backward-compat matrix row: crc32 == 0 WITHOUT the flag is a coincidence
  // (or corruption), never "crc disabled" — old packages keep verifying.
  h.crc32 = 0;
  CHECK(!FrameHeaderOk(h, payload, 4, kTestMagic, kTestVersion));
}

// ---- recorder drain integration ---------------------------------------------

struct CaptureResult {
  std::string pkg;
  uint64_t frames_written = 0;
  uint64_t holes_written = 0;
};

// Writes kFrameCount frames (fill byte = 0x10 + f) on a fresh channel and runs
// one full capture with the given CRC policy. before_ns covers everything.
CaptureResult RunCapture(const char* chan, const char* out, CrcPolicy policy,
                         bool transport_crc, int corrupt_frame) {
  PinnedSlotWriter w;
  CHECK(w.Create(chan, kSlotBytes, kSlotCount, 0));
  // REC-21: transport CRC defaults off for bulk channels; opt in only when
  // the scenario needs per-frame CRC on the wire.
  w.SetCrcEnabled(transport_crc);
  for (uint32_t f = 0; f < kFrameCount; ++f) {
    PinnedSlotWriter::BeginView bv = w.Begin(kSlotBytes);
    CHECK(bv.payload != nullptr);
    std::memset(bv.payload, static_cast<int>(0x10 + f), kSlotBytes);
    w.Commit(NowNs());
    SleepNs(200000);
  }
  if (corrupt_frame >= 0) {
    // Flip a payload byte AFTER commit (transport CRC stays stale): the slot
    // no longer matches its committed crc32.
    ShmRegion rw;
    CHECK(rw.OpenRetry(chan, PinnedSlotWriter::RegionBytes(kSlotBytes,
                                                         kSlotCount, 0),
                       1000000000ULL, /*read_only=*/false));
    uint8_t* slot = static_cast<uint8_t*>(rw.data()) +
                    PinnedSlotWriter::SlotsOffset(0) +
                    static_cast<uint64_t>(corrupt_frame) *
                        PinnedSlotWriter::SlotStride(kSlotBytes) +
                    sizeof(PinnedSlotHeader);
    slot[17] ^= 0xFF;
    rw.Unmap();
  }

  triggered::Config ecfg;
  triggered::SourceConfig sc;
  sc.kind = triggered::SourceConfig::Kind::kPinnedSlot;
  sc.shm_name = chan;
  sc.channel_name = "cloud_1";
  sc.crc = policy;
  ecfg.sources = {sc};
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
  CaptureResult r;
  r.pkg = rec.last_package_dir();
  r.frames_written = rec.stats().frames_written;
  r.holes_written = rec.stats().holes_written;
  rec.Shutdown();
  w.Close();
  ShmRegion::UnlinkName(chan);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(chan).c_str());
  return r;
}

struct IndexRow {
  uint64_t mono_ns;
  triggered::FrameIndexRecord rec;
  bool is_gap;
};

std::vector<IndexRow> ReadIndex(const std::string& pkg) {
  ThymeBinReader rd((pkg + "/frames/cloud_1.index.tbin").c_str());
  CHECK(rd.ok());
  std::vector<IndexRow> rows;
  ThymeBinDataEntry e;
  while (rd.ReadNext(e) == ThymeBinReader::ReadResult::kOk) {
    IndexRow row{};
    row.mono_ns = e.mono_ns;
    row.is_gap = e.type == ThymeBinEntryType::kGap;
    if (!row.is_gap) {
      CHECK(e.type == ThymeBinEntryType::kData);
      CHECK(e.payload.size() == sizeof(triggered::FrameIndexRecord));
      std::memcpy(&row.rec, e.payload.data(), sizeof(row.rec));
    }
    rows.push_back(row);
  }
  return rows;
}

std::vector<uint8_t> ReadRawSegment(const std::string& pkg, uint64_t offset,
                                    uint32_t len) {
  FILE* raw = std::fopen((pkg + "/frames/cloud_1.raw").c_str(), "rb");
  CHECK(raw != nullptr);
  std::vector<uint8_t> seg(len);
  CHECK(fseeko(raw, static_cast<off_t>(offset), SEEK_SET) == 0);
  CHECK(std::fread(seg.data(), 1, len, raw) == len);
  CHECK(std::fclose(raw) == 0);
  return seg;
}

void TestDrainPolicies(const char* tag) {
  char chan[96], out[96];
  const int me = static_cast<int>(getpid());

  // crc=package (the pinned_slot default): transport CRC off, drain skips
  // per-frame verification; the frame index still anchors each segment with
  // a recomputed payload_crc and the REC-18 package manifest verifies.
  {
    std::snprintf(chan, sizeof(chan), "/crcpol_pkg_%s_%d", tag, me);
    std::snprintf(out, sizeof(out), "/tmp/crcpol_pkg_%s_%d", tag, me);
    const CaptureResult r =
        RunCapture(chan, out, CrcPolicy::kDefault, /*transport_crc=*/false,
                   /*corrupt_frame=*/-1);
    CHECK(r.frames_written == kFrameCount);
    CHECK(r.holes_written == 0);
    CHECK(triggered::TriggeredRecorder::PackageComplete(r.pkg));
    const std::vector<IndexRow> rows = ReadIndex(r.pkg);
    CHECK(rows.size() == kFrameCount);
    for (const IndexRow& row : rows) {
      CHECK(!row.is_gap);
      CHECK(row.rec.payload_crc != 0);  // recomputed anchor for --verify
      const std::vector<uint8_t> seg =
          ReadRawSegment(r.pkg, row.rec.offset, row.rec.len);
      CHECK(Crc32(seg.data(), seg.size()) == row.rec.payload_crc);
    }
    // REC-18 path: the package manifest is the integrity boundary.
    const integrity::VerifyResult v =
        integrity::VerifyPackageManifest(r.pkg, "");
    CHECK(v.state == integrity::ManifestState::kOk);
    CHECK(v.ok());
  }

  // crc=frame (unchanged pre-REC-21 behavior): a stale transport CRC is
  // verified at drain and the corrupt frame degrades to a GAP.
  {
    std::snprintf(chan, sizeof(chan), "/crcpol_frm_%s_%d", tag, me);
    std::snprintf(out, sizeof(out), "/tmp/crcpol_frm_%s_%d", tag, me);
    const CaptureResult r =
        RunCapture(chan, out, CrcPolicy::kFrame, /*transport_crc=*/true,
                   /*corrupt_frame=*/2);
    CHECK(r.frames_written == kFrameCount - 1);
    CHECK(r.holes_written == 1);
    const std::vector<IndexRow> rows = ReadIndex(r.pkg);
    CHECK(rows.size() == kFrameCount);
    uint32_t gaps = 0;
    for (const IndexRow& row : rows) {
      if (row.is_gap) ++gaps;
    }
    CHECK(gaps == 1);
  }

  // crc=package on the same corrupted channel: the drain skips the per-frame
  // check and records the frame; the stored (stale) payload_crc no longer
  // matches the bytes, so `thyme-replay verify` still catches it on demand.
  {
    std::snprintf(chan, sizeof(chan), "/crcpol_pkc_%s_%d", tag, me);
    std::snprintf(out, sizeof(out), "/tmp/crcpol_pkc_%s_%d", tag, me);
    const CaptureResult r =
        RunCapture(chan, out, CrcPolicy::kPackage, /*transport_crc=*/true,
                   /*corrupt_frame=*/2);
    CHECK(r.frames_written == kFrameCount);
    CHECK(r.holes_written == 0);
    const std::vector<IndexRow> rows = ReadIndex(r.pkg);
    CHECK(rows.size() == kFrameCount);
    bool saw_stale_anchor = false;
    for (const IndexRow& row : rows) {
      const std::vector<uint8_t> seg =
          ReadRawSegment(r.pkg, row.rec.offset, row.rec.len);
      if (Crc32(seg.data(), seg.size()) != row.rec.payload_crc) {
        saw_stale_anchor = true;
      }
    }
    CHECK(saw_stale_anchor);  // per-frame verify skipped, anchor preserved
  }

  // crc=off: frames drain without any per-frame anchor (payload_crc = 0);
  // only the package manifest covers them.
  {
    std::snprintf(chan, sizeof(chan), "/crcpol_off_%s_%d", tag, me);
    std::snprintf(out, sizeof(out), "/tmp/crcpol_off_%s_%d", tag, me);
    const CaptureResult r =
        RunCapture(chan, out, CrcPolicy::kOff, /*transport_crc=*/false,
                   /*corrupt_frame=*/-1);
    CHECK(r.frames_written == kFrameCount);
    CHECK(r.holes_written == 0);
    const std::vector<IndexRow> rows = ReadIndex(r.pkg);
    CHECK(rows.size() == kFrameCount);
    for (const IndexRow& row : rows) {
      CHECK(row.rec.payload_crc == 0);
    }
    const integrity::VerifyResult v =
        integrity::VerifyPackageManifest(r.pkg, "");
    CHECK(v.ok());
  }
}

}  // namespace

int main(int argc, char** argv) {
  const char* tag = argc > 1 ? argv[1] : "a";
  TestConfigParsing();
  TestFrameHeaderPolicy();
  TestDrainPolicies(tag);
  return 0;
}
