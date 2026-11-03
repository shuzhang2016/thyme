// test_replay_seek_index.cpp — REP-06 seek/index/manifest metadata.
//
// Builds two variants of one synthetic package (replay_synthetic_package.h):
//
//   A: every .tbin carries the REC-04 footer index;
//   B: the same package with the footers truncated (unindexed/legacy shape,
//      which the reader seeks through the block table built at Open).
//
// Asserts:
//   A: per-channel first/last mono_ns, record/data/gap counts, byte offsets,
//      geometry (min/max payload, .raw extent) and CRC status are exposed by
//      ReplayChannel and by `list --json` / `info --json`.
//   B: SeekMonoNs(t) matches the full-scan suffix exactly for both variants
//      (identical event sets and payload bytes), and the records probed per
//      seek stay within one block per source instead of the record count —
//      the O(log n) index path and the scan-built fallback path.
//   C: a target before the first record and past the last record behave as
//      documented (start of stream / empty suffix).

#include "replay_synthetic_package.h"

#include "replay_cli.h"

#include "thyme/check.h"
#include "thyme/replay/replay_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

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

uint64_t FileSize(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
}

struct Ev {
  uint64_t mono_ns = 0;
  uint32_t channel = 0;
  uint32_t kind = 0;
  uint32_t struct_id = 0;
  uint32_t len = 0;
  uint64_t raw_offset = 0;
  uint32_t payload_crc = 0;
  std::vector<uint8_t> bytes;
};

void CollectAfterSeek(const std::string& dir, bool has_target, uint64_t target,
                      std::vector<Ev>* out, uint64_t* scanned,
                      uint64_t* seek_calls) {
  replay::ReplayPackage pkg;
  CHECK(pkg.Open(dir) == replay::ReplayStatus::kOk);
  if (has_target) {
    CHECK(pkg.SeekMonoNs(target) == replay::ReplayStatus::kOk);
  }
  if (scanned != nullptr) {
    *scanned = pkg.seek_records_scanned();
  }
  if (seek_calls != nullptr) {
    *seek_calls = pkg.seek_calls();
  }
  replay::ReplayEvent ev;
  while (pkg.NextEvent(&ev) == replay::ReplayStatus::kOk) {
    Ev e;
    e.mono_ns = ev.mono_ns;
    e.channel = ev.channel;
    e.kind = static_cast<uint32_t>(ev.kind);
    e.struct_id = ev.struct_id;
    e.len = ev.len;
    e.raw_offset = ev.raw_offset;
    e.payload_crc = ev.payload_crc;
    if (ev.payload != nullptr && ev.len != 0) {
      e.bytes.assign(ev.payload, ev.payload + ev.len);
    }
    out->push_back(std::move(e));
  }
  CHECK(pkg.status() == replay::ReplayStatus::kEnd);
}

void CheckSame(const std::vector<Ev>& a, const std::vector<Ev>& b) {
  CHECK(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].mono_ns == b[i].mono_ns);
    CHECK(a[i].channel == b[i].channel);
    CHECK(a[i].kind == b[i].kind);
    CHECK(a[i].struct_id == b[i].struct_id);
    CHECK(a[i].len == b[i].len);
    CHECK(a[i].raw_offset == b[i].raw_offset);
    CHECK(a[i].payload_crc == b[i].payload_crc);
    CHECK(a[i].bytes == b[i].bytes);
  }
}

const replay::ReplayChannel* ChannelByName(const replay::ReplayPackage& pkg,
                                           const std::string& name) {
  for (const replay::ReplayChannel& c : pkg.channels()) {
    if (c.name == name) {
      return &c;
    }
  }
  return nullptr;
}

}  // namespace

int main() {
  const uint32_t kSmallRows = 300;
  const uint32_t kFrameRows = 200;
  replay_synth::Spec spec;
  spec.small_rows = kSmallRows;
  spec.frame_rows = kFrameRows;

  char root_buf[160];
  std::snprintf(root_buf, sizeof(root_buf), "/tmp/thyme_rep06_%ld",
                static_cast<long>(getpid()));
  const std::string root = root_buf;
  RmTree(root);
  CHECK(mkdir(root.c_str(), 0700) == 0);
  const std::string indexed = root + "/indexed";
  const std::string unindexed = root + "/unindexed";
  std::string err;
  CHECK(replay_synth::WritePackage(indexed, spec, &err));
  CHECK(replay_synth::WritePackage(unindexed, spec, &err));
  CHECK(replay_synth::MakeUnindexed(unindexed, &err));

  const uint64_t expected_total = 5 + kSmallRows + kFrameRows + 1;

  // ---- A: per-channel metadata (core + CLI list/info) ----
  {
    replay::ReplayPackage pkg;
    CHECK(pkg.Open(indexed) == replay::ReplayStatus::kOk);
    CHECK(pkg.channels().size() == 3);
    const replay::ReplayChannel* meta = ChannelByName(pkg, "meta");
    const replay::ReplayChannel* small = ChannelByName(pkg, "small");
    const replay::ReplayChannel* cam = ChannelByName(pkg, "cam_a");
    CHECK(meta != nullptr && small != nullptr && cam != nullptr);
    CHECK(meta->record_count == 5 && meta->data_count == 4);
    CHECK(meta->first_mono_ns == replay_synth::MetaMono(0));
    CHECK(meta->last_mono_ns == replay_synth::MetaMono(3));
    CHECK(small->record_count == kSmallRows && small->data_count == kSmallRows);
    CHECK(small->gap_count == 0);
    CHECK(small->first_mono_ns == replay_synth::SmallMono(0));
    CHECK(small->last_mono_ns == replay_synth::SmallMono(kSmallRows - 1));
    CHECK(small->min_data_len == sizeof(replay_synth::Pod16));
    CHECK(small->max_data_len == sizeof(replay_synth::Pod16));
    CHECK(cam->record_count == kFrameRows + 1);
    CHECK(cam->data_count == kFrameRows && cam->gap_count == 1);
    CHECK(cam->first_mono_ns == replay_synth::FrameMono(0));
    CHECK(cam->last_mono_ns == replay_synth::FrameMono(kFrameRows - 1));
    CHECK(cam->min_data_len == replay_synth::FrameLen(0));
    CHECK(cam->max_data_len == replay_synth::FrameLen(4));
    CHECK(cam->first_raw_offset == 0);
    CHECK(cam->last_raw_end == FileSize(indexed + "/frames/cam_a.raw"));
    CHECK(cam->frame_crc_declared + cam->frame_crc_absent == kFrameRows);
    for (const replay::ReplayChannel& c : pkg.channels()) {
      CHECK(c.has_footer_index);
      CHECK(c.index_crc_checked == 1);
      CHECK(c.record_crc_checked == c.record_count);
      CHECK(c.data_offset > 0);
      CHECK(c.first_record_offset == c.data_offset);
      CHECK(c.last_record_offset >= c.first_record_offset);
      CHECK(c.last_record_offset < c.records_end_offset);
      CHECK(c.records_end_offset < FileSize(c.tbin_path));
      CHECK(c.seek_entries ==
            (c.record_count + kThymeBinIndexBlockSize - 1) /
                kThymeBinIndexBlockSize);
      CHECK(c.seek_stride == kThymeBinIndexBlockSize);
    }
  }
  {
    replaycli::PackageReport rep;
    std::ostringstream json;
    CHECK(replaycli::RunList(indexed, true, json, &rep) == replaycli::kExitOk);
    const replaycli::ChannelReport* small = rep.Find("small");
    const replaycli::ChannelReport* cam = rep.Find("cam_a");
    CHECK(small != nullptr && cam != nullptr);
    CHECK(small->indexed && small->index_crc_checked == 1);
    CHECK(small->record_crc_checked == small->records);
    CHECK(small->first_record_offset == small->data_offset);
    CHECK(small->seek_entries > 1 && small->seek_stride == 64);
    CHECK(small->min_payload_bytes == sizeof(replay_synth::Pod16));
    CHECK(cam->last_raw_end == FileSize(indexed + "/frames/cam_a.raw"));
    CHECK(cam->first_raw_offset == 0);
    CHECK(cam->frame_crc_declared + cam->frame_crc_absent == cam->data);
    const std::string js = json.str();
    CHECK(js.find("\"seek\"") != std::string::npos);
    CHECK(js.find("\"index\":\"footer\"") != std::string::npos);
    CHECK(js.find("\"record_checked\":") != std::string::npos);
    CHECK(js.find("\"frames_declared\":") != std::string::npos);
    CHECK(js.find("\"offsets\"") != std::string::npos);
    CHECK(js.find("\"geometry\"") != std::string::npos);
    CHECK(js.find("\"complexity\":\"O(log n)\"") != std::string::npos);

    replaycli::PackageReport irep;
    std::ostringstream ijson;
    CHECK(replaycli::RunInfo(indexed, true, ijson, &irep) ==
          replaycli::kExitOk);
    CHECK(ijson.str().find("\"index\":\"footer\"") != std::string::npos);
    CHECK(ijson.str().find("\"records_bytes\":") != std::string::npos);
  }

  // ---- B1: unindexed metadata (scan-built block table) ----
  {
    replaycli::PackageReport rep;
    std::ostringstream json;
    CHECK(replaycli::RunList(unindexed, true, json, &rep) ==
          replaycli::kExitOk);
    for (const replaycli::ChannelReport& c : rep.channels) {
      CHECK(!c.indexed);
      CHECK(c.index_crc_checked == 0);
      CHECK(c.seek_stride == 256);
      CHECK(c.seek_entries == (c.records + 255) / 256);
      CHECK(c.tbin_bytes == c.records_end_offset);
      CHECK(c.record_crc_checked == c.records);
    }
    CHECK(json.str().find("\"index\":\"scan\"") != std::string::npos);
  }

  // ---- B2: full-scan equivalence ----
  std::vector<Ev> baseline;
  std::vector<Ev> flat_baseline;
  CollectAfterSeek(indexed, false, 0, &baseline, nullptr, nullptr);
  CollectAfterSeek(unindexed, false, 0, &flat_baseline, nullptr, nullptr);
  CHECK(baseline.size() == expected_total);
  CheckSame(baseline, flat_baseline);
  for (std::size_t i = 1; i < baseline.size(); ++i) {
    CHECK(baseline[i].mono_ns >= baseline[i - 1].mono_ns);
  }
  const uint64_t first_mono = baseline.front().mono_ns;
  CHECK(first_mono == replay_synth::MetaMono(0));
  const uint64_t last_mono = baseline.back().mono_ns;

  // ---- B3: seek equivalence + bounded probe count (index and fallback) ----
  const uint64_t targets[] = {
      0,
      first_mono,
      replay_synth::SmallMono(kSmallRows / 2),
      replay_synth::FrameMono(kFrameRows / 2),
      replay_synth::FrameMono(kFrameRows / 2) + 1,
      last_mono,
      last_mono + 1,
  };
  for (const uint64_t target : targets) {
    for (const bool use_indexed : {true, false}) {
      const std::string& dir = use_indexed ? indexed : unindexed;
      uint64_t scanned = 0;
      uint64_t calls = 0;
      std::vector<Ev> suffix;
      CollectAfterSeek(dir, true, target, &suffix, &scanned, &calls);
      CHECK(calls == 1);
      // Expected suffix = the baseline from the first event >= target.
      std::vector<Ev> expected;
      for (const Ev& e : baseline) {
        if (e.mono_ns >= target) {
          expected.push_back(e);
        }
      }
      CheckSame(expected, suffix);
      if (target == 0 || target == first_mono) {
        CHECK(suffix.size() == baseline.size());
      }
      if (target > last_mono) {
        CHECK(suffix.empty());
      }
      // One block per source bounds the probe count on both paths.
      const uint64_t stride = use_indexed ? kThymeBinIndexBlockSize : 256;
      CHECK(scanned <= 3 * stride);
      CHECK(scanned < baseline.size());
    }
  }

  // ---- C: a target before the first record starts at the beginning ----
  {
    std::vector<Ev> suffix;
    CollectAfterSeek(indexed, true, first_mono - 500, &suffix, nullptr,
                     nullptr);
    CheckSame(baseline, suffix);
  }

  RmTree(root);
  return 0;
}
