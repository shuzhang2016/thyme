// RT-04: acquire() cost of the default cursor+scan path vs the opt-in O(1)
// free-list path. Correctness is asserted (same slot set / generation
// protocol); timing magnitudes are never asserted by the binary, so a slow CI
// box cannot fail the run.
//
// PERF-06: with --json the run is machine-readable and the per-runner gate
// (tools/ci/check_bench.py) compares ns/op, p99 and p999 against the baseline
// recorded for the same runner tag. Numbers are host-specific evidence, never
// a target-board budget.
//
// Fragmented: N-1 slots are held (WRITING) and exactly one is recyclable. The
// scan path must probe the held prefix on every acquire (O(N)); the free-list
// pops the recyclable slot in O(1). Dense: every slot is free, the scan path
// hits the next cursor position (O(1)) -- the common case the default is
// already good at.
//
// usage: bench_object_pool [--json PATH] [--tag RUNNER_TAG]

#include "thyme/check.h"
#include "thyme/runtime/object_pool.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

#ifndef THYME_BENCH_BUILD_TYPE
#define THYME_BENCH_BUILD_TYPE "unknown"
#endif

using namespace thyme;

namespace {

struct alignas(64) Cell {
  uint64_t x;
};

constexpr std::size_t kSlots = 10000;
constexpr uint64_t kScanIters = 2000;
constexpr uint64_t kFreeListIters = 200000;
constexpr uint64_t kDenseIters = 500000;
constexpr uint64_t kPercentileSamples = 2048;

volatile uint64_t g_sink = 0;

using Clock = std::chrono::steady_clock;

struct Timing {
  double ns_per_op = 0.0;
  double p99_ns_per_op = 0.0;
  double p999_ns_per_op = 0.0;
};

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

std::string Toolchain() {
#if defined(__clang__)
  return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("GCC ") + __VERSION__;
#else
  return "unknown";
#endif
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

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  return out;
}

double NsPerOp(Clock::time_point t0, Clock::time_point t1, uint64_t iters) {
  const double ns =
      std::chrono::duration<double, std::nano>(t1 - t0).count();
  return iters == 0 ? 0.0 : ns / static_cast<double>(iters);
}

// Median batch timing (run time / iterations) plus nearest-rank p99/p999 over
// individually timed operations. The clock read is part of each sample, so the
// tail is comparable only against the same runner's baseline.
template <typename Fn>
Timing Measure(uint64_t iters, Fn&& round_trip) {
  for (int i = 0; i < 64; ++i) round_trip();
  const auto t0 = Clock::now();
  for (uint64_t i = 0; i < iters; ++i) round_trip();
  const auto t1 = Clock::now();

  std::vector<double> samples(kPercentileSamples);
  for (uint64_t i = 0; i < kPercentileSamples; ++i) {
    const auto a = Clock::now();
    round_trip();
    const auto b = Clock::now();
    samples[static_cast<std::size_t>(i)] =
        std::chrono::duration<double, std::nano>(b - a).count();
  }
  std::sort(samples.begin(), samples.end());
  const auto at = [&](double pct) {
    std::size_t rank = static_cast<std::size_t>(
        pct / 100.0 * static_cast<double>(kPercentileSamples) + 0.999999);
    if (rank == 0) rank = 1;
    if (rank > kPercentileSamples) rank = kPercentileSamples;
    return samples[rank - 1];
  };
  Timing out;
  out.ns_per_op = NsPerOp(t0, t1, iters);
  out.p99_ns_per_op = at(99.0);
  out.p999_ns_per_op = at(99.9);
  return out;
}

template <bool kFreeList>
Timing FragmentedAcquireNs() {
  using Pool = ObjectPool<Cell, kSlots, kFreeList>;
  Pool pool;
  std::vector<uint32_t> idx(kSlots);
  for (std::size_t i = 0; i < kSlots; ++i) {
    idx[i] = pool.acquire();
    CHECK(idx[i] != Pool::kInvalidIndex);
  }
  // Release exactly one slot; every other slot stays owned (WRITING).
  const uint32_t target = idx[kSlots / 2];
  pool.publish(target);
  CHECK(pool.release(target, pool.generation(target)));

  const auto round_trip = [&pool, target]() {
    const uint32_t got = pool.acquire();
    CHECK(got == target);
    const uint32_t gen = pool.generation(got);
    pool.publish(got);
    CHECK(pool.release(got, gen));
    g_sink += got;
  };
  constexpr uint64_t kIters = kFreeList ? kFreeListIters : kScanIters;
  return Measure(kIters, round_trip);
}

template <bool kFreeList>
Timing DenseAcquireNs() {
  using Pool = ObjectPool<Cell, kSlots, kFreeList>;
  Pool pool;
  const auto round_trip = [&pool]() {
    const uint32_t idx = pool.acquire();
    CHECK(idx != Pool::kInvalidIndex);
    const uint32_t gen = pool.generation(idx);
    pool.publish(idx);
    CHECK(pool.release(idx, gen));
    g_sink += idx;
  };
  return Measure(kDenseIters, round_trip);
}

struct Metric {
  const char* name;
  uint64_t iterations;
  Timing timing;
};

void EmitJson(const std::string& path, const std::string& tag,
              const std::vector<Metric>& metrics) {
  char host[256] = {0};
  gethostname(host, sizeof(host) - 1);
  std::string out = "{\n";
  out += "  \"schema\": 1,\n";
  out += "  \"kind\": \"thyme-bench-object-pool\",\n";
  out += "  \"generated_at\": \"" + TimestampUtc() + "\",\n";
  out += "  \"git_sha\": \"" + JsonEscape(GitSha()) + "\",\n";
  out += "  \"runner_tag\": \"" + JsonEscape(tag) + "\",\n";
  out += "  \"hostname\": \"" + JsonEscape(host) + "\",\n";
  out += "  \"host\": {\"hostname\": \"" + JsonEscape(host) +
         "\", \"machine\": \"" + JsonEscape(ArchName()) + "\"},\n";
  out += "  \"cpu\": \"" + JsonEscape(CpuModel()) + "\",\n";
  out += "  \"toolchain\": \"" + JsonEscape(Toolchain()) + "\",\n";
  out += "  \"build_type\": \"" + std::string(THYME_BENCH_BUILD_TYPE) + "\",\n";
  out += "  \"ok\": true,\n";
  out += "  \"benchmarks\": [\n";
  for (std::size_t i = 0; i < metrics.size(); ++i) {
    const Metric& m = metrics[i];
    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  "    {\"name\": \"%s\", \"iterations\": %llu, "
                  "\"ns_per_op\": %.4f, \"p99_ns_per_op\": %.4f, "
                  "\"p999_ns_per_op\": %.4f, \"bytes_per_op\": 0, "
                  "\"gbps\": null}",
                  m.name, static_cast<unsigned long long>(m.iterations),
                  m.timing.ns_per_op, m.timing.p99_ns_per_op,
                  m.timing.p999_ns_per_op);
    out += buf;
    if (i + 1 < metrics.size()) out += ",";
    out += "\n";
  }
  out += "  ]\n}\n";

  FILE* f = std::fopen(path.c_str(), "w");
  if (f == nullptr) {
    std::fprintf(stderr, "bench_object_pool: cannot write %s\n", path.c_str());
    return;
  }
  std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
}

void PrintMetric(const Metric& m) {
  std::printf("  %-34s %10llu %10.1f ns/op  p99 %10.1f  p999 %10.1f\n",
              m.name, static_cast<unsigned long long>(m.iterations),
              m.timing.ns_per_op, m.timing.p99_ns_per_op,
              m.timing.p999_ns_per_op);
}

}  // namespace

int main(int argc, char** argv) {
  std::string json_path;
  std::string tag;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json" && i + 1 < argc) {
      json_path = argv[++i];
    } else if (arg == "--tag" && i + 1 < argc) {
      tag = argv[++i];
    } else if (arg == "-h" || arg == "--help") {
      std::printf("usage: bench_object_pool [--json PATH] [--tag RUNNER_TAG]\n"
                  "  THYME_BENCH_RUNNER_TAG sets the default tag\n");
      return 0;
    } else {
      std::fprintf(stderr, "bench_object_pool: unknown argument %s\n",
                   arg.c_str());
      return 2;
    }
  }
  if (tag.empty()) tag = RunnerTag();

  // The scan path's fragmented case probes ~N slots per acquire; keep its
  // iteration count moderate so the bench stays a few hundred milliseconds.
  const std::vector<Metric> metrics = {
      {"object_pool_fragmented_scan", kScanIters,
       FragmentedAcquireNs<false>()},
      {"object_pool_fragmented_freelist", kFreeListIters,
       FragmentedAcquireNs<true>()},
      {"object_pool_dense_scan", kDenseIters, DenseAcquireNs<false>()},
      {"object_pool_dense_freelist", kDenseIters, DenseAcquireNs<true>()},
  };

  std::printf("object_pool acquire bench: N=%zu slots, one thread, runner=%s\n",
              kSlots, tag.c_str());
  for (const Metric& m : metrics) PrintMetric(m);
  std::printf("  fragmented speedup: %.1fx\n",
              metrics[0].timing.ns_per_op / metrics[1].timing.ns_per_op);

  if (!json_path.empty()) EmitJson(json_path, tag, metrics);
  return 0;
}
