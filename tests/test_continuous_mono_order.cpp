// test_continuous_mono_order.cpp — continuous slices keep
// per-source mono_ns order.
//
// The continuous collector enqueues descriptors struct-by-struct per tick
// (struct-major), but small.tbin is a single file across every struct and the
// replay reader (ReplayPackage) requires each source's rows non-decreasing in
// mono_ns. The engine re-sorts the pending descriptor window at the start of
// every drain pass (SortContinuousQueue) and demotes any descriptor that
// still falls below the slice's written watermark to a GAP at the watermark
// (ContinuousStats::out_of_order) — never an out-of-order row.
//
//   1. struct-major publish, interleaved mono_ns: two structs published in
//      full batches (all of struct 0, then all of struct 1) with alternating
//      timestamps; the slice rolls mid-batch. Every slice's small.tbin must
//      be non-decreasing and hold the exact interleaved grid, and a
//      ReplayPackage walk must see zero mono regressions.
//   2. watermark demotion: a sample published with a mono_ns below the
//      already-written watermark arrives in a later drain pass (the case a
//      per-window sort cannot fix) — it must become a GAP at the watermark,
//      counted in out_of_order, and the file must stay ordered.
//   3. overrun holes are stamped with the struct's last collected mono
//      (SourceState::cont_last_mono), so the GAP row sits at the point of
//      loss in the timeline instead of regressing the file.
//
// No gtest: plain CHECK, like the other recorder tests.
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/replay/replay_reader.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

constexpr uint64_t kMs = 1000000ULL;
constexpr uint64_t kBase = 1000000000ULL;  // synthetic grid origin (1 s)
constexpr uint32_t kDepth = 4096;          // per-struct ring depth

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

std::vector<std::string> ListSlices(const std::string& path) {
  std::vector<std::string> out;
  DIR* d = opendir(path.c_str());
  if (d == nullptr) return out;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    if (e->d_type == DT_DIR &&
        std::strncmp(e->d_name, "continuous_", 11) == 0) {
      out.push_back(path + "/" + e->d_name);
    }
  }
  closedir(d);
  // readdir order is arbitrary; sort by the numeric seq in the name.
  std::sort(out.begin(), out.end(), [](const std::string& x,
                                       const std::string& y) {
    auto seq_of = [](const std::string& s) {
      const std::size_t a = s.find("continuous_") + 11;
      return std::strtoull(s.c_str() + a, nullptr, 10);
    };
    return seq_of(x) < seq_of(y);
  });
  return out;
}

std::vector<ThymeBinDataEntry> ReadAll(const std::string& path) {
  ThymeBinReader rd(path.c_str());
  CHECK(rd.ok());
  std::vector<ThymeBinDataEntry> out;
  ThymeBinDataEntry e;
  while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
    out.push_back(e);
  }
  return out;
}

// Merged-order walk of a package: counts mono_ns regressions exactly like
// ReplayPlayer's stats().mono_regressions (the merged stream can only
// regress when a source file itself is not mono-sorted).
uint64_t MonoRegressions(const std::string& dir, uint64_t* data_events,
                         uint64_t* gap_events) {
  replay::ReplayPackage pkg;
  CHECK(pkg.Open(dir) == replay::ReplayStatus::kOk);
  replay::ReplayEvent ev;
  uint64_t prev = 0;
  uint64_t regressions = 0;
  bool has = false;
  *data_events = 0;
  *gap_events = 0;
  while (pkg.NextEvent(&ev) == replay::ReplayStatus::kOk) {
    if (has && ev.mono_ns < prev) ++regressions;
    prev = ev.mono_ns;
    has = true;
    if (ev.kind == replay::ReplayKind::kData) *data_events += 1;
    if (ev.kind == replay::ReplayKind::kGap) *gap_events += 1;
  }
  return regressions;
}

void WriteS0(RecordMirrorRingPage* page, uint32_t k, uint64_t mono) {
  Pod16 p{k, k * 2, k * 3};
  CHECK(page->TryWriteRing(0, &p, sizeof(p), mono, k));
}

void WriteS1(RecordMirrorRingPage* page, uint32_t k, uint64_t mono) {
  Pod32 p{};
  p.a = 0x100000000ULL + k;
  p.b = 0xFFFFFFFFULL - k;
  CHECK(page->TryWriteRing(1, &p, sizeof(p), mono, k));
}

// Tick until both descriptor buckets are empty (settle_batch bounded drain).
// The first Tick is unconditional: CollectContinuous runs inside Tick, so an
// initially empty queue still needs one pass to pick up published samples.
void DrainAll(triggered::TriggeredRecorder& rec) {
  rec.Tick();
  for (int i = 0; i < 100000 && rec.pending_bytes() != 0; ++i) rec.Tick();
  CHECK(rec.pending_bytes() == 0);
}

}  // namespace

int main() {
  alarm(120);
  const int me = static_cast<int>(getpid());
  char page_name[96], out_dir[96];
  std::snprintf(page_name, sizeof(page_name), "/c9mono_page_%d", me);
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/c9mono_%d", me);
  ShmRegion::UnlinkName(page_name);
  RmTree(out_dir);

  // One mirror page, two small structs at interleaved timestamps.
  record_mirror::RingClient mirror;
  {
    ThymeStructDesc descs[2]{};
    descs[0].struct_id = 0;
    descs[0].contract_version = 1;
    descs[0].payload_size = sizeof(Pod16);
    std::snprintf(descs[0].name, sizeof(descs[0].name), "%s", "Pod16");
    descs[1].struct_id = 1;
    descs[1].contract_version = 1;
    descs[1].payload_size = sizeof(Pod32);
    std::snprintf(descs[1].name, sizeof(descs[1].name), "%s", "Pod32");
    const RecordMirrorRingSpec specs[2] = {{sizeof(Pod16), kDepth},
                                           {sizeof(Pod32), kDepth}};
    CHECK(mirror.Create(page_name, descs, specs, 2, 3));
  }
  RecordMirrorRingPage* page = mirror.Page();

  triggered::Config ecfg;
  ecfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name, ""}};
  ecfg.out_dir = out_dir;
  ecfg.settle_batch = 64;  // small: the drain lags collection on purpose
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(ecfg));

  // ---- 1. struct-major publish, interleaved mono: per-pass sort ----
  {
    triggered::ContinuousConfig ccfg;
    ccfg.slice_bytes = 8000;  // rolls mid-batch: order must survive the cut
    ccfg.slice_ns = 0;
    CHECK(rec.ArmContinuous(ccfg));
    // All of struct 0 then all of struct 1 (struct-major), timestamps interleaved.
    for (uint32_t k = 0; k < 300; ++k) WriteS0(page, k, kBase + 2 * k * kMs);
    for (uint32_t k = 0; k < 300; ++k) {
      WriteS1(page, k, kBase + (2 * k + 1) * kMs);
    }
    DrainAll(rec);
    rec.DisarmContinuous();
    CHECK(rec.continuous_stats().samples_written == 600);
    CHECK(rec.continuous_stats().holes_written == 0);
    CHECK(rec.continuous_stats().out_of_order == 0);
    CHECK(rec.continuous_stats().slices_done >= 2);  // rolled mid-batch

    // Cross-slice concatenation: the merged grid must be exactly (base + j ms, struct j%2).
    const std::vector<std::string> slices = ListSlices(out_dir);
    CHECK(slices.size() == rec.continuous_stats().slices_done);
    uint64_t j = 0;
    for (const std::string& s : slices) {
      CHECK(triggered::TriggeredRecorder::PackageComplete(s));
      uint64_t data = 0;
      uint64_t gaps = 0;
      CHECK(MonoRegressions(s, &data, &gaps) == 0);  // replay merge does not regress
      CHECK(gaps == 0);
      for (const auto& e : ReadAll(s + "/small.tbin")) {
        CHECK(e.type == ThymeBinEntryType::kData);
        CHECK(e.mono_ns == kBase + j * kMs);
        CHECK(e.struct_id == j % 2);
        if (e.struct_id == 0) {
          Pod16 p{};
          std::memcpy(&p, e.payload.data(), sizeof(p));
          CHECK(p.a == j / 2 && p.b == p.a * 2 && p.c == p.a * 3);
        } else {
          Pod32 p{};
          std::memcpy(&p, e.payload.data(), sizeof(p));
          CHECK(p.a == 0x100000000ULL + j / 2);
        }
        ++j;
      }
    }
    CHECK(j == 600);
  }

  // ---- 2. late sample: demoted to a GAP at the watermark ----
  {
    RmTree(out_dir);
    triggered::ContinuousConfig ccfg;
    ccfg.slice_bytes = 1u << 20;  // single slice
    ccfg.slice_ns = 0;
    CHECK(rec.ArmContinuous(ccfg));
    const uint64_t t1 = kBase + 1000 * kMs;
    WriteS1(page, 1000, t1);
    DrainAll(rec);  // watermark advances to t1
    // A later tick brings a sample stamped BELOW the written watermark (the
    // preempted-producer case a per-window sort cannot repair) plus a fresh
    // in-order sample.
    WriteS0(page, 500, t1 - 5 * kMs);  // late: must be demoted
    WriteS1(page, 1001, t1 + 5 * kMs);
    DrainAll(rec);
    rec.DisarmContinuous();
    CHECK(rec.continuous_stats().out_of_order == 1);
    CHECK(rec.continuous_stats().samples_written == 2);
    CHECK(rec.continuous_stats().holes_written == 1);

    const std::vector<std::string> slices = ListSlices(out_dir);
    CHECK(slices.size() == 1);
    CHECK(triggered::TriggeredRecorder::PackageComplete(slices[0]));
    uint64_t data = 0;
    uint64_t gaps = 0;
    CHECK(MonoRegressions(slices[0], &data, &gaps) == 0);
    CHECK(gaps == 1);
    const std::vector<ThymeBinDataEntry> rows =
        ReadAll(slices[0] + "/small.tbin");
    CHECK(rows.size() == 3);
    CHECK(rows[0].type == ThymeBinEntryType::kData);
    CHECK(rows[0].struct_id == 1 && rows[0].mono_ns == t1);
    CHECK(rows[1].type == ThymeBinEntryType::kGap);
    CHECK(rows[1].struct_id == 0 && rows[1].mono_ns == t1);  // pinned to the watermark
    CHECK(rows[2].type == ThymeBinEntryType::kData);
    CHECK(rows[2].struct_id == 1 && rows[2].mono_ns == t1 + 5 * kMs);
  }

  // ---- 3. overrun holes stamped at the point of loss ----
  {
    RmTree(out_dir);
    triggered::ContinuousConfig ccfg;
    ccfg.slice_bytes = 1u << 20;
    ccfg.slice_ns = 0;
    CHECK(rec.ArmContinuous(ccfg));
    const uint64_t b3 = kBase + 10000 * kMs;
    // Batch A: 200 samples; one Tick collects all (cont_last_mono[0] =
    // b3+199ms) but the batch-bound drain writes only the first 64.
    for (uint32_t k = 0; k < 200; ++k) WriteS0(page, k, b3 + k * kMs);
    rec.Tick();
    // Batch B without a tick: depth+8 publishes. The ring keeps only the
    // last `depth` samples, so this evicts (a) the 136 collected-but-not-
    // yet-drained samples (they become readback holes at their own mono)
    // and (b) 8 never-collected samples (mono b3+200..b3+207ms) whose hole
    // descriptors must be stamped b3+199ms (last collected), not 0.
    for (uint32_t i = 0; i < kDepth + 8; ++i) {
      WriteS0(page, 1000 + i, b3 + (200 + i) * kMs);
    }
    DrainAll(rec);
    rec.DisarmContinuous();
    CHECK(rec.continuous_stats().holes_written == 136 + 8);
    CHECK(rec.continuous_stats().samples_written == 64 + kDepth);
    CHECK(rec.continuous_stats().out_of_order == 0);

    const std::vector<std::string> slices = ListSlices(out_dir);
    CHECK(slices.size() == 1);
    uint64_t data = 0;
    uint64_t gaps = 0;
    CHECK(MonoRegressions(slices[0], &data, &gaps) == 0);
    CHECK(gaps == 136 + 8);
    const std::vector<ThymeBinDataEntry> rows =
        ReadAll(slices[0] + "/small.tbin");
    CHECK(rows.size() == 4304);  // 4160 data + 144 gaps
    uint64_t prev = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      CHECK(i == 0 || rows[i].mono_ns >= prev);  // file is monotonic
      prev = rows[i].mono_ns;
    }
    // rows 0..63: batch-A data; rows 64..199: readback holes at their own
    // mono (b3+64..b3+199ms); rows 200..207: the 8 overrun holes.
    for (std::size_t i = 0; i < 64; ++i) {
      CHECK(rows[i].type == ThymeBinEntryType::kData);
      CHECK(rows[i].mono_ns == b3 + i * kMs);
    }
    for (std::size_t i = 64; i < 208; ++i) {
      CHECK(rows[i].type == ThymeBinEntryType::kGap);
      CHECK(rows[i].struct_id == 0);
    }
    for (std::size_t i = 64; i < 200; ++i) {
      CHECK(rows[i].mono_ns == b3 + i * kMs);  // readback hole: its own timestamp
    }
    for (std::size_t i = 200; i < 208; ++i) {
      // The hole is pinned at the loss point (the last mono collected for that
      // struct), not 0 / the file head; it therefore sorts after the b3+199ms
      // hole rather than at the very front of the file.
      CHECK(rows[i].mono_ns == b3 + 199 * kMs);
    }
    CHECK(rows[208].type == ThymeBinEntryType::kData);
    CHECK(rows[208].mono_ns == b3 + 208 * kMs);
  }

  std::printf("test_continuous_mono_order: OK\n");
  rec.Shutdown();
  mirror.Shutdown(true);
  RmTree(out_dir);
  return 0;
}
