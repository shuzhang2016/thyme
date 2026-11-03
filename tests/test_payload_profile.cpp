// test_payload_profile.cpp — REC-27 big-frame payload profile descriptor.
//
// Core stores an opaque (profile, version, params_digest) per big-frame
// channel. It never parses the parameter string. Covered here:
//
//   * config keys profile/profile_version/profile_params (pinned_slot only);
//   * HashProfileParams empty=0, collision-with-0 remaps to 1;
//   * capture writes schema.json payload_profiles, a 128 B .raw header and a
//     meta.tbin PayProf row;
//   * unknown profile --decode-profile refuses decode but keeps the raw
//     export; a registered profile resolves; missing profile stays raw.
#include "replay_export.h"

#include "thyme/check.h"
#include "thyme/record/record_schema.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/trigger_config.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

constexpr uint32_t kSlotBytes = 256;
constexpr uint32_t kSlotCount = 8;
constexpr uint32_t kRawHeaderBytes = 128;
constexpr char kKnownProfile[] = "inno-lidar/pointcloud@v2";

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

bool Parse(const std::string& text, trigger_cfg::CenterConfig* cfg,
           std::string* err) {
  return trigger_cfg::ParseCenterConfig(text, cfg, err);
}

void TestConfigAndHash() {
  CHECK(schema::HashProfileParams("") == 0);
  CHECK(schema::HashProfileParams("time_field=offset:0") != 0);
  CHECK(schema::HashProfileParams("a") != schema::HashProfileParams("b"));

  const char* kBase = R"(
[trigger_center]
allow_empty_sources = true
[recorder]
output_dir = /tmp/thyme_payload_profile
[channel./cloud_1]
kind = pinned_slot
name = cloud_1
profile = inno-lidar/pointcloud@v2
profile_version = 1
profile_params = time_field=offset:0
)";
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(Parse(kBase, &cfg, &err));
  CHECK(cfg.recorder_sources.size() == 1);
  CHECK(cfg.recorder_sources[0].payload_profile == kKnownProfile);
  CHECK(cfg.recorder_sources[0].payload_profile_version == 1);
  CHECK(cfg.recorder_sources[0].payload_profile_params ==
        "time_field=offset:0");
  CHECK(cfg.recorder_sources[0].payload_profile_digest ==
        schema::HashProfileParams("time_field=offset:0"));

  CHECK(!Parse(std::string(kBase) + "\n[channel./ring]\nkind = mirror_ring\n"
                                    "profile = inno-lidar/pointcloud@v2\n",
               &cfg, &err));
  CHECK(err.find("profile is only valid for pinned_slot") != std::string::npos);

  CHECK(!Parse("[trigger_center]\nallow_empty_sources = true\n"
               "[recorder]\noutput_dir = /tmp/x\n"
               "[channel./cloud_1]\nkind = pinned_slot\nname = cloud_1\n"
               "profile_version = 1\n",
               &cfg, &err));
  CHECK(err.find("profile_version/profile_params require profile") !=
        std::string::npos);
}

struct Capture {
  std::string pkg;
};

Capture RunCapture(const char* chan, const char* out, const char* profile,
                   uint32_t version, const char* params) {
  PinnedSlotWriter w;
  CHECK(w.Create(chan, kSlotBytes, kSlotCount, 0));
  for (uint32_t f = 0; f < 3; ++f) {
    PinnedSlotWriter::BeginView bv = w.Begin(kSlotBytes);
    CHECK(bv.payload != nullptr);
    std::memset(bv.payload, static_cast<int>(0x30 + f), kSlotBytes);
    w.Commit(NowNs());
    SleepNs(200000);
  }
  triggered::Config ecfg;
  triggered::SourceConfig sc;
  sc.kind = triggered::SourceConfig::Kind::kPinnedSlot;
  sc.shm_name = chan;
  sc.channel_name = "cloud_1";
  if (profile != nullptr) sc.payload_profile = profile;
  sc.payload_profile_version = version;
  if (params != nullptr) sc.payload_profile_params = params;
  ecfg.sources = {sc};
  ecfg.out_dir = out;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(ecfg));
  CHECK(rec.Trigger(1, 60000000000ULL, 0));
  const uint64_t deadline = NowNs() + 10000000000ULL;
  while (rec.phase() != triggered::Phase::kArmed) {
    rec.Tick();
    CHECK(NowNs() < deadline);
    SleepNs(100000);
  }
  Capture r;
  r.pkg = rec.last_package_dir();
  CHECK(triggered::TriggeredRecorder::PackageComplete(r.pkg));
  rec.Shutdown();
  w.Close();
  ShmRegion::UnlinkName(chan);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(chan).c_str());
  return r;
}

void TestCaptureSidecarAndRawHeader() {
  const int me = static_cast<int>(getpid());
  char chan[96];
  char out[96];
  std::snprintf(chan, sizeof(chan), "/thyme_rec27_%d", me);
  std::snprintf(out, sizeof(out), "/tmp/thyme_rec27_%d", me);
  RmTree(out);
  ShmRegion::UnlinkName(chan);

  const Capture cap =
      RunCapture(chan, out, kKnownProfile, 1, /*params=*/nullptr);
  schema::PackageSchema sc;
  std::string err;
  CHECK(schema::ReadPackageSchema(cap.pkg, &sc, &err) ==
        schema::ReadState::kOk);
  CHECK(sc.version == 3);
  CHECK(sc.payload_profiles.size() == 1);
  CHECK(sc.payload_profiles[0].channel == "cloud_1");
  CHECK(sc.payload_profiles[0].profile == kKnownProfile);
  CHECK(sc.payload_profiles[0].version == 1);
  CHECK(sc.payload_profiles[0].params_digest == 0);

  const std::string raw_path = cap.pkg + "/frames/cloud_1.raw";
  CHECK(FileSize(raw_path) >= kRawHeaderBytes + kSlotBytes);
  FILE* raw = std::fopen(raw_path.c_str(), "rb");
  CHECK(raw != nullptr);
  char magic[8] = {};
  CHECK(std::fread(magic, 1, 8, raw) == 8);
  CHECK(std::memcmp(magic, "THYRAW1", 7) == 0);
  uint32_t version = 0, header_bytes = 0;
  CHECK(std::fread(&version, 4, 1, raw) == 1);
  CHECK(std::fread(&header_bytes, 4, 1, raw) == 1);
  CHECK(version == 2);
  CHECK(header_bytes == kRawHeaderBytes);
  CHECK(std::fseek(raw, 24, SEEK_SET) == 0);
  char channel_key[32] = {};
  CHECK(std::fread(channel_key, 1, 32, raw) == 32);
  CHECK(std::string(channel_key) == "cloud_1");
  uint32_t profile_version = 0, profile_bytes = 0;
  uint64_t digest = 0;
  CHECK(std::fread(&profile_version, 4, 1, raw) == 1);
  CHECK(std::fread(&profile_bytes, 4, 1, raw) == 1);
  CHECK(std::fread(&digest, 8, 1, raw) == 1);
  char profile[56] = {};
  CHECK(std::fread(profile, 1, 56, raw) == 56);
  CHECK(profile_version == 1);
  CHECK(std::string(profile) == kKnownProfile);
  std::fclose(raw);

  ThymeBinReader rd((cap.pkg + "/meta.tbin").c_str());
  CHECK(rd.ok());
  bool saw_profile = false;
  ThymeBinDataEntry e;
  while (rd.ReadNext(e) == ThymeBinReader::ReadResult::kOk) {
    if (e.struct_id != triggered::kPayloadProfileStructId) continue;
    CHECK(e.payload.size() == sizeof(triggered::PayloadProfileRecord));
    triggered::PayloadProfileRecord row{};
    std::memcpy(&row, e.payload.data(), sizeof(row));
    CHECK(std::string(row.channel) == "cloud_1");
    CHECK(std::string(row.profile) == kKnownProfile);
    CHECK(row.profile_version == 1);
    CHECK((row.flags & triggered::kPayloadProfileFlagHasProfile) != 0);
    saw_profile = true;
  }
  CHECK(saw_profile);

  replaycli::ExportOptions opt;
  opt.package_dir = cap.pkg;
  opt.out_path = std::string(out) + "/known.ndjson";
  opt.format = replaycli::ExportFormat::kNdjson;
  opt.decode_profile = true;
  opt.verify_crc = false;
  std::ostringstream dump;
  replaycli::ExportSummary sum;
  CHECK(replaycli::RunExport(opt, dump, &sum) == replaycli::kExitOk);
  CHECK(sum.ok);
  CHECK(!sum.decode_refused);
  CHECK(FileSize(opt.out_path) > 0);

  RmTree(out);
}

void TestUnknownProfileKeepsRaw() {
  const int me = static_cast<int>(getpid());
  char chan[96];
  char out[96];
  std::snprintf(chan, sizeof(chan), "/thyme_rec27u_%d", me);
  std::snprintf(out, sizeof(out), "/tmp/thyme_rec27u_%d", me);
  RmTree(out);
  ShmRegion::UnlinkName(chan);

  const Capture cap =
      RunCapture(chan, out, "vendor/unknown@v9", 9, /*params=*/nullptr);
  replaycli::ExportOptions opt;
  opt.package_dir = cap.pkg;
  opt.out_path = std::string(out) + "/unknown.ndjson";
  opt.format = replaycli::ExportFormat::kNdjson;
  opt.decode_profile = true;
  opt.verify_crc = false;
  std::ostringstream dump;
  replaycli::ExportSummary sum;
  CHECK(replaycli::RunExport(opt, dump, &sum) == replaycli::kExitFailure);
  CHECK(!sum.ok);
  CHECK(sum.decode_refused);
  CHECK(sum.error.find("unknown payload profile") != std::string::npos);
  CHECK(sum.error.find("raw export kept") != std::string::npos);
  CHECK(FileSize(opt.out_path) > 0);
  CHECK(!sum.removed_partial);

  opt.decode_profile = false;
  opt.out_path = std::string(out) + "/raw.ndjson";
  std::ostringstream dump2;
  replaycli::ExportSummary sum2;
  CHECK(replaycli::RunExport(opt, dump2, &sum2) == replaycli::kExitOk);
  CHECK(sum2.ok);
  CHECK(!sum2.decode_refused);

  RmTree(out);
}

void TestMissingProfileStaysRaw() {
  const int me = static_cast<int>(getpid());
  char chan[96];
  char out[96];
  std::snprintf(chan, sizeof(chan), "/thyme_rec27n_%d", me);
  std::snprintf(out, sizeof(out), "/tmp/thyme_rec27n_%d", me);
  RmTree(out);
  ShmRegion::UnlinkName(chan);

  const Capture cap =
      RunCapture(chan, out, /*profile=*/nullptr, 0, /*params=*/nullptr);
  schema::PackageSchema sc;
  std::string err;
  CHECK(schema::ReadPackageSchema(cap.pkg, &sc, &err) ==
        schema::ReadState::kOk);
  CHECK(sc.payload_profiles.size() == 1);
  CHECK(sc.payload_profiles[0].profile.empty());

  replaycli::ExportOptions opt;
  opt.package_dir = cap.pkg;
  opt.out_path = std::string(out) + "/none.ndjson";
  opt.format = replaycli::ExportFormat::kNdjson;
  opt.decode_profile = true;
  opt.verify_crc = false;
  std::ostringstream dump;
  replaycli::ExportSummary sum;
  CHECK(replaycli::RunExport(opt, dump, &sum) == replaycli::kExitOk);
  CHECK(sum.ok);
  CHECK(!sum.decode_refused);
  RmTree(out);
}

}  // namespace

int main() {
  TestConfigAndHash();
  TestCaptureSidecarAndRawHeader();
  TestUnknownProfileKeepsRaw();
  TestMissingProfileStaysRaw();
  std::printf("test_payload_profile: OK\n");
  return 0;
}
