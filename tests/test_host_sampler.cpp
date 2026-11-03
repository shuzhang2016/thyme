// test_host_sampler.cpp — OBS-14: machine-level sampling.
//
// The parsers are driven from synthetic /proc files through the path seam, so
// aggregate/per-core CPU, load, memory and the system fd table are covered on
// any host. The dump thread is exercised against the real /proc with a short
// interval and the resulting file is walked block by block.
#include "thyme/check.h"
#include "thyme/runtime/host_sampler.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

std::string g_dir;

std::string P(const std::string& rel) { return g_dir + "/" + rel; }

void Mkdirs(const std::string& path) {
  std::string cur;
  for (std::size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (!cur.empty() && cur != "/") {
        if (mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) {
          CHECK(false);
          return;
        }
      }
    }
    if (i < path.size()) cur.push_back(path[i]);
  }
}

void Write(const std::string& rel, const char* text) {
  const std::string path = P(rel);
  Mkdirs(path.substr(0, path.rfind('/')));
  FILE* f = std::fopen(path.c_str(), "w");
  CHECK(f != nullptr);
  std::fputs(text, f);
  std::fclose(f);
}

uint64_t LastValue(const HostSampler& s, uint8_t series) {
  HostSampler::Record recs[512];
  const uint32_t n = s.CopyRecords(recs, 512);
  uint64_t value = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (recs[i].series == series) value = recs[i].value;
  }
  return value;
}

void TestSynthetic() {
  Write("stat",
        "cpu  100 10 40 800 50 5 5 0 0 0\n"
        "cpu0 60 10 20 400 20 5 5 0 0 0\n"
        "cpu1 40 0 20 400 30 0 0 0 0 0\n");
  Write("loadavg", "1.50 0.25 0.00 2/123 4567\n");
  Write("meminfo", "MemTotal:       16384000 kB\nMemFree: 1000 kB\n"
                   "MemAvailable:    8192000 kB\n");
  Write("file-nr", "1000\t0\t9223372036854775807\n");

  HostSampler::Paths paths;
  const std::string stat = P("stat");
  const std::string loadavg = P("loadavg");
  const std::string meminfo = P("meminfo");
  const std::string file_nr = P("file-nr");
  paths.stat = stat.c_str();
  paths.loadavg = loadavg.c_str();
  paths.meminfo = meminfo.c_str();
  paths.file_nr = file_nr.c_str();

  HostSampler s;
  CHECK(s.Start(paths));
  CHECK(s.cores() == 2);

  // First sample after Start: baselines were taken from the same file, so
  // every delta is zero; gauges are already valid.
  CHECK(s.SampleNow(1000000000ull) == 6 + 3 + 2 + 2 + 2);
  CHECK(LastValue(s, HostSampler::kLoad1Milli) == 1500);
  CHECK(LastValue(s, HostSampler::kLoad5Milli) == 250);
  CHECK(LastValue(s, HostSampler::kLoad15Milli) == 0);
  CHECK(LastValue(s, HostSampler::kMemTotalKb) == 16384000);
  CHECK(LastValue(s, HostSampler::kMemAvailableKb) == 8192000);
  CHECK(LastValue(s, HostSampler::kFileNrAllocated) == 1000);
  CHECK(LastValue(s, HostSampler::kFileNrMax) == HostSampler::kUnlimited);

  // Bump counters by exactly one tick each (HZ is 100 on Linux, so 1 tick =
  // 10 ms = 10000000 ns; the ratio only matters for the core test below).
  Write("stat",
        "cpu  110 10 45 890 60 5 5 100 0 0\n"
        "cpu0 70 10 25 400 30 5 5 100 0 0\n"
        "cpu1 40 0 20 490 30 0 0 0 0 0\n");
  CHECK(s.SampleNow(2000000000ull) == 6 + 3 + 2 + 2 + 2);
  const uint64_t tick_ns = 1000000000ull / static_cast<uint64_t>(sysconf(_SC_CLK_TCK));
  CHECK(LastValue(s, HostSampler::kCpuUserNs) == 10 * tick_ns);   // 110-100
  CHECK(LastValue(s, HostSampler::kCpuSystemNs) == 5 * tick_ns);  // 45-40
  CHECK(LastValue(s, HostSampler::kCpuIdleNs) == 90 * tick_ns);   // 890-800
  CHECK(LastValue(s, HostSampler::kCpuIowaitNs) == 10 * tick_ns); // 60-50
  CHECK(LastValue(s, HostSampler::kCpuStealNs) == 100 * tick_ns);
  // core0 busy = user+nice+system+irq+softirq+steal; idle/iowait excluded.
  CHECK(LastValue(s, static_cast<uint8_t>(HostSampler::kCoreBase + 0)) ==
        115 * tick_ns);  // user+nice+sys+irq+softirq+steal delta
  CHECK(LastValue(s, static_cast<uint8_t>(HostSampler::kCoreBase + 1)) == 0);

  // Name block: 13 fixed + one per core, and the per-core name carries its id.
  PerfLogWriter w(0, 0, HostSampler::kFixedNames + HostSampler::kMaxCpus);
  s.FlushMetadata(w, 0);
  const uint8_t* d = w.data();
  CHECK(d[4] == kPerfSeriesNameBlockType);
  CHECK(ReadU32(&d[6]) == 15);
  std::string text(reinterpret_cast<const char*>(d), w.size());
  CHECK(text.find("host_cpu_user_ns") != std::string::npos);
  CHECK(text.find("core0_busy_ns") != std::string::npos);
  CHECK(text.find("core1_busy_ns") != std::string::npos);
  CHECK(text.find("core2_busy_ns") == std::string::npos);
}

void TestCoreCap() {
  // 70 cores in /proc/stat: only the first 64 are emitted, aggregate still ok.
  std::string stat = "cpu 1 0 1 1 0 0 0 0 0 0\n";
  for (int i = 0; i < 70; ++i) {
    char line[128];
    std::snprintf(line, sizeof(line), "cpu%d 1 0 1 1 0 0 0 0 0 0\n", i);
    stat += line;
  }
  Write("cap_stat", stat.c_str());
  HostSampler::Paths paths;
  const std::string stat_path = P("cap_stat");
  paths.stat = stat_path.c_str();
  paths.loadavg = P("missing").c_str();
  paths.meminfo = P("missing").c_str();
  paths.file_nr = P("missing").c_str();
  HostSampler s;
  CHECK(s.Start(paths));
  CHECK(s.cores() == HostSampler::kMaxCpus);
  // 6 aggregate + 64 cores; load/mem/fd files are absent, so no rows for them.
  CHECK(s.SampleNow(1000000000ull) == 6 + HostSampler::kMaxCpus);
}

void TestDumpThread() {
  const std::string path = P("host_dump.bin");
  HostDumpThread t;
  CHECK(t.Start(path.c_str(), 5000000ull));  // 5 ms, test speed-up
  CHECK(t.running());
  struct timespec ts { 0, 40000000 };         // 40 ms -> several dumps
  nanosleep(&ts, nullptr);
  t.Stop();
  CHECK(!t.running());

  FILE* f = std::fopen(path.c_str(), "rb");
  CHECK(f != nullptr);
  std::vector<uint8_t> data;
  uint8_t buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    data.insert(data.end(), buf, buf + n);
  }
  std::fclose(f);
  CHECK(data.size() > 4);
  CHECK(std::memcmp(data.data(), "T9P1", 4) == 0);
  // Walk the blocks: one type-8 name block, at least one type-5 sample block,
  // and no unparsable tail.
  std::size_t p = 4;
  bool saw_names = false, saw_sys = false;
  while (p + 6 <= data.size()) {
    const uint8_t typ = data[p];
    const uint32_t cnt = ReadU32(&data[p + 2]);
    p += 6;
    std::size_t rec = 0;
    if (typ == 5) {
      rec = 10;
      saw_sys = true;
    } else if (typ == 8) {
      rec = kPerfSeriesNameRecordBytes;
      saw_names = true;
    } else if (typ == kPerfManifestBlockType) {
      rec = kPerfManifestRecordBytes;
    } else if (typ == kPerfNodeBudgetBlockType) {
      rec = kPerfNodeBudgetRecordBytes;
    } else if (typ == kPerfClockAnchorBlockType) {
      rec = kPerfClockAnchorRecordBytes;
    } else {
      break;
    }
    p += rec * cnt;
  }
  CHECK(saw_names);
  CHECK(saw_sys);
  CHECK(p == data.size());
}

void TestRealHost() {
  HostSampler s;
  CHECK(s.Start());
  CHECK(s.cores() >= 1);
  const uint32_t n = s.SampleNow(1000000000ull);
  CHECK(n >= 6);
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/thyme_host_sampler_XXXXXX";
  char* dir = mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (dir == nullptr) return 1;
  g_dir = dir;

  TestSynthetic();
  TestCoreCap();
  TestDumpThread();
  TestRealHost();

  const std::string cmd = "rm -rf '" + g_dir + "'";
  (void)std::system(cmd.c_str());
  return 0;
}
