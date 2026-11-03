// bench_trigger_seal.cpp — TRG-14 trigger->seal latency bench.
//
// Fires N triggers through the real Center -> TriggeredRecorder pipeline
// (in-process producer: tap-wired SPMC ring -> mirror page, plus a small
// PinnedSlot channel) and reports the distribution of the TRG-14 metric:
//
//   trigger->seal latency =
//     mono_ns of the center's OBS-05 FSM kSubmitAccept event (start anchor)
//     -> mono_ns of full seal completion in the recorder
//        (TriggeredRecorder::last_seal_mono_ns(): every file fsync'ed, wal
//        seal row + REC-18 manifest written, package dir sync'ed).
//
// Like bench_core (CI-05) the numbers are absolute ns for the host that ran
// them and are NEVER asserted: the exit code gates only the measurement
// plumbing (every trigger accepted, accept/seal pairing by trigger_id, every
// package complete with a non-zero wal seal stamp). The SLO budgets derived
// from this bench live in docs/guides/trigger-config.md as reference values.
//
// usage: bench_trigger_seal [--iters N] [--json PATH] [--tag RUNNER_TAG]
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

#ifndef THYME_BENCH_BUILD_TYPE
#define THYME_BENCH_BUILD_TYPE "unknown"
#endif

using namespace thyme;

namespace {

constexpr uint64_t kBeforeNs = 20000000ULL;  // 20 ms pre window
constexpr uint64_t kAfterNs = 50000000ULL;   // 50 ms post window
constexpr uint32_t kFrameBytes = 1000;

struct Pod16 {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};

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

std::string Trim(const std::string& s) {
  const auto begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::string ArchName() {
  struct utsname info {};
  if (uname(&info) != 0) return "unknown";
  return info.machine;
}

// Same runner-tag convention as bench_core: per runner, never per target.
std::string RunnerTag() {
  const char* env = std::getenv("THYME_BENCH_RUNNER_TAG");
  if (env != nullptr && *env != '\0') return env;
  const char* ci = std::getenv("CI");
  return ArchName() + ((ci != nullptr && *ci != '\0') ? "-ci" : "-local");
}

std::string GitSha() {
  FILE* pipe = popen("git rev-parse --short HEAD 2>/dev/null", "r");
  if (pipe == nullptr) return "unknown";
  char buf[64] = {0};
  const char* line = fgets(buf, sizeof(buf), pipe);
  pclose(pipe);
  if (line == nullptr) return "unknown";
  const std::string sha = Trim(buf);
  return sha.empty() ? "unknown" : sha;
}

std::string CpuModel() {
  FILE* f = fopen("/proc/cpuinfo", "r");
  if (f == nullptr) return "unknown";
  char line[512];
  std::string model;
  while (fgets(line, sizeof(line), f) != nullptr) {
    if (std::strncmp(line, "model name", 10) == 0) {
      const char* colon = std::strchr(line, ':');
      if (colon != nullptr) {
        model = Trim(colon + 1);
        break;
      }
    }
  }
  fclose(f);
  return model.empty() ? "unknown" : model;
}

std::string TimestampUtc() {
  const std::time_t now = std::time(nullptr);
  struct tm tm_utc {};
  gmtime_r(&now, &tm_utc);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  return buf;
}

// ---- host wiring: TriggerCenter hooks -> TriggeredRecorder (e2e pattern) --
triggered::TriggeredRecorder* g_rec;

bool HookReady(void*) { return g_rec->ready(); }
bool HookActive(void*) { return g_rec->phase() != triggered::Phase::kArmed; }
bool HookStart(void*, uint64_t id, uint64_t t0, uint64_t before,
               uint64_t after, bool force_rt, bool high_priority,
               const std::vector<std::string>& channels) {
  return g_rec->Trigger(id, before, after, t0, !force_rt, high_priority,
                        channels);
}
bool HookPreempt(void*) { return g_rec->Preempt(); }

// wal.log seal-row stamp of a sealed package (durable TRG-14 record).
bool WalSealMono(const std::string& pkg_dir, uint64_t* mono_ns) {
  FILE* f = std::fopen((pkg_dir + "/wal.log").c_str(), "rb");
  if (f == nullptr) return false;
  triggered::WalHeader wh{};
  bool ok = std::fread(&wh, sizeof(wh), 1, f) == 1;
  triggered::WalRow last{};
  bool any = false;
  while (ok) {
    triggered::WalRow row{};
    if (std::fread(&row, sizeof(row), 1, f) != 1) break;
    last = row;
    any = true;
  }
  std::fclose(f);
  if (!ok || !any || last.op != triggered::kWalOpSeal) return false;
  *mono_ns = last.mono_ns;
  return true;
}

uint64_t Percentile(const std::vector<uint64_t>& sorted, double p) {
  // Nearest-rank: p in (0, 100].
  std::size_t idx = static_cast<std::size_t>(
      p / 100.0 * static_cast<double>(sorted.size()) + 0.999999);
  if (idx == 0) idx = 1;
  if (idx > sorted.size()) idx = sorted.size();
  return sorted[idx - 1];
}

}  // namespace

int main(int argc, char** argv) {
  int iters = 64;
  std::string json_path;
  std::string tag;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--iters" && i + 1 < argc) {
      iters = std::atoi(argv[++i]);
    } else if (arg == "--json" && i + 1 < argc) {
      json_path = argv[++i];
    } else if (arg == "--tag" && i + 1 < argc) {
      tag = argv[++i];
    } else if (arg == "-h" || arg == "--help") {
      std::printf(
          "usage: bench_trigger_seal [--iters N] [--json PATH] [--tag TAG]\n"
          "  THYME_BENCH_RUNNER_TAG sets the default tag\n");
      return 0;
    } else {
      std::fprintf(stderr, "bench_trigger_seal: unknown argument %s\n",
                   arg.c_str());
      return 2;
    }
  }
  if (iters < 4) iters = 4;
  if (tag.empty()) tag = RunnerTag();
  alarm(300);

  const int me = static_cast<int>(getpid());
  char page_name[64], ring_name[64], chan_name[64], out_dir[96], sock[96];
  std::snprintf(page_name, sizeof(page_name), "/btseal_page_%d", me);
  std::snprintf(ring_name, sizeof(ring_name), "/btseal_ring_%d", me);
  std::snprintf(chan_name, sizeof(chan_name), "/btseal_chan_%d", me);
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/btseal_out_%d", me);
  std::snprintf(sock, sizeof(sock), "/tmp/btseal_%d.sock", me);
  RmTree(out_dir);
  ShmRegion::UnlinkName(page_name);
  ShmRegion::UnlinkName(ring_name);
  ShmRegion::UnlinkName(chan_name);

  // In-process producer.
  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = sizeof(Pod16);
  std::snprintf(desc.name, sizeof(desc.name), "Pod16");
  RecordMirrorRingSpec spec{sizeof(Pod16), 512};
  record_mirror::RingClient mirror;
  CHECK(mirror.Create(page_name, &desc, &spec, 1, 1));
  ShmSpmcRing<Pod16, 16> ring;
  CHECK(ring.Create(ring_name));
  ring.SetRecordTap(mirror.Page(), 0);
  PinnedSlotWriter frames;
  CHECK(frames.Create(chan_name, 4096, 128, 0));

  triggered::Config rcfg;
  rcfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name, ""},
                  {triggered::SourceConfig::Kind::kPinnedSlot, chan_name,
                   "h264_front"}};
  rcfg.out_dir = out_dir;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(rcfg));
  g_rec = &rec;

  trigger_cfg::CenterConfig ccfg;
  ccfg.socket_path = sock;
  trigger_cfg::SourceRule bench;
  bench.name = "bench";
  bench.before_ns = kBeforeNs;
  bench.after_ns = kAfterNs;
  bench.priority = 10;
  bench.mute_ns = 0;            // back-to-back triggers
  bench.flush_realtime = true;  // no idle gate: measures the realtime path
  bench.quota_class = "bench";
  ccfg.sources = {bench};
  ccfg.quota_limits = {{"bench", 0}};

  trigger_center::Hooks hooks;
  hooks.recorder_ready = HookReady;
  hooks.capture_active = HookActive;
  hooks.start_capture = HookStart;
  hooks.preempt_capture = HookPreempt;
  trigger_center::Center center;
  CHECK(center.Init(ccfg, hooks));

  uint64_t pub = 0;
  auto publish = [&] {
    Pod16 p{pub, static_cast<uint32_t>(pub * 2), static_cast<uint32_t>(pub * 3)};
    ring.Push(p);
    PinnedSlotWriter::BeginView bv = frames.Begin(kFrameBytes);
    CHECK(bv.payload != nullptr);
    std::memset(bv.payload, static_cast<int>(pub & 0xFF), kFrameBytes);
    frames.Commit();
    ++pub;
  };
  auto tick = [&] {
    center.Tick();
    rec.Tick();
  };

  // Producer history for the pre window.
  {
    const uint64_t w0 = NowNs();
    while (NowNs() - w0 < 40000000ULL) {
      publish();
      tick();
      SleepNs(250000);
    }
  }

  std::vector<uint64_t> latencies;
  latencies.reserve(static_cast<std::size_t>(iters));
  uint64_t wal_seal_mono = 0;
  for (int it = 0; it < iters; ++it) {
    const trigger_center::SubmitResult r = center.Submit("bench", -1, 0,
                                                         false, false);
    CHECK(r.code == trigger_proto::TriggerCode::kAccepted);
    const uint64_t id = r.trigger_id;

    // Start anchor: the accept stamp from the OBS-05 FSM ring.
    uint64_t accept_mono = 0;
    {
      trigger_center::FsmEvent ev[trigger_center::Center::kFsmEventRingSize];
      const uint32_t n =
          center.CopyFsmEvents(ev, trigger_center::Center::kFsmEventRingSize);
      for (uint32_t i = 0; i < n; ++i) {
        if (ev[i].trigger_id == id &&
            ev[i].reason ==
                static_cast<uint8_t>(trigger_center::FsmReason::kSubmitAccept)) {
          accept_mono = ev[i].mono_ns;
        }
      }
    }
    CHECK(accept_mono != 0);

    const uint64_t deadline = NowNs() + 30000000000ULL;
    while (rec.last_seal_trigger_id() != id) {
      tick();
      publish();
      SleepNs(250000);
      CHECK(NowNs() < deadline);
    }
    const uint64_t seal_mono = rec.last_seal_mono_ns();
    CHECK(seal_mono >= accept_mono);
    CHECK(triggered::TriggeredRecorder::PackageComplete(
        rec.last_package_dir()));
    // Durable record: the package's wal seal row is stamped and ordered.
    CHECK(WalSealMono(rec.last_package_dir(), &wal_seal_mono));
    CHECK(wal_seal_mono != 0 && wal_seal_mono >= accept_mono &&
          wal_seal_mono <= seal_mono);
    latencies.push_back(seal_mono - accept_mono);
    tick();  // let the center observe completion before the next submit
  }

  std::vector<uint64_t> sorted = latencies;
  std::sort(sorted.begin(), sorted.end());
  const uint64_t p50 = Percentile(sorted, 50.0);
  const uint64_t p90 = Percentile(sorted, 90.0);
  const uint64_t p99 = Percentile(sorted, 99.0);
  const uint64_t p999 = Percentile(sorted, 99.9);
  double mean = 0;
  for (uint64_t v : latencies) mean += static_cast<double>(v);
  mean /= static_cast<double>(latencies.size());

  std::printf("bench_trigger_seal: runner=%s git=%s build=%s iters=%d "
              "window=%llums+%llums\n",
              tag.c_str(), GitSha().c_str(), THYME_BENCH_BUILD_TYPE, iters,
              static_cast<unsigned long long>(kBeforeNs / 1000000),
              static_cast<unsigned long long>(kAfterNs / 1000000));
  std::printf("%-8s %12s %12s\n", "stat", "ns", "ms");
  const auto row = [](const char* name, uint64_t v) {
    std::printf("%-8s %12llu %12.3f\n", name,
                static_cast<unsigned long long>(v),
                static_cast<double>(v) / 1e6);
  };
  row("min", sorted.front());
  row("p50", p50);
  row("p90", p90);
  row("p99", p99);
  row("p999", p999);
  row("max", sorted.back());
  std::printf("%-8s %12.0f %12.3f\n", "mean", mean, mean / 1e6);

  if (!json_path.empty()) {
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    std::string out = "{\n";
    out += "  \"schema\": 1,\n";
    out += "  \"kind\": \"thyme-bench-trigger-seal\",\n";
    out += "  \"generated_at\": \"" + TimestampUtc() + "\",\n";
    out += "  \"git_sha\": \"" + GitSha() + "\",\n";
    out += "  \"runner_tag\": \"" + tag + "\",\n";
    out += "  \"hostname\": \"" + std::string(host) + "\",\n";
    out += "  \"host\": {\"hostname\": \"" + std::string(host) +
           "\", \"machine\": \"" + ArchName() + "\"},\n";
    out += "  \"cpu\": \"" + CpuModel() + "\",\n";
    out += "  \"build_type\": \"" + std::string(THYME_BENCH_BUILD_TYPE) + "\",\n";
    out += "  \"ok\": true,\n";
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "  \"config\": {\"iters\": %d, \"before_ns\": %llu, "
                  "\"after_ns\": %llu},\n"
                  "  \"trigger_to_seal_ns\": {\"n\": %zu, \"min\": %llu, "
                  "\"p50\": %llu, \"p90\": %llu, \"p99\": %llu, "
                  "\"p999\": %llu, \"max\": %llu, \"mean\": %.1f}\n",
                  iters, static_cast<unsigned long long>(kBeforeNs),
                  static_cast<unsigned long long>(kAfterNs), sorted.size(),
                  static_cast<unsigned long long>(sorted.front()),
                  static_cast<unsigned long long>(p50),
                  static_cast<unsigned long long>(p90),
                  static_cast<unsigned long long>(p99),
                  static_cast<unsigned long long>(p999),
                  static_cast<unsigned long long>(sorted.back()), mean);
    out += buf;
    out += "}\n";
    FILE* f = std::fopen(json_path.c_str(), "w");
    if (f == nullptr) {
      std::fprintf(stderr, "bench_trigger_seal: cannot write %s\n",
                   json_path.c_str());
      return 1;
    }
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
  }

  center.Shutdown();
  rec.Shutdown();
  frames.Close();
  RmTree(out_dir);
  ShmRegion::UnlinkName(page_name);
  ShmRegion::UnlinkName(ring_name);
  ShmRegion::UnlinkName(chan_name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(chan_name).c_str());
  return 0;
}
