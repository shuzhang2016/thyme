// test_continuous_pinned_slot.cpp — production continuous mode for big frames
// (productionizing design 6.6): streaming drain (no descriptor quota), lag drops,
// GOP-aligned slicing for h264, per-channel decimation, exclusion rules, and
// the startup rate-plan validation (rate-limit floor enforce/warn).
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/trigger_config.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
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
    if (e->d_type == DT_DIR) RmTree(sub);
    else std::remove(sub.c_str());
  }
  closedir(d);
  rmdir(path.c_str());
}

std::vector<std::string> SliceDirs(const std::string& out) {
  std::vector<std::string> r;
  DIR* d = opendir(out.c_str());
  if (d == nullptr) return r;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    if (e->d_type == DT_DIR &&
        std::strncmp(e->d_name, "continuous_", 11) == 0) {
      r.push_back(out + "/" + e->d_name);
    }
  }
  closedir(d);
  std::sort(r.begin(), r.end(), [](const std::string& x, const std::string& y) {
    auto seq_of = [](const std::string& s) {
      return std::strtoull(s.c_str() + s.find("continuous_") + 11, nullptr, 10);
    };
    return seq_of(x) < seq_of(y);
  });
  return r;
}

std::vector<ThymeBinDataEntry> ReadAll(const std::string& path) {
  ThymeBinReader rd(path.c_str());
  CHECK(rd.ok());
  std::vector<ThymeBinDataEntry> out;
  ThymeBinDataEntry e;
  while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) out.push_back(e);
  return out;
}

// Producer: mirror page (sid0 = Pod16) + h264 channel (is_idr every 10 frames) + point cloud.
struct Producer {
  record_mirror::RingClient mirror;
  PinnedSlotWriter h264;
  PinnedSlotWriter cloud;
  uint64_t i = 0;

  void Start(const char* page, const char* h264_name, const char* cloud_name) {
    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = 16;
    std::snprintf(desc.name, sizeof(desc.name), "Pod16");
    RecordMirrorRingSpec spec{16, 4096};
    CHECK(mirror.Create(page, &desc, &spec, 1, 3));
    CHECK(h264.Create(h264_name, 2048, 64, 0));
    CHECK(cloud.Create(cloud_name, 2048, 64, 0));
  }
  void Step() {
    const uint64_t v = i;
    CHECK(mirror.Page()->TryWriteRing(0, &v, sizeof(v), NowNs(),
                                      static_cast<uint32_t>(i)));
    PinnedSlotWriter::BeginView bv = h264.Begin(100);
    CHECK(bv.payload != nullptr);
    std::memset(bv.payload, static_cast<int>(i & 0xFF), 100);
    bv.header->flags.store(i % 10 == 0 ? triggered::kH264FlagIsIdr : 0,
                           std::memory_order_relaxed);
    h264.Commit();
    bv = cloud.Begin(100);
    CHECK(bv.payload != nullptr);
    std::memset(bv.payload, static_cast<int>((i * 3) & 0xFF), 100);
    bv.header->flags.store(0, std::memory_order_relaxed);
    cloud.Commit();
    ++i;
  }
  void Stop() {
    h264.Close();
    cloud.Close();
    mirror.Shutdown(true);
  }
};

struct Fixture {
  triggered::TriggeredRecorder rec;
  Producer prod;

  void Init(const char* page, const char* h264_name, const char* cloud_name,
            const char* out, bool h264_typed) {
    prod.Start(page, h264_name, cloud_name);
    triggered::Config ecfg;
    triggered::SourceConfig h;
    h.kind = triggered::SourceConfig::Kind::kPinnedSlot;
    h.shm_name = h264_name;
    h.channel_name = "h264_front";
    h.h264_stream = h264_typed;
    h.bitrate_kbps = 4000;
    triggered::SourceConfig c;
    c.kind = triggered::SourceConfig::Kind::kPinnedSlot;
    c.shm_name = cloud_name;
    c.channel_name = "cloud_1";
    c.rate_hz = 10;
    c.avg_frame_bytes = 100;
    ecfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page, ""}, h,
                    c};
    ecfg.out_dir = out;
    CHECK(rec.Init(ecfg));
  }
};

void TickFor(triggered::TriggeredRecorder& rec, uint64_t ns) {
  const uint64_t end = NowNs() + ns;
  while (NowNs() < end) {
    rec.Tick();
    SleepNs(200000);
  }
}

}  // namespace

int main() {
  alarm(120);
  const int me = static_cast<int>(getpid());
  char page[96], h264c[96], cloud[96], out[96];
  std::snprintf(page, sizeof(page), "/c9cbf_page_%d", me);
  std::snprintf(h264c, sizeof(h264c), "/c9cbf_h264_%d", me);
  std::snprintf(cloud, sizeof(cloud), "/c9cbf_cloud_%d", me);
  std::snprintf(out, sizeof(out), "/tmp/c9cbf_%d", me);
  ShmRegion::UnlinkName(page);
  ShmRegion::UnlinkName(h264c);
  ShmRegion::UnlinkName(cloud);
  RmTree(out);

  // ---- 1. streaming drain timeliness: pending_bytes does not grow with big frames ----
  {
    Fixture fx;
    fx.Init(page, h264c, cloud, out, true);
    triggered::ContinuousConfig ccfg;
    ccfg.channels = {"small_struct:*", "pinned_slot:*"};
    ccfg.slice_bytes = 1000000;  // no rolling
    CHECK(fx.rec.ArmContinuous(ccfg));
    for (int k = 0; k < 100; ++k) {
      fx.prod.Step();
      fx.rec.Tick();
      SleepNs(200000);
    }
    // Big frames stream: the descriptor quota holds only a transient
    // small-struct backlog, essentially 0.
    CHECK(fx.rec.pending_bytes() <= 16 * 4);
    CHECK(fx.rec.continuous_stats().frames_written == 200);  // 100 each for h264+cloud
    CHECK(fx.rec.continuous_stats().samples_written == 100);
    CHECK(fx.rec.continuous_stats().pinned_slot_drops == 0);
    fx.rec.DisarmContinuous();
    fx.prod.Stop();
  }

  // ---- 2. overwrites counted as drops while the wait_idle gate is held ----
  RmTree(out);
  {
    Fixture fx;
    fx.Init(page, h264c, cloud, out, true);
    triggered::ContinuousConfig ccfg;
    ccfg.channels = {"pinned_slot:*"};  // big frames only
    ccfg.wait_idle = true;
    CHECK(fx.rec.ArmContinuous(ccfg));
    fx.rec.SetContinuousGate(false);  // busy: no read, no drain
    for (int k = 0; k < 100; ++k) fx.prod.Step();  // 64-slot ring wraps and overwrites
    fx.rec.SetContinuousGate(true);   // idle: resume; gap-skip counts drops
    for (int k = 0; k < 10; ++k) {
      fx.prod.Step();
      fx.rec.Tick();
    }
    TickFor(fx.rec, 10000000ULL);
    CHECK(fx.rec.continuous_stats().pinned_slot_drops > 0);
    fx.rec.DisarmContinuous();
    fx.prod.Stop();
  }

  // ---- 3. GOP-aligned slices (h264): each slice starts with is_idr,
  // sequences contiguous, no loss ----
  RmTree(out);
  {
    Fixture fx;
    fx.Init(page, h264c, cloud, out, true);
    triggered::ContinuousConfig ccfg;
    ccfg.channels = {"pinned_slot:h264_front"};  // h264 only
    ccfg.slice_bytes = 1500;                  // rolls after ~10 frames
    CHECK(fx.rec.ArmContinuous(ccfg));
    for (int k = 0; k < 55; ++k) {
      fx.prod.Step();
      fx.rec.Tick();
      SleepNs(300000);
    }
    TickFor(fx.rec, 10000000ULL);
    fx.rec.DisarmContinuous();
    const auto slices = SliceDirs(out);
    CHECK(slices.size() >= 3);  // 55 frames, ~10 per slice, GOP-alignment delay
    uint64_t expect_seq = UINT64_MAX;
    for (std::size_t si = 0; si < slices.size(); ++si) {
      CHECK(triggered::TriggeredRecorder::PackageComplete(slices[si]));
      const auto idx = ReadAll(slices[si] + "/frames/h264_front.index.tbin");
      CHECK(!idx.empty());
      for (const auto& e : idx) {
        CHECK(e.type == ThymeBinEntryType::kData);
        if (expect_seq != UINT64_MAX) {
          CHECK(e.source_sequence == expect_seq);  // contiguous across slices, no loss/duplicate
        }
        expect_seq = e.source_sequence + 1;
      }
      if (si > 0) {
        // A roll-cut slice (not the first) must start with an IDR frame.
        triggered::FrameIndexRecord ir{};
        std::memcpy(&ir, idx.front().payload.data(), sizeof(ir));
        CHECK((ir.flags & triggered::kH264FlagIsIdr) != 0);
      }
    }
    // The cut point is recorded in meta (detail = mono of the IDR frame).
    const auto meta = ReadAll(slices[1] + "/meta.tbin");
    bool saw_detail = false;
    for (const auto& e : meta) {
      if (e.struct_id == triggered::kMetaStructId && e.source_sequence == 2) {
        // cut-point detail of the previous slice
      }
      if (e.struct_id == triggered::kMetaStructId && e.source_sequence == 1) {
        saw_detail = true;
      }
    }
    CHECK(saw_detail);
    CHECK(fx.rec.continuous_stats().roll_forced == 0);  // an IDR arrives every 10 frames
    fx.prod.Stop();
  }

  // ---- 4. point-cloud decimation dec=3 ----
  RmTree(out);
  {
    Fixture fx;
    fx.Init(page, h264c, cloud, out, false);
    triggered::ContinuousConfig ccfg;
    ccfg.channels = {"pinned_slot:cloud_*@dec=3"};
    ccfg.slice_bytes = 1000000;
    CHECK(fx.rec.ArmContinuous(ccfg));
    for (int k = 0; k < 30; ++k) {
      fx.prod.Step();
      fx.rec.Tick();
      SleepNs(200000);
    }
    TickFor(fx.rec, 10000000ULL);
    fx.rec.DisarmContinuous();
    // Frames after the SeekToHead start taken at arm time: only frame_index % 3 == 0 is recorded.
    const auto slices = SliceDirs(out);
    uint64_t n = 0;
    for (const auto& s : slices) {
      for (const auto& e : ReadAll(s + "/frames/cloud_1.index.tbin")) {
        CHECK(e.source_sequence % 3 == 0);
        ++n;
      }
    }
    CHECK(n >= 8 && n <= 11);  // 30 frames / 3 (start aligned to the SeekToHead position)
    fx.prod.Stop();
  }

  // ---- 5. h264 with dec is rejected at config time (with line number) ----
  {
    const std::string text =
        "[channel./h264_front]\nkind = h264_stream\nname = h264_front\n"
        "[continuous]\nenable = true\nchannels = pinned_slot:h264_front@dec=2\n";
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(!trigger_cfg::ParseCenterConfig(text, &cfg, &err));
    CHECK(err.find("line 6") != std::string::npos);  // the channels line
    CHECK(err.find("GOP") != std::string::npos);
  }

  // ---- 6. exclusion list: small_struct:* excludes one source ----
  RmTree(out);
  {
    // Second mirror page (to be excluded).
    char page2[96];
    std::snprintf(page2, sizeof(page2), "/c9cbf_page2_%d", me);
    ShmRegion::UnlinkName(page2);
    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = 16;
    std::snprintf(desc.name, sizeof(desc.name), "Pod16");
    RecordMirrorRingSpec spec{16, 128};
    record_mirror::RingClient mirror2;
    CHECK(mirror2.Create(page2, &desc, &spec, 1, 4));

    Fixture fx;
    fx.Init(page, h264c, cloud, out, false);
    triggered::Config ecfg2;
    ecfg2.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page, ""},
                     {triggered::SourceConfig::Kind::kMirrorRing, page2, ""}};
    ecfg2.out_dir = out;
    fx.rec.Shutdown();
    CHECK(fx.rec.Init(ecfg2));

    triggered::ContinuousConfig ccfg;
    ccfg.channels = {"small_struct:*", "!small_struct:" + std::string(page2)};
    ccfg.slice_bytes = 1000000;
    CHECK(fx.rec.ArmContinuous(ccfg));
    for (int k = 0; k < 20; ++k) {
      fx.prod.Step();  // page1
      const uint64_t v = 0xDEAD0000 + k;
      CHECK(mirror2.Page()->TryWriteRing(0, &v, sizeof(v), NowNs(),
                                         static_cast<uint32_t>(k)));
      fx.rec.Tick();
      SleepNs(200000);
    }
    TickFor(fx.rec, 10000000ULL);
    fx.rec.DisarmContinuous();
    const auto slices = SliceDirs(out);
    CHECK(!slices.empty());
    uint64_t n = 0;
    for (const auto& s : slices) {
      for (const auto& e : ReadAll(s + "/small.tbin")) {
        CHECK(e.type == ThymeBinEntryType::kData);
        uint64_t v = 0;
        std::memcpy(&v, e.payload.data(), sizeof(v));
        CHECK(v < 0xDEAD0000);  // samples from the excluded source never appear
        ++n;
      }
    }
    CHECK(n == 20);
    mirror2.Shutdown(true);
    fx.prod.Stop();
  }

  // ---- 7. startup bitrate accounting: enforce rejects / warn allows ----
  {
    const std::string text =
        "[recorder]\noutput_dir = /tmp/x\nio_rate_limit_mbps = 1\n"
        "disk_quota_mb = 1000\n"
        "[channel./h264_front]\nkind = h264_stream\nname = h264_front\n"
        "bitrate_kbps = 4000\n"
        "[continuous]\nenable = true\nchannels = pinned_slot:*\n";
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(trigger_cfg::ParseCenterConfig(text, &cfg, &err));
    trigger_cfg::ContinuousPlanReport rep;
    std::string warn;
    // 4 Mbps = 500000 B/s; rate limit 1 MB/s = 1000000 B/s > 500000: not over the limit.
    CHECK(trigger_cfg::ValidateContinuousPlan(cfg, &rep, &warn, &err));
    CHECK(rep.sustained_bps == 500000);
    CHECK(rep.disk_bytes_per_hour == 500000ull * 3600);  // ~1.68 GB/h
    // 1000 MB / 1.8e9 B/h ~= 0.58 h
    CHECK(rep.quota_coverage_hours > 0.5 && rep.quota_coverage_hours < 0.65);
    CHECK(!rep.rate_limit_too_low);
    // Rate limit 100 KB/s < 500 KB/s: warn allows, enforce rejects.
    cfg.recorder_io_rate_limit_bps = 100000;
    cfg.cont_rate_check = "warn";
    CHECK(trigger_cfg::ValidateContinuousPlan(cfg, &rep, &warn, &err));
    CHECK(rep.rate_limit_too_low);
    CHECK(!warn.empty());
    cfg.cont_rate_check = "enforce";
    CHECK(!trigger_cfg::ValidateContinuousPlan(cfg, &rep, &warn, &err));
    CHECK(!err.empty());
  }

  RmTree(out);
  ShmRegion::UnlinkName(h264c);
  ShmRegion::UnlinkName(cloud);
  return 0;
}
