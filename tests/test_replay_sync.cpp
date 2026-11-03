// test_replay_sync.cpp — REP-05: synchronized replay of one package into a
// live big-frame channel and a live small-struct ring.
//
// The fixture records a real trigger package with two channels at different
// rates (small samples every 4 ms, big frames every 8 ms). The player merges
// the package timeline and injects every event into fresh targets:
//
//   A: as-fast full replay — per-channel counts/bytes, big-frame payloads,
//      flags and CRC, small-struct payloads, monotonic mono_ns across both
//      channels, no loss, skew counters sane;
//   B: wall-clock pacing — rate 2.0 approximately halves wall time;
//   C: Step() dispatches exactly one event and stays paused;
//   D: Stop() from another thread terminates a blocked Run();
//   E: mapping errors fail Open; unmapped channels are counted, not fatal.
#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/replay/replay_player.h"
#include "thyme/replay/replay_reader.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

constexpr uint64_t kPaceNs = 4000000;
constexpr uint64_t kBeforeNs = 100000000;
constexpr uint64_t kAfterNs = 200000000;
constexpr uint64_t kTriggerIter = 30;
constexpr uint64_t kTotalIters = 90;
constexpr uint32_t kFrameSlotBytes = 4096;
constexpr uint32_t kFrameSlots = 128;
constexpr uint32_t kRingDepth = 1024;

struct Pod16 {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};

uint32_t FrameLen(uint64_t frame) {
  return 512 + static_cast<uint32_t>(frame % 8) * 256;
}

void FillFrame(uint8_t* p, uint32_t len, uint64_t frame) {
  for (uint32_t j = 0; j < len; ++j) {
    p[j] = static_cast<uint8_t>(frame * 13 + j * 7);
  }
}

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

void RmTree(const std::string& path) {
  DIR* d = opendir(path.c_str());
  if (d == nullptr) {
    return;
  }
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') {
      continue;
    }
    const std::string sub = path + "/" + e->d_name;
    struct stat st {};
    if (stat(sub.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      RmTree(sub);
    } else {
      std::remove(sub.c_str());
    }
  }
  closedir(d);
  rmdir(path.c_str());
}

struct Fixture {
  std::string root;
  std::string page_name;
  std::string chan_name;
  std::string pkg_dir;
  record_mirror::RingClient mirror;
  PinnedSlotWriter frames;
  triggered::TriggeredRecorder rec;
  uint64_t sample_seq = 0;
  uint64_t frame_seq = 0;

  void Init() {
    char buf[160];
    const long me = static_cast<long>(getpid());
    std::snprintf(buf, sizeof(buf), "/tmp/thyme_rep05_%ld", me);
    root = buf;
    RmTree(root);
    CHECK(mkdir(root.c_str(), 0700) == 0);
    CHECK(mkdir((root + "/out").c_str(), 0700) == 0);
    std::snprintf(buf, sizeof(buf), "/thyme_rep05_p_%ld", me);
    page_name = buf;
    std::snprintf(buf, sizeof(buf), "/thyme_rep05_c_%ld", me);
    chan_name = buf;
    ShmRegion::UnlinkName(page_name.c_str());
    ShmRegion::UnlinkName(chan_name.c_str());

    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = sizeof(Pod16);
    std::snprintf(desc.name, sizeof(desc.name), "Pod16");
    RecordMirrorRingSpec spec{sizeof(Pod16), 256};
    CHECK(mirror.Create(page_name.c_str(), &desc, &spec, 1, 7));
    CHECK(frames.Create(chan_name.c_str(), kFrameSlotBytes, kFrameSlots, 0));
    frames.SetCrcEnabled(true);  // REC-21: transport CRC defaults off

    triggered::Config cfg;
    cfg.out_dir = root + "/out";
    triggered::SourceConfig mirror_src;
    mirror_src.kind = triggered::SourceConfig::Kind::kMirrorRing;
    mirror_src.shm_name = page_name;
    triggered::SourceConfig frame_src;
    frame_src.kind = triggered::SourceConfig::Kind::kPinnedSlot;
    frame_src.shm_name = chan_name;
    frame_src.channel_name = "cloud_1";
    cfg.sources = {mirror_src, frame_src};
    CHECK(rec.Init(cfg));
  }

  void Publish(uint64_t iter) {
    Pod16 p{};
    p.a = sample_seq;
    p.b = static_cast<uint32_t>(sample_seq * 2);
    p.c = static_cast<uint32_t>(sample_seq * 3);
    CHECK(mirror.Page()->TryWriteRing(0, &p, sizeof(p), NowNs(),
                                      static_cast<uint32_t>(sample_seq)));
    sample_seq += 1;
    if (iter % 2 != 0) {
      return;
    }
    const uint32_t len = FrameLen(frame_seq);
    PinnedSlotWriter::BeginView bv = frames.Begin(len);
    CHECK(bv.payload != nullptr);
    FillFrame(bv.payload, len, frame_seq);
    bv.header->flags.store(frame_seq % 4 == 0 ? 1u : 0u,
                           std::memory_order_relaxed);
    frames.Commit(NowNs());
    frame_seq += 1;
  }

  bool Record() {
    for (uint64_t i = 0; i < kTriggerIter; ++i) {
      Publish(i);
      rec.Tick();
      SleepNs(kPaceNs);
    }
    CHECK(rec.Trigger(1, kBeforeNs, kAfterNs));
    for (uint64_t i = kTriggerIter; i < kTotalIters; ++i) {
      Publish(i);
      rec.Tick();
      SleepNs(kPaceNs);
    }
    const uint64_t deadline = NowNs() + 15000000000ULL;
    while (rec.phase() != triggered::Phase::kArmed) {
      rec.Tick();
      if (NowNs() >= deadline) {
        return false;
      }
      SleepNs(100000);
    }
    pkg_dir = rec.last_package_dir();
    return !pkg_dir.empty();
  }

  void Shutdown() {
    rec.Shutdown();
    frames.Close();
    mirror.Shutdown(true);
  }

  void Cleanup() {
    Shutdown();
    RmTree(root);
  }
};

struct ExpectedFrame {
  uint64_t mono_ns = 0;
  uint32_t len = 0;
  uint32_t flags = 0;
  uint32_t crc = 0;
  std::vector<uint8_t> bytes;
};

struct ExpectedSample {
  uint64_t mono_ns = 0;
  uint32_t source_sequence = 0;
  std::vector<uint8_t> bytes;
};

struct Expected {
  std::vector<ExpectedFrame> frames;
  std::vector<ExpectedSample> samples;
  uint64_t events = 0;
  uint64_t first_ns = 0;
  uint64_t last_ns = 0;
};

Expected Scan(const std::string& dir, uint32_t big_ch, uint32_t small_ch) {
  Expected out;
  replay::ReplayPackage pkg;
  CHECK(pkg.Open(dir) == replay::ReplayStatus::kOk);
  out.first_ns = pkg.first_mono_ns();
  out.last_ns = pkg.last_mono_ns();
  replay::ReplayEvent ev;
  while (pkg.NextEvent(&ev) == replay::ReplayStatus::kOk) {
    out.events += 1;
    if (ev.channel == big_ch && ev.kind == replay::ReplayKind::kData) {
      ExpectedFrame f;
      f.mono_ns = ev.mono_ns;
      f.len = ev.len;
      f.flags = ev.frame_flags;
      f.crc = ev.payload_crc;
      f.bytes.assign(ev.payload, ev.payload + ev.len);
      out.frames.push_back(std::move(f));
    } else if (ev.channel == small_ch &&
               ev.kind == replay::ReplayKind::kData) {
      ExpectedSample s;
      s.mono_ns = ev.mono_ns;
      s.source_sequence = ev.source_sequence;
      s.bytes.assign(ev.payload, ev.payload + ev.len);
      out.samples.push_back(std::move(s));
    }
  }
  return out;
}

uint32_t FindChannel(replay::ReplayPackage& pkg, const std::string& name) {
  for (const replay::ReplayChannel& c : pkg.channels()) {
    if (c.name == name) {
      return c.id;
    }
  }
  CHECK(false);
  return 0;
}

struct Target {
  std::string frame_name;
  std::string ring_name;
  PinnedSlotWriter writer;
  record_mirror::RingClient ring;

  void Init(const char* tag) {
    char buf[128];
    const long me = static_cast<long>(getpid());
    std::snprintf(buf, sizeof(buf), "/thyme_rep05_%s_bf_%ld", tag, me);
    frame_name = buf;
    std::snprintf(buf, sizeof(buf), "/thyme_rep05_%s_ring_%ld", tag, me);
    ring_name = buf;
    ShmRegion::UnlinkName(frame_name.c_str());
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(frame_name.c_str()).c_str());
    ShmRegion::UnlinkName(ring_name.c_str());
    CHECK(writer.Create(frame_name.c_str(), kFrameSlotBytes, kFrameSlots, 0));
    writer.SetCrcEnabled(true);  // REC-21: transport CRC defaults off
    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = sizeof(Pod16);
    std::snprintf(desc.name, sizeof(desc.name), "Pod16");
    RecordMirrorRingSpec spec{sizeof(Pod16), kRingDepth};
    CHECK(ring.Create(ring_name.c_str(), &desc, &spec, 1, 9));
  }

  void Close() {
    writer.Close();
    ring.Shutdown(true);
  }
};

replay::ReplayPlayerConfig MakeConfig(uint32_t big_ch, uint32_t small_ch,
                                      Target& t) {
  replay::ReplayPlayerConfig cfg;
  cfg.pinned_slots.push_back(replay::PinnedSlotTarget{big_ch, &t.writer});
  replay::SmallStructTarget st;
  st.channel = small_ch;
  st.struct_id = 0;
  st.ring = t.ring.Page();
  st.target_struct_id = 0;
  cfg.small_structs.push_back(st);
  return cfg;
}

void CheckBigTarget(Target& t, const Expected& exp) {
  CHECK(t.writer.head() == exp.frames.size());
  PinnedSlotReader reader;
  CHECK(reader.Open(t.frame_name.c_str(), 2000000000ULL));
  for (uint64_t i = 0; i < exp.frames.size(); ++i) {
    const ExpectedFrame& want = exp.frames[i];
    PinnedSlotReader::View v;
    CHECK(reader.ReadFrame(i, v) == PinnedSlotReader::ReadResult::kOk);
    CHECK(v.data_len == want.len);
    CHECK(v.mono_ns == want.mono_ns);
    CHECK(v.flags == want.flags);
    CHECK(v.crc32 == want.crc);
    CHECK(v.crc_ok);
    CHECK(std::memcmp(v.payload, want.bytes.data(), want.len) == 0);
  }
}

void CheckSmallTarget(Target& t, const Expected& exp) {
  RecordMirrorRingPage* page = t.ring.Page();
  CHECK(page->RingHead(0) == exp.samples.size());
  uint8_t buf[256];
  for (uint64_t i = 0; i < exp.samples.size(); ++i) {
    const ExpectedSample& want = exp.samples[i];
    uint32_t size = 0;
    uint64_t mono = 0;
    uint32_t seq = 0;
    CHECK(page->ReadRingSample(0, i, buf, sizeof(buf), size, mono, seq));
    CHECK(size == want.bytes.size());
    CHECK(mono == want.mono_ns);
    // REP-12: the default policy renumbers source_sequence per target from 0
    // in injection order (want.source_sequence is the RECORDED producer
    // sequence, which starts mid-stream).
    CHECK(seq == i);
    CHECK(std::memcmp(buf, want.bytes.data(), size) == 0);
  }
}

uint64_t TimePacedReplay(const std::string& dir, double rate, uint32_t big_ch,
                         uint32_t small_ch, Target& t, const Expected& exp) {
  replay::ReplayPlayer player;
  CHECK(player.Open(dir, MakeConfig(big_ch, small_ch, t)) ==
        replay::ReplayStatus::kOk);
  CHECK(player.SetRate(rate) == replay::ReplayStatus::kOk);
  const uint64_t begin = NowNs();
  CHECK(player.Run() == replay::ReplayStatus::kOk);
  const uint64_t wall = NowNs() - begin;
  CHECK(player.stats().injected == exp.frames.size() + exp.samples.size());
  CHECK(player.stats().mono_regressions == 0);
  return wall;
}

}  // namespace

int main() {
  Fixture fx;
  fx.Init();
  CHECK(fx.Record());
  fx.Shutdown();

  replay::ReplayPackage pkg;
  CHECK(pkg.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
  const uint32_t big_ch = FindChannel(pkg, "cloud_1");
  const uint32_t small_ch = FindChannel(pkg, "small");
  const Expected exp = Scan(fx.pkg_dir, big_ch, small_ch);
  CHECK(exp.frames.size() > 20);
  CHECK(exp.samples.size() > 40);
  CHECK(exp.last_ns > exp.first_ns);
  const uint64_t span = exp.last_ns - exp.first_ns;
  uint64_t big_bytes = 0;
  for (const ExpectedFrame& f : exp.frames) {
    big_bytes += f.len;
  }

  // ---- A: as-fast synchronized replay into fresh targets ----
  {
    Target t;
    t.Init("a");
    replay::ReplayPlayer player;
    CHECK(player.Open(fx.pkg_dir, MakeConfig(big_ch, small_ch, t)) ==
          replay::ReplayStatus::kOk);
    CHECK(player.is_open());
    CHECK(player.channels().size() == pkg.channels().size());
    player.AsFast(true);
    CHECK(player.as_fast());
    const uint64_t begin = NowNs();
    CHECK(player.Run() == replay::ReplayStatus::kOk);
    const uint64_t wall = NowNs() - begin;

    const replay::PlayerStats& st = player.stats();
    CHECK(st.events == exp.events);
    CHECK(st.injected == exp.frames.size() + exp.samples.size());
    CHECK(st.skipped == 0);
    CHECK(st.failed == 0);
    CHECK(st.ignored > 0);  // meta.tbin trigger/stats rows
    CHECK(st.mono_regressions == 0);
    CHECK(st.first_event_ns == exp.first_ns);
    CHECK(st.last_event_ns == exp.last_ns);
    CHECK(st.last_skew_ns == 0);  // as-fast: no pacing clock
    CHECK(wall < span / 2 + 200000000ULL);

    const replay::PlayerChannelStats& bcs = player.channels()[big_ch];
    CHECK(bcs.kind == replay::ReplayChannelKind::kPinnedSlot);
    CHECK(bcs.events == exp.frames.size());
    CHECK(bcs.injected == exp.frames.size());
    CHECK(bcs.bytes == big_bytes);
    CHECK(bcs.skipped == 0);
    CHECK(bcs.last_event_ns == exp.frames.back().mono_ns);

    const replay::PlayerChannelStats& scs = player.channels()[small_ch];
    CHECK(scs.kind == replay::ReplayChannelKind::kSmall);
    CHECK(scs.events == exp.samples.size());
    CHECK(scs.injected == exp.samples.size());
    CHECK(scs.bytes == exp.samples.size() * sizeof(Pod16));
    CHECK(scs.skipped == 0);
    CHECK(scs.last_event_ns == exp.samples.back().mono_ns);

    // Both channels landed in their targets, in package order.
    CheckBigTarget(t, exp);
    CheckSmallTarget(t, exp);
    CHECK(player.state() == replay::ReplaySessionState::kEnded);
    CHECK(player.delivered_events() == exp.events);
    t.Close();
  }

  // ---- B: rate 2.0 approximately halves wall time ----
  {
    Target one;
    Target two;
    one.Init("b1");
    two.Init("b2");
    const uint64_t wall_one =
        TimePacedReplay(fx.pkg_dir, 1.0, big_ch, small_ch, one, exp);
    const uint64_t wall_two =
        TimePacedReplay(fx.pkg_dir, 2.0, big_ch, small_ch, two, exp);
    CHECK(wall_one >= span / 2);
    CHECK(wall_two < wall_one);
    CHECK(wall_two < wall_one * 8 / 10 + 20000000ULL);
    // The paced runs must have produced the same targets.
    CheckBigTarget(one, exp);
    CheckSmallTarget(one, exp);
    CheckBigTarget(two, exp);
    CheckSmallTarget(two, exp);
    std::printf(
        "sync replay timing: span=%.3f ms wall@1.0=%.3f ms wall@2.0=%.3f ms\n",
        static_cast<double>(span) / 1e6, static_cast<double>(wall_one) / 1e6,
        static_cast<double>(wall_two) / 1e6);
    one.Close();
    two.Close();
  }

  // ---- C: Step dispatches exactly one event and stays paused ----
  {
    Target t;
    t.Init("c");
    replay::ReplayPlayer player;
    CHECK(player.Open(fx.pkg_dir, MakeConfig(big_ch, small_ch, t)) ==
          replay::ReplayStatus::kOk);
    player.AsFast(true);
    player.Pause();
    CHECK(player.paused());
    CHECK(player.Step() == replay::ReplayStatus::kOk);
    CHECK(player.stats().events == 1);
    CHECK(player.delivered_events() == 1);
    CHECK(player.paused());
    CHECK(player.state() == replay::ReplaySessionState::kPaused);
    const uint64_t injected_1 = player.stats().injected;
    CHECK(injected_1 + player.stats().ignored + player.stats().skipped == 1);
    CHECK(player.Step() == replay::ReplayStatus::kOk);
    CHECK(player.stats().events == 2);
    CHECK(player.paused());
    CHECK(injected_1 + player.stats().ignored + player.stats().skipped >= 1);
    CHECK(player.stats().mono_regressions == 0);
    player.Resume();
    CHECK(!player.paused());
    CHECK(player.Run() == replay::ReplayStatus::kOk);
    CHECK(player.stats().events == exp.events);
    CHECK(player.stats().injected == exp.frames.size() + exp.samples.size());
    CHECK(player.stats().mono_regressions == 0);
    t.Close();
  }

  // ---- D: Stop() from another thread terminates a blocked Run() ----
  {
    Target t;
    t.Init("d");
    replay::ReplayPlayer player;
    CHECK(player.Open(fx.pkg_dir, MakeConfig(big_ch, small_ch, t)) ==
          replay::ReplayStatus::kOk);
    player.Pause();
    std::atomic<int> result{99};
    std::thread runner([&player, &result] {
      result.store(static_cast<int>(player.Run()), std::memory_order_relaxed);
    });
    SleepNs(50000000);
    const uint64_t begin = NowNs();
    player.Stop();
    runner.join();
    const uint64_t wall = NowNs() - begin;
    CHECK(result.load() ==
          static_cast<int>(replay::ReplayStatus::kStopped));
    CHECK(player.state() == replay::ReplaySessionState::kStopped);
    CHECK(wall < 2000000000ULL);
    CHECK(player.Step() == replay::ReplayStatus::kStopped);
    t.Close();
  }

  // ---- E: mapping errors fail Open; unmapped channels are counted ----
  {
    Target t;
    t.Init("e");
    {
      // Big-frame channel mapped as small struct: invalid.
      replay::ReplayPlayer player;
      replay::ReplayPlayerConfig cfg;
      replay::SmallStructTarget st;
      st.channel = big_ch;
      st.struct_id = 0;
      st.ring = t.ring.Page();
      st.target_struct_id = 0;
      cfg.small_structs.push_back(st);
      CHECK(player.Open(fx.pkg_dir, cfg) ==
            replay::ReplayStatus::kInvalidArgument);
      CHECK(!player.is_open());
      CHECK(!player.error().empty());
    }
    {
      // Small channel mapped as big frame: invalid.
      replay::ReplayPlayer player;
      replay::ReplayPlayerConfig cfg;
      cfg.pinned_slots.push_back(replay::PinnedSlotTarget{small_ch, &t.writer});
      CHECK(player.Open(fx.pkg_dir, cfg) ==
            replay::ReplayStatus::kInvalidArgument);
      CHECK(!player.error().empty());
    }
    {
      // Only the big channel mapped: small data is counted as unmapped and
      // the run still completes.
      replay::ReplayPlayer player;
      replay::ReplayPlayerConfig cfg;
      cfg.pinned_slots.push_back(replay::PinnedSlotTarget{big_ch, &t.writer});
      CHECK(player.Open(fx.pkg_dir, cfg) == replay::ReplayStatus::kOk);
      player.AsFast(true);
      CHECK(player.Run() == replay::ReplayStatus::kOk);
      CHECK(player.stats().injected == exp.frames.size());
      CHECK(player.stats().failed == exp.samples.size());
      CHECK(player.stats().events == exp.events);
      CHECK(t.writer.head() == exp.frames.size());
    }
    t.Close();
  }

  fx.Cleanup();
  std::printf("test_replay_sync: OK (%zu frames, %zu samples, %llu events)\n",
              exp.frames.size(), exp.samples.size(),
              static_cast<unsigned long long>(exp.events));
  return 0;
}
