// test_proc_ext.cpp — OBS-13: optional ProcSampler sources.
//
// Every source is exercised through the path seam (ProcExtPaths) with a
// synthetic tree, so the test is deterministic on any host and covers the
// cgroup v1 and v2 layouts separately. The real host is also probed once to
// assert the capability bitmask never claims a source that cannot be read.
#include "thyme/check.h"
#include "thyme/runtime/proc_sampler.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
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
  const std::string dir = path.substr(0, path.rfind('/'));
  Mkdirs(dir);
  FILE* f = std::fopen(path.c_str(), "w");
  CHECK(f != nullptr);
  std::fputs(text, f);
  std::fclose(f);
}

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t SumSeries(const ProcSampler& s, uint8_t series) {
  ProcSampler::Record recs[512];
  const uint32_t n = s.CopyRecords(recs, 512);
  uint64_t sum = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (recs[i].series == series) sum += recs[i].value;
  }
  return sum;
}

uint64_t LastSeries(const ProcSampler& s, uint8_t series) {
  ProcSampler::Record recs[512];
  const uint32_t n = s.CopyRecords(recs, 512);
  uint64_t v = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (recs[i].series == series) v = recs[i].value;
  }
  return v;
}

uint64_t CountSeries(const ProcSampler& s, uint8_t series) {
  ProcSampler::Record recs[512];
  const uint32_t n = s.CopyRecords(recs, 512);
  uint64_t c = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (recs[i].series == series) ++c;
  }
  return c;
}

// Text block of a FlushMetadata writer, for name assertions.
std::string MetadataText(const ProcSampler& s) {
  PerfLogWriter w;
  s.FlushMetadata(w, 1);
  std::string out(reinterpret_cast<const char*>(w.data()), w.size());
  return out;
}

void TestEnvParsing() {
  {
    const ProcSampler::Options o = ProcSampler::OptionsFromEnv("");
    CHECK(!o.cgroup && !o.psi && !o.disk && !o.net && o.thread_slots == 0);
  }
  {
    const ProcSampler::Options o = ProcSampler::OptionsFromEnv("psi");
    CHECK(o.psi && !o.cgroup && o.thread_slots == 0);
  }
  {
    const ProcSampler::Options o =
        ProcSampler::OptionsFromEnv("cgroup, disk ,net");
    CHECK(o.cgroup && o.disk && o.net && !o.psi);
  }
  {
    const ProcSampler::Options o = ProcSampler::OptionsFromEnv("all");
    CHECK(o.cgroup && o.psi && o.disk && o.net &&
          o.thread_slots == kProcExtMaxThreads);
  }
  {
    // Unknown tokens are ignored (older runtime, newer launcher).
    const ProcSampler::Options o = ProcSampler::OptionsFromEnv("psi,etw");
    CHECK(o.psi && !o.net);
  }
  {
    // "pressured" must not match "psi".
    const ProcSampler::Options o = ProcSampler::OptionsFromEnv("pressured");
    CHECK(!o.psi);
  }
}

void TestDefaultOff() {
  ProcSampler s;
  CHECK(s.Start(ProcSampler::Options{}));
  CHECK(s.ext_requested() == 0);
  CHECK(s.ext_supported() == 0);
  CHECK(s.SampleNow(NowNs()) == 16);
  CHECK(CountSeries(s, ProcSampler::kExtCapsSeries) == 0);
  const std::string meta = MetadataText(s);
  CHECK(meta.find("ext_caps") == std::string::npos);
  CHECK(meta.find("cgroup_cpu_usage_us") == std::string::npos);
}

void TestCgroupV2() {
  // cgroup_file says the unified hierarchy is mounted and this process lives
  // at /foo; the synthetic root then holds foo/*.
  Write("v2_cgroup", "0::/foo\n");
  Write("v2/foo/cgroup.controllers", "cpu memory io\n");
  Write("v2/foo/cpu.stat",
        "usage_usec 1000\nuser_usec 600\nsystem_usec 400\nnr_periods 5\n"
        "nr_throttled 2\nthrottled_usec 300\n");
  Write("v2/foo/memory.current", "2048\n");
  Write("v2/foo/memory.max", "max\n");
  Write("v2/foo/memory.events", "low 0\nhigh 0\nmax 0\noom 3\noom_kill 1\n");
  Write("v2/foo/io.stat", "8:0 rbytes=2048 wbytes=4096\n8:16 rbytes=2048\n");

  const std::string cg_file = P("v2_cgroup");
  const std::string cg_root = P("v2");
  ProcSampler::Options opt;
  opt.cgroup = true;
  opt.paths.cgroup_file = cg_file.c_str();
  opt.paths.cgroup_root = cg_root.c_str();

  ProcSampler s;
  CHECK(s.Start(opt));
  CHECK((s.ext_supported() & kExtSrcCgroup) != 0);
  CHECK(s.SampleNow(NowNs()) > 16);  // + caps + cgroup fields

  // Instants are read on the first sample; deltas are zero because Begin()
  // took the baseline.
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupMemCurrent)) == 2);        // 2048 B
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupMemMax)) == 0xFFFFFFFFu);  // max
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupOomEvents)) == 0);
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupIoRead)) == 0);
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupCpuUsage)) == 0);

  // Bump every counter; the next sample reports the deltas.
  Write("v2/foo/cpu.stat",
        "usage_usec 6000\nuser_usec 3600\nsystem_usec 2400\nnr_periods 9\n"
        "nr_throttled 5\nthrottled_usec 2300\n");
  Write("v2/foo/memory.current", "4096\n");
  Write("v2/foo/memory.events", "oom 6\n");
  Write("v2/foo/io.stat", "8:0 rbytes=4096 wbytes=8192\n8:16 rbytes=4096\n");
  CHECK(s.SampleNow(NowNs()) > 16);
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupCpuUsage)) == 5000);       // us
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupCpuThrottled)) == 2000);   // us
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupCpuNrThrottled)) == 3);
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupOomEvents)) == 3);
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupIoRead)) == 4);  // 8192 B -> KiB
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupIoWrite)) == 4);
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupMemCurrent)) == 4);

  const std::string meta = MetadataText(s);
  CHECK(meta.find("ext_caps") != std::string::npos);
  CHECK(meta.find("cgroup_cpu_usage_us") != std::string::npos);
  CHECK(meta.find("cgroup_io_write_kb") != std::string::npos);
  CHECK(meta.find("host_psi_cpu_some_us") == std::string::npos);
}

void TestCgroupV1() {
  Write("v1_cgroup", "8:cpu,cpuacct:/foo\n7:memory:/foo\n6:blkio:/foo\n");
  Write("v1/cpu,cpuacct/foo/cpuacct.usage", "9000000\n");
  Write("v1/cpu,cpuacct/foo/cpu.stat",
        "nr_periods 7\nnr_throttled 2\nthrottled_time 5000000\n");
  Write("v1/memory/foo/memory.usage_in_bytes", "1048576\n");
  Write("v1/memory/foo/memory.limit_in_bytes", "9223372036854771712\n");
  Write("v1/memory/foo/memory.oom_control", "oom_kill 4\nunder_oom 0\n");
  Write("v1/blkio/foo/blkio.throttle.io_service_bytes",
        "8:0 Read 10240\n8:0 Write 20480\nTotal 30720\n");

  const std::string cg_file = P("v1_cgroup");
  const std::string cg_root = P("v1");
  ProcSampler::Options opt;
  opt.cgroup = true;
  opt.paths.cgroup_file = cg_file.c_str();
  opt.paths.cgroup_root = cg_root.c_str();

  ProcSampler s;
  CHECK(s.Start(opt));
  CHECK((s.ext_supported() & kExtSrcCgroup) != 0);
  CHECK(s.SampleNow(NowNs()) > 16);
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupMemCurrent)) == 1024);
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupMemMax)) == 0xFFFFFFFFu);
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupOomEvents)) == 0);

  Write("v1/cpu,cpuacct/foo/cpuacct.usage", "11000000\n");
  Write("v1/cpu,cpuacct/foo/cpu.stat",
        "nr_periods 9\nnr_throttled 4\nthrottled_time 8000000\n");
  Write("v1/memory/foo/memory.usage_in_bytes", "2097152\n");
  Write("v1/memory/foo/memory.oom_control", "oom_kill 5\n");
  Write("v1/blkio/foo/blkio.throttle.io_service_bytes",
        "8:0 Read 17408\n8:0 Write 33792\nTotal 51200\n");
  CHECK(s.SampleNow(NowNs()) > 16);
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupCpuUsage)) == 2000);     // ns->us
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupCpuThrottled)) == 3000); // ns->us
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupCpuNrThrottled)) == 2);
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupOomEvents)) == 1);
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupIoRead)) == 7);   // 7168 B
  CHECK(LastSeries(s, ProcExtSeries(kExtCgroupIoWrite)) == 13); // 13312 B
}

void TestCgroupUnsupported() {
  // A cgroupfs the process cannot read must not be claimed: request it, see
  // ext_caps with the bit clear, and get no cgroup fields at all.
  Write("none_cgroup", "0::/gone\n");
  const std::string cg_file = P("none_cgroup");
  const std::string cg_root = P("does_not_exist");
  ProcSampler::Options opt;
  opt.cgroup = true;
  opt.paths.cgroup_file = cg_file.c_str();
  opt.paths.cgroup_root = cg_root.c_str();
  ProcSampler s;
  CHECK(s.Start(opt));
  CHECK(s.ext_requested() != 0);
  CHECK((s.ext_supported() & kExtSrcCgroup) == 0);
  const uint32_t n = s.SampleNow(NowNs());
  CHECK(n == 17);  // 16 base + ext_caps
  CHECK(LastSeries(s, ProcSampler::kExtCapsSeries) == 0);
  CHECK(CountSeries(s, ProcExtSeries(kExtCgroupMemCurrent)) == 0);
  const std::string meta = MetadataText(s);
  CHECK(meta.find("ext_caps") != std::string::npos);
  CHECK(meta.find("cgroup_cpu_usage_us") == std::string::npos);
}

void TestPsi() {
  Write("psi/cpu", "some avg10=0.00 avg60=0.01 avg300=0.08 total=1000\n");
  Write("psi/memory",
        "some avg10=0.00 avg60=0.00 avg300=0.00 total=2000\n"
        "full avg10=0.00 avg60=0.00 avg300=0.00 total=500\n");
  Write("psi/io",
        "some avg10=0.00 avg60=0.00 avg300=0.00 total=3000\n"
        "full avg10=0.00 avg60=0.00 avg300=0.00 total=100\n");

  const std::string psi_dir = P("psi");
  ProcSampler::Options opt;
  opt.psi = true;
  opt.paths.pressure_dir = psi_dir.c_str();
  ProcSampler s;
  CHECK(s.Start(opt));
  CHECK((s.ext_supported() & kExtSrcPsi) != 0);
  CHECK(s.SampleNow(NowNs()) == 16 + 1 + 5);

  Write("psi/cpu", "some avg10=0.00 avg60=0.01 avg300=0.08 total=4000\n");
  Write("psi/memory",
        "some avg10=0.00 avg60=0.00 avg300=0.00 total=3500\n"
        "full avg10=0.00 avg60=0.00 avg300=0.00 total=1500\n");
  Write("psi/io",
        "some avg10=0.00 avg60=0.00 avg300=0.00 total=6000\n"
        "full avg10=0.00 avg60=0.00 avg300=0.00 total=1100\n");
  CHECK(s.SampleNow(NowNs()) == 16 + 1 + 5);
  CHECK(LastSeries(s, ProcExtSeries(kExtPsiCpuSome)) == 3000);
  CHECK(LastSeries(s, ProcExtSeries(kExtPsiMemSome)) == 1500);
  CHECK(LastSeries(s, ProcExtSeries(kExtPsiMemFull)) == 1000);
  CHECK(LastSeries(s, ProcExtSeries(kExtPsiIoSome)) == 3000);
  CHECK(LastSeries(s, ProcExtSeries(kExtPsiIoFull)) == 1000);
}

void TestDiskNet() {
  // Only sda exists under the synthetic /sys/block: sda1 is a partition and
  // must be skipped so the same bytes are not counted twice.
  Write("sys_block/sda/queue", "nomerges 0\n");
  Write("diskstats",
        "   8       0 sda 10 0 2048 0 20 0 4096 0 0 0 0\n"
        "   8       1 sda1 5 0 512 0 5 0 512 0 0 0 0\n");
  Write("netdev",
        "Inter-|   Receive                                                |  "
        "Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed "
        "multicast|bytes    packets errs drop fifo colls carrier compressed\n"
        "    lo: 999999 1 0 0 0 0 0 0 999999 1 0 0 0 0 0 0\n"
        "  eth0: 10240 1 0 0 0 0 0 0 20480 1 0 0 0 0 0 0\n");

  const std::string diskstats = P("diskstats");
  const std::string netdev = P("netdev");
  const std::string sys_block = P("sys_block");
  ProcSampler::Options opt;
  opt.disk = true;
  opt.net = true;
  opt.paths.diskstats = diskstats.c_str();
  opt.paths.netdev = netdev.c_str();
  opt.paths.sys_block = sys_block.c_str();
  ProcSampler s;
  CHECK(s.Start(opt));
  CHECK((s.ext_supported() & (kExtSrcDisk | kExtSrcNet)) ==
        (kExtSrcDisk | kExtSrcNet));
  CHECK(s.SampleNow(NowNs()) == 16 + 1 + 4);

  Write("diskstats",
        "   8       0 sda 20 0 4096 0 40 0 8192 0 0 0 0\n"
        "   8       1 sda1 10 0 1024 0 10 0 1024 0 0 0 0\n");
  Write("netdev",
        "header1\nheader2\n"
        "    lo: 999999 1 0 0 0 0 0 0 999999 1 0 0 0 0 0 0\n"
        "  eth0: 30720 1 0 0 0 0 0 0 40960 1 0 0 0 0 0 0\n");
  CHECK(s.SampleNow(NowNs()) == 16 + 1 + 4);
  // 2048 sectors * 512 B = 1 MiB read -> 1024 KiB; 4096 sectors -> 2048 KiB.
  CHECK(LastSeries(s, ProcExtSeries(kExtDiskRead)) == 1024);
  CHECK(LastSeries(s, ProcExtSeries(kExtDiskWrite)) == 2048);
  CHECK(LastSeries(s, ProcExtSeries(kExtNetRx)) == 20);   // 20480 B
  CHECK(LastSeries(s, ProcExtSeries(kExtNetTx)) == 20);   // 20480 B
}

void TestThreads() {
  ProcSampler::Options opt;
  opt.thread_slots = 2;
  ProcSampler s;
  CHECK(s.Start(opt));
  const int tid = static_cast<int>(syscall(SYS_gettid));
  const int slot = s.RegisterThread(tid, "main");
  CHECK(slot == 0);
  CHECK(s.thread_count() == 1);
  CHECK(s.RegisterThread(tid, "main") == -1);  // duplicate
  CHECK(s.RegisterThread(0, "bad") == -1);

  // Burn well past one 10 ms tick so the utime/stime delta is non-zero.
  const uint64_t end = NowNs() + 60000000ull;
  volatile uint64_t x = 0;
  while (NowNs() < end) {
    for (int i = 0; i < 1000; ++i) x += static_cast<uint64_t>(i);
  }
  CHECK(s.SampleNow(NowNs()) == 16 + 1 + 1);
  const uint8_t series = static_cast<uint8_t>(kProcExtThreadBase + slot);
  CHECK(CountSeries(s, series) == 1);
  // 60 ms of spinning must show up as tens of millions of ns; a plain tick
  // count (2) would mean the ns conversion regressed.
  CHECK(SumSeries(s, series) >= 1000000);

  const std::string meta = MetadataText(s);
  CHECK(meta.find("thr_main_cpu_ns") != std::string::npos);
}

void TestThreadSlotBound() {
  ProcSampler::Options opt;
  opt.thread_slots = 1;
  ProcSampler s;
  CHECK(s.Start(opt));
  CHECK(s.RegisterThread(static_cast<int>(syscall(SYS_gettid)), "a") == 0);
  CHECK(s.RegisterThread(static_cast<int>(syscall(SYS_gettid)) + 1, "b") == -1);
  CHECK(s.thread_count() == 1);
}

void TestRealHostCapsHonest() {
  // Ask for everything on the real host. Whatever is unsupported must show up
  // as a clear bit in ext_caps, and its series must never be emitted.
  ProcSampler::Options opt = ProcSampler::OptionsFromEnv("all");
  ProcSampler s;
  CHECK(s.Start(opt));
  const uint8_t caps = s.ext_supported();
  CHECK((caps & kExtSrcThreads) != 0);  // /proc/self/task always exists
  const uint32_t n = s.SampleNow(NowNs());
  CHECK(n >= 17);
  CHECK(LastSeries(s, ProcSampler::kExtCapsSeries) == caps);
  const struct {
    uint8_t bit;
    ExtField first;
  } groups[] = {{kExtSrcCgroup, kExtCgroupCpuUsage},
                {kExtSrcPsi, kExtPsiCpuSome},
                {kExtSrcDisk, kExtDiskRead},
                {kExtSrcNet, kExtNetRx}};
  for (const auto& g : groups) {
    const bool on = (caps & g.bit) != 0;
    CHECK(CountSeries(s, ProcExtSeries(g.first)) == (on ? 1u : 0u));
  }
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/thyme_proc_ext_XXXXXX";
  char* dir = mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (dir == nullptr) return 1;
  g_dir = dir;

  TestEnvParsing();
  TestDefaultOff();
  TestCgroupV2();
  TestCgroupV1();
  TestCgroupUnsupported();
  TestPsi();
  TestDiskNet();
  TestThreads();
  TestThreadSlotBound();
  TestRealHostCapsHonest();

  const std::string cmd = "rm -rf '" + g_dir + "'";
  (void)std::system(cmd.c_str());
  return 0;
}
