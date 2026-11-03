#pragma once

#include "generated/wiring.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

struct ReplayOptions {
  double rate = 1.0;
  bool as_fast = false;
  bool inject_all = false;
  std::vector<std::string> include_topics;
  std::vector<std::string> exclude_topics;
  uint64_t from_mono_ns = 0;
  uint64_t to_mono_ns = 0;
  const char* inject_stage = nullptr;
};

struct ReplayStats {
  std::size_t entries_read = 0;
  std::size_t entries_injected = 0;
  std::size_t entries_skipped = 0;
  std::size_t inject_failures = 0;
};

// Blocks until all scheduled entries are injected or thyme::App::stop_requested().
ReplayStats RunReplay(Pipeline& pipeline, const char* bin_path,
                      const ReplayOptions& opt);

// Scales a recording-time delta by the replay rate. The result is clamped to
// the int64 range: an unclamped double->int64 conversion of a huge
// delta_ns / rate overflows and wraps negative, which made timed replay
// inject immediately instead of waiting.
inline int64_t ReplayScaledDelayNs(uint64_t delta_ns, double rate) {
  if (!(rate > 0.0)) rate = 1.0;
  const double scaled = static_cast<double>(delta_ns) / rate;
  constexpr double kMax =
      static_cast<double>(std::numeric_limits<int64_t>::max());
  if (scaled >= kMax) return std::numeric_limits<int64_t>::max();
  return static_cast<int64_t>(scaled);
}
