#include "dummy/lab/join_metrics.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dummy::lab {
namespace {

uint32_t Percentile(const uint32_t* sorted, std::size_t n, double q) {
  if (n == 0) return 0;
  const std::size_t idx =
      std::min<std::size_t>(n - 1, static_cast<std::size_t>(n * q));
  return sorted[idx];
}

}  // namespace

JoinMetricNode ComputeJoinMetric(const char* name, const uint32_t* samples,
                                 std::size_t count) {
  JoinMetricNode out{name, count, 0, 0, 0, 0};
  if (count == 0) return out;
  uint32_t scratch[4096];
  const std::size_t n = std::min(count, std::size(scratch));
  std::copy(samples, samples + n, scratch);
  std::sort(scratch, scratch + n);
  out.p50 = Percentile(scratch, n, 0.50);
  out.p95 = Percentile(scratch, n, 0.95);
  out.p99 = Percentile(scratch, n, 0.99);
  out.max = scratch[n - 1];
  out.n = n;
  return out;
}

void ExportJoinMetrics(const JoinMetricNode* nodes, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    const JoinMetricNode& m = nodes[i];
    if (m.n == 0 || m.name == nullptr) continue;
    std::printf(
        "METRICS {\"name\":\"%s\",\"n\":%zu,\"p50\":%u,\"p95\":%u,"
        "\"p99\":%u,\"max\":%u}\n",
        m.name, m.n, m.p50, m.p95, m.p99, m.max);
  }
  std::fflush(stdout);
}

std::vector<ParsedJoinMetric> ParseMetricsLog(const char* path) {
  std::vector<ParsedJoinMetric> out;
  std::FILE* f = std::fopen(path, "r");
  if (f == nullptr) return out;
  char line[512];
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    if (std::strncmp(line, "METRICS ", 8) != 0) continue;
    ParsedJoinMetric m{};
    char nm[64];
    unsigned long long n = 0, p50 = 0, p95 = 0, p99 = 0, mx = 0;
    if (std::sscanf(line + 8,
                    " {\"name\":\"%63[^\"]\",\"n\":%llu,\"p50\":%llu,"
                    "\"p95\":%llu,\"p99\":%llu,\"max\":%llu}",
                    nm, &n, &p50, &p95, &p99, &mx) != 6) {
      continue;
    }
    std::snprintf(m.name, sizeof(m.name), "%s", nm);
    m.n = n;
    m.p50 = p50;
    m.p95 = p95;
    m.p99 = p99;
    m.max = mx;
    out.push_back(std::move(m));
  }
  std::fclose(f);
  return out;
}

}  // namespace dummy::lab
