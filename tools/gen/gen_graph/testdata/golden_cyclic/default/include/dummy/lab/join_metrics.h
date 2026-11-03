#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dummy::lab {

struct JoinMetricNode {
  const char* name = nullptr;
  std::size_t n = 0;
  uint32_t p50 = 0;
  uint32_t p95 = 0;
  uint32_t p99 = 0;
  uint32_t max = 0;
};

// Compute p50/p95/p99/max from latency samples (ns). Empty -> zeros.
JoinMetricNode ComputeJoinMetric(const char* name, const uint32_t* samples,
                                 std::size_t count);

// Lab-only: one METRICS JSON object per node on stdout when enabled by caller.
void ExportJoinMetrics(const JoinMetricNode* nodes, std::size_t count);

struct ParsedJoinMetric {
  char name[64]{};
  uint64_t n = 0;
  uint64_t p50 = 0;
  uint64_t p95 = 0;
  uint64_t p99 = 0;
  uint64_t max = 0;
};

// Parse METRICS lines from a log file (eval_chains supervisor capture).
std::vector<ParsedJoinMetric> ParseMetricsLog(const char* path);

}  // namespace dummy::lab
