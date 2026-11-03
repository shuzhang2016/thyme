#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "thyme/replay/replay_reader.h"

namespace thyme {
namespace replay {

struct ReplayTimelineSlice {
  std::string dir;
  uint64_t slice_seq = 0;  // WAL continuous sequence, or first_mono_ns fallback
  uint64_t first_mono_ns = 0;
  uint64_t last_mono_ns = 0;
  uint64_t timeline_first_ns = 0;
  uint64_t timeline_last_ns = 0;
  uint64_t sample_period_ns = 0;
  int64_t offset_ns = 0;
  bool has_events = false;
};

ReplayStatus BuildReplayStitchPlan(std::vector<ReplayTimelineSlice>* slices,
                                   std::string* error);
bool ShiftReplayTimestamp(uint64_t source_ns, int64_t offset_ns,
                          uint64_t* timeline_ns);
bool UnshiftReplayTimestamp(uint64_t timeline_ns, int64_t offset_ns,
                            uint64_t* source_ns);

}  // namespace replay
}  // namespace thyme
