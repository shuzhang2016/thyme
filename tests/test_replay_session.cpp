// test_replay_session.cpp — REP-02: paced ReplaySession over a real package.
//
// Verifies:
//   A: AsFast replays the whole timeline quickly and in order;
//   B: SetRate(2.0) approximately halves wall time (loose bounds);
//   C: Pause/Step deliver one event per Step and keep the session paused;
//   D: a paused NextEvent() is interrupted by Stop() from another thread (no
//      forever-blocking pause);
//   E: Seek resets the timeline and restarts pacing at the new position;
//   F: invalid rates and Step after Stop return explicit statuses.
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/replay/replay_reader.h"
#include "thyme/replay/replay_session.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <chrono>
#include <cmath>
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

constexpr uint64_t kPaceNs = 4000000;      // 4 ms
constexpr uint64_t kBeforeNs = 100000000;  // 100 ms
constexpr uint64_t kAfterNs = 200000000;   // 200 ms
constexpr uint64_t kTriggerIter = 30;
constexpr uint64_t kTotalIters = 90;

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
  std::string aux_name;
  std::string pkg_dir;
  record_mirror::RingClient mirror;
  PinnedSlotWriter frames;
  PinnedSlotWriter frames_aux;
  triggered::TriggeredRecorder rec;
  uint64_t next_frame = 0;
  bool shutdown_done = false;

  void Init() {
    char buf[160];
    const long me = static_cast<long>(getpid());
    std::snprintf(buf, sizeof(buf), "/tmp/thyme_rep02_%ld", me);
    root = buf;
    RmTree(root);
    CHECK(mkdir(root.c_str(), 0700) == 0);
    CHECK(mkdir((root + "/out").c_str(), 0700) == 0);
    std::snprintf(buf, sizeof(buf), "/thyme_rep02_p_%ld", me);
    page_name = buf;
    std::snprintf(buf, sizeof(buf), "/thyme_rep02_c_%ld", me);
    chan_name = buf;
    std::snprintf(buf, sizeof(buf), "/thyme_rep02_a_%ld", me);
    aux_name = buf;
    ShmRegion::UnlinkName(page_name.c_str());
    ShmRegion::UnlinkName(chan_name.c_str());
    ShmRegion::UnlinkName(aux_name.c_str());

    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = sizeof(Pod16);
    std::snprintf(desc.name, sizeof(desc.name), "Pod16");
    RecordMirrorRingSpec spec{sizeof(Pod16), 256};
    CHECK(mirror.Create(page_name.c_str(), &desc, &spec, 1, 7));
    CHECK(frames.Create(chan_name.c_str(), 4096, 128, 0));
    CHECK(frames_aux.Create(aux_name.c_str(), 4096, 128, 0));

    triggered::Config cfg;
    cfg.out_dir = root + "/out";
    triggered::SourceConfig mirror_src;
    mirror_src.kind = triggered::SourceConfig::Kind::kMirrorRing;
    mirror_src.shm_name = page_name;
    triggered::SourceConfig frame_src;
    frame_src.kind = triggered::SourceConfig::Kind::kPinnedSlot;
    frame_src.shm_name = chan_name;
    frame_src.channel_name = "cloud_1";
    triggered::SourceConfig aux_src;
    aux_src.kind = triggered::SourceConfig::Kind::kPinnedSlot;
    aux_src.shm_name = aux_name;
    aux_src.channel_name = "aux_0";
    cfg.sources = {mirror_src, frame_src, aux_src};
    CHECK(rec.Init(cfg));
  }

  void Publish() {
    Pod16 p{};
    p.a = next_frame;
    p.b = static_cast<uint32_t>(next_frame * 2);
    p.c = static_cast<uint32_t>(next_frame * 3);
    CHECK(mirror.Page()->TryWriteRing(0, &p, sizeof(p), NowNs(),
                                      static_cast<uint32_t>(next_frame)));
    const uint32_t len = FrameLen(next_frame);
    PinnedSlotWriter::BeginView bv = frames.Begin(len);
    CHECK(bv.payload != nullptr);
    FillFrame(bv.payload, len, next_frame);
    frames.Commit(NowNs());
    PinnedSlotWriter::BeginView aux = frames_aux.Begin(len);
    CHECK(aux.payload != nullptr);
    FillFrame(aux.payload, len, next_frame + 1000);
    frames_aux.Commit(NowNs());
    next_frame += 1;
  }

  bool Record() {
    for (uint64_t i = 0; i < kTriggerIter; ++i) {
      Publish();
      rec.Tick();
      SleepNs(kPaceNs);
    }
    CHECK(rec.Trigger(1, kBeforeNs, kAfterNs));
    for (uint64_t i = kTriggerIter; i < kTotalIters; ++i) {
      Publish();
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
    if (shutdown_done) {
      return;
    }
    shutdown_done = true;
    rec.Shutdown();
    frames.Close();
    frames_aux.Close();
    mirror.Shutdown(true);
  }
};

struct Snap {
  uint64_t mono_ns = 0;
  uint32_t channel = 0;
  replay::ReplayKind kind = replay::ReplayKind::kData;
  uint64_t seq = 0;

  bool operator==(const Snap& o) const {
    return mono_ns == o.mono_ns && channel == o.channel && kind == o.kind &&
           seq == o.seq;
  }
};

Snap ToSnap(const replay::ReplayEvent& ev) {
  Snap s;
  s.mono_ns = ev.mono_ns;
  s.channel = ev.channel;
  s.kind = ev.kind;
  s.seq = ev.record_sequence;
  return s;
}

std::vector<Snap> CollectAll(replay::ReplaySession& session) {
  std::vector<Snap> out;
  replay::ReplayEvent ev;
  replay::ReplayStatus st = replay::ReplayStatus::kOk;
  while ((st = session.NextEvent(&ev)) == replay::ReplayStatus::kOk) {
    out.push_back(ToSnap(ev));
  }
  CHECK(st == replay::ReplayStatus::kEnd);
  return out;
}

uint64_t TimeReplay(const std::string& dir, double rate,
                    std::vector<Snap>* events) {
  replay::ReplaySession session;
  CHECK(session.Open(dir) == replay::ReplayStatus::kOk);
  CHECK(session.SetRate(rate) == replay::ReplayStatus::kOk);
  const uint64_t begin = NowNs();
  *events = CollectAll(session);
  return NowNs() - begin;
}

}  // namespace

int main() {
  Fixture fx;
  fx.Init();
  CHECK(fx.Record());
  fx.Shutdown();

  // Reference order from the package reader.
  replay::ReplayPackage pkg;
  CHECK(pkg.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
  std::vector<Snap> expected;
  {
    replay::ReplayEvent ev;
    while (pkg.NextEvent(&ev) == replay::ReplayStatus::kOk) {
      expected.push_back(ToSnap(ev));
    }
  }
  CHECK(!expected.empty());
  const uint64_t span = pkg.last_mono_ns() - pkg.first_mono_ns();
  CHECK(span > 50000000);  // the fixture must span a measurable window

  // ---- A: AsFast replays the timeline quickly and in order ----
  {
    replay::ReplaySession session;
    CHECK(session.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
    CHECK(!session.as_fast());
    CHECK(session.rate() == 1.0);
    session.AsFast(true);
    CHECK(session.as_fast());
    const uint64_t begin = NowNs();
    const std::vector<Snap> got = CollectAll(session);
    const uint64_t wall = NowNs() - begin;
    CHECK(got.size() == expected.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
      CHECK(got[i] == expected[i]);
    }
    CHECK(session.state() == replay::ReplaySessionState::kEnded);
    CHECK(session.status() == replay::ReplayStatus::kEnd);
    CHECK(session.delivered_events() == expected.size());
    // As-fast must not pace on the ~300 ms timeline.
    CHECK(wall < span / 2 + 100000000ULL);
  }

  // ---- B: rate 2.0 approximately halves wall time ----
  {
    std::vector<Snap> at_one;
    std::vector<Snap> at_two;
    const uint64_t wall_one = TimeReplay(fx.pkg_dir, 1.0, &at_one);
    const uint64_t wall_two = TimeReplay(fx.pkg_dir, 2.0, &at_two);
    CHECK(at_one.size() == expected.size());
    CHECK(at_two.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
      CHECK(at_one[i] == expected[i]);
      CHECK(at_two[i] == expected[i]);
    }
    // Pacing actually happened at rate 1 (at least half the mono span).
    CHECK(wall_one >= span / 2);
    // Rate 2 must be clearly faster; loose bound so a loaded CI box does not
    // flake as long as both runs are stretched proportionally.
    CHECK(wall_two < wall_one * 8 / 10 + 20000000ULL);
    CHECK(wall_two < wall_one);
    std::printf("replay timing: span=%.3f ms wall@1.0=%.3f ms wall@2.0=%.3f ms\n",
                static_cast<double>(span) / 1e6,
                static_cast<double>(wall_one) / 1e6,
                static_cast<double>(wall_two) / 1e6);
  }

  // ---- C: pause/step semantics ----
  {
    replay::ReplaySession session;
    CHECK(session.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
    session.AsFast(true);
    session.Pause();
    CHECK(session.paused());
    CHECK(session.state() == replay::ReplaySessionState::kPaused);
    for (std::size_t i = 0; i < 5; ++i) {
      replay::ReplayEvent ev;
      CHECK(session.Step(&ev) == replay::ReplayStatus::kOk);
      CHECK(ToSnap(ev) == expected[i]);
      CHECK(session.paused());
      CHECK(session.state() == replay::ReplaySessionState::kPaused);
    }
    session.Resume();
    CHECK(!session.paused());
    replay::ReplayEvent ev;
    CHECK(session.NextEvent(&ev) == replay::ReplayStatus::kOk);
    CHECK(ToSnap(ev) == expected[5]);
    // Drain the rest and step past the end.
    std::vector<Snap> rest;
    while (session.NextEvent(&ev) == replay::ReplayStatus::kOk) {
      rest.push_back(ToSnap(ev));
    }
    CHECK(rest.size() == expected.size() - 6);
    CHECK(session.Step(&ev) == replay::ReplayStatus::kEnd);
    CHECK(session.state() == replay::ReplaySessionState::kEnded);
  }

  // ---- D: Stop() interrupts a paused NextEvent() ----
  {
    replay::ReplaySession session;
    CHECK(session.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
    session.Pause();
    std::thread stopper([&session] {
      SleepNs(50000000);
      session.Stop();
    });
    const uint64_t begin = NowNs();
    replay::ReplayEvent ev;
    const replay::ReplayStatus st = session.NextEvent(&ev);
    const uint64_t wall = NowNs() - begin;
    stopper.join();
    CHECK(st == replay::ReplayStatus::kStopped);
    CHECK(session.stopped());
    CHECK(session.state() == replay::ReplaySessionState::kStopped);
    CHECK(wall < 2000000000ULL);  // never blocks forever
    CHECK(wall >= 30000000ULL);
    CHECK(session.Step(&ev) == replay::ReplayStatus::kStopped);
  }

  // ---- E: Seek repositions and the next event is immediate ----
  {
    const uint64_t target = expected[expected.size() / 2].mono_ns + 1;
    std::size_t expect = 0;
    while (expect < expected.size() && expected[expect].mono_ns < target) {
      expect += 1;
    }
    replay::ReplaySession session;
    CHECK(session.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
    CHECK(session.Seek(target) == replay::ReplayStatus::kOk);
    const uint64_t begin = NowNs();
    replay::ReplayEvent ev;
    CHECK(session.NextEvent(&ev) == replay::ReplayStatus::kOk);
    CHECK((NowNs() - begin) < 50000000ULL);
    CHECK(ToSnap(ev) == expected[expect]);
  }

  // ---- F: invalid API usage ----
  {
    replay::ReplaySession session;
    replay::ReplayEvent ev;
    CHECK(session.NextEvent(&ev) == replay::ReplayStatus::kInvalidArgument);
    CHECK(session.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
    CHECK(session.SetRate(0.0) == replay::ReplayStatus::kInvalidArgument);
    CHECK(session.SetRate(-1.0) == replay::ReplayStatus::kInvalidArgument);
    CHECK(session.SetRate(std::nan("")) == replay::ReplayStatus::kInvalidArgument);
    CHECK(session.SetRate(0.5) == replay::ReplayStatus::kOk);
    CHECK(session.rate() == 0.5);
    CHECK(session.NextEvent(nullptr) == replay::ReplayStatus::kInvalidArgument);
  }

  RmTree(fx.root);
  std::printf("test_replay_session: OK\n");
  return 0;
}
