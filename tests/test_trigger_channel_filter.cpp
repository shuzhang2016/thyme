// test_trigger_channel_filter.cpp — TRG-18 end-to-end per-source channel
// filter: a trigger with `channels = a,c` must write only those [channel.*]
// entries (raw + index + manifest records); an empty list records everything.
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/session_lease.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
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

void RmTree(const std::string& path) {
  DIR* d = opendir(path.c_str());
  if (d == nullptr) return;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const std::string sub = path + "/" + e->d_name;
    if (e->d_type == DT_DIR) {
      RmTree(sub);
    } else {
      std::remove(sub.c_str());
    }
  }
  closedir(d);
  rmdir(path.c_str());
}

bool Exists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

uint32_t CountData(const std::string& path) {
  ThymeBinReader rd(path.c_str());
  CHECK(rd.ok());
  uint32_t n = 0;
  ThymeBinDataEntry e;
  while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
    if (e.type == ThymeBinEntryType::kData) ++n;
  }
  return n;
}

void FillFrame(uint8_t* p, uint32_t len, uint64_t seq) {
  for (uint32_t i = 0; i < len; ++i) {
    p[i] = static_cast<uint8_t>(seq * 31 + i * 7);
  }
}

void Publish(PinnedSlotWriter& w, uint64_t first, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t len = 256;
    PinnedSlotWriter::BeginView bv = w.Begin(len);
    CHECK(bv.payload != nullptr);
    FillFrame(bv.payload, len, first + i);
    w.Commit(NowNs());
  }
}

void RunCapture(triggered::TriggeredRecorder& rec) {
  const uint64_t deadline = NowNs() + 15000000000ULL;
  while (rec.phase() != triggered::Phase::kArmed) {
    rec.Tick();
    CHECK(NowNs() < deadline);
    SleepNs(50000);
  }
}

triggered::CaptureStatsRecord Stats(const std::string& pkg) {
  const std::vector<ThymeBinDataEntry> meta = [&] {
    ThymeBinReader rd((pkg + "/meta.tbin").c_str());
    CHECK(rd.ok());
    std::vector<ThymeBinDataEntry> out;
    ThymeBinDataEntry e;
    while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
      out.push_back(e);
    }
    return out;
  }();
  triggered::CaptureStatsRecord csr{};
  bool found = false;
  for (const auto& e : meta) {
    if (e.struct_id == triggered::kMetaStructId &&
        e.payload.size() == sizeof(csr)) {
      std::memcpy(&csr, e.payload.data(), sizeof(csr));
      found = true;
    }
  }
  CHECK(found);
  return csr;
}

void TestAggregateStructFilter() {
  const int me = static_cast<int>(getpid());
  char shm[64];
  char dir[96];
  std::snprintf(shm, sizeof(shm), "/c9agg_%d", me);
  std::snprintf(dir, sizeof(dir), "/tmp/thyme_c9agg_%d", me);
  ShmRegion::UnlinkName(shm);
  RmTree(dir);

  ThymeStructDesc descs[2]{};
  descs[0].struct_id = 0;
  descs[1].struct_id = 1;
  descs[0].payload_size = 8;
  descs[1].payload_size = 8;
  descs[0].contract_version = 1;
  descs[1].contract_version = 1;
  std::snprintf(descs[0].name, sizeof(descs[0].name), "alpha");
  std::snprintf(descs[1].name, sizeof(descs[1].name), "bravo");
  RecordMirrorRingSpec specs[2] = {{8, 4}, {8, 4}};
  record_mirror::RingClient mirror;
  CHECK(mirror.Create(shm, descs, specs, 2, 1));
  RecordMirrorRingPage* page = mirror.Page();
  CHECK(page != nullptr);
  const uint64_t stamp = NowNs();
  for (uint32_t i = 0; i < 2; ++i) {
    uint64_t sample = 10 + i;
    CHECK(page->TryWriteRing(0, &sample, 8, stamp, i));
    sample = 20 + i;
    CHECK(page->TryWriteRing(1, &sample, 8, stamp, i));
  }

  triggered::SourceConfig source;
  source.kind = triggered::SourceConfig::Kind::kMirrorRing;
  source.shm_name = shm;
  source.record_channels = {"alpha", "bravo"};
  std::vector<triggered::ChannelSelectorRule> rules;
  CHECK(triggered::ParseChannelSelectors(
      {"small_struct:*", "!small_struct:bravo"}, &rules, nullptr));
  CHECK(triggered::MatchChannelSelector(rules, source) != nullptr);
  uint32_t decimate = 1;
  CHECK(triggered::SelectRecordStruct(rules, source, 0, &decimate));
  CHECK(!triggered::SelectRecordStruct(rules, source, 1, &decimate));

  triggered::Config cfg;
  cfg.sources.push_back(source);
  cfg.out_dir = dir;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(cfg));
  CHECK(rec.Trigger(1, 10000000000ULL, 0, 0, false, false,
                    {"small_struct:alpha"}));
  RunCapture(rec);
  CHECK(rec.stats().captures_done == 1);
  CHECK(rec.stats().samples_written == 2);
  const std::string pkg = rec.last_package_dir();
  ThymeBinReader rd((pkg + "/small.tbin").c_str());
  CHECK(rd.ok());
  ThymeBinDataEntry entry;
  uint32_t alpha = 0;
  uint32_t bravo = 0;
  while (rd.ReadNext(entry) == ThymeBinReader::ReadResult::kOk) {
    if (entry.type != ThymeBinEntryType::kData) continue;
    if (entry.struct_id == 0) ++alpha;
    if (entry.struct_id == 1) ++bravo;
  }
  CHECK(alpha == 2);
  CHECK(bravo == 0);

  rec.Shutdown();
  mirror.Shutdown(true);
  char lease[64];
  CHECK(SessionLeaseNameForRoot(dir, lease, sizeof(lease)));
  ShmRegion::UnlinkName(lease);
  RmTree(dir);
}

void PublishMarked(PinnedSlotWriter& w, uint64_t mono, bool idr) {
  PinnedSlotWriter::BeginView bv = w.Begin(32);
  CHECK(bv.payload != nullptr);
  bv.header->flags.store(idr ? triggered::kH264FlagIsIdr : 0,
                          std::memory_order_relaxed);
  w.Commit(mono);
}

struct IdxRow {
  uint64_t seq = 0;
  uint32_t flags = 0;
};

std::vector<IdxRow> IndexRows(const std::string& path) {
  ThymeBinReader rd(path.c_str());
  CHECK(rd.ok());
  std::vector<IdxRow> out;
  ThymeBinDataEntry e;
  while (rd.ReadNext(e) == ThymeBinReader::ReadResult::kOk) {
    if (e.type != ThymeBinEntryType::kData) continue;
    CHECK(e.payload.size() == sizeof(triggered::FrameIndexRecord));
    triggered::FrameIndexRecord ir{};
    std::memcpy(&ir, e.payload.data(), sizeof(ir));
    out.push_back(IdxRow{e.source_sequence, ir.flags});
  }
  return out;
}

// t0=1550, before=200. Frames at 1000(IDR),1100,1200,1300(IDR),1400,1500,1600.
// gop 300 starts at the IDR at 1300. gop 0 starts at 1400. A later non-IDR
// at 1700 is kept only when gop covers it, and the following IDR is not.
std::vector<IdxRow> CaptureGop(const char* shm, const char* dir,
                               uint64_t gop_ns, bool post_tail) {
  PinnedSlotWriter w;
  CHECK(w.Create(shm, 256, 32, 0));
  triggered::Config cfg;
  cfg.out_dir = dir;
  cfg.reattach_scan_ns = 1000000;
  cfg.post_grace_ns = 1000000ULL;
  triggered::SourceConfig sc;
  sc.kind = triggered::SourceConfig::Kind::kPinnedSlot;
  sc.h264_stream = true;
  sc.shm_name = shm;
  sc.channel_name = "front";
  sc.gop_ns = gop_ns;
  cfg.sources.push_back(sc);
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(cfg));
  const uint64_t mono[] = {1000, 1100, 1200, 1300, 1400, 1500, 1600};
  const bool idr[] = {true, false, false, true, false, false, false};
  for (int i = 0; i < 7; ++i) PublishMarked(w, mono[i], idr[i]);
  CHECK(rec.Trigger(1, 200, 0, 1550, false, false));
  if (post_tail) {
    PublishMarked(w, 1700, false);
    PublishMarked(w, 1800, true);
    PublishMarked(w, 1900, false);
  }
  RunCapture(rec);
  const std::vector<IdxRow> rows =
      IndexRows(rec.last_package_dir() + "/frames/front.index.tbin");
  rec.Shutdown();
  w.Close();
  char lease[64];
  CHECK(SessionLeaseNameForRoot(dir, lease, sizeof(lease)));
  ShmRegion::UnlinkName(lease);
  ShmRegion::UnlinkName(shm);
  RmTree(dir);
  return rows;
}

void ExpectSeq(const std::vector<IdxRow>& rows, const uint64_t* seq,
               std::size_t n) {
  CHECK(rows.size() == n);
  for (std::size_t i = 0; i < n; ++i) CHECK(rows[i].seq == seq[i]);
}

void TestGopWindow() {
  const int me = static_cast<int>(getpid());
  char shm[96];
  char dir[96];
  std::snprintf(shm, sizeof(shm), "/c9gop_%d", me);
  std::snprintf(dir, sizeof(dir), "/tmp/c9gop_%d", me);
  ShmRegion::UnlinkName(shm);
  RmTree(dir);

  {
    const std::vector<IdxRow> rows = CaptureGop(shm, dir, 300, false);
    const uint64_t seq[] = {3, 4, 5};
    ExpectSeq(rows, seq, 3);
    CHECK((rows[0].flags & triggered::kH264FlagIsIdr) != 0);
  }
  {
    const std::vector<IdxRow> rows = CaptureGop(shm, dir, 0, false);
    const uint64_t seq[] = {4, 5};
    ExpectSeq(rows, seq, 2);
  }
  {
    const std::vector<IdxRow> rows = CaptureGop(shm, dir, 300, true);
    const uint64_t seq[] = {3, 4, 5, 7};
    ExpectSeq(rows, seq, 4);
    CHECK((rows.back().flags & triggered::kH264FlagIsIdr) == 0);
  }
  {
    const std::vector<IdxRow> rows = CaptureGop(shm, dir, 0, true);
    const uint64_t seq[] = {4, 5};
    ExpectSeq(rows, seq, 2);
  }
}

}  // namespace

int main() {
  const int me = static_cast<int>(getpid());
  char names[3][96];
  char out_dir[96];
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/c9chan_%d", me);
  for (int i = 0; i < 3; ++i) {
    std::snprintf(names[i], sizeof(names[i]), "/c9chan_%d_%d", me, i);
  }
  RmTree(out_dir);
  for (const auto& n : names) ShmRegion::UnlinkName(n);

  const char* channel_names[3] = {"cam_a", "cam_b", "cam_c"};
  PinnedSlotWriter writers[3];
  for (int i = 0; i < 3; ++i) {
    CHECK(writers[i].Create(names[i], 1024, 64, 0));
  }

  triggered::Config cfg;
  cfg.out_dir = out_dir;
  cfg.reattach_scan_ns = 1000000;
  cfg.post_grace_ns = 20000000ULL;
  for (int i = 0; i < 3; ++i) {
    triggered::SourceConfig sc;
    sc.kind = triggered::SourceConfig::Kind::kPinnedSlot;
    sc.shm_name = names[i];
    sc.channel_name = channel_names[i];
    cfg.sources.push_back(sc);
  }
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(cfg));

  // ---- 1. filter for 2 of 3 channels ----
  for (int i = 0; i < 3; ++i) Publish(writers[i], 0, 4);  // 4 frames each
  CHECK(rec.Trigger(1, 10000000000ULL, 0, 0, false, false,
                    {"cam_a", "cam_c"}));
  RunCapture(rec);
  CHECK(rec.stats().captures_done == 1);
  {
    const std::string pkg = rec.last_package_dir();
    CHECK(triggered::TriggeredRecorder::PackageComplete(pkg));
    CHECK(Exists(pkg + "/frames/cam_a.raw"));
    CHECK(Exists(pkg + "/frames/cam_a.index.tbin"));
    CHECK(!Exists(pkg + "/frames/cam_b.raw"));
    CHECK(!Exists(pkg + "/frames/cam_b.index.tbin"));
    CHECK(Exists(pkg + "/frames/cam_c.raw"));
    CHECK(Exists(pkg + "/frames/cam_c.index.tbin"));
    CHECK(CountData(pkg + "/frames/cam_a.index.tbin") == 4);
    CHECK(CountData(pkg + "/frames/cam_c.index.tbin") == 4);
    const triggered::CaptureStatsRecord csr = Stats(pkg);
    CHECK(csr.frames_written == 8);
    CHECK(csr.samples_written == 0);
    CHECK(csr.descriptors == csr.frames_written);
  }

  // ---- 2. empty channel list = all channels ----
  for (int i = 0; i < 3; ++i) Publish(writers[i], 4, 4);  // 8 frames each
  CHECK(rec.Trigger(2, 10000000000ULL, 0, 0, false, false));
  RunCapture(rec);
  CHECK(rec.stats().captures_done == 2);
  {
    const std::string pkg = rec.last_package_dir();
    CHECK(triggered::TriggeredRecorder::PackageComplete(pkg));
    for (int i = 0; i < 3; ++i) {
      CHECK(Exists(pkg + "/frames/" + channel_names[i] + ".raw"));
      CHECK(CountData(pkg + "/frames/" + channel_names[i] +
                      ".index.tbin") == 8);
    }
    CHECK(Stats(pkg).frames_written == 24);
  }

  // ---- 3. single-channel filter by name ----
  for (int i = 0; i < 3; ++i) Publish(writers[i], 8, 4);  // 12 frames each
  CHECK(rec.Trigger(3, 10000000000ULL, 0, 0, false, false, {"cam_b"}));
  RunCapture(rec);
  CHECK(rec.stats().captures_done == 3);
  {
    const std::string pkg = rec.last_package_dir();
    CHECK(triggered::TriggeredRecorder::PackageComplete(pkg));
    CHECK(!Exists(pkg + "/frames/cam_a.raw"));
    CHECK(!Exists(pkg + "/frames/cam_c.raw"));
    CHECK(Exists(pkg + "/frames/cam_b.index.tbin"));
    CHECK(CountData(pkg + "/frames/cam_b.index.tbin") == 12);
    CHECK(Stats(pkg).frames_written == 12);
  }

  // ---- 4. shared glob/exclusion/decimate grammar ----
  CHECK(rec.Trigger(4, 10000000000ULL, 0, 0, false, false,
                    {"pinned_slot:cam_*@dec=3", "!pinned_slot:cam_b"}));
  RunCapture(rec);
  CHECK(rec.stats().captures_done == 4);
  {
    const std::string pkg = rec.last_package_dir();
    CHECK(!Exists(pkg + "/frames/cam_b.raw"));
    CHECK(CountData(pkg + "/frames/cam_a.index.tbin") == 4);
    CHECK(CountData(pkg + "/frames/cam_c.index.tbin") == 4);
    CHECK(Stats(pkg).frames_written == 8);
  }

  // ---- 5. legacy bare shm selector also decimates the real capture ----
  CHECK(rec.Trigger(5, 10000000000ULL, 0, 0, false, false,
                    {std::string(names[1]) + "@dec=4"}));
  RunCapture(rec);
  CHECK(rec.stats().captures_done == 5);
  CHECK(CountData(rec.last_package_dir() + "/frames/cam_b.index.tbin") == 3);
  CHECK(!Exists(rec.last_package_dir() + "/frames/cam_a.raw"));
  CHECK(!Exists(rec.last_package_dir() + "/frames/cam_c.raw"));

  // Exclusion priority can produce no work; reject instead of sealing empty.
  CHECK(!rec.Trigger(6, 0, 0, 0, false, false, {"!pinned_slot:*"}));
  CHECK(rec.phase() == triggered::Phase::kArmed);

  rec.Shutdown();
  TestAggregateStructFilter();
  TestGopWindow();
  for (int i = 0; i < 3; ++i) {
    writers[i].Close();
    ShmRegion::UnlinkName(names[i]);
  }
  RmTree(out_dir);
  std::printf("test_trigger_channel_filter: all checks passed\n");
  return 0;
}
