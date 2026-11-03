// test_record_replay_record.cpp — REP-07/REP-14: deterministic
// record -> replay -> record regression.
//
// One synthetic trigger package A is built on a fixed mono_ns grid and fed
// through the real capture -> replay -> capture pipeline:
//
//   A   synthetic trigger package (meta.tbin + small.tbin + frames/*, wal seal,
//       SEC-03 manifest) written through the recorder's own serialization
//       primitives (ThymeBinEncoder/ThymeBinFileSink, FrameIndexRecord, WalRow,
//       integrity::WritePackageManifest) with a fixed timestamp grid:
//         * two small structs at different rates (2 ms / 4 ms);
//         * two big-frame channels at different rates (20 ms / 25 ms);
//         * one trigger row and one GAP hole;
//       The grid is synthetic because TriggeredRecorder harvests on wall-clock
//       windows and turns preempted slots into holes at timing-dependent
//       moments; the on-disk bytes are the ones the recorder's drain writes.
//
//   B   A is replayed as-fast through ReplayPlayer into fresh SHM targets (a
//       mirror ring page plus two PinnedSlotStores). The injected stream is
//       re-recorded by a *real* TriggeredRecorder fed from those targets (a
//       fixed pre-window capture: every sample/frame is already in the targets,
//       so the capture needs no wall-clock wait) and sealed as package C. The
//       round is repeated three times into fresh targets.
//
// Comparisons (A vs C, then C1 vs C2 vs C3):
//   * per-channel event order and mono_ns;
//   * struct_id and source_sequence;
//   * payload bytes;
//   * big-frame frame_flags and payload CRC (index record CRC + target slot
//     CRC, both checked to equal Crc32(payload));
//   * per-channel data counts;
//   * meta.tbin's trigger and two data rows against each capture's expected
//     window/config/statistics, including the raw metadata payload bytes;
//   * the SEC-03 manifest of A and of every C verifies (chain + per-file
//     sha256).
// C1 == C2 == C3 is asserted byte-for-byte on the canonical event stream and
// every deterministic package file (meta/small/frame data and schema). The wal
// descriptor stream and seal envelope are compared separately: only the seal
// row's monotonic commit timestamp may differ.
//
// REP-14 control-row exemption: GAP and trigger rows have no payload injector.
// Replay ignores exactly one of each, so A's hole is not re-recorded and C's
// sole trigger is the new fixed-window capture request, not A's trigger. The
// two meta data rows do have an explicit scratch-ring mapping and are injected;
// that ring is observational only because the real recorder regenerates the
// new capture's own meta.tbin.
//
// A final three-slice stitch replay is also re-recorded twice; C1 == C2 uses
// the same canonical stream and deterministic-file comparison, without A == C.
// Runtime: all rounds take well under a second; there are no sleeps.
// THYME_REP07_REPEATS extends the single-package round count (>= 3) for soak runs.

#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/package_integrity.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/replay/replay_player.h"
#include "thyme/replay/replay_playlist.h"
#include "thyme/replay/replay_reader.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace thyme;

namespace {

constexpr uint64_t kMs = 1000000ULL;
constexpr uint64_t kGridStart = 40 * kMs;  // first event on the grid
constexpr uint64_t kTriggerId = 7;
constexpr uint64_t kT0 = 100 * kMs;        // trigger row in package A
constexpr uint64_t kBefore = 60 * kMs;     // A window: [40 ms, 140 ms]
constexpr uint64_t kAfter = 40 * kMs;
constexpr uint64_t kLastMono = 140 * kMs;  // last event on the grid
constexpr uint64_t kRerecordBefore = kLastMono - kGridStart;  // full window
constexpr uint32_t kFrameSlotBytes = 4096;
constexpr uint32_t kFrameSlots = 64;
constexpr uint32_t kRingDepth = 4096;
constexpr uint32_t kSmallCount0 = 51;  // 40 ms .. 140 ms, every 2 ms
constexpr uint32_t kSmallCount1 = 25;  // 41 ms .. 137 ms, every 4 ms
constexpr uint32_t kFrontCount = 6;
constexpr uint32_t kRearCount = 4;
constexpr uint64_t kSmallData = kSmallCount0 + kSmallCount1;
constexpr uint64_t kFrameData = kFrontCount + kRearCount;
constexpr uint64_t kDataEvents = kSmallData + kFrameData;

const char kSmallChannel[] = "small";
const char kFrontChannel[] = "h264_front";
const char kRearChannel[] = "h264_rear";
const char kMetaChannel[] = "meta";

struct Pod16 {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};

struct Pod32 {
  uint64_t a;
  uint64_t b;
  uint8_t fill[16];
};

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

std::vector<uint8_t> Pod16Bytes(uint32_t seq) {
  Pod16 p{};
  p.a = seq;
  p.b = seq * 2;
  p.c = seq * 3;
  std::vector<uint8_t> out(sizeof(p));
  std::memcpy(out.data(), &p, sizeof(p));
  return out;
}

std::vector<uint8_t> Pod32Bytes(uint32_t seq) {
  Pod32 p{};
  p.a = 0x100000000ULL + seq;
  p.b = 0xFFFFFFFFULL - seq;
  for (uint32_t i = 0; i < sizeof(p.fill); ++i) {
    p.fill[i] = static_cast<uint8_t>((seq * 17 + i * 29) & 0xFFu);
  }
  std::vector<uint8_t> out(sizeof(p));
  std::memcpy(out.data(), &p, sizeof(p));
  return out;
}

std::vector<uint8_t> FrameBytes(uint32_t channel, uint32_t index,
                                uint32_t len) {
  std::vector<uint8_t> out(len);
  for (uint32_t j = 0; j < len; ++j) {
    out[j] = static_cast<uint8_t>((channel * 37 + index * 13 + j * 7) & 0xFFu);
  }
  return out;
}

struct SmallEvent {
  uint64_t mono_ns = 0;
  uint16_t struct_id = 0;
  uint32_t source_sequence = 0;
  std::vector<uint8_t> payload;
};

struct FrameEvent {
  uint64_t mono_ns = 0;
  uint32_t source_sequence = 0;
  uint32_t flags = 0;
  std::vector<uint8_t> payload;
};

struct GapEvent {
  uint64_t mono_ns = 0;
  uint16_t struct_id = 0;
  uint16_t reason = 0;
  uint32_t missed = 0;
};

struct Grid {
  std::vector<SmallEvent> small;
  std::vector<GapEvent> gaps;
  std::vector<FrameEvent> front;
  std::vector<FrameEvent> rear;
};

Grid BuildGrid() {
  Grid g;
  // Mixed rates, no two small samples share a timestamp: ties inside one
  // channel would make the file order the tie-break and hide merge bugs.
  for (uint32_t i = 0; i < kSmallCount0; ++i) {
    SmallEvent e;
    e.mono_ns = 40 * kMs + static_cast<uint64_t>(i) * 2 * kMs;
    e.struct_id = 0;
    e.source_sequence = i;
    e.payload = Pod16Bytes(i);
    g.small.push_back(std::move(e));
  }
  for (uint32_t i = 0; i < kSmallCount1; ++i) {
    SmallEvent e;
    e.mono_ns = 41 * kMs + static_cast<uint64_t>(i) * 4 * kMs;
    e.struct_id = 1;
    e.source_sequence = i;
    e.payload = Pod32Bytes(i);
    g.small.push_back(std::move(e));
  }
  GapEvent gap;
  gap.mono_ns = 63 * kMs;
  gap.struct_id = 0;
  gap.reason = triggered::kGapReasonOverrun;
  gap.missed = 1;
  g.gaps.push_back(gap);

  for (uint32_t i = 0; i < kFrontCount; ++i) {
    FrameEvent e;
    e.mono_ns = 40 * kMs + static_cast<uint64_t>(i) * 20 * kMs;
    e.source_sequence = i;
    e.flags = (i % 3 == 0) ? 1u : 0u;  // sparse IDR bit
    e.payload = FrameBytes(1, i, 512 + (i % 8) * 256);
    g.front.push_back(std::move(e));
  }
  for (uint32_t i = 0; i < kRearCount; ++i) {
    FrameEvent e;
    e.mono_ns = 45 * kMs + static_cast<uint64_t>(i) * 25 * kMs;
    e.source_sequence = i;
    e.flags = (i % 2 == 0) ? 1u : 0u;
    e.payload = FrameBytes(2, i, 768 + ((i * 3) % 5) * 192);
    g.rear.push_back(std::move(e));
  }
  return g;
}

ThymeStructDesc MakeDesc(uint16_t sid, uint32_t payload, const char* name) {
  ThymeStructDesc d{};
  d.struct_id = sid;
  d.contract_version = 1;
  d.payload_size = payload;
  std::snprintf(d.name, sizeof(d.name), "%s", name);
  return d;
}

ThymeStructDesc SmallDesc(uint16_t sid) {
  if (sid == 0) return MakeDesc(0, sizeof(Pod16), "Pod16");
  return MakeDesc(1, sizeof(Pod32), "Pod32");
}

void DataRingSpecs(ThymeStructDesc* descs, RecordMirrorRingSpec* specs) {
  descs[0] = SmallDesc(0);
  descs[1] = SmallDesc(1);
  specs[0] = RecordMirrorRingSpec{sizeof(Pod16), kRingDepth};
  specs[1] = RecordMirrorRingSpec{sizeof(Pod32), kRingDepth};
}

void EnsureDirs(const std::string& dir) {
  std::filesystem::create_directories(dir + "/frames");
}

// ---------------------------------------------------------------------------
// Package A: same file set and record layout the triggered drain produces.
// ---------------------------------------------------------------------------

void WriteMetaTbin(const std::string& dir, uint64_t config_hash) {
  const std::vector<ThymeStructDesc> regs = {
      SmallDesc(0), SmallDesc(1),
      MakeDesc(triggered::kFrameIndexStructId,
               static_cast<uint32_t>(sizeof(triggered::FrameIndexRecord)),
               "FrameIdx"),
      MakeDesc(triggered::kMetaStructId,
               static_cast<uint32_t>(sizeof(triggered::TriggerMetaRecord)),
               "TrigMeta")};
  ThymeBinFileSink sink((dir + "/meta.tbin").c_str());
  CHECK(sink.ok());
  ThymeBinEncoder enc(&sink);
  CHECK(enc.WriteHeader(kTriggerId, kGridStart, regs.data(), regs.size()));
  CHECK(enc.WriteTrigger(kTriggerId, kT0, kBefore, kAfter));
  const triggered::TriggerMetaRecord tm{kT0, kBefore, kAfter, config_hash};
  CHECK(enc.WriteData(0, triggered::kMetaStructId, kT0, 0, &tm, sizeof(tm)));
  triggered::CaptureStatsRecord csr{};
  csr.descriptors = kDataEvents + 1;  // data + one hole
  csr.samples_written = kSmallData;
  csr.frames_written = kFrameData;
  csr.holes_written = 1;
  CHECK(enc.WriteData(1, triggered::kMetaStructId, kT0, 1, &csr, sizeof(csr)));
  CHECK(enc.Finish());
  CHECK(sink.Sync());
}

struct SmallRow {
  uint64_t mono_ns = 0;
  uint16_t struct_id = 0;
  bool gap = false;
  const SmallEvent* data = nullptr;
  const GapEvent* hole = nullptr;
};

void WriteSmallTbin(const std::string& dir, const Grid& g) {
  std::vector<SmallRow> rows;
  rows.reserve(g.small.size() + g.gaps.size());
  for (const SmallEvent& e : g.small) {
    rows.push_back(SmallRow{e.mono_ns, e.struct_id, false, &e, nullptr});
  }
  for (const GapEvent& e : g.gaps) {
    rows.push_back(SmallRow{e.mono_ns, e.struct_id, true, nullptr, &e});
  }
  std::sort(rows.begin(), rows.end(), [](const SmallRow& a, const SmallRow& b) {
    if (a.mono_ns != b.mono_ns) return a.mono_ns < b.mono_ns;
    if (a.struct_id != b.struct_id) return a.struct_id < b.struct_id;
    return !a.gap && b.gap;  // data first on a full tie
  });
  const ThymeStructDesc regs[2] = {SmallDesc(0), SmallDesc(1)};
  ThymeBinFileSink sink((dir + "/small.tbin").c_str());
  CHECK(sink.ok());
  ThymeBinEncoder enc(&sink);
  CHECK(enc.WriteHeader(kTriggerId, kGridStart, regs, 2));
  uint64_t rec_seq = 0;
  for (const SmallRow& r : rows) {
    if (r.gap) {
      CHECK(enc.WriteGap(r.hole->struct_id, r.hole->mono_ns, r.hole->reason,
                         r.hole->missed));
    } else {
      CHECK(enc.WriteData(rec_seq++, r.struct_id, r.data->mono_ns,
                          r.data->source_sequence, r.data->payload.data(),
                          static_cast<uint32_t>(r.data->payload.size())));
    }
  }
  CHECK(enc.Finish());
  CHECK(sink.Sync());
}

void WriteFrameChannel(const std::string& dir, const char* name,
                       const std::vector<FrameEvent>& frames) {
  const std::string base = dir + "/frames/" + name;
  FILE* raw = std::fopen((base + ".raw").c_str(), "wb");
  CHECK(raw != nullptr);
  ThymeBinFileSink sink((base + ".index.tbin").c_str());
  CHECK(sink.ok());
  ThymeBinEncoder enc(&sink);
  const ThymeStructDesc regs[2] = {
      MakeDesc(triggered::kFrameIndexStructId,
               static_cast<uint32_t>(sizeof(triggered::FrameIndexRecord)),
               "FrameIdx"),
      MakeDesc(triggered::kFrameGapStructId, 0, "FrameGap")};
  CHECK(enc.WriteHeader(kTriggerId, kGridStart, regs, 2));
  uint64_t offset = 0;
  uint64_t rec_seq = 0;
  for (const FrameEvent& f : frames) {
    const uint32_t len = static_cast<uint32_t>(f.payload.size());
    CHECK(std::fwrite(f.payload.data(), 1, len, raw) == len);
    const uint32_t crc = Crc32(f.payload.data(), len);
    const triggered::FrameIndexRecord idx{offset, len, f.flags, crc};
    CHECK(enc.WriteData(rec_seq++, triggered::kFrameIndexStructId, f.mono_ns,
                        f.source_sequence, &idx, sizeof(idx)));
    offset += len;
  }
  CHECK(enc.Finish());
  CHECK(sink.Sync());
  CHECK(std::fflush(raw) == 0);
  CHECK(std::fclose(raw) == 0);
}

void WriteWal(const std::string& dir, const Grid& g) {
  FILE* f = std::fopen((dir + "/wal.log").c_str(), "wb");
  CHECK(f != nullptr);
  const triggered::WalHeader wh{{'T', 'R', 'I', 'G', 'W', 'A', 'L', '1'},
                                kTriggerId};
  CHECK(std::fwrite(&wh, sizeof(wh), 1, f) == 1);
  struct WalDesc {
    uint8_t result;
    uint16_t struct_id;
    uint32_t len;
    uint64_t index;
    uint64_t mono_ns;
  };
  std::vector<WalDesc> rows;
  for (const SmallEvent& e : g.small) {
    rows.push_back(WalDesc{triggered::kWalOk, e.struct_id,
                           static_cast<uint32_t>(e.payload.size()),
                           e.source_sequence, e.mono_ns});
  }
  for (const GapEvent& e : g.gaps) {
    rows.push_back(WalDesc{triggered::kWalHole, e.struct_id, 0, 0, e.mono_ns});
  }
  for (const FrameEvent& e : g.front) {
    rows.push_back(WalDesc{triggered::kWalOk, 0,
                           static_cast<uint32_t>(e.payload.size()),
                           e.source_sequence, e.mono_ns});
  }
  for (const FrameEvent& e : g.rear) {
    rows.push_back(WalDesc{triggered::kWalOk, 0,
                           static_cast<uint32_t>(e.payload.size()),
                           e.source_sequence, e.mono_ns});
  }
  std::sort(rows.begin(), rows.end(), [](const WalDesc& a, const WalDesc& b) {
    if (a.mono_ns != b.mono_ns) return a.mono_ns < b.mono_ns;
    if (a.struct_id != b.struct_id) return a.struct_id < b.struct_id;
    return a.index < b.index;
  });
  for (const WalDesc& w : rows) {
    const triggered::WalRow row{triggered::kWalOpDescriptor, w.result,
                                w.struct_id, w.len, w.index, w.mono_ns};
    CHECK(std::fwrite(&row, sizeof(row), 1, f) == 1);
  }
  const triggered::WalRow seal{triggered::kWalOpSeal, 0, 0, 0, 0, 0};
  CHECK(std::fwrite(&seal, sizeof(seal), 1, f) == 1);
  CHECK(std::fflush(f) == 0);
  CHECK(std::fclose(f) == 0);
}

void SealPackageManifest(const std::string& dir, uint64_t t0, uint64_t before,
                         uint64_t after, uint64_t seal_time_ns) {
  integrity::PackageInfo info;
  info.kind = integrity::PackageKind::kTrigger;
  info.seal_time_ns = seal_time_ns;
  info.session_id = kTriggerId;
  info.trigger_id = kTriggerId;
  info.t0_ns = t0;
  info.before_ns = before;
  info.after_ns = after;
  std::string err;
  CHECK(integrity::WritePackageManifest(dir, info, "", &err));
}

void WritePackageA(const std::string& dir, const Grid& g) {
  EnsureDirs(dir);
  ThymeStructDesc descs[2];
  RecordMirrorRingSpec specs[2];
  DataRingSpecs(descs, specs);
  const uint64_t config_hash = RecordMirrorRingPage::HashConfig(descs, specs, 2);
  WriteMetaTbin(dir, config_hash);
  WriteSmallTbin(dir, g);
  WriteFrameChannel(dir, kFrontChannel, g.front);
  WriteFrameChannel(dir, kRearChannel, g.rear);
  WriteWal(dir, g);
  SealPackageManifest(dir, kT0, kBefore, kAfter, 1700000000000000000ULL);
}

// ---------------------------------------------------------------------------
// Canonical event stream (A and every re-recording are compared through it).
// ---------------------------------------------------------------------------

uint32_t RankOf(const std::string& name) {
  if (name == kSmallChannel) return 1;
  if (name == kFrontChannel) return 2;
  if (name == kRearChannel) return 3;
  return 0;  // meta has a separate capture-envelope comparison
}

struct CanonEvent {
  uint32_t rank = 0;
  uint64_t mono_ns = 0;
  uint16_t struct_id = 0;
  uint32_t source_sequence = 0;
  uint32_t flags = 0;
  uint32_t crc = 0;
  std::vector<uint8_t> payload;
};

struct HoleInfo {
  std::string channel;
  uint64_t mono_ns = 0;
  uint16_t struct_id = 0;
  uint16_t reason = 0;
  uint32_t missed = 0;
};

struct TriggerInfo {
  uint64_t mono_ns = 0;
  uint64_t trigger_id = 0;
  uint64_t before_ns = 0;
  uint64_t after_ns = 0;
};

struct Scan {
  std::vector<std::string> channel_names;
  std::vector<CanonEvent> data;
  std::vector<CanonEvent> meta;
  std::map<std::string, uint64_t> data_per_channel;
  std::map<std::string, uint64_t> gaps_per_channel;
  std::vector<HoleInfo> holes;
  std::vector<TriggerInfo> triggers;
  uint64_t sessions = 0;
  uint64_t first_mono_ns = 0;
  uint64_t last_mono_ns = 0;
};

Scan ScanPackage(const std::string& dir) {
  replay::ReplayPackage pkg;
  CHECK(pkg.Open(dir) == replay::ReplayStatus::kOk);
  Scan out;
  for (const replay::ReplayChannel& c : pkg.channels()) {
    out.channel_names.push_back(c.name);
  }
  out.first_mono_ns = pkg.first_mono_ns();
  out.last_mono_ns = pkg.last_mono_ns();
  replay::ReplayEvent ev;
  while (pkg.NextEvent(&ev) == replay::ReplayStatus::kOk) {
    const std::string& name = pkg.channels()[ev.channel].name;
    switch (ev.kind) {
      case replay::ReplayKind::kData: {
        const uint32_t rank = RankOf(name);
        CanonEvent c;
        c.rank = rank;
        c.mono_ns = ev.mono_ns;
        c.struct_id = static_cast<uint16_t>(ev.struct_id);
        c.source_sequence = ev.source_sequence;
        c.flags = ev.frame_flags;
        c.crc = ev.payload_crc;
        if (ev.len != 0) {
          CHECK(ev.payload != nullptr);
          c.payload.assign(ev.payload, ev.payload + ev.len);
        }
        if (rank == 0) {
          CHECK(name == kMetaChannel);
          // REC-27 payload-profile rows are control metadata: they depend on
          // the recorder's channel configuration, not on the replayed data
          // stream. Package A (hand-written fixture) has none while a real
          // re-recording writes one per big-frame channel, so comparing them
          // would test the fixture, not determinism. Coverage for the rows
          // themselves lives in test_payload_profile.
          if (ev.struct_id == triggered::kPayloadProfileStructId) {
            continue;
          }
          CHECK(ev.struct_id == triggered::kMetaStructId);
          CHECK(ev.source_sequence <= 1);
          out.meta.push_back(std::move(c));
        } else {
          out.data.push_back(std::move(c));
          out.data_per_channel[name] += 1;
        }
        break;
      }
      case replay::ReplayKind::kGap: {
        HoleInfo h;
        h.channel = name;
        h.mono_ns = ev.mono_ns;
        h.struct_id = static_cast<uint16_t>(ev.struct_id);
        h.reason = ev.gap_reason;
        h.missed = ev.source_sequence;
        out.holes.push_back(h);
        out.gaps_per_channel[name] += 1;
        break;
      }
      case replay::ReplayKind::kTrigger: {
        CHECK(name == kMetaChannel);
        out.triggers.push_back(
            TriggerInfo{ev.mono_ns, ev.trigger_id, ev.pre_roll_ns, ev.post_roll_ns});
        break;
      }
      case replay::ReplayKind::kSession:
        out.sessions += 1;
        break;
    }
  }
  return out;
}

void AppendU16(std::vector<uint8_t>* out, uint16_t v) {
  out->insert(out->end(), reinterpret_cast<const uint8_t*>(&v),
              reinterpret_cast<const uint8_t*>(&v) + sizeof(v));
}

void AppendU32(std::vector<uint8_t>* out, uint32_t v) {
  out->insert(out->end(), reinterpret_cast<const uint8_t*>(&v),
              reinterpret_cast<const uint8_t*>(&v) + sizeof(v));
}

void AppendU64(std::vector<uint8_t>* out, uint64_t v) {
  out->insert(out->end(), reinterpret_cast<const uint8_t*>(&v),
              reinterpret_cast<const uint8_t*>(&v) + sizeof(v));
}

// Fixed-width, self-describing serialization of the compared fields. Byte
// comparison of these buffers is the determinism assertion.
std::vector<uint8_t> CanonBytes(const std::vector<CanonEvent>& events) {
  std::vector<uint8_t> out;
  out.reserve(events.size() * 48);
  for (const CanonEvent& e : events) {
    AppendU32(&out, e.rank);
    AppendU64(&out, e.mono_ns);
    AppendU16(&out, e.struct_id);
    AppendU32(&out, e.source_sequence);
    AppendU32(&out, e.flags);
    AppendU32(&out, e.crc);
    AppendU32(&out, static_cast<uint32_t>(e.payload.size()));
    out.insert(out.end(), e.payload.begin(), e.payload.end());
  }
  return out;
}

// Expected stream built straight from the grid, independent of package A:
// it checks the package writer itself, not just A-vs-C equality.
std::vector<CanonEvent> ExpectedStream(const Grid& g) {
  std::vector<CanonEvent> out;
  for (const SmallEvent& e : g.small) {
    CanonEvent c;
    c.rank = RankOf(kSmallChannel);
    c.mono_ns = e.mono_ns;
    c.struct_id = e.struct_id;
    c.source_sequence = e.source_sequence;
    c.payload = e.payload;
    out.push_back(std::move(c));
  }
  for (const FrameEvent& e : g.front) {
    CanonEvent c;
    c.rank = RankOf(kFrontChannel);
    c.mono_ns = e.mono_ns;
    c.struct_id = triggered::kFrameIndexStructId;
    c.source_sequence = e.source_sequence;
    c.flags = e.flags;
    c.crc = Crc32(e.payload.data(), e.payload.size());
    c.payload = e.payload;
    out.push_back(std::move(c));
  }
  for (const FrameEvent& e : g.rear) {
    CanonEvent c;
    c.rank = RankOf(kRearChannel);
    c.mono_ns = e.mono_ns;
    c.struct_id = triggered::kFrameIndexStructId;
    c.source_sequence = e.source_sequence;
    c.flags = e.flags;
    c.crc = Crc32(e.payload.data(), e.payload.size());
    c.payload = e.payload;
    out.push_back(std::move(c));
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const CanonEvent& a, const CanonEvent& b) {
                     if (a.mono_ns != b.mono_ns) return a.mono_ns < b.mono_ns;
                     return a.rank < b.rank;
                   });
  return out;
}

void CheckExpectedStream(const std::vector<CanonEvent>& got,
                         const std::vector<CanonEvent>& want) {
  CHECK(got.size() == want.size());
  for (std::size_t i = 0; i < got.size(); ++i) {
    CHECK(got[i].rank == want[i].rank);
    CHECK(got[i].mono_ns == want[i].mono_ns);
    CHECK(got[i].struct_id == want[i].struct_id);
    CHECK(got[i].source_sequence == want[i].source_sequence);
    CHECK(got[i].flags == want[i].flags);
    CHECK(got[i].crc == want[i].crc);
    CHECK(got[i].payload == want[i].payload);
  }
}

void CheckTrigger(const Scan& scan, uint64_t t0, uint64_t before,
                  uint64_t after) {
  CHECK(scan.triggers.size() == 1);
  CHECK(scan.triggers[0].mono_ns == t0);
  CHECK(scan.triggers[0].trigger_id == kTriggerId);
  CHECK(scan.triggers[0].before_ns == before);
  CHECK(scan.triggers[0].after_ns == after);
}

void CheckMeta(const Scan& scan, uint64_t t0, uint64_t before, uint64_t after,
               uint64_t config_hash, uint64_t holes) {
  CheckTrigger(scan, t0, before, after);
  CHECK(scan.meta.size() == 2);
  const triggered::TriggerMetaRecord meta{t0, before, after, config_hash};
  const triggered::CaptureStatsRecord stats{kDataEvents + holes,
                                            kSmallData,
                                            kFrameData,
                                            holes,
                                            0,
                                            0,
                                            0};
  for (std::size_t i = 0; i < scan.meta.size(); ++i) {
    const CanonEvent& got = scan.meta[i];
    CHECK(got.rank == 0);
    CHECK(got.mono_ns == t0);
    CHECK(got.struct_id == triggered::kMetaStructId);
    CHECK(got.source_sequence == i);
    CHECK(got.flags == 0);
    CHECK(got.crc == 0);
    if (i == 0) {
      CHECK(got.payload.size() == sizeof(meta));
      CHECK(std::memcmp(got.payload.data(), &meta, sizeof(meta)) == 0);
    } else {
      CHECK(got.payload.size() == sizeof(stats));
      CHECK(std::memcmp(got.payload.data(), &stats, sizeof(stats)) == 0);
    }
  }
}

// One source of truth for "the same stream": order, timestamps, ids, flags,
// CRC and payload bytes are all compared element by element.
void CheckSameStream(const std::vector<uint8_t>& a,
                     const std::vector<uint8_t>& b, const char* what) {
  if (a == b) return;
  CHECK(a.size() == b.size());
  std::size_t at = 0;
  while (at < a.size() && a[at] == b[at]) ++at;
  std::fprintf(stderr, "%s: streams differ at byte %zu (%zu vs %zu bytes)\n",
               what, at, a.size(), b.size());
  CHECK(false);
}

std::vector<uint8_t> ReadFileBytes(const std::string& path) {
  const uint64_t size = std::filesystem::file_size(path);
  std::vector<uint8_t> out(static_cast<std::size_t>(size));
  FILE* f = std::fopen(path.c_str(), "rb");
  CHECK(f != nullptr);
  if (!out.empty()) {
    CHECK(std::fread(out.data(), 1, out.size(), f) == out.size());
  }
  CHECK(std::fclose(f) == 0);
  return out;
}

void CheckDeterministicFiles(const std::string& a, const std::string& b) {
  static const char* const kFiles[] = {
      "meta.tbin",
      "small.tbin",
      "frames/h264_front.index.tbin",
      "frames/h264_front.raw",
      "frames/h264_rear.index.tbin",
      "frames/h264_rear.raw",
      "schema.json",
  };
  for (const char* file : kFiles) {
    CheckSameStream(ReadFileBytes(a + "/" + file),
                    ReadFileBytes(b + "/" + file), file);
  }
}

void CheckWalEnvelope(const std::string& a, const std::string& b) {
  const std::vector<uint8_t> bytes[2] = {ReadFileBytes(a + "/wal.log"),
                                         ReadFileBytes(b + "/wal.log")};
  std::vector<triggered::WalRow> stable[2];
  for (std::size_t side = 0; side < 2; ++side) {
    const auto& input = bytes[side];
    CHECK(input.size() >=
          sizeof(triggered::WalHeader) + sizeof(triggered::WalRow));
    CHECK((input.size() - sizeof(triggered::WalHeader)) %
              sizeof(triggered::WalRow) ==
          0);
    for (std::size_t offset = sizeof(triggered::WalHeader); offset < input.size();
         offset += sizeof(triggered::WalRow)) {
      triggered::WalRow row{};
      std::memcpy(&row, input.data() + offset, sizeof(row));
      if (row.op == triggered::kWalOpClockAnchor) {
        CHECK(row.result == triggered::kWalOk);
        CHECK(row.index != 0);
        CHECK(row.mono_ns != 0);
      } else if (row.op == triggered::kWalOpLifecycle) {
        CHECK(row.len == 0);
        CHECK(row.index != 0);
        CHECK(row.mono_ns != 0);
      } else {
        stable[side].push_back(row);
      }
    }
    triggered::PackageLifecycleState lifecycle;
    CHECK(triggered::ReadPackageLifecycle(side == 0 ? a : b, &lifecycle));
    CHECK(lifecycle.state == triggered::PackageLifecycle::kSealed);
    CHECK(lifecycle.abort_reason == triggered::PackageAbortReason::kNone);
    CHECK(!stable[side].empty());
    const triggered::WalRow& seal = stable[side].back();
    CHECK(seal.op == triggered::kWalOpSeal);
    CHECK(seal.result == 0);
    CHECK(seal.struct_id == 0);
    CHECK(seal.len == 0);
    CHECK(seal.index == 0);
    CHECK(seal.mono_ns != 0);
  }
  CHECK(std::memcmp(bytes[0].data(), bytes[1].data(),
                    sizeof(triggered::WalHeader)) == 0);
  CHECK(stable[0].size() == stable[1].size());
  for (std::size_t i = 0; i < stable[0].size(); ++i) {
    const auto& left = stable[0][i];
    const auto& right = stable[1][i];
    CHECK(std::memcmp(&left, &right, offsetof(triggered::WalRow, mono_ns)) == 0);
    if (left.op != triggered::kWalOpSeal) {
      CHECK(left.mono_ns == right.mono_ns);
    }
  }
}

uint32_t FindChannel(replay::ReplayPackage* pkg, const char* name) {
  for (const replay::ReplayChannel& c : pkg->channels()) {
    if (c.name == name) return c.id;
  }
  CHECK(false);
  return 0;
}

// ---------------------------------------------------------------------------
// Fresh replay targets: one mirror ring page for the data structs, one for
// the meta rows (scratch), and one PinnedSlotStore per frame channel.
// ---------------------------------------------------------------------------

struct Targets {
  std::string data_name;
  std::string meta_name;
  std::string front_name;
  std::string rear_name;
  record_mirror::RingClient data;
  record_mirror::RingClient meta;
  PinnedSlotWriter front;
  PinnedSlotWriter rear;

  void Init(uint32_t round) {
    char buf[160];
    const long me = static_cast<long>(getpid());
    std::snprintf(buf, sizeof(buf), "/thyme_rep07_%ld_%u_data", me, round);
    data_name = buf;
    std::snprintf(buf, sizeof(buf), "/thyme_rep07_%ld_%u_meta", me, round);
    meta_name = buf;
    std::snprintf(buf, sizeof(buf), "/thyme_rep07_%ld_%u_front", me, round);
    front_name = buf;
    std::snprintf(buf, sizeof(buf), "/thyme_rep07_%ld_%u_rear", me, round);
    rear_name = buf;
    ShmRegion::UnlinkName(data_name.c_str());
    ShmRegion::UnlinkName(meta_name.c_str());
    ShmRegion::UnlinkName(front_name.c_str());
    ShmRegion::UnlinkName(rear_name.c_str());

    ThymeStructDesc descs[2];
    RecordMirrorRingSpec specs[2];
    DataRingSpecs(descs, specs);
    CHECK(data.Create(data_name.c_str(), descs, specs, 2, 9));
    const ThymeStructDesc meta_desc =
        MakeDesc(triggered::kMetaStructId,
                 static_cast<uint32_t>(sizeof(triggered::CaptureStatsRecord)),
                 "TrigMeta");
    const RecordMirrorRingSpec meta_spec{sizeof(triggered::CaptureStatsRecord),
                                         16};
    CHECK(meta.Create(meta_name.c_str(), &meta_desc, &meta_spec, 1, 9));
    CHECK(front.Create(front_name.c_str(), kFrameSlotBytes, kFrameSlots, 0));
    CHECK(rear.Create(rear_name.c_str(), kFrameSlotBytes, kFrameSlots, 0));
    // REC-21: transport CRC defaults off; these checks compare slot crc32.
    front.SetCrcEnabled(true);
    rear.SetCrcEnabled(true);
  }

  void Close() {
    front.Close();
    rear.Close();
    ShmRegion::UnlinkName(front_name.c_str());
    ShmRegion::UnlinkName(
        PinnedSlotWriter::LeaseShmName(front_name.c_str()).c_str());
    ShmRegion::UnlinkName(rear_name.c_str());
    ShmRegion::UnlinkName(
        PinnedSlotWriter::LeaseShmName(rear_name.c_str()).c_str());
    data.Shutdown(true);
    meta.Shutdown(true);
  }
};

replay::ReplayPlayerConfig MakePlayerConfig(
    uint32_t small_ch, uint32_t meta_ch, uint32_t front_ch, uint32_t rear_ch,
    Targets* t) {
  replay::ReplayPlayerConfig cfg;
  // REP-12: pin the replay session envelope so embedded headers would also be
  // deterministic if a future grid payload used Frame<T>.
  cfg.header_policy.session_id = 0x14E14ULL;
  cfg.pinned_slots.push_back(replay::PinnedSlotTarget{front_ch, &t->front});
  cfg.pinned_slots.push_back(replay::PinnedSlotTarget{rear_ch, &t->rear});
  replay::SmallStructTarget s0;
  s0.channel = small_ch;
  s0.struct_id = 0;
  s0.ring = t->data.Page();
  s0.target_struct_id = 0;
  cfg.small_structs.push_back(s0);
  replay::SmallStructTarget s1;
  s1.channel = small_ch;
  s1.struct_id = 1;
  s1.ring = t->data.Page();
  s1.target_struct_id = 1;
  cfg.small_structs.push_back(s1);
  replay::SmallStructTarget sm;
  sm.channel = meta_ch;
  sm.struct_id = triggered::kMetaStructId;
  sm.ring = t->meta.Page();
  sm.target_struct_id = 0;
  cfg.small_structs.push_back(sm);
  return cfg;
}

void CheckTargetRing(Targets& t, const Grid& g) {
  RecordMirrorRingPage* page = t.data.Page();
  CHECK(page->RingHead(0) == kSmallCount0);
  CHECK(page->RingHead(1) == kSmallCount1);
  uint8_t buf[256];
  for (uint32_t struct_id = 0; struct_id < 2; ++struct_id) {
    const std::size_t count = struct_id == 0 ? kSmallCount0 : kSmallCount1;
    for (std::size_t i = 0; i < count; ++i) {
      const SmallEvent& want = struct_id == 0 ? g.small[i]
                                              : g.small[kSmallCount0 + i];
      uint32_t size = 0;
      uint64_t mono = 0;
      uint32_t seq = 0;
      CHECK(page->ReadRingSample(static_cast<uint16_t>(struct_id), i, buf,
                                 sizeof(buf), size, mono, seq));
      CHECK(size == want.payload.size());
      CHECK(mono == want.mono_ns);
      CHECK(seq == want.source_sequence);
      CHECK(std::memcmp(buf, want.payload.data(), size) == 0);
    }
  }
  CHECK(t.meta.Page()->RingHead(0) == 2);  // trigger meta + stats rows
}

void CheckTargetFrames(const std::string& name,
                       const std::vector<FrameEvent>& want) {
  PinnedSlotReader reader;
  CHECK(reader.Open(name.c_str(), 2000000000ULL));
  CHECK(reader.head() == want.size());
  for (std::size_t i = 0; i < want.size(); ++i) {
    PinnedSlotReader::View v;
    CHECK(reader.ReadFrame(i, v) == PinnedSlotReader::ReadResult::kOk);
    CHECK(v.mono_ns == want[i].mono_ns);
    CHECK(v.data_len == want[i].payload.size());
    CHECK(v.flags == want[i].flags);
    CHECK(v.crc_ok);
    CHECK(v.crc32 == Crc32(want[i].payload.data(), want[i].payload.size()));
    CHECK(std::memcmp(v.payload, want[i].payload.data(), v.data_len) == 0);
  }
  reader.Close();
}

// ---------------------------------------------------------------------------
// One record -> replay -> record round.
// ---------------------------------------------------------------------------

struct Round {
  std::string pkg_dir;
  std::vector<uint8_t> canon;
  Scan scan;
  replay::PlayerStats player{};
};

Round RunRound(const std::string& root, uint32_t round,
               const std::string& package_a, const Grid& grid) {
  replay::ReplayPackage pkg;
  CHECK(pkg.Open(package_a) == replay::ReplayStatus::kOk);
  const uint32_t small_ch = FindChannel(&pkg, kSmallChannel);
  const uint32_t front_ch = FindChannel(&pkg, kFrontChannel);
  const uint32_t rear_ch = FindChannel(&pkg, kRearChannel);
  const uint32_t meta_ch = FindChannel(&pkg, kMetaChannel);
  pkg.Close();

  Targets t;
  t.Init(round);

  // ---- B: as-fast replay into the fresh targets ----
  replay::ReplayPlayer player;
  CHECK(player.Open(package_a, MakePlayerConfig(small_ch, meta_ch, front_ch,
                                                rear_ch, &t)) ==
        replay::ReplayStatus::kOk);
  player.AsFast(true);
  CHECK(player.Run() == replay::ReplayStatus::kOk);
  const replay::PlayerStats& st = player.stats();
  CHECK(st.events == kDataEvents + 4);  // + trigger, 2 meta rows, 1 hole
  CHECK(st.injected == kDataEvents + 2);
  CHECK(st.failed == 0);
  CHECK(st.skipped == 0);
  CHECK(st.ignored == 2);  // REP-14 exemption: exactly trigger + hole
  const replay::PlayerChannelStats& meta_stats = player.channels()[meta_ch];
  CHECK(meta_stats.events == 3);
  CHECK(meta_stats.injected == 2);
  CHECK(meta_stats.ignored == 1);  // kTrigger: no payload target, not replayed
  CHECK(meta_stats.skipped == 0);
  const replay::PlayerChannelStats& small_stats = player.channels()[small_ch];
  CHECK(small_stats.events == kSmallData + 1);
  CHECK(small_stats.injected == kSmallData);
  CHECK(small_stats.ignored == 1);  // kGap: no payload target, not replayed
  CHECK(small_stats.skipped == 0);
  CHECK(st.mono_regressions == 0);
  CHECK(st.first_event_ns == kGridStart);
  CHECK(st.last_event_ns == kLastMono);
  CHECK(st.last_skew_ns == 0);  // as-fast: no pacing clock
  CHECK(player.pinned_slot_injector().stats().sequence_errors == 0);
  CHECK(player.pinned_slot_injector().stats().crc_mismatches == 0);
  CHECK(player.small_struct_injector().stats().sequence_errors == 0);

  CheckTargetRing(t, grid);
  CheckTargetFrames(t.front_name, grid.front);
  CheckTargetFrames(t.rear_name, grid.rear);

  // ---- C: re-record the injected targets with the real recorder ----
  std::string c_out = root + "/round" + std::to_string(round) + "/c";
  std::filesystem::remove_all(c_out);
  triggered::Config cfg;
  cfg.sources.push_back(triggered::SourceConfig{
      triggered::SourceConfig::Kind::kMirrorRing, t.data_name, ""});
  cfg.sources.push_back(triggered::SourceConfig{
      triggered::SourceConfig::Kind::kPinnedSlot, t.front_name, kFrontChannel});
  cfg.sources.push_back(triggered::SourceConfig{
      triggered::SourceConfig::Kind::kPinnedSlot, t.rear_name, kRearChannel});
  cfg.out_dir = c_out;
  // Every event is already in the targets and the whole grid is in the pre
  // window, so the post window is empty: no wall-clock wait.
  cfg.post_grace_ns = 0;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(cfg));
  CHECK(rec.Trigger(kTriggerId, kRerecordBefore, 0, kLastMono));
  const uint64_t deadline = NowNs() + 15000000000ULL;
  while (rec.phase() != triggered::Phase::kArmed) {
    rec.Tick();
    CHECK(NowNs() < deadline);
  }
  CHECK(rec.stats().captures_done == 1);
  CHECK(rec.stats().samples_written == kSmallData);
  CHECK(rec.stats().frames_written == kFrameData);
  CHECK(rec.stats().holes_written == 0);
  CHECK(rec.stats().pending_evicted == 0);
  Round out;
  out.pkg_dir = rec.last_package_dir();
  CHECK(!out.pkg_dir.empty());
  CHECK(triggered::TriggeredRecorder::PackageComplete(out.pkg_dir));
  rec.Shutdown();
  t.Close();

  out.player = st;
  out.scan = ScanPackage(out.pkg_dir);
  out.canon = CanonBytes(out.scan.data);
  return out;
}

Round RunStitchRound(const std::string& root, uint32_t round,
                     const std::vector<std::string>& package_dirs) {
  replay::ReplayPackage pkg;
  CHECK(pkg.Open(package_dirs.front()) == replay::ReplayStatus::kOk);
  const uint32_t small_ch = FindChannel(&pkg, kSmallChannel);
  const uint32_t front_ch = FindChannel(&pkg, kFrontChannel);
  const uint32_t rear_ch = FindChannel(&pkg, kRearChannel);
  const uint32_t meta_ch = FindChannel(&pkg, kMetaChannel);
  pkg.Close();

  Targets t;
  t.Init(20 + round);
  replay::ReplayPlaylist playlist;
  replay::ReplayPlayerConfig player_cfg =
      MakePlayerConfig(small_ch, meta_ch, front_ch, rear_ch, &t);
  player_cfg.stitch_timeline = true;
  CHECK(playlist.Open(package_dirs, player_cfg) == replay::ReplayStatus::kOk);
  CHECK(playlist.slices().size() == 3);
  const uint64_t first_ns = playlist.slices().front().timeline_first_ns;
  const uint64_t last_ns = playlist.slices().back().timeline_last_ns;
  playlist.player().AsFast(true);
  CHECK(playlist.Run() == replay::ReplayStatus::kOk);
  CHECK(playlist.stats().mono_regressions == 0);
  CHECK(playlist.stats().join_duplicates == 0);
  CHECK(playlist.stats().injected == kDataEvents + 6);
  CHECK(playlist.stats().failed == 0);

  const std::string c_out = root + "/stitch_round" + std::to_string(round) + "/c";
  triggered::Config cfg;
  cfg.sources.push_back(triggered::SourceConfig{
      triggered::SourceConfig::Kind::kMirrorRing, t.data_name, ""});
  cfg.sources.push_back(triggered::SourceConfig{
      triggered::SourceConfig::Kind::kPinnedSlot, t.front_name, kFrontChannel});
  cfg.sources.push_back(triggered::SourceConfig{
      triggered::SourceConfig::Kind::kPinnedSlot, t.rear_name, kRearChannel});
  cfg.out_dir = c_out;
  cfg.post_grace_ns = 0;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(cfg));
  CHECK(rec.Trigger(kTriggerId, last_ns - first_ns, 0, last_ns));
  const uint64_t deadline = NowNs() + 15000000000ULL;
  while (rec.phase() != triggered::Phase::kArmed) {
    rec.Tick();
    CHECK(NowNs() < deadline);
  }
  CHECK(rec.stats().captures_done == 1);
  CHECK(rec.stats().samples_written == kSmallData);
  CHECK(rec.stats().frames_written == kFrameData);
  CHECK(rec.stats().holes_written == 0);
  Round out;
  out.pkg_dir = rec.last_package_dir();
  CHECK(!out.pkg_dir.empty());
  CHECK(triggered::TriggeredRecorder::PackageComplete(out.pkg_dir));
  rec.Shutdown();
  out.player = playlist.stats();
  playlist.Close();
  t.Close();
  out.scan = ScanPackage(out.pkg_dir);
  out.canon = CanonBytes(out.scan.data);
  return out;
}

// ---------------------------------------------------------------------------
// SEC-03: a replayed package C is a normal sealed package and must verify.
// ---------------------------------------------------------------------------

void CheckManifest(const std::string& dir, uint64_t t0, uint64_t before,
                   uint64_t after, const char* what) {
  const integrity::VerifyResult r =
      integrity::VerifyPackageManifest(dir, "");
  if (!r.ok()) {
    std::fprintf(stderr, "%s: manifest verify failed: %s\n", what,
                 r.error.c_str());
  }
  CHECK(r.state == integrity::ManifestState::kOk);
  CHECK(r.ok());
  CHECK(r.chain_ok);
  CHECK(r.files_missing == 0);
  CHECK(r.files_mismatch == 0);
  CHECK(r.files_unlisted == 0);
  CHECK(r.files_ok == r.files.size());
  CHECK(r.info.kind == integrity::PackageKind::kTrigger);
  CHECK(r.info.session_id == kTriggerId);
  CHECK(r.info.trigger_id == kTriggerId);
  CHECK(r.info.t0_ns == t0);
  CHECK(r.info.before_ns == before);
  CHECK(r.info.after_ns == after);
  CHECK(r.info.seal_time_ns != 0);
}

Grid SliceGrid(const Grid& source, uint64_t first_ns, uint64_t last_ns) {
  Grid out;
  for (const SmallEvent& event : source.small) {
    if (event.mono_ns >= first_ns && event.mono_ns < last_ns) {
      out.small.push_back(event);
    }
  }
  for (const GapEvent& event : source.gaps) {
    if (event.mono_ns >= first_ns && event.mono_ns < last_ns) {
      out.gaps.push_back(event);
    }
  }
  for (const FrameEvent& event : source.front) {
    if (event.mono_ns >= first_ns && event.mono_ns < last_ns) {
      out.front.push_back(event);
    }
  }
  for (const FrameEvent& event : source.rear) {
    if (event.mono_ns >= first_ns && event.mono_ns < last_ns) {
      out.rear.push_back(event);
    }
  }
  return out;
}

uint32_t RepeatCount() {
  const char* env = std::getenv("THYME_REP07_REPEATS");
  long n = env != nullptr ? std::strtol(env, nullptr, 10) : 3;
  if (n < 3) n = 3;  // determinism needs A > B1 == B2 == B3
  if (n > 32) n = 32;
  return static_cast<uint32_t>(n);
}

}  // namespace

int main() {
  alarm(60);
  const std::string root =
      std::string("/tmp/thyme_rep07_") + std::to_string(getpid());
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  const Grid grid = BuildGrid();
  const std::string dir_a = root + "/pkg_a";
  WritePackageA(dir_a, grid);
  CHECK(triggered::TriggeredRecorder::PackageComplete(dir_a));
  CheckManifest(dir_a, kT0, kBefore, kAfter, "package A");

  // A itself: layout, grid order, control rows.
  const Scan a = ScanPackage(dir_a);
  CHECK(a.channel_names == std::vector<std::string>(
                               {kMetaChannel, kSmallChannel, kFrontChannel,
                                kRearChannel}));
  CHECK(a.first_mono_ns == kGridStart);
  CHECK(a.last_mono_ns == kLastMono);
  CHECK(a.sessions == 0);
  ThymeStructDesc descs[2];
  RecordMirrorRingSpec specs[2];
  DataRingSpecs(descs, specs);
  const uint64_t config_hash = RecordMirrorRingPage::HashConfig(descs, specs, 2);
  CheckMeta(a, kT0, kBefore, kAfter, config_hash, 1);
  CHECK(a.data.size() == kDataEvents);
  CHECK(a.data_per_channel.at(kSmallChannel) == kSmallData);
  CHECK(a.data_per_channel.at(kFrontChannel) == kFrontCount);
  CHECK(a.data_per_channel.at(kRearChannel) == kRearCount);
  CHECK(a.holes.size() == 1);
  CHECK(a.holes[0].channel == kSmallChannel);
  CHECK(a.holes[0].mono_ns == 63 * kMs);
  CHECK(a.holes[0].struct_id == 0);
  CHECK(a.holes[0].reason == triggered::kGapReasonOverrun);
  CHECK(a.holes[0].missed == 1);
  CHECK(a.gaps_per_channel.at(kSmallChannel) == 1);
  CheckExpectedStream(a.data, ExpectedStream(grid));

  // ---- three record -> replay -> record rounds in fresh targets ----
  const uint32_t repeats = RepeatCount();
  std::vector<Round> rounds;
  rounds.reserve(repeats);
  const uint64_t begin = NowNs();
  for (uint32_t i = 1; i <= repeats; ++i) {
    rounds.push_back(RunRound(root, i, dir_a, grid));
  }
  const uint64_t elapsed = NowNs() - begin;

  const std::vector<uint8_t> a_canon = CanonBytes(a.data);
  for (std::size_t i = 0; i < rounds.size(); ++i) {
    const Round& r = rounds[i];
    char what[48];
    std::snprintf(what, sizeof(what), "round %zu", i + 1);
    // Channel layout is reproduced by the re-recording.
    CHECK(r.scan.channel_names == a.channel_names);
    // Data stream: order, mono_ns, struct_id, sequence, flags, CRC, payload
    // bytes, counts.
    CheckSameStream(a_canon, r.canon, what);
    CHECK(r.scan.data_per_channel == a.data_per_channel);
    CHECK(r.scan.sessions == 0);
    CheckMeta(r.scan, kLastMono, kRerecordBefore, 0, config_hash, 0);
    // The injectors do not replay control rows: C has no hole, and CheckMeta
    // proves its trigger is the new fixed-window request rather than A's row.
    CHECK(r.scan.holes.empty());
    // Every frame row carries the CRC the .raw bytes reproduce.
    for (const CanonEvent& e : r.scan.data) {
      if (e.rank == RankOf(kSmallChannel)) continue;
      CHECK(e.crc == Crc32(e.payload.data(), e.payload.size()));
      CHECK(e.crc != 0);
    }
    // SEC-03: a replay-derived package seals and verifies like any capture.
    CheckManifest(r.pkg_dir, kLastMono, kRerecordBefore, 0, what);
  }

  // REP-14: C1 == C2 == C3 on both the canonical stream and actual files.
  // WAL comparison exempts only its seal commit stamp; the realtime manifest
  // envelope is verified above rather than included in byte determinism.
  for (std::size_t i = 1; i < rounds.size(); ++i) {
    char what[48];
    std::snprintf(what, sizeof(what), "round 1 vs round %zu", i + 1);
    CheckSameStream(rounds[0].canon, rounds[i].canon, what);
    CheckDeterministicFiles(rounds[0].pkg_dir, rounds[i].pkg_dir);
    CheckWalEnvelope(rounds[0].pkg_dir, rounds[i].pkg_dir);
  }

  std::vector<std::string> stitch_dirs;
  const uint64_t stitch_bounds[4] = {0, 65 * kMs, 105 * kMs,
                                     std::numeric_limits<uint64_t>::max()};
  for (std::size_t i = 0; i < 3; ++i) {
    const std::string dir = root + "/stitch_a" + std::to_string(i);
    WritePackageA(dir, SliceGrid(grid, stitch_bounds[i], stitch_bounds[i + 1]));
    stitch_dirs.push_back(dir);
  }
  std::vector<Round> stitch_rounds;
  for (uint32_t i = 1; i <= 2; ++i) {
    stitch_rounds.push_back(RunStitchRound(root, i, stitch_dirs));
  }
  CheckSameStream(stitch_rounds[0].canon, stitch_rounds[1].canon,
                  "stitch C1 vs C2");
  CheckDeterministicFiles(stitch_rounds[0].pkg_dir, stitch_rounds[1].pkg_dir);
  CheckWalEnvelope(stitch_rounds[0].pkg_dir, stitch_rounds[1].pkg_dir);

  std::printf(
      "test_record_replay_record: OK (%llu data events/channel-counts "
      "%llu/%llu/%llu, %u rounds in %.1f ms)\n",
      static_cast<unsigned long long>(a.data.size()),
      static_cast<unsigned long long>(a.data_per_channel.at(kSmallChannel)),
      static_cast<unsigned long long>(a.data_per_channel.at(kFrontChannel)),
      static_cast<unsigned long long>(a.data_per_channel.at(kRearChannel)),
      repeats, static_cast<double>(elapsed) / 1e6);
  std::filesystem::remove_all(root);
  return 0;
}
