// test_continuous.cpp — continuous-acquisition mode (design 6.6) on the
// TriggeredRecorder engine: Arm -> rolling sealed slices -> gap-free coverage
// across slice boundaries -> Disarm seals; pinned_slot excluded by default and
// included via whitelist; concurrency with a triggered capture (slices keep
// rolling, shared pending quota with continuous evicted first); retention
// priority (slices deleted before trigger packages, high/ untouched).
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/retention.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
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

bool PathExists(const std::string& p) {
  struct stat st {};
  return stat(p.c_str(), &st) == 0;
}

void RmTree(const std::string& path) {
  DIR* d = opendir(path.c_str());
  if (d == nullptr) return;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const std::string sub = path + "/" + e->d_name;
    if (e->d_type == DT_DIR) RmTree(sub);
    else std::remove(sub.c_str());
  }
  closedir(d);
  rmdir(path.c_str());
}

std::vector<std::string> ListDirs(const std::string& path,
                                  const char* prefix) {
  std::vector<std::string> out;
  DIR* d = opendir(path.c_str());
  if (d == nullptr) return out;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    if (e->d_type == DT_DIR &&
        std::strncmp(e->d_name, prefix, std::strlen(prefix)) == 0) {
      out.push_back(path + "/" + e->d_name);
    }
  }
  closedir(d);
  return out;
}

std::vector<ThymeBinDataEntry> ReadAll(const std::string& path) {
  ThymeBinReader rd(path.c_str());
  CHECK(rd.ok());
  std::vector<ThymeBinDataEntry> out;
  ThymeBinDataEntry e;
  while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) out.push_back(e);
  return out;
}

struct Pod16 {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};

// In-process producer. Step() uses a synthetic monotonic clock (base +
// i*step); StepNow() stamps the real clock (needed when a triggered capture's
// wall-clock window must cover the samples).
struct Producer {
  record_mirror::RingClient mirror;
  PinnedSlotWriter frames;
  bool has_frames = false;
  uint64_t i = 0;
  uint64_t base = 0;

  void Start(const char* page_name, const char* chan_name, bool with_frames) {
    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = sizeof(Pod16);
    std::snprintf(desc.name, sizeof(desc.name), "Pod16");
    RecordMirrorRingSpec spec{sizeof(Pod16), 4096};
    CHECK(mirror.Create(page_name, &desc, &spec, 1, 3));
    // Synthetic clock starts 1 h in the past: sections 1-2 (synthetic monos)
    // never overlap section 3's real-time trigger windows.
    base = NowNs() - 3600000000000ULL;
    has_frames = with_frames;
    if (with_frames) CHECK(frames.Create(chan_name, 2048, 64, 0));
  }
  void Write(uint64_t mono) {
    Pod16 p{i, static_cast<uint32_t>(i * 2), static_cast<uint32_t>(i * 3)};
    CHECK(mirror.Page()->TryWriteRing(0, &p, sizeof(p), mono,
                                      static_cast<uint32_t>(i)));
    ++i;
  }
  void Step(uint64_t step_ns) { Write(base + i * step_ns); }
  void StepNow() { Write(NowNs()); }
  void Frame() {
    PinnedSlotWriter::BeginView bv = frames.Begin(700);
    CHECK(bv.payload != nullptr);
    std::memset(bv.payload, static_cast<int>(i & 0xFF), 700);
    frames.Commit(NowNs());
  }
  void Stop() {
    frames.Close();
    mirror.Shutdown(true);
  }
};

void TickFor(triggered::TriggeredRecorder& rec, uint64_t ns) {
  const uint64_t end = NowNs() + ns;
  while (NowNs() < end) {
    rec.Tick();
    SleepNs(200000);
  }
}

triggered::CaptureStatsRecord SliceStats(const std::string& dir) {
  triggered::CaptureStatsRecord csr{};
  for (const auto& e : ReadAll(dir + "/meta.tbin")) {
    if (e.struct_id == triggered::kMetaStructId && e.source_sequence == 1) {
      std::memcpy(&csr, e.payload.data(), sizeof(csr));
    }
  }
  return csr;
}

}  // namespace

int main() {
  alarm(120);
  const int me = static_cast<int>(getpid());
  char page_name[96], chan_name[96], out_dir[96];
  std::snprintf(page_name, sizeof(page_name), "/c9cont_page_%d", me);
  std::snprintf(chan_name, sizeof(chan_name), "/c9cont_chan_%d", me);
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/c9cont_%d", me);
  ShmRegion::UnlinkName(page_name);
  ShmRegion::UnlinkName(chan_name);
  RmTree(out_dir);

  Producer prod;
  prod.Start(page_name, chan_name, true);

  triggered::Config ecfg;
  ecfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name, ""},
                  {triggered::SourceConfig::Kind::kPinnedSlot, chan_name,
                   "h264_front"}};
  ecfg.out_dir = out_dir;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(ecfg));

  // ---- 1. Arm, produce, roll slices by bytes, Disarm seals ----
  triggered::ContinuousConfig ccfg;
  ccfg.slice_bytes = 4096;  // ~80 Pod16 descriptors per slice
  ccfg.slice_ns = 0;
  CHECK(rec.ArmContinuous(ccfg));
  for (int k = 0; k < 300; ++k) {
    prod.Step(1000000);  // synthetic 1 ms apart
    if (k % 20 == 0) rec.Tick();
  }
  TickFor(rec, 30000000ULL);  // let the drain catch up
  rec.DisarmContinuous();
  CHECK(rec.continuous_stats().slices_done >= 3);
  CHECK(rec.continuous_stats().samples_written == 300);
  CHECK(rec.continuous_stats().holes_written == 0);

  // Every slice sealed; coverage gap-free and ordered across slice boundaries.
  std::vector<std::string> slices = ListDirs(out_dir, "continuous_");
  // readdir order is arbitrary; sort by the numeric seq in the name
  // (lexical order breaks at seq >= 10).
  std::sort(slices.begin(), slices.end(), [](const std::string& x,
                                             const std::string& y) {
    auto seq_of = [](const std::string& s) {
      const std::size_t a = s.find("continuous_") + 11;
      return std::strtoull(s.c_str() + a, nullptr, 10);
    };
    return seq_of(x) < seq_of(y);
  });
  CHECK(slices.size() == rec.continuous_stats().slices_done);
  uint64_t expect_a = 0;
  uint64_t total_rows = 0;
  for (const std::string& s : slices) {
    CHECK(triggered::TriggeredRecorder::PackageComplete(s));
    bool has_cont_meta = false;
    for (const auto& e : ReadAll(s + "/meta.tbin")) {
      if (e.struct_id == triggered::kMetaStructId && e.source_sequence == 2) {
        has_cont_meta = true;  // continuous-mode marker record
      }
    }
    CHECK(has_cont_meta);
    for (const auto& e : ReadAll(s + "/small.tbin")) {
      CHECK(e.type == ThymeBinEntryType::kData);
      Pod16 p{};
      std::memcpy(&p, e.payload.data(), sizeof(p));
      CHECK(p.a == expect_a);  // adjacent slices cover mono continuously, no gap
      CHECK(p.b == p.a * 2 && p.c == p.a * 3);
      ++expect_a;
      ++total_rows;
    }
  }
  CHECK(total_rows == 300);
  // Large frames are excluded by default.
  for (const std::string& s : slices) {
    CHECK(!PathExists(s + "/frames/h264_front.raw"));
  }

  // ---- 2. shared small-struct glob + real decimation ----
  RmTree(out_dir);
  CHECK(mkdir(out_dir, 0700) == 0);
  ccfg.channels = {"small_struct:*@dec=3"};
  ccfg.slice_bytes = 100000;
  CHECK(rec.ArmContinuous(ccfg));
  for (int k = 0; k < 30; ++k) {
    prod.Step(1000000);
    rec.Tick();
  }
  TickFor(rec, 30000000ULL);
  rec.DisarmContinuous();
  CHECK(rec.continuous_stats().samples_written == 10);
  for (const std::string& s : ListDirs(out_dir, "continuous_")) {
    for (const auto& e : ReadAll(s + "/small.tbin")) {
      CHECK(e.type == ThymeBinEntryType::kData);
      CHECK(e.source_sequence % 3 == 0);
    }
  }
  ccfg.channels = {"small_struct:/does_not_match_*"};
  CHECK(!rec.ArmContinuous(ccfg));  // zero selected channels cannot auto-arm

  // ---- 3. pinned_slot whitelist ----
  RmTree(out_dir);
  CHECK(mkdir(out_dir, 0700) == 0);
  ccfg.channels = {"small_struct:*", "pinned_slot:h264_front"};
  ccfg.slice_bytes = 100000;  // one slice
  CHECK(rec.ArmContinuous(ccfg));
  for (int k = 0; k < 50; ++k) {
    prod.Step(1000000);
    prod.Frame();
    if (k % 10 == 0) rec.Tick();
  }
  TickFor(rec, 30000000ULL);
  rec.DisarmContinuous();
  CHECK(rec.continuous_stats().frames_written == 50);
  {
    const std::vector<std::string> s2 = ListDirs(out_dir, "continuous_");
    CHECK(!s2.empty());
    uint64_t idx_rows = 0;
    for (const std::string& s : s2) {
      CHECK(triggered::TriggeredRecorder::PackageComplete(s));
      if (PathExists(s + "/frames/h264_front.index.tbin")) {
        idx_rows += ReadAll(s + "/frames/h264_front.index.tbin").size();
        CHECK(PathExists(s + "/frames/h264_front.raw"));
      }
    }
    CHECK(idx_rows == 50);
  }

  // ---- 3. concurrency + shared quota: continuous evicted first ----
  // Mirror-only engine keeps the arithmetic small (a triggered capture also
  // harvests pinned_slot, whose 700 B descriptors would dominate a tiny quota).
  rec.Shutdown();
  triggered::Config ecfg2;
  ecfg2.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name,
                    ""}};
  ecfg2.out_dir = out_dir;
  ecfg2.pending_bytes_quota = 1200;  // 75 Pod16 descriptors across both buckets
  CHECK(rec.Init(ecfg2));

  triggered::ContinuousConfig ccfg3;
  ccfg3.wait_idle = true;      // gate closed: queue fills without draining
  ccfg3.slice_bytes = 100000;
  CHECK(rec.ArmContinuous(ccfg3));
  rec.SetContinuousGate(false);
  for (int k = 0; k < 40; ++k) {
    prod.StepNow();
    rec.Tick();
    SleepNs(200000);
  }
  CHECK(rec.pending_bytes() == 40 * sizeof(Pod16));  // all in the cont bucket

  // Triggered capture harvests the same recent history (before=200 ms covers
  // only this section's real-time samples; sections 1-2 used synthetic monos
  // far in the past): the continuous bucket must be evicted FIRST (triggered
  // packages first), and the triggered capture loses nothing of its own.
  CHECK(rec.Trigger(1, 200000000ULL, 30000000ULL));  // before 200ms, post 30ms
  // 640B(cont) + 640B(triggered) > 1200B quota -> exactly 5 cont descriptors
  // (80B) evicted; the triggered bucket is untouched (triggered packages first).
  CHECK(rec.continuous_stats().queue_evicted == 5);
  for (int k = 0; k < 15; ++k) {  // post window traffic
    prod.StepNow();
    rec.Tick();
    SleepNs(1000000);
  }
  CHECK(rec.pending_bytes() <= 1200);  // capped by the shared quota
  CHECK(rec.Preempt());                // truncate and seal (realtime)
  CHECK(rec.phase() == triggered::Phase::kArmed);
  const triggered::CaptureStatsRecord csr =
      SliceStats(rec.last_package_dir());
  CHECK(csr.holes_written == 0);
  CHECK(csr.pending_evicted == 0);       // not a single triggered record lost
  CHECK(csr.samples_written >= 40);
  CHECK(triggered::TriggeredRecorder::PackageComplete(
      rec.last_package_dir()));

  // Continuous kept collecting through the whole triggered capture.
  rec.SetContinuousGate(true);  // idle -> drain the surviving queue
  for (int k = 0; k < 10; ++k) {
    prod.StepNow();
    rec.Tick();
    SleepNs(1000000);
  }
  TickFor(rec, 20000000ULL);
  rec.DisarmContinuous();
  CHECK(rec.continuous_stats().slices_done >= 1);
  CHECK(rec.continuous_stats().samples_written >= 10);

  // ---- 4. retention priority: slices deleted before trigger packages ----
  {
    retention::Manager rm;
    retention::Config rcfg;
    rcfg.out_dir = out_dir;
    CHECK(rm.Init(rcfg));
    uint64_t total = 0, cont_total = 0, trig_total = 0;
    for (const auto& p : rm.Scan()) {
      total += p.bytes;
      const std::string leaf = p.dir.substr(p.dir.find_last_of('/') + 1);
      if (leaf.compare(0, 11, "continuous_") == 0) cont_total += p.bytes;
      else trig_total += p.bytes;
    }
    CHECK(cont_total > 0 && trig_total > 0);
    rcfg.disk_quota_bytes = total - cont_total;  // exactly the slices' worth
    CHECK(rm.Init(rcfg));
    CHECK(rm.Enforce({}) >= cont_total);
    CHECK(ListDirs(out_dir, "continuous_").empty());  // continuous slices deleted first
    CHECK(!ListDirs(out_dir, "1_").empty());          // triggered packages kept
  }

  prod.Stop();
  rec.Shutdown();
  RmTree(out_dir);
  ShmRegion::UnlinkName(chan_name);
  return 0;
}
