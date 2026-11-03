// test_replay_header_rewrite.cpp — REP-12: header rewrite policy on
// injection.
//
// A synthetic package mixes framed payloads (an app-level FrameHeader, the
// Frame<P> wire format, embedded in the payload) with plain POD payloads:
//
//   small.tbin   struct 0: framed, CRC sealed, hdr session 0x1111, embedded
//                sequence 100+i, embedded mono_ns 500 ns behind the record;
//                recorded producer sequence 700+i
//                struct 1: framed, ABI-12 no-CRC seal (flag + crc32 == 0)
//                struct 2: plain 16-byte POD (no FrameHeader)
//   frames/cam0  framed, CRC sealed, session 0x2222, sequence 500+i; target
//                channel has CRC enabled
//   frames/cam1  framed, CRC sealed; target channel has CRC disabled (slot
//                crc32 must stay 0 while the embedded header is re-sealed)
//
// Asserted:
//   A: default policy — mono_ns is the replay-scheduled (timeline) time at
//      slot level AND in the embedded header, session_id becomes the replay
//      session id, sequences are renumbered per target from 0, embedded CRCs
//      re-seal per ABI-12 (no-CRC frames keep the flag and crc32 == 0), the
//      big-frame slot CRC is recomputed over the rewritten payload (0 when
//      the channel has CRC disabled), the ring page's own session_id is never
//      touched, plain payloads are byte-identical.
//   B: opt-in (rewrite_embedded_frame=false, renumber_sequence=false)
//      reproduces the pre-REP-12 behavior byte-for-byte, recorded producer
//      sequences included.
//   C: session_id == 0 lets the player generate ONE id shared by both
//      injectors.
//   D: DetectEmbeddedFrame rejects short/oversize/invalid/corrupt payloads
//      and accepts both CRC flavors.
//   E: mono-only rewrite (preserve session + sequence) rewrites just
//      hdr.mono_ns and re-seals.
//   F: a standalone-channel synchronous sink observes the same rewrite and
//      sequence rules, POD passthrough, oversize rejection and failure retry.
//   G: per-target-struct gates deduplicate shared targets, reject token 0,
//      coexist across structs, roll back failed opens, survive Close/reopen,
//      report lost ownership as kGated, and support explicit no-gate mode.

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/replay/replay_player.h"
#include "thyme/replay/replay_reader.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

using namespace thyme;

namespace {

constexpr uint64_t kMs = 1000000ULL;
constexpr uint32_t kFrameMagic = 0x70426B21u;
constexpr uint16_t kFrameVersion = 3;
constexpr uint64_t kRecordedSmallSession = 0x1111;
constexpr uint64_t kRecordedFrameSession = 0x2222;
constexpr uint64_t kReplaySession = 0xABCDEF00ULL;
constexpr uint32_t kRingSession = 9;  // target page's own identity
constexpr uint32_t kRingDepth = 64;
constexpr uint32_t kSlotBytes = 4096;
constexpr uint32_t kSlotCount = 16;
constexpr uint64_t kTriggerId = 7;
constexpr uint32_t kBodySmall = 32;
constexpr uint32_t kBodyFrame = 200;

constexpr uint32_t kFramedSmallLen = sizeof(FrameHeader) + kBodySmall;  // 68
constexpr uint32_t kFramedBigLen = sizeof(FrameHeader) + kBodyFrame;    // 236

// ---- framed payload factory ------------------------------------------------

std::vector<uint8_t> MakeFramedPayload(uint32_t body_len, uint32_t seq,
                                       uint64_t session, uint64_t mono_ns,
                                       uint8_t seed, bool no_crc) {
  std::vector<uint8_t> out(sizeof(FrameHeader) + body_len);
  for (uint32_t j = 0; j < body_len; ++j) {
    out[sizeof(FrameHeader) + j] = static_cast<uint8_t>(seed * 31u + j * 11u);
  }
  FrameHeader h{};
  h.session_id = session;
  h.mono_ns = mono_ns;
  h.sequence = seq;
  h.size = static_cast<uint16_t>(out.size());
  h.valid = 1;
  if (no_crc) {
    FrameHeaderSealNoCrc(h, kFrameMagic, kFrameVersion);
  } else {
    FrameHeaderSeal(h, out.data() + sizeof(FrameHeader), body_len, kFrameMagic,
                    kFrameVersion);
  }
  std::memcpy(out.data(), &h, sizeof(h));
  return out;
}

FrameHeader HdrOf(const uint8_t* p) {
  FrameHeader h;
  std::memcpy(&h, p, sizeof(h));
  return h;
}

// ---- package ---------------------------------------------------------------

struct SmallRow {
  uint64_t mono_ns = 0;
  uint16_t struct_id = 0;
  uint32_t source_sequence = 0;
  bool gap = false;
  std::vector<uint8_t> payload;
};

struct FrameRow {
  uint64_t mono_ns;
  uint32_t source_sequence;
  uint32_t flags;
  std::vector<uint8_t> payload;
};

struct Grid {
  std::vector<SmallRow> small;  // sorted by mono_ns (gap included)
  std::vector<FrameRow> cam0;
  std::vector<FrameRow> cam1;
};

ThymeStructDesc MakeDesc(uint16_t sid, uint32_t payload, const char* name) {
  ThymeStructDesc d{};
  d.struct_id = sid;
  d.contract_version = 1;
  d.payload_size = payload;
  std::snprintf(d.name, sizeof(d.name), "%s", name);
  return d;
}

Grid BuildGrid() {
  Grid g;
  // struct 0: framed + CRC; embedded header deliberately disagrees with the
  // record (mono 500 ns behind, session 0x1111, sequence 100+i).
  for (uint32_t i = 0; i < 5; ++i) {
    SmallRow r;
    r.mono_ns = 40 * kMs + i * 4 * kMs;
    r.struct_id = 0;
    r.source_sequence = 700 + i;
    r.payload =
        MakeFramedPayload(kBodySmall, 100 + i, kRecordedSmallSession,
                          r.mono_ns - 500, static_cast<uint8_t>(i), false);
    g.small.push_back(std::move(r));
  }
  // struct 1: framed, ABI-12 no-CRC seal.
  for (uint32_t i = 0; i < 4; ++i) {
    SmallRow r;
    r.mono_ns = 42 * kMs + i * 6 * kMs;
    r.struct_id = 1;
    r.source_sequence = 800 + i;
    r.payload = MakeFramedPayload(kBodySmall, 200 + i, kRecordedSmallSession,
                                  r.mono_ns - 500,
                                  static_cast<uint8_t>(0x40 + i), true);
    g.small.push_back(std::move(r));
  }
  // struct 2: plain POD — must pass through byte-identical under any policy.
  for (uint32_t i = 0; i < 3; ++i) {
    SmallRow r;
    r.mono_ns = 41 * kMs + i * 8 * kMs;
    r.struct_id = 2;
    r.source_sequence = 900 + i;
    r.payload.assign(16, 0);
    for (uint32_t j = 0; j < 16; ++j) {
      r.payload[j] = static_cast<uint8_t>(i * 17u + j * 5u);
    }
    g.small.push_back(std::move(r));
  }
  SmallRow gap;
  gap.mono_ns = 63 * kMs;
  gap.struct_id = 0;
  gap.gap = true;
  g.small.push_back(gap);
  // The small source must be non-decreasing in mono_ns (reader contract).
  std::stable_sort(g.small.begin(), g.small.end(),
                   [](const SmallRow& a, const SmallRow& b) {
                     if (a.mono_ns != b.mono_ns) return a.mono_ns < b.mono_ns;
                     return a.struct_id < b.struct_id;
                   });

  for (uint32_t i = 0; i < 4; ++i) {
    FrameRow r;
    r.mono_ns = 40 * kMs + i * 10 * kMs;
    r.source_sequence = 600 + i;
    r.flags = i % 2;
    r.payload = MakeFramedPayload(kBodyFrame, 500 + i, kRecordedFrameSession,
                                  r.mono_ns - 500,
                                  static_cast<uint8_t>(0x80 + i), false);
    g.cam0.push_back(std::move(r));
  }
  for (uint32_t i = 0; i < 2; ++i) {
    FrameRow r;
    r.mono_ns = 44 * kMs + i * 12 * kMs;
    r.source_sequence = 650 + i;
    r.flags = 0;
    r.payload = MakeFramedPayload(kBodyFrame, 550 + i, kRecordedFrameSession,
                                  r.mono_ns - 500,
                                  static_cast<uint8_t>(0xA0 + i), false);
    g.cam1.push_back(std::move(r));
  }
  return g;
}

void WriteMetaTbin(const std::string& dir) {
  const std::vector<ThymeStructDesc> regs = {
      MakeDesc(0, kFramedSmallLen, "FramedCrc"),
      MakeDesc(1, kFramedSmallLen, "FramedNoCrc"), MakeDesc(2, 16, "Pod16"),
      MakeDesc(triggered::kFrameIndexStructId,
               static_cast<uint32_t>(sizeof(triggered::FrameIndexRecord)),
               "FrameIdx"),
      MakeDesc(triggered::kMetaStructId,
               static_cast<uint32_t>(sizeof(triggered::TriggerMetaRecord)),
               "TrigMeta")};
  ThymeBinFileSink sink((dir + "/meta.tbin").c_str());
  CHECK(sink.ok());
  ThymeBinEncoder enc(&sink);
  CHECK(enc.WriteHeader(kTriggerId, 40 * kMs, regs.data(), regs.size()));
  CHECK(enc.WriteTrigger(kTriggerId, 100 * kMs, 60 * kMs, 40 * kMs));
  CHECK(enc.Finish());
  CHECK(sink.Sync());
}

void WriteSmallTbin(const std::string& dir, const Grid& g) {
  const ThymeStructDesc regs[3] = {MakeDesc(0, kFramedSmallLen, "FramedCrc"),
                                   MakeDesc(1, kFramedSmallLen, "FramedNoCrc"),
                                   MakeDesc(2, 16, "Pod16")};
  ThymeBinFileSink sink((dir + "/small.tbin").c_str());
  CHECK(sink.ok());
  ThymeBinEncoder enc(&sink);
  CHECK(enc.WriteHeader(kTriggerId, 40 * kMs, regs, 3));
  uint64_t rec_seq = 0;
  for (const SmallRow& r : g.small) {
    if (r.gap) {
      CHECK(enc.WriteGap(r.struct_id, r.mono_ns, triggered::kGapReasonOverrun,
                         1));
    } else {
      CHECK(enc.WriteData(rec_seq++, r.struct_id, r.mono_ns, r.source_sequence,
                          r.payload.data(),
                          static_cast<uint32_t>(r.payload.size())));
    }
  }
  CHECK(enc.Finish());
  CHECK(sink.Sync());
}

void WriteFrameChannel(const std::string& dir, const char* name,
                       const std::vector<FrameRow>& frames) {
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
  CHECK(enc.WriteHeader(kTriggerId, 40 * kMs, regs, 2));
  uint64_t offset = 0;
  uint64_t rec_seq = 0;
  for (const FrameRow& f : frames) {
    const uint32_t len = static_cast<uint32_t>(f.payload.size());
    CHECK(std::fwrite(f.payload.data(), 1, len, raw) == len);
    const triggered::FrameIndexRecord idx{offset, len, f.flags,
                                          Crc32(f.payload.data(), len)};
    CHECK(enc.WriteData(rec_seq++, triggered::kFrameIndexStructId, f.mono_ns,
                        f.source_sequence, &idx, sizeof(idx)));
    offset += len;
  }
  CHECK(enc.Finish());
  CHECK(sink.Sync());
  CHECK(std::fflush(raw) == 0);
  CHECK(std::fclose(raw) == 0);
}

void WritePackage(const std::string& dir, const Grid& g) {
  std::filesystem::create_directories(dir + "/frames");
  WriteMetaTbin(dir);
  WriteSmallTbin(dir, g);
  WriteFrameChannel(dir, "cam0", g.cam0);
  WriteFrameChannel(dir, "cam1", g.cam1);
}

// ---- targets ---------------------------------------------------------------

struct Targets {
  std::string ring_name;
  std::string cam0_name;
  std::string cam1_name;
  record_mirror::RingClient ring;
  PinnedSlotWriter cam0;
  PinnedSlotWriter cam1;

  void Init(const char* tag) {
    char buf[160];
    const long me = static_cast<long>(getpid());
    std::snprintf(buf, sizeof(buf), "/thyme_rep12_%s_ring_%ld", tag, me);
    ring_name = buf;
    std::snprintf(buf, sizeof(buf), "/thyme_rep12_%s_c0_%ld", tag, me);
    cam0_name = buf;
    std::snprintf(buf, sizeof(buf), "/thyme_rep12_%s_c1_%ld", tag, me);
    cam1_name = buf;
    ShmRegion::UnlinkName(ring_name.c_str());
    ShmRegion::UnlinkName(cam0_name.c_str());
    ShmRegion::UnlinkName(
        PinnedSlotWriter::LeaseShmName(cam0_name.c_str()).c_str());
    ShmRegion::UnlinkName(cam1_name.c_str());
    ShmRegion::UnlinkName(
        PinnedSlotWriter::LeaseShmName(cam1_name.c_str()).c_str());
    const ThymeStructDesc descs[3] = {
        MakeDesc(0, kFramedSmallLen, "FramedCrc"),
        MakeDesc(1, kFramedSmallLen, "FramedNoCrc"), MakeDesc(2, 16, "Pod16")};
    const RecordMirrorRingSpec specs[3] = {{kFramedSmallLen, kRingDepth},
                                           {kFramedSmallLen, kRingDepth},
                                           {16, kRingDepth}};
    CHECK(ring.Create(ring_name.c_str(), descs, specs, 3, kRingSession));
    CHECK(cam0.Create(cam0_name.c_str(), kSlotBytes, kSlotCount, 0));
    CHECK(cam1.Create(cam1_name.c_str(), kSlotBytes, kSlotCount, 0));
    // REC-21: transport CRC defaults off — cam0 is the CRC-on contrast.
    cam0.SetCrcEnabled(true);
    cam1.SetCrcEnabled(false);
  }

  void Close() {
    cam0.Close();
    cam1.Close();
    ShmRegion::UnlinkName(cam0_name.c_str());
    ShmRegion::UnlinkName(
        PinnedSlotWriter::LeaseShmName(cam0_name.c_str()).c_str());
    ShmRegion::UnlinkName(cam1_name.c_str());
    ShmRegion::UnlinkName(
        PinnedSlotWriter::LeaseShmName(cam1_name.c_str()).c_str());
    ring.Shutdown(true);
  }
};

replay::ReplayPlayerConfig MakeConfig(uint32_t small_ch, uint32_t cam0_ch,
                                      uint32_t cam1_ch, Targets* t) {
  replay::ReplayPlayerConfig cfg;
  cfg.pinned_slots.push_back(replay::PinnedSlotTarget{cam0_ch, &t->cam0});
  cfg.pinned_slots.push_back(replay::PinnedSlotTarget{cam1_ch, &t->cam1});
  for (uint16_t s = 0; s < 3; ++s) {
    replay::SmallStructTarget st;
    st.channel = small_ch;
    st.struct_id = s;
    st.ring = t->ring.Page();
    st.target_struct_id = s;
    cfg.small_structs.push_back(st);
  }
  return cfg;
}

uint32_t FindChannel(replay::ReplayPackage& pkg, const char* name) {
  for (const replay::ReplayChannel& c : pkg.channels()) {
    if (c.name == name) return c.id;
  }
  CHECK(false);
  return 0;
}

// Rows of one struct in timeline order (gaps excluded).
std::vector<const SmallRow*> RowsOf(const Grid& g, uint16_t struct_id) {
  std::vector<const SmallRow*> out;
  for (const SmallRow& r : g.small) {
    if (!r.gap && r.struct_id == struct_id) out.push_back(&r);
  }
  return out;
}

void CheckRewrittenFrame(const uint8_t* got, const SmallRow& rec, uint32_t seq,
                         uint64_t session, bool mono_rewritten) {
  CHECK(std::memcmp(got + sizeof(FrameHeader),
                    rec.payload.data() + sizeof(FrameHeader),
                    rec.payload.size() - sizeof(FrameHeader)) == 0);
  const FrameHeader h = HdrOf(got);
  CHECK(h.magic == kFrameMagic);
  CHECK(h.version == kFrameVersion);
  CHECK(h.size == rec.payload.size());
  CHECK(h.valid == 1);
  CHECK(h.sequence == seq);
  CHECK(h.session_id == session);
  CHECK(h.mono_ns == (mono_rewritten ? rec.mono_ns : rec.mono_ns - 500));
  const FrameHeader recorded = HdrOf(rec.payload.data());
  if (FrameCrcDisabled(recorded)) {
    // ABI-12: a no-CRC frame keeps the flag set and crc32 == 0.
    CHECK(FrameCrcDisabled(h));
    CHECK(h.crc32 == 0);
  } else {
    CHECK(!FrameCrcDisabled(h));
    CHECK(FrameHeaderOk(h, got + sizeof(FrameHeader),
                        rec.payload.size() - sizeof(FrameHeader), kFrameMagic,
                        kFrameVersion));
  }
}

struct FrameView {
  uint64_t mono_ns = 0;
  uint32_t flags = 0;
  uint32_t crc32 = 0;
  bool crc_ok = false;
  std::vector<uint8_t> payload;
};

std::vector<FrameView> ReadFrames(const std::string& name, std::size_t count) {
  PinnedSlotReader reader;
  CHECK(reader.Open(name.c_str(), 2000000000ULL, /*lease_read_only=*/true));
  std::vector<FrameView> out;
  for (std::size_t i = 0; i < count; ++i) {
    PinnedSlotReader::View v;
    CHECK(reader.ReadFrame(i, v) == PinnedSlotReader::ReadResult::kOk);
    FrameView f;
    f.mono_ns = v.mono_ns;
    f.flags = v.flags;
    f.crc32 = v.crc32;
    f.crc_ok = v.crc_ok;
    f.payload.assign(v.payload, v.payload + v.data_len);
    out.push_back(std::move(f));
  }
  reader.Close();
  return out;
}

void CheckRewrittenPinnedSlot(const FrameView& got, const FrameRow& rec,
                            uint32_t seq, uint64_t session, bool channel_crc) {
  CHECK(got.mono_ns == rec.mono_ns);
  CHECK(got.flags == rec.flags);
  CHECK(got.payload.size() == rec.payload.size());
  if (channel_crc) {
    // The slot CRC is recomputed by Commit over the REWRITTEN payload.
    CHECK(got.crc_ok);
    CHECK(got.crc32 == Crc32(got.payload.data(), got.payload.size()));
    CHECK(got.payload != rec.payload);  // session id changed -> bytes differ
  } else {
    CHECK(got.crc32 == 0);
  }
  const FrameHeader h = HdrOf(got.payload.data());
  CHECK(h.session_id == session);
  CHECK(h.sequence == seq);
  CHECK(h.mono_ns == rec.mono_ns);
  CHECK(FrameHeaderOk(h, got.payload.data() + sizeof(FrameHeader), kBodyFrame,
                      kFrameMagic, kFrameVersion));
  CHECK(std::memcmp(got.payload.data() + sizeof(FrameHeader),
                    rec.payload.data() + sizeof(FrameHeader), kBodyFrame) == 0);
}

struct CapturedSample {
  uint8_t bytes[256]{};
  uint64_t mono_ns = 0;
  uint32_t size = 0;
  uint32_t source_sequence = 0;
  uint16_t target_struct_id = 0;
};

struct CapturingSink {
  CapturedSample samples[8]{};
  uint32_t attempts = 0;
  uint32_t accepted = 0;
  bool fail_next = false;
};

bool CapturePublish(void* ctx, const void* payload, uint32_t size,
                    uint64_t mono_ns, uint32_t source_sequence,
                    uint16_t target_struct_id) {
  CapturingSink& sink = *static_cast<CapturingSink*>(ctx);
  ++sink.attempts;
  if (sink.fail_next) {
    sink.fail_next = false;
    return false;
  }
  CapturedSample& sample = sink.samples[sink.accepted++];
  std::memcpy(sample.bytes, payload, size);
  sample.size = size;
  sample.mono_ns = mono_ns;
  sample.source_sequence = source_sequence;
  sample.target_struct_id = target_struct_id;
  return true;
}

}  // namespace

int main() {
  const std::string root =
      std::string("/tmp/thyme_rep12_") + std::to_string(getpid());
  std::filesystem::remove_all(root);
  const std::string dir = root + "/pkg";
  const Grid grid = BuildGrid();
  WritePackage(dir, grid);

  replay::ReplayPackage pkg;
  CHECK(pkg.Open(dir) == replay::ReplayStatus::kOk);
  const uint32_t small_ch = FindChannel(pkg, "small");
  const uint32_t cam0_ch = FindChannel(pkg, "cam0");
  const uint32_t cam1_ch = FindChannel(pkg, "cam1");
  pkg.Close();
  const std::vector<const SmallRow*> s0 = RowsOf(grid, 0);
  const std::vector<const SmallRow*> s1 = RowsOf(grid, 1);
  const std::vector<const SmallRow*> s2 = RowsOf(grid, 2);

  // ---- D: embedded-frame detection unit checks ----
  {
    const std::vector<uint8_t> framed =
        MakeFramedPayload(kBodySmall, 1, 2, 3, 0, false);
    CHECK(replay::DetectEmbeddedFrame(framed.data(),
                                      static_cast<uint32_t>(framed.size())));
    const std::vector<uint8_t> nocrc =
        MakeFramedPayload(kBodySmall, 1, 2, 3, 0, true);
    CHECK(replay::DetectEmbeddedFrame(nocrc.data(),
                                      static_cast<uint32_t>(nocrc.size())));
    CHECK(!replay::DetectEmbeddedFrame(nullptr, 64));
    CHECK(!replay::DetectEmbeddedFrame(framed.data(), 20));  // too short
    std::vector<uint8_t> bad = framed;
    bad[0] = 0;  // magic == 0
    bad[1] = 0;
    bad[2] = 0;
    bad[3] = 0;
    CHECK(!replay::DetectEmbeddedFrame(bad.data(),
                                       static_cast<uint32_t>(bad.size())));
    bad = framed;
    bad[28] = 0;  // valid != 1
    CHECK(!replay::DetectEmbeddedFrame(bad.data(),
                                       static_cast<uint32_t>(bad.size())));
    bad = framed;
    bad[4] ^= 0x01;  // size field no longer matches the span
    CHECK(!replay::DetectEmbeddedFrame(bad.data(),
                                       static_cast<uint32_t>(bad.size())));
    bad = framed;
    bad.back() ^= 0x5A;  // payload byte flipped: CRC mismatch
    CHECK(!replay::DetectEmbeddedFrame(bad.data(),
                                       static_cast<uint32_t>(bad.size())));
    bad = nocrc;
    bad[32] = 1;  // crc32 != 0 on a crc-disabled frame violates ABI-12
    CHECK(!replay::DetectEmbeddedFrame(bad.data(),
                                       static_cast<uint32_t>(bad.size())));
    // A plain POD never looks like a frame.
    const uint8_t pod[16] = {};
    CHECK(!replay::DetectEmbeddedFrame(pod, sizeof(pod)));
  }

  // ---- A: default policy (explicit replay session id) ----
  {
    Targets t;
    t.Init("a");
    replay::ReplayPlayer player;
    replay::ReplayPlayerConfig cfg = MakeConfig(small_ch, cam0_ch, cam1_ch, &t);
    cfg.header_policy.session_id = kReplaySession;
    CHECK(player.Open(dir, cfg) == replay::ReplayStatus::kOk);
    CHECK(player.replay_session_id() == kReplaySession);
    player.AsFast(true);
    CHECK(player.Run() == replay::ReplayStatus::kOk);
    CHECK(player.stats().injected == 18);
    CHECK(player.stats().failed == 0);
    CHECK(player.stats().mono_regressions == 0);
    CHECK(player.small_struct_injector().stats().headers_rewritten == 9);
    CHECK(player.pinned_slot_injector().stats().headers_rewritten == 6);

    RecordMirrorRingPage* page = t.ring.Page();
    // The target page's own session identity is never rewritten.
    CHECK(page->header.session_id.load(std::memory_order_relaxed) ==
          kRingSession);
    uint8_t buf[256];
    for (uint32_t i = 0; i < s0.size(); ++i) {
      uint32_t size = 0;
      uint64_t mono = 0;
      uint32_t seq = 0;
      CHECK(page->ReadRingSample(0, i, buf, sizeof(buf), size, mono, seq));
      CHECK(size == kFramedSmallLen);
      CHECK(mono == s0[i]->mono_ns);  // replay-scheduled time
      CHECK(seq == i);                // renumbered from 0
      CheckRewrittenFrame(buf, *s0[i], i, kReplaySession, true);
    }
    for (uint32_t i = 0; i < s1.size(); ++i) {
      uint32_t size = 0;
      uint64_t mono = 0;
      uint32_t seq = 0;
      CHECK(page->ReadRingSample(1, i, buf, sizeof(buf), size, mono, seq));
      CHECK(mono == s1[i]->mono_ns);
      CHECK(seq == i);
      CheckRewrittenFrame(buf, *s1[i], i, kReplaySession, true);
    }
    for (uint32_t i = 0; i < s2.size(); ++i) {
      uint32_t size = 0;
      uint64_t mono = 0;
      uint32_t seq = 0;
      CHECK(page->ReadRingSample(2, i, buf, sizeof(buf), size, mono, seq));
      CHECK(mono == s2[i]->mono_ns);
      CHECK(seq == i);
      CHECK(size == s2[i]->payload.size());
      CHECK(std::memcmp(buf, s2[i]->payload.data(), size) == 0);
    }

    const std::vector<FrameView> f0 = ReadFrames(t.cam0_name, grid.cam0.size());
    for (uint32_t i = 0; i < f0.size(); ++i) {
      CheckRewrittenPinnedSlot(f0[i], grid.cam0[i], i, kReplaySession,
                             /*channel_crc=*/true);
    }
    const std::vector<FrameView> f1 = ReadFrames(t.cam1_name, grid.cam1.size());
    for (uint32_t i = 0; i < f1.size(); ++i) {
      // Channel CRC disabled: slot crc32 stays 0, the embedded FrameHeader is
      // still rewritten and re-sealed.
      CheckRewrittenPinnedSlot(f1[i], grid.cam1[i], i, kReplaySession,
                             /*channel_crc=*/false);
    }
    player.Close();
    t.Close();
  }

  // ---- B: opt-in pre-REP-12 behavior (byte-for-byte, recorded sequences) --
  {
    Targets t;
    t.Init("b");
    replay::ReplayPlayer player;
    replay::ReplayPlayerConfig cfg = MakeConfig(small_ch, cam0_ch, cam1_ch, &t);
    cfg.header_policy.rewrite_embedded_frame = false;
    cfg.header_policy.renumber_sequence = false;
    cfg.header_policy.session_id = kReplaySession;
    CHECK(player.Open(dir, cfg) == replay::ReplayStatus::kOk);
    player.AsFast(true);
    CHECK(player.Run() == replay::ReplayStatus::kOk);
    CHECK(player.stats().injected == 18);
    CHECK(player.small_struct_injector().stats().headers_rewritten == 0);
    CHECK(player.pinned_slot_injector().stats().headers_rewritten == 0);

    RecordMirrorRingPage* page = t.ring.Page();
    uint8_t buf[256];
    const std::vector<const SmallRow*>* structs[3] = {&s0, &s1, &s2};
    for (uint32_t sid = 0; sid < 3; ++sid) {
      const std::vector<const SmallRow*>& rows = *structs[sid];
      for (uint32_t i = 0; i < rows.size(); ++i) {
        uint32_t size = 0;
        uint64_t mono = 0;
        uint32_t seq = 0;
        CHECK(page->ReadRingSample(static_cast<uint16_t>(sid), i, buf,
                                   sizeof(buf), size, mono, seq));
        CHECK(mono == rows[i]->mono_ns);
        CHECK(seq == rows[i]->source_sequence);  // recorded producer sequence
        CHECK(size == rows[i]->payload.size());
        CHECK(std::memcmp(buf, rows[i]->payload.data(), size) == 0);
      }
    }
    const std::vector<FrameView> f0 = ReadFrames(t.cam0_name, grid.cam0.size());
    for (uint32_t i = 0; i < f0.size(); ++i) {
      CHECK(f0[i].mono_ns == grid.cam0[i].mono_ns);
      CHECK(f0[i].payload == grid.cam0[i].payload);  // untouched, CRC included
      CHECK(f0[i].crc32 ==
            Crc32(grid.cam0[i].payload.data(), grid.cam0[i].payload.size()));
    }
    player.Close();
    t.Close();
  }

  // ---- C: session_id == 0 -> the player generates ONE id for both injectors
  {
    Targets t;
    t.Init("c");
    replay::ReplayPlayer player;
    replay::ReplayPlayerConfig cfg = MakeConfig(small_ch, cam0_ch, cam1_ch, &t);
    CHECK(player.Open(dir, cfg) == replay::ReplayStatus::kOk);
    const uint64_t generated = player.replay_session_id();
    CHECK(generated != 0);
    CHECK(player.pinned_slot_injector().session_id() == generated);
    CHECK(player.small_struct_injector().session_id() == generated);
    player.AsFast(true);
    CHECK(player.Run() == replay::ReplayStatus::kOk);
    uint8_t buf[256];
    uint32_t size = 0;
    uint64_t mono = 0;
    uint32_t seq = 0;
    CHECK(
        t.ring.Page()->ReadRingSample(0, 0, buf, sizeof(buf), size, mono, seq));
    CHECK(HdrOf(buf).session_id == generated);
    const std::vector<FrameView> f0 = ReadFrames(t.cam0_name, 1);
    CHECK(HdrOf(f0[0].payload.data()).session_id == generated);
    player.Close();
    t.Close();
  }

  // ---- E: mono-only rewrite (session and sequence preserved) ----
  {
    Targets t;
    t.Init("e");
    replay::ReplayPlayer player;
    replay::ReplayPlayerConfig cfg = MakeConfig(small_ch, cam0_ch, cam1_ch, &t);
    cfg.header_policy.rewrite_session = false;
    cfg.header_policy.renumber_sequence = false;
    cfg.header_policy.session_id = kReplaySession;
    CHECK(player.Open(dir, cfg) == replay::ReplayStatus::kOk);
    player.AsFast(true);
    CHECK(player.Run() == replay::ReplayStatus::kOk);
    CHECK(player.small_struct_injector().stats().headers_rewritten == 9);

    RecordMirrorRingPage* page = t.ring.Page();
    uint8_t buf[256];
    for (uint32_t i = 0; i < s0.size(); ++i) {
      uint32_t size = 0;
      uint64_t mono = 0;
      uint32_t seq = 0;
      CHECK(page->ReadRingSample(0, i, buf, sizeof(buf), size, mono, seq));
      CHECK(seq == s0[i]->source_sequence);  // ring slot keeps recorded seq
      CheckRewrittenFrame(buf, *s0[i], 100 + i, kRecordedSmallSession, true);
    }
    player.Close();
    t.Close();
  }

  // ---- F: standalone-channel synchronous sink ----
  {
    replay::ReplayChannel channel;
    channel.id = 0;
    channel.kind = replay::ReplayChannelKind::kSmall;
    channel.name = "standalone";
    channel.registry = {MakeDesc(0, kFramedSmallLen, "FramedCrc"),
                        MakeDesc(2, 16, "Pod16")};
    CapturingSink sink;
    replay::SmallStructTarget targets[2];
    targets[0].struct_id = 0;
    targets[0].target_struct_id = 7;
    targets[0].publish = &CapturePublish;
    targets[0].publish_ctx = &sink;
    targets[0].payload_capacity = kFramedSmallLen;
    targets[1].struct_id = 2;
    targets[1].target_struct_id = 9;
    targets[1].publish = &CapturePublish;
    targets[1].publish_ctx = &sink;
    targets[1].payload_capacity = 16;
    replay::SmallStructInjector injector;
    replay::ReplayHeaderPolicy policy;
    policy.session_id = kReplaySession;
    injector.SetHeaderPolicy(policy);
    CHECK(injector.Open(channel, targets, 2) == replay::ReplayStatus::kOk);
    CHECK(injector.channels()[0].geometry_fits);
    CHECK(injector.channels()[1].geometry_fits);

    replay::ReplayEvent event;
    event.struct_id = 0;
    event.record_sequence = 1;
    event.source_sequence = s0[0]->source_sequence;
    event.mono_ns = s0[0]->mono_ns;
    event.len = static_cast<uint32_t>(s0[0]->payload.size());
    event.payload = s0[0]->payload.data();
    CHECK(injector.Inject(event) == replay::InjectStatus::kOk);
    CHECK(sink.accepted == 1);
    CHECK(sink.samples[0].target_struct_id == 7);
    CHECK(sink.samples[0].mono_ns == s0[0]->mono_ns);
    CHECK(sink.samples[0].source_sequence == 0);
    CheckRewrittenFrame(sink.samples[0].bytes, *s0[0], 0, kReplaySession, true);

    event.struct_id = 2;
    event.source_sequence = s2[0]->source_sequence;
    event.mono_ns = s2[0]->mono_ns;
    event.len = static_cast<uint32_t>(s2[0]->payload.size());
    event.payload = s2[0]->payload.data();
    CHECK(injector.Inject(event) == replay::InjectStatus::kOk);
    CHECK(sink.accepted == 2);
    CHECK(sink.samples[1].target_struct_id == 9);
    CHECK(sink.samples[1].source_sequence == 0);
    CHECK(std::memcmp(sink.samples[1].bytes, s2[0]->payload.data(),
                      s2[0]->payload.size()) == 0);

    const uint8_t oversize[kFramedSmallLen + 1]{};
    event.struct_id = 0;
    event.len = sizeof(oversize);
    event.payload = oversize;
    CHECK(injector.Inject(event) == replay::InjectStatus::kOversize);
    CHECK(sink.attempts == 2);
    CHECK(sink.accepted == 2);

    event.struct_id = 0;
    event.record_sequence = 2;
    event.source_sequence = s0[1]->source_sequence;
    event.mono_ns = s0[1]->mono_ns;
    event.len = static_cast<uint32_t>(s0[1]->payload.size());
    event.payload = s0[1]->payload.data();
    sink.fail_next = true;
    CHECK(injector.Inject(event) == replay::InjectStatus::kWriteFailed);
    CHECK(sink.attempts == 3);
    CHECK(sink.accepted == 2);
    CHECK(injector.Inject(event) == replay::InjectStatus::kOk);
    CHECK(sink.accepted == 3);
    CHECK(sink.samples[2].source_sequence == 1);
    CheckRewrittenFrame(sink.samples[2].bytes, *s0[1], 1, kReplaySession, true);

    replay::SmallStructTarget invalid;
    CHECK(injector.Open(channel, &invalid, 1) ==
          replay::ReplayStatus::kInvalidArgument);
  }

  // ---- G: per-target-struct producer gates ----
  {
    Targets t;
    t.Init("g");
    replay::ReplayPackage gate_pkg;
    CHECK(gate_pkg.Open(dir) == replay::ReplayStatus::kOk);
    RecordMirrorRingPage* page = t.ring.Page();
    auto target = [=](uint16_t source, uint16_t destination) {
      replay::SmallStructTarget out;
      out.channel = small_ch;
      out.struct_id = source;
      out.ring = page;
      out.target_struct_id = destination;
      return out;
    };
    auto event = [=](const SmallRow& row) {
      replay::ReplayEvent out;
      out.channel = small_ch;
      out.struct_id = row.struct_id;
      out.record_sequence = row.source_sequence + 1;
      out.source_sequence = row.source_sequence;
      out.mono_ns = row.mono_ns;
      out.len = static_cast<uint32_t>(row.payload.size());
      out.payload = row.payload.data();
      return out;
    };

    // Two source structs may share one target, but its gate is acquired once.
    replay::SmallStructTarget shared[2] = {target(0, 0), target(1, 0)};
    ProducerGate* gate0 = page->producer_gate(0);
    const uint64_t generation =
        gate0->generation.load(std::memory_order_acquire);
    replay::SmallStructInjector owner;
    CHECK(owner.Open(gate_pkg, shared, 2) == replay::ReplayStatus::kOk);
    CHECK(gate0->generation.load(std::memory_order_acquire) == generation + 1);
    const uint64_t owner_token =
        gate0->owner_token.load(std::memory_order_acquire);
    CHECK(owner_token != 0);

    replay::SmallStructTarget only0 = target(0, 0);
    replay::SmallStructInjector conflict;
    CHECK(conflict.Open(gate_pkg, &only0, 1) == replay::ReplayStatus::kBusy);
    CHECK(gate0->owner_token.load(std::memory_order_acquire) == owner_token);

    // A different struct on the same page has an independent gate.
    replay::SmallStructTarget only1 = target(1, 1);
    replay::SmallStructInjector coexist;
    CHECK(coexist.Open(gate_pkg, &only1, 1) == replay::ReplayStatus::kOk);
    ProducerGate* gate1 = page->producer_gate(1);
    CHECK(gate1->owner_token.load(std::memory_order_acquire) != 0);
    CHECK(gate1->owner_token.load(std::memory_order_acquire) != owner_token);

    // A later kBusy rolls back a gate acquired earlier in the same Open.
    replay::SmallStructTarget rollback[2] = {target(2, 2), target(0, 0)};
    replay::SmallStructInjector rolled_back;
    CHECK(rolled_back.Open(gate_pkg, rollback, 2) ==
          replay::ReplayStatus::kBusy);
    ProducerGate* gate2 = page->producer_gate(2);
    CHECK(gate2->owner_token.load(std::memory_order_acquire) == 0);

    // Plain token-0 producers are rejected while replay writes with its token.
    CHECK(!page->TryWriteRing(0, s0[0]->payload.data(),
                              static_cast<uint32_t>(s0[0]->payload.size()),
                              s0[0]->mono_ns, 0));
    CHECK(owner.gate_rejects() == 1);
    CHECK(page->RingHead(0) == 0);
    replay::ReplayEvent event0 = event(*s0[0]);
    CHECK(owner.Inject(event0) == replay::InjectStatus::kOk);
    CHECK(page->RingHead(0) == 1);
    replay::ReplayEvent event1 = event(*s1[0]);
    CHECK(coexist.Inject(event1) == replay::InjectStatus::kOk);
    CHECK(page->RingHead(1) == 1);

    owner.Close();
    CHECK(gate0->owner_token.load(std::memory_order_acquire) == 0);
    CHECK(page->TryWriteRing(0, s0[0]->payload.data(),
                             static_cast<uint32_t>(s0[0]->payload.size()),
                             s0[0]->mono_ns, 0));
    CHECK(page->RingHead(0) == 2);
    CHECK(owner.Open(gate_pkg, &only0, 1) == replay::ReplayStatus::kOk);

    // Age the lease and reclaim it with a foreign owner. The old replay must
    // report kGated, not an ordinary ring write failure, and must not publish.
    gate0->heartbeat_ns.store(1, std::memory_order_release);
    const uint64_t foreign_token = ProducerGateNewToken();
    CHECK(ProducerGateAcquire(*gate0, foreign_token, ProducerGateNowNs(),
                              kProducerGateDefaultTimeoutNs) ==
          GateResult::kStolen);
    CHECK(owner.Inject(event0) == replay::InjectStatus::kGated);
    CHECK(owner.stats().gated == 1);
    CHECK(owner.stats().write_failures == 0);
    CHECK(owner.channels()[0].gated == 1);
    CHECK(page->RingHead(0) == 2);
    CHECK(owner.gate_steals() == 1);
    owner.Close();
    CHECK(gate0->owner_token.load(std::memory_order_acquire) == foreign_token);
    CHECK(ProducerGateRelease(*gate0, foreign_token) == GateResult::kOk);
    coexist.Close();

    // Explicit no-gate mode preserves the legacy token-0 behavior.
    replay::SmallStructTarget only2 = target(2, 2);
    replay::SmallStructInjector legacy;
    legacy.SetGateEnabled(false);
    CHECK(!legacy.gate_enabled());
    CHECK(legacy.Open(gate_pkg, &only2, 1) == replay::ReplayStatus::kOk);
    CHECK(gate2->owner_token.load(std::memory_order_acquire) == 0);
    CHECK(page->TryWriteRing(2, s2[0]->payload.data(),
                             static_cast<uint32_t>(s2[0]->payload.size()),
                             s2[0]->mono_ns, 0));
    replay::ReplayEvent event2 = event(*s2[0]);
    CHECK(legacy.Inject(event2) == replay::InjectStatus::kOk);
    CHECK(page->RingHead(2) == 2);
    legacy.Close();
    gate_pkg.Close();
    t.Close();
  }

  std::filesystem::remove_all(root);
  std::printf("test_replay_header_rewrite: OK\n");
  return 0;
}
