// test_replay_determinism.cpp — replay determinism.
//
// "Replay determinism: rate/pause/seek/trigger alignment; two replays of the same package give identical results." The
// record -> replay -> record regression (test_record_replay_record) already
// proves byte-equal re-recording in as-fast mode; this test extends coverage
// to the paced/session controls:
//
//   * the same package replayed twice as-fast produces byte-identical target
//     contents (ring samples + big-frame slots, header rewrite included);
//   * one wall-clock run at 10x with a pause+resume between two events and a
//     no-op Seek mid-stream lands the same bytes;
//   * pause/step boundaries neither drop nor duplicate events (pause before
//     the first event, Step() k times, resume, run to the end);
//   * Seek determinism: Seek(t) mid-stream then run == a fresh session
//     started at t (same event set; byte-identical under the preserve-sequence
//     opt-in, and under the default renumber policy the suffix sequences
//     continue at exactly the pre-seek injection count).
//
// The package mixes a framed small struct (embedded FrameHeader — exercises
// the frame-header rewrite under every mode), a plain POD small struct, a GAP hole,
// a trigger row, and one big-frame channel. The replay session id is fixed so
// the rewritten headers are byte-stable across runs. Timestamps injected are
// the timeline mono_ns, so wall-clock pacing does not change the contents.

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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

constexpr uint64_t kMs = 1000000ULL;
constexpr uint32_t kFrameMagic = 0x70426B21u;
constexpr uint16_t kFrameVersion = 3;
constexpr uint64_t kRecordedSession = 0x3333;
constexpr uint64_t kReplaySession = 0xD37E11ULL;  // fixed: byte-stable rewrite
constexpr uint32_t kRingDepth = 128;
constexpr uint32_t kSlotBytes = 2048;
constexpr uint32_t kSlotCount = 32;
constexpr uint64_t kTriggerId = 7;
constexpr uint32_t kBodySmall = 32;
constexpr uint32_t kFramedSmallLen = sizeof(FrameHeader) + kBodySmall;

constexpr uint32_t kStruct0Count = 40;  // framed, 2 ms grid from 40 ms
constexpr uint32_t kStruct1Count = 20;  // plain POD, 4 ms grid from 41 ms
constexpr uint32_t kFrameCount = 8;     // 10 ms grid from 40 ms
constexpr uint64_t kDataEvents = kStruct0Count + kStruct1Count + kFrameCount;
constexpr uint64_t kAllEvents = kDataEvents + 2;  // + trigger + gap

void SleepNs(uint64_t ns) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
  nanosleep(&ts, nullptr);
}

std::vector<uint8_t> MakeFramedPayload(uint32_t seq, uint64_t mono_ns,
                                       uint8_t seed) {
  std::vector<uint8_t> out(kFramedSmallLen);
  for (uint32_t j = 0; j < kBodySmall; ++j) {
    out[sizeof(FrameHeader) + j] =
        static_cast<uint8_t>(seed * 31u + j * 11u);
  }
  FrameHeader h{};
  h.session_id = kRecordedSession;
  h.mono_ns = mono_ns;
  h.sequence = seq;
  h.size = static_cast<uint16_t>(out.size());
  h.valid = 1;
  FrameHeaderSeal(h, out.data() + sizeof(FrameHeader), kBodySmall, kFrameMagic,
                  kFrameVersion);
  std::memcpy(out.data(), &h, sizeof(h));
  return out;
}

ThymeStructDesc MakeDesc(uint16_t sid, uint32_t payload, const char* name) {
  ThymeStructDesc d{};
  d.struct_id = sid;
  d.contract_version = 1;
  d.payload_size = payload;
  std::snprintf(d.name, sizeof(d.name), "%s", name);
  return d;
}

void WriteMetaTbin(const std::string& dir) {
  const std::vector<ThymeStructDesc> regs = {
      MakeDesc(0, kFramedSmallLen, "Framed"), MakeDesc(1, 16, "Pod16"),
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

void WriteSmallTbin(const std::string& dir) {
  struct Row {
    uint64_t mono_ns;
    uint16_t struct_id;
    uint32_t source_sequence;
    bool gap;
    std::vector<uint8_t> payload;
  };
  std::vector<Row> rows;
  for (uint32_t i = 0; i < kStruct0Count; ++i) {
    const uint64_t mono = 40 * kMs + i * 2 * kMs;
    rows.push_back(Row{mono, 0, 300 + i, false,
                       MakeFramedPayload(300 + i, mono - 500, static_cast<uint8_t>(i))});
  }
  for (uint32_t i = 0; i < kStruct1Count; ++i) {
    std::vector<uint8_t> pod(16);
    for (uint32_t j = 0; j < 16; ++j) {
      pod[j] = static_cast<uint8_t>(i * 17u + j * 5u);
    }
    rows.push_back(Row{41 * kMs + i * 4 * kMs, 1, 400 + i, false,
                       std::move(pod)});
  }
  rows.push_back(Row{63 * kMs, 1, 0, true, {}});
  std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.mono_ns != b.mono_ns) return a.mono_ns < b.mono_ns;
    return a.struct_id < b.struct_id;
  });
  const ThymeStructDesc regs[2] = {MakeDesc(0, kFramedSmallLen, "Framed"),
                                   MakeDesc(1, 16, "Pod16")};
  ThymeBinFileSink sink((dir + "/small.tbin").c_str());
  CHECK(sink.ok());
  ThymeBinEncoder enc(&sink);
  CHECK(enc.WriteHeader(kTriggerId, 40 * kMs, regs, 2));
  uint64_t rec_seq = 0;
  for (const Row& r : rows) {
    if (r.gap) {
      CHECK(enc.WriteGap(r.struct_id, r.mono_ns, triggered::kGapReasonOverrun,
                         2));
    } else {
      CHECK(enc.WriteData(rec_seq++, r.struct_id, r.mono_ns, r.source_sequence,
                          r.payload.data(),
                          static_cast<uint32_t>(r.payload.size())));
    }
  }
  CHECK(enc.Finish());
  CHECK(sink.Sync());
}

uint32_t FrameLen(uint32_t i) { return 512 + (i % 4) * 128; }

void WriteFrameChannel(const std::string& dir) {
  const std::string base = dir + "/frames/cam0";
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
  for (uint32_t i = 0; i < kFrameCount; ++i) {
    const uint32_t len = FrameLen(i);
    std::vector<uint8_t> payload(len);
    for (uint32_t j = 0; j < len; ++j) {
      payload[j] = static_cast<uint8_t>(i * 13u + j * 7u);
    }
    CHECK(std::fwrite(payload.data(), 1, len, raw) == len);
    const triggered::FrameIndexRecord idx{offset, len, i % 2,
                                          Crc32(payload.data(), len)};
    CHECK(enc.WriteData(i, triggered::kFrameIndexStructId,
                        40 * kMs + i * 10 * kMs, 600 + i, &idx, sizeof(idx)));
    offset += len;
  }
  CHECK(enc.Finish());
  CHECK(sink.Sync());
  CHECK(std::fflush(raw) == 0);
  CHECK(std::fclose(raw) == 0);
}

void WritePackage(const std::string& dir) {
  std::filesystem::create_directories(dir + "/frames");
  WriteMetaTbin(dir);
  WriteSmallTbin(dir);
  WriteFrameChannel(dir);
}

// ---- targets + canonical read-back -----------------------------------------

struct Targets {
  std::string ring_name;
  std::string cam_name;
  record_mirror::RingClient ring;
  PinnedSlotWriter cam;

  void Init(const char* tag) {
    char buf[160];
    const long me = static_cast<long>(getpid());
    std::snprintf(buf, sizeof(buf), "/thyme_rec13_%s_ring_%ld", tag, me);
    ring_name = buf;
    std::snprintf(buf, sizeof(buf), "/thyme_rec13_%s_cam_%ld", tag, me);
    cam_name = buf;
    ShmRegion::UnlinkName(ring_name.c_str());
    ShmRegion::UnlinkName(cam_name.c_str());
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(cam_name.c_str()).c_str());
    const ThymeStructDesc descs[2] = {MakeDesc(0, kFramedSmallLen, "Framed"),
                                      MakeDesc(1, 16, "Pod16")};
    const RecordMirrorRingSpec specs[2] = {{kFramedSmallLen, kRingDepth},
                                           {16, kRingDepth}};
    CHECK(ring.Create(ring_name.c_str(), descs, specs, 2, 9));
    CHECK(cam.Create(cam_name.c_str(), kSlotBytes, kSlotCount, 0));
  }

  void Close() {
    cam.Close();
    ShmRegion::UnlinkName(cam_name.c_str());
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(cam_name.c_str()).c_str());
    ring.Shutdown(true);
  }
};

replay::ReplayPlayerConfig MakeConfig(uint32_t small_ch, uint32_t cam_ch,
                                      Targets* t) {
  replay::ReplayPlayerConfig cfg;
  cfg.pinned_slots.push_back(replay::PinnedSlotTarget{cam_ch, &t->cam});
  for (uint16_t s = 0; s < 2; ++s) {
    replay::SmallStructTarget st;
    st.channel = small_ch;
    st.struct_id = s;
    st.ring = t->ring.Page();
    st.target_struct_id = s;
    cfg.small_structs.push_back(st);
  }
  cfg.header_policy.session_id = kReplaySession;
  return cfg;
}

void AppendU32(std::vector<uint8_t>* out, uint32_t v) {
  out->insert(out->end(), reinterpret_cast<const uint8_t*>(&v),
              reinterpret_cast<const uint8_t*>(&v) + sizeof(v));
}

void AppendU64(std::vector<uint8_t>* out, uint64_t v) {
  out->insert(out->end(), reinterpret_cast<const uint8_t*>(&v),
              reinterpret_cast<const uint8_t*>(&v) + sizeof(v));
}

// Canonical byte image of the injected targets, per target in a fixed order,
// starting at the given per-target sample/frame offsets (0 for a full run).
// with_seq = false excludes the renumbered source_sequence (used for the
// seek comparison, where the offset differs by construction).
struct Canon {
  std::vector<uint8_t> bytes;
  std::vector<uint32_t> seq0;
  std::vector<uint32_t> seq1;
  uint64_t head0 = 0;
  uint64_t head1 = 0;
  uint64_t fhead = 0;
};

Canon Capture(Targets& t, uint64_t off0, uint64_t off1, uint64_t offf,
              bool with_seq) {
  Canon c;
  RecordMirrorRingPage* page = t.ring.Page();
  c.head0 = page->RingHead(0);
  c.head1 = page->RingHead(1);
  c.fhead = t.cam.head();
  uint8_t buf[512];
  for (uint16_t sid = 0; sid < 2; ++sid) {
    const uint64_t head = sid == 0 ? c.head0 : c.head1;
    const uint64_t off = sid == 0 ? off0 : off1;
    for (uint64_t i = off; i < head; ++i) {
      uint32_t size = 0;
      uint64_t mono = 0;
      uint32_t seq = 0;
      CHECK(page->ReadRingSample(sid, i, buf, sizeof(buf), size, mono, seq));
      AppendU64(&c.bytes, mono);
      AppendU32(&c.bytes, size);
      if (with_seq) AppendU32(&c.bytes, seq);
      c.bytes.insert(c.bytes.end(), buf, buf + size);
      (sid == 0 ? c.seq0 : c.seq1).push_back(seq);
    }
  }
  PinnedSlotReader reader;
  CHECK(reader.Open(t.cam_name.c_str(), 2000000000ULL,
                    /*lease_read_only=*/true));
  for (uint64_t i = offf; i < c.fhead; ++i) {
    PinnedSlotReader::View v;
    CHECK(reader.ReadFrame(i, v) == PinnedSlotReader::ReadResult::kOk);
    AppendU64(&c.bytes, v.mono_ns);
    AppendU32(&c.bytes, v.flags);
    AppendU32(&c.bytes, v.crc32);
    AppendU32(&c.bytes, v.data_len);
    c.bytes.insert(c.bytes.end(), v.payload, v.payload + v.data_len);
  }
  reader.Close();
  return c;
}

void CheckSameBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                    const char* what) {
  if (a == b) return;
  std::size_t at = 0;
  while (at < a.size() && at < b.size() && a[at] == b[at]) ++at;
  std::fprintf(stderr, "%s: streams differ at byte %zu (%zu vs %zu bytes)\n",
               what, at, a.size(), b.size());
  CHECK(false);
}

void CheckFullRunStats(const replay::PlayerStats& st) {
  CHECK(st.events == kAllEvents);
  CHECK(st.injected == kDataEvents);
  CHECK(st.ignored == 2);  // trigger + gap
  CHECK(st.skipped == 0);
  CHECK(st.failed == 0);
  CHECK(st.mono_regressions == 0);
}

struct RunOut {
  Canon canon;
  replay::PlayerStats stats;
};

RunOut RunAsFast(const std::string& dir, uint32_t small_ch, uint32_t cam_ch,
                 const char* tag) {
  Targets t;
  t.Init(tag);
  replay::ReplayPlayer player;
  CHECK(player.Open(dir, MakeConfig(small_ch, cam_ch, &t)) ==
        replay::ReplayStatus::kOk);
  player.AsFast(true);
  CHECK(player.Run() == replay::ReplayStatus::kOk);
  RunOut out;
  out.stats = player.stats();
  out.canon = Capture(t, 0, 0, 0, true);
  t.Close();
  return out;
}

}  // namespace

int main() {
  const std::string root =
      std::string("/tmp/thyme_rec13_") + std::to_string(getpid());
  std::filesystem::remove_all(root);
  const std::string dir = root + "/pkg";
  WritePackage(dir);

  replay::ReplayPackage pkg;
  CHECK(pkg.Open(dir) == replay::ReplayStatus::kOk);
  uint32_t small_ch = 0;
  uint32_t cam_ch = 0;
  for (const replay::ReplayChannel& c : pkg.channels()) {
    if (c.name == "small") small_ch = c.id;
    if (c.name == "cam0") cam_ch = c.id;
  }
  pkg.Close();

  // The merged event list (for seek points and prefix counting).
  struct EvInfo {
    uint64_t mono_ns;
    uint32_t channel;
    uint32_t struct_id;
    replay::ReplayKind kind;
  };
  std::vector<EvInfo> evs;
  {
    replay::ReplayPackage p2;
    CHECK(p2.Open(dir) == replay::ReplayStatus::kOk);
    replay::ReplayEvent ev;
    while (p2.NextEvent(&ev) == replay::ReplayStatus::kOk) {
      evs.push_back(EvInfo{ev.mono_ns, ev.channel, ev.struct_id, ev.kind});
    }
  }
  CHECK(evs.size() == kAllEvents);

  // ---- 1/2: two as-fast runs are byte-identical ----
  const RunOut a1 = RunAsFast(dir, small_ch, cam_ch, "a1");
  CheckFullRunStats(a1.stats);
  CHECK(a1.canon.head0 == kStruct0Count);
  CHECK(a1.canon.head1 == kStruct1Count);
  CHECK(a1.canon.fhead == kFrameCount);
  // Renumbered from 0 in injection order.
  for (uint32_t i = 0; i < kStruct0Count; ++i) CHECK(a1.canon.seq0[i] == i);
  for (uint32_t i = 0; i < kStruct1Count; ++i) CHECK(a1.canon.seq1[i] == i);
  const RunOut a2 = RunAsFast(dir, small_ch, cam_ch, "a2");
  CheckFullRunStats(a2.stats);
  CheckSameBytes(a1.canon.bytes, a2.canon.bytes, "as-fast run 1 vs run 2");

  // ---- 3: wall-clock 10x + pause/resume between two events + no-op seek ----
  {
    Targets t;
    t.Init("b");
    replay::ReplayPlayer player;
    CHECK(player.Open(dir, MakeConfig(small_ch, cam_ch, &t)) ==
          replay::ReplayStatus::kOk);
    CHECK(player.SetRate(10.0) == replay::ReplayStatus::kOk);
    uint64_t n = 0;
    bool paused_once = false;
    bool seeked = false;
    for (;;) {
      const replay::ReplayStatus st = player.Pump();
      if (st == replay::ReplayStatus::kEnd) break;
      CHECK(st == replay::ReplayStatus::kOk);
      ++n;
      if (n == 30 && !paused_once) {
        // Pause exactly on an event boundary; the wait must not consume or
        // duplicate the next event.
        paused_once = true;
        player.Pause();
        CHECK(player.paused());
        SleepNs(30000000ULL);  // ~4 timeline seconds at 10x
        player.Resume();
        CHECK(!player.paused());
      }
      if (n == 50 && !seeked) {
        // No-op seek: t is the next event's own timestamp, so the event set
        // is unchanged (rate/pause/seek alignment).
        seeked = true;
        CHECK(evs[50].mono_ns > evs[49].mono_ns);
        CHECK(player.Seek(evs[50].mono_ns) == replay::ReplayStatus::kOk);
      }
    }
    CHECK(n == kAllEvents);
    CheckFullRunStats(player.stats());
    const Canon c = Capture(t, 0, 0, 0, true);
    CheckSameBytes(a1.canon.bytes, c.bytes, "paced 10x + pause + seek");
    t.Close();
  }

  // ---- 4: pause before the first event, Step() k times, resume ----
  {
    Targets t;
    t.Init("c");
    replay::ReplayPlayer player;
    CHECK(player.Open(dir, MakeConfig(small_ch, cam_ch, &t)) ==
          replay::ReplayStatus::kOk);
    player.AsFast(true);
    player.Pause();
    CHECK(player.paused());
    constexpr uint64_t kSteps = 15;
    for (uint64_t i = 0; i < kSteps; ++i) {
      CHECK(player.Step() == replay::ReplayStatus::kOk);
      CHECK(player.delivered_events() == i + 1);  // exactly one per Step
      CHECK(player.paused());
    }
    player.Resume();
    CHECK(player.Run() == replay::ReplayStatus::kOk);
    CHECK(player.delivered_events() == kAllEvents);
    CheckFullRunStats(player.stats());
    const Canon c = Capture(t, 0, 0, 0, true);
    CheckSameBytes(a1.canon.bytes, c.bytes, "pause + step x15 + resume");
    t.Close();
  }

  // ---- 5: Seek(t) mid-stream == fresh session started at t ----
  {
    constexpr std::size_t kPrefix = 25;    // events injected before the seek
    constexpr std::size_t kSeekIdx = 40;   // suffix starts at this event
    CHECK(evs[kSeekIdx].mono_ns > evs[kPrefix - 1].mono_ns);
    const uint64_t t_seek = evs[kSeekIdx].mono_ns;
    uint64_t pre0 = 0;
    uint64_t pre1 = 0;
    uint64_t pref = 0;
    for (std::size_t i = 0; i < kPrefix; ++i) {
      if (evs[i].kind != replay::ReplayKind::kData) continue;
      if (evs[i].channel == small_ch) {
        (evs[i].struct_id == 0 ? pre0 : pre1) += 1;
      } else if (evs[i].channel == cam_ch) {
        pref += 1;
      }
    }
    CHECK(pre0 > 0 && pre1 > 0 && pref > 0);
    // Expected suffix timeline per target (from the merged event list).
    std::vector<uint64_t> suffix_mono0;
    std::vector<uint64_t> suffix_mono1;
    for (std::size_t i = kSeekIdx; i < evs.size(); ++i) {
      if (evs[i].kind != replay::ReplayKind::kData) continue;
      if (evs[i].channel == small_ch) {
        (evs[i].struct_id == 0 ? suffix_mono0 : suffix_mono1)
            .push_back(evs[i].mono_ns);
      }
    }

    // 5a: sequence preservation (opt-in) makes the two runs fully
    // byte-identical — the strongest form of "same event set".
    replay::PlayerStats fresh_stats;
    Canon fresh;
    {
      Targets t;
      t.Init("d0");
      replay::ReplayPlayerConfig cfg = MakeConfig(small_ch, cam_ch, &t);
      cfg.header_policy.renumber_sequence = false;
      replay::ReplayPlayer player;
      CHECK(player.Open(dir, cfg) == replay::ReplayStatus::kOk);
      CHECK(player.Seek(t_seek) == replay::ReplayStatus::kOk);
      player.AsFast(true);
      CHECK(player.Run() == replay::ReplayStatus::kOk);
      fresh_stats = player.stats();
      fresh = Capture(t, 0, 0, 0, true);
      t.Close();
    }
    CHECK(fresh_stats.events == kAllEvents - kSeekIdx);
    CHECK(fresh_stats.mono_regressions == 0);

    Targets t;
    t.Init("d1");
    replay::ReplayPlayerConfig cfg = MakeConfig(small_ch, cam_ch, &t);
    cfg.header_policy.renumber_sequence = false;
    replay::ReplayPlayer player;
    CHECK(player.Open(dir, cfg) == replay::ReplayStatus::kOk);
    player.AsFast(true);
    for (std::size_t i = 0; i < kPrefix; ++i) {
      CHECK(player.Pump() == replay::ReplayStatus::kOk);
    }
    CHECK(player.Seek(t_seek) == replay::ReplayStatus::kOk);
    CHECK(player.Run() == replay::ReplayStatus::kOk);
    CHECK(player.stats().events == kPrefix + (kAllEvents - kSeekIdx));
    CHECK(player.stats().mono_regressions == 0);
    const Canon mid = Capture(t, pre0, pre1, pref, true);
    t.Close();
    CheckSameBytes(fresh.bytes, mid.bytes, "seek: fresh-from-t vs mid-stream");

    // 5b: with the default renumber policy the mid-stream seek keeps the
    // per-target counters monotonic: suffix sequences continue at the prefix
    // count (slot level AND inside the rewritten embedded FrameHeader), and
    // the timeline is the fresh run's timeline.
    {
      Targets t2;
      t2.Init("d2");
      replay::ReplayPlayer player2;
      CHECK(player2.Open(dir, MakeConfig(small_ch, cam_ch, &t2)) ==
            replay::ReplayStatus::kOk);
      player2.AsFast(true);
      for (std::size_t i = 0; i < kPrefix; ++i) {
        CHECK(player2.Pump() == replay::ReplayStatus::kOk);
      }
      CHECK(player2.Seek(t_seek) == replay::ReplayStatus::kOk);
      CHECK(player2.Run() == replay::ReplayStatus::kOk);
      RecordMirrorRingPage* page = t2.ring.Page();
      CHECK(page->RingHead(0) == pre0 + suffix_mono0.size());
      CHECK(page->RingHead(1) == pre1 + suffix_mono1.size());
      uint8_t buf[512];
      for (std::size_t i = 0; i < suffix_mono0.size(); ++i) {
        uint32_t size = 0;
        uint64_t mono = 0;
        uint32_t seq = 0;
        CHECK(page->ReadRingSample(0, pre0 + i, buf, sizeof(buf), size, mono,
                                   seq));
        CHECK(mono == suffix_mono0[i]);
        CHECK(seq == pre0 + i);
        FrameHeader h;
        std::memcpy(&h, buf, sizeof(h));
        CHECK(h.sequence == pre0 + i);
      }
      for (std::size_t i = 0; i < suffix_mono1.size(); ++i) {
        uint32_t size = 0;
        uint64_t mono = 0;
        uint32_t seq = 0;
        CHECK(page->ReadRingSample(1, pre1 + i, buf, sizeof(buf), size, mono,
                                   seq));
        CHECK(mono == suffix_mono1[i]);
        CHECK(seq == pre1 + i);
      }
      t2.Close();
    }
  }

  std::filesystem::remove_all(root);
  std::printf("test_replay_determinism: OK\n");
  return 0;
}
