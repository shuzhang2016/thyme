// bench_ad_stack: run the full AD stack pipeline for N seconds, then emit a
// CSV of per-thread wake/work percentiles, drop/deadline counts, and one
// pipeline row with end-to-end latency percentiles.
//
// CLI:
//   --seconds N   run duration in seconds (default 60)
//   --out PATH    CSV output path (default /tmp/ad_stack_bench.csv)
//   --json PATH   optional JSON summary path
//
// Exit codes:
//   0  functional pass (no drops)
//   1  functional fail (total drops > 0)
//   GATE_WARN lines are informational only and never affect the exit code.

#include "generated/wiring.h"
#include "tools/bench/bench_health_map.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

struct Percentiles {
  uint32_t p50;
  uint32_t p99;
  uint32_t max;
};

// Compute p50 / p99 / max from a sample buffer. `scratch` must hold >= n
// entries; malloc after Join is acceptable. Empty histogram -> all zeros.
// Returns false when n < min_samples (percentiles left zero).
bool ComputePercentiles(const uint32_t* src, std::size_t n, uint32_t* scratch,
                        std::size_t min_samples, Percentiles* out) {
  if (n < min_samples || n == 0) {
    *out = {0, 0, 0};
    return false;
  }
  std::copy(src, src + n, scratch);
  std::sort(scratch, scratch + n);
  out->p50 = scratch[n / 2];
  std::size_t idx99 = (n * 99) / 100;
  if (idx99 >= n) idx99 = n - 1;
  out->p99 = scratch[idx99];
  out->max = scratch[n - 1];
  return true;
}

struct ThreadRow {
  const char* name;
  uint32_t period_hz;
  uint64_t loops;
  Percentiles wake;
  Percentiles work;
  std::size_t wake_n = 0;
  std::size_t work_n = 0;
  uint64_t drops;
  uint32_t deadline_miss;
};

void PrintGate(const char* label, bool ok) {
  std::fprintf(stderr, "%s %s\n", ok ? "GATE_OK" : "GATE_WARN", label);
}

constexpr std::size_t kMinBenchSamples = 10;

// Soft wake jitter budget from declared period only (no node-name branches).
uint32_t WakeP99ThresholdNs(uint32_t hz) {
  if (hz == 0) return 2000000u;
  const uint64_t period_ns = 1000000000ULL / hz;
  uint64_t thresh = period_ns / 5;  // 20% of period
  if (thresh < 1000000u) thresh = 1000000u;
  if (thresh > 2000000u) thresh = 2000000u;
  return static_cast<uint32_t>(thresh);
}

// Pipeline-latency source: the last cyclic node that is not a health_reporter.
// health_reporter is a noworker aggregator (always last, enforced by the
// generated graph_check) whose latency samples are empty; the last real
// consumer of the chain carries the end-to-end latency.
std::size_t PipelineLatencyHealthIdx() {
  std::size_t last = 0;
  for (std::size_t i = 0; i < kBenchCyclicCount; ++i) {
    const std::size_t hi = kBenchCyclicHealthIdx[i];
    if (std::strcmp(kGraphNodes[hi].type, "health_reporter") != 0) last = hi;
  }
  return last;
}

bool WriteJsonSummary(const char* path, int seconds, uint64_t total_drops,
                      const ThreadRow* rows, std::size_t row_count,
                      std::size_t lat_samples, const Percentiles& lat) {
  std::FILE* fp = std::fopen(path, "w");
  if (!fp) {
    std::fprintf(stderr, "bench: cannot open json %s for write\n", path);
    return false;
  }
  std::fprintf(fp,
               "{\n  \"seconds\": %d,\n  \"total_drops\": %llu,\n"
               "  \"threads\": [\n",
               seconds, static_cast<unsigned long long>(total_drops));
  for (std::size_t i = 0; i < row_count; ++i) {
    std::fprintf(fp,
        "    {\"name\":\"%s\",\"period_hz\":%u,\"loops\":%llu,"
        "\"p50_wake_err_ns\":%u,\"p99_wake_err_ns\":%u,\"max_wake_err_ns\":%u,"
        "\"p50_work_ns\":%u,\"p99_work_ns\":%u,\"max_work_ns\":%u,"
        "\"drops\":%llu,\"deadline_miss\":%u}%s\n",
        rows[i].name, rows[i].period_hz,
        static_cast<unsigned long long>(rows[i].loops),
        rows[i].wake.p50, rows[i].wake.p99, rows[i].wake.max,
        rows[i].work.p50, rows[i].work.p99, rows[i].work.max,
        static_cast<unsigned long long>(rows[i].drops), rows[i].deadline_miss,
        (i + 1 < row_count) ? "," : "");
  }
  std::fprintf(fp,
      "  ],\n  \"pipeline\": {\"samples\": %zu, \"p50_lat_ns\": %u, "
      "\"p99_lat_ns\": %u, \"max_lat_ns\": %u}\n}\n",
      lat_samples, lat.p50, lat.p99, lat.max);
  std::fclose(fp);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  int seconds = 60;
  const char* out_path = "/tmp/ad_stack_bench.csv";
  const char* json_path = nullptr;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      seconds = std::atoi(argv[++i]);
      if (seconds <= 0) seconds = 60;
    } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out_path = argv[++i];
    } else if (std::strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
      json_path = argv[++i];
    }
  }

  // Pools + rings + threads wired via the generated Pipeline struct.
  auto p = Pipeline::Create();

  p->Start();

  std::this_thread::sleep_for(std::chrono::seconds(seconds));

  p->RequestStop();
  p->Join();

  // Scratch buffers for percentile sort (malloc after Join is OK). Use a
  // distinct sort buffer per call so ComputePercentiles never std::copy's a
  // buffer onto itself (UB for overlapping ranges).
  std::array<uint32_t, 4096> wake_samples;
  std::array<uint32_t, 4096> work_samples;
  std::array<uint32_t, 4096> lat_samples;
  std::array<uint32_t, 4096> sort_buf;

  ThreadRow rows[kBenchCyclicCount];
  uint64_t total_drops = 0;
  for (std::size_t i = 0; i < kBenchCyclicCount; ++i) {
    const std::size_t hi = kBenchCyclicHealthIdx[i];
    thyme::CyclicThread* th = p->ThreadByNodeId(kGraphNodes[hi].id);
    if (!th) {
      std::fprintf(stderr, "bench: no CyclicThread for health[%zu] (%s)\n", hi,
                   kGraphNodes[hi].id);
      return 2;
    }
    thyme::HealthSlot* slot = &p->health[hi];
    rows[i].name = th->name();
    rows[i].period_hz = th->period_hz();
    rows[i].loops = th->loop_count();

    std::size_t wn = th->CopyWakeErrs(wake_samples.data(), 4096);
    rows[i].wake_n = wn;
    ComputePercentiles(wake_samples.data(), wn, sort_buf.data(), kMinBenchSamples,
                       &rows[i].wake);

    std::size_t kn = th->CopyWorkNs(work_samples.data(), 4096);
    rows[i].work_n = kn;
    ComputePercentiles(work_samples.data(), kn, sort_buf.data(), kMinBenchSamples,
                       &rows[i].work);

    rows[i].drops = slot->drop_cnt.load(std::memory_order_relaxed);
    rows[i].deadline_miss = slot->deadline_miss_cnt.load(std::memory_order_relaxed);
    total_drops += rows[i].drops;
  }

  // Pipeline latency row: end-to-end latency recorded by the chain's last real
  // consumer (consumer-side latency samples, same ring contract as work).
  const std::size_t lat_hi = PipelineLatencyHealthIdx();
  thyme::CyclicThread* sink = p->ThreadByNodeId(kGraphNodes[lat_hi].id);
  std::size_t ln =
      sink != nullptr ? sink->CopyLatenciesNs(lat_samples.data(), 4096) : 0;
  Percentiles lat{};
  const bool lat_ok = ComputePercentiles(lat_samples.data(), ln, sort_buf.data(),
                                         kMinBenchSamples, &lat);

  // Write CSV.
  std::FILE* fp = std::fopen(out_path, "w");
  if (!fp) {
    std::fprintf(stderr, "bench: cannot open %s for write\n", out_path);
    return 2;
  }
  std::fprintf(fp,
      "thread,period_hz,loops,p50_wake_err_ns,p99_wake_err_ns,max_wake_err_ns,"
      "p50_work_ns,p99_work_ns,max_work_ns,drops,deadline_miss\n");
  for (std::size_t i = 0; i < kBenchCyclicCount; ++i) {
    std::fprintf(fp, "%s,%u,%llu,%u,%u,%u,%u,%u,%u,%llu,%u\n",
        rows[i].name, rows[i].period_hz,
        static_cast<unsigned long long>(rows[i].loops),
        rows[i].wake.p50, rows[i].wake.p99, rows[i].wake.max,
        rows[i].work.p50, rows[i].work.p99, rows[i].work.max,
        static_cast<unsigned long long>(rows[i].drops),
        rows[i].deadline_miss);
  }
  // Pipeline row: latency percentiles occupy the wake_err columns; the rest 0.
  std::fprintf(fp, "pipeline,0,%zu,%u,%u,%u,0,0,0,0,0\n",
      ln, lat.p50, lat.p99, lat.max);
  std::fclose(fp);

  if (json_path && !WriteJsonSummary(json_path, seconds, total_drops, rows,
                                     kBenchCyclicCount, ln, lat)) {
    return 2;
  }

  // Gates (stderr). Informational: GATE_WARN never forces a non-zero exit;
  // the exit code reflects total_drops only (checked at the end).
  std::fprintf(stderr, "=== gates (seconds=%d, out=%s) ===\n", seconds, out_path);
  if (json_path) {
    std::fprintf(stderr, "json=%s\n", json_path);
  }

  for (std::size_t i = 0; i < kBenchCyclicCount; ++i) {
    uint32_t hz = rows[i].period_hz;
    char buf[128];
    const bool wake_ok = rows[i].wake_n >= kMinBenchSamples;
    const bool work_ok = rows[i].work_n >= kMinBenchSamples;
    const uint32_t wake_thresh = WakeP99ThresholdNs(hz);
    {
      bool ok = wake_ok && rows[i].wake.p99 < wake_thresh;
      std::snprintf(buf, sizeof(buf), "%s p99_wake_err=%u ns < %u ns",
                    rows[i].name, rows[i].wake.p99, wake_thresh);
      PrintGate(buf, ok);
    }
    // work p99 < 20% of period
    if (hz > 0) {
      uint64_t period_ns = 1000000000ULL / hz;
      uint64_t work_thresh = period_ns / 5;  // 20%
      bool ok = work_ok && rows[i].work.p99 < work_thresh;
      std::snprintf(buf, sizeof(buf), "%s p99_work=%u ns < %llu ns (20%% period)",
                    rows[i].name, rows[i].work.p99,
                    static_cast<unsigned long long>(work_thresh));
      PrintGate(buf, ok);
    }
  }

  // pipeline p99 < 200e6 ns
  {
    bool ok = lat_ok && lat.p99 < 200000000u;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "pipeline p99_lat=%u ns < 200ms", lat.p99);
    PrintGate(buf, ok);
  }

  // drops == 0
  {
    bool ok = (total_drops == 0);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "total_drops=%llu == 0",
                  static_cast<unsigned long long>(total_drops));
    PrintGate(buf, ok);
  }

  std::fprintf(stderr, "=== end gates ===\n");
  std::fprintf(stderr, "total_drops=%llu lat_samples=%zu\n",
               static_cast<unsigned long long>(total_drops), ln);

  // Functional fail if any drops.
  if (total_drops > 0) {
    std::fprintf(stderr, "FAIL: total_drops=%llu (>0)\n",
                 static_cast<unsigned long long>(total_drops));
    return 1;
  }
  return 0;
}
