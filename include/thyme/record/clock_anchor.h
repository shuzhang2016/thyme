#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace thyme {
namespace triggered {

// A CLOCK_MONOTONIC / CLOCK_REALTIME pair persisted in a package WAL. Conversion
// uses the latest anchor at or before the target (the first anchor for earlier
// records) and carries the monotonic delta across without interpolation.
struct ClockAnchor {
  uint64_t mono_ns;
  uint64_t realtime_ns;
};

// Old packages without anchor rows return true with an empty list. Malformed
// headers, truncated rows, I/O errors and regressing anchor timestamps fail.
bool ReadPackageClockAnchors(const std::string& package_dir,
                             std::vector<ClockAnchor>* out);

bool MonoToRealtimeNs(const ClockAnchor& anchor, uint64_t mono_ns,
                      uint64_t* realtime_ns);
bool MonoToRealtimeNs(const std::vector<ClockAnchor>& anchors, uint64_t mono_ns,
                      uint64_t* realtime_ns);

}  // namespace triggered
}  // namespace thyme
