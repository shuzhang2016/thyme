#include "thyme/replay/replay_timeline.h"

#include <limits>

namespace thyme {
namespace replay {

bool ShiftReplayTimestamp(uint64_t source_ns, int64_t offset_ns,
                          uint64_t* timeline_ns) {
  if (offset_ns >= 0) {
    const uint64_t offset = static_cast<uint64_t>(offset_ns);
    if (source_ns > std::numeric_limits<uint64_t>::max() - offset) {
      return false;
    }
    *timeline_ns = source_ns + offset;
    return true;
  }
  const uint64_t offset = static_cast<uint64_t>(-(offset_ns + 1)) + 1;
  if (source_ns < offset) {
    return false;
  }
  *timeline_ns = source_ns - offset;
  return true;
}

bool UnshiftReplayTimestamp(uint64_t timeline_ns, int64_t offset_ns,
                            uint64_t* source_ns) {
  if (offset_ns == std::numeric_limits<int64_t>::min()) {
    const uint64_t offset = uint64_t{1} << 63;
    if (timeline_ns > std::numeric_limits<uint64_t>::max() - offset) {
      return false;
    }
    *source_ns = timeline_ns + offset;
    return true;
  }
  return ShiftReplayTimestamp(timeline_ns, -offset_ns, source_ns);
}

namespace {

bool MakeOffset(uint64_t source_ns, uint64_t target_ns, int64_t* offset_ns) {
  if (target_ns >= source_ns) {
    const uint64_t delta = target_ns - source_ns;
    if (delta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return false;
    }
    *offset_ns = static_cast<int64_t>(delta);
    return true;
  }
  const uint64_t delta = source_ns - target_ns;
  const uint64_t min_magnitude = uint64_t{1} << 63;
  if (delta > min_magnitude) {
    return false;
  }
  *offset_ns = delta == min_magnitude ? std::numeric_limits<int64_t>::min()
                                      : -static_cast<int64_t>(delta);
  return true;
}

}  // namespace

ReplayStatus BuildReplayStitchPlan(std::vector<ReplayTimelineSlice>* slices,
                                   std::string* error) {
  if (slices == nullptr || slices->empty()) {
    if (error != nullptr) {
      *error = "stitch plan is empty";
    }
    return ReplayStatus::kInvalidArgument;
  }
  uint64_t previous_period_ns = 1;
  uint64_t previous_last_ns = 0;
  bool have_previous = false;
  for (std::size_t i = 0; i < slices->size(); ++i) {
    ReplayTimelineSlice& slice = (*slices)[i];
    ReplayPackage pkg;
    const ReplayStatus open_status = pkg.Open(slice.dir);
    if (open_status != ReplayStatus::kOk) {
      if (error != nullptr) {
        *error =
            "stitch preflight failed for " + slice.dir + ": " + pkg.error();
      }
      return open_status;
    }
    slice.first_mono_ns = pkg.first_mono_ns();
    slice.last_mono_ns = pkg.last_mono_ns();
    slice.has_events = false;
    for (const ReplayChannel& channel : pkg.channels()) {
      slice.has_events = slice.has_events || channel.record_count != 0;
    }
    uint64_t period_ns = 0;
    bool have_data = false;
    uint64_t previous_data_ns = 0;
    ReplayEvent event;
    ReplayStatus status = ReplayStatus::kOk;
    while ((status = pkg.NextEvent(&event)) == ReplayStatus::kOk) {
      if (event.kind != ReplayKind::kData) {
        continue;
      }
      if (have_data && event.mono_ns > previous_data_ns) {
        const uint64_t delta = event.mono_ns - previous_data_ns;
        if (period_ns == 0 || delta < period_ns) {
          period_ns = delta;
        }
      }
      previous_data_ns = event.mono_ns;
      have_data = true;
    }
    if (status != ReplayStatus::kEnd) {
      if (error != nullptr) {
        *error = "stitch scan failed for " + slice.dir + ": " + pkg.error();
      }
      return status;
    }
    slice.sample_period_ns = period_ns != 0 ? period_ns : previous_period_ns;
    if (period_ns != 0) {
      previous_period_ns = period_ns;
    }
    slice.offset_ns = 0;
    slice.timeline_first_ns = slice.first_mono_ns;
    slice.timeline_last_ns = slice.last_mono_ns;
    if (!slice.has_events) {
      continue;
    }
    if (have_previous) {
      if (previous_last_ns >
          std::numeric_limits<uint64_t>::max() - slice.sample_period_ns) {
        if (error != nullptr) {
          *error = "stitch timestamp overflow at slice " + std::to_string(i);
        }
        return ReplayStatus::kInvalidArgument;
      }
      const uint64_t target_ns = previous_last_ns + slice.sample_period_ns;
      if (!MakeOffset(slice.first_mono_ns, target_ns, &slice.offset_ns) ||
          !ShiftReplayTimestamp(slice.first_mono_ns, slice.offset_ns,
                                &slice.timeline_first_ns) ||
          !ShiftReplayTimestamp(slice.last_mono_ns, slice.offset_ns,
                                &slice.timeline_last_ns)) {
        if (error != nullptr) {
          *error = "stitch offset overflow at slice " + std::to_string(i);
        }
        return ReplayStatus::kInvalidArgument;
      }
    }
    previous_last_ns = slice.timeline_last_ns;
    have_previous = true;
  }
  return ReplayStatus::kOk;
}

}  // namespace replay
}  // namespace thyme
