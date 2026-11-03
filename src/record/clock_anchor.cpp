#include "thyme/record/clock_anchor.h"

#include "thyme/record/file_ptr.h"
#include "thyme/record/triggered_recorder.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/stat.h>

namespace thyme {
namespace triggered {

bool ReadPackageClockAnchors(const std::string& package_dir,
                             std::vector<ClockAnchor>* out) {
  if (out == nullptr) return false;
  out->clear();
  FilePtr f(std::fopen((package_dir + "/wal.log").c_str(), "rb"));
  if (!f) return false;
  struct stat st {};
  if (fstat(fileno(f.get()), &st) != 0 ||
      st.st_size < static_cast<off_t>(sizeof(WalHeader)) ||
      (static_cast<uint64_t>(st.st_size) - sizeof(WalHeader)) %
              sizeof(WalRow) !=
          0) {
    return false;
  }
  WalHeader header{};
  if (std::fread(&header, sizeof(header), 1, f.get()) != 1 ||
      std::memcmp(header.magic, kWalMagic, sizeof(kWalMagic)) != 0) {
    return false;
  }
  WalRow row{};
  while (std::fread(&row, sizeof(row), 1, f.get()) == 1) {
    if (row.op != kWalOpClockAnchor) continue;
    if (row.result != kWalOk || row.struct_id != 0 || row.len != 0 ||
        (!out->empty() && row.index < out->back().mono_ns)) {
      out->clear();
      return false;
    }
    out->push_back(ClockAnchor{row.index, row.mono_ns});
  }
  if (std::ferror(f.get()) != 0) {
    out->clear();
    return false;
  }
  return true;
}

bool MonoToRealtimeNs(const ClockAnchor& anchor, uint64_t mono_ns,
                      uint64_t* realtime_ns) {
  if (realtime_ns == nullptr) return false;
  if (mono_ns >= anchor.mono_ns) {
    const uint64_t delta = mono_ns - anchor.mono_ns;
    if (anchor.realtime_ns > std::numeric_limits<uint64_t>::max() - delta) {
      return false;
    }
    *realtime_ns = anchor.realtime_ns + delta;
  } else {
    const uint64_t delta = anchor.mono_ns - mono_ns;
    if (anchor.realtime_ns < delta) return false;
    *realtime_ns = anchor.realtime_ns - delta;
  }
  return true;
}

bool MonoToRealtimeNs(const std::vector<ClockAnchor>& anchors, uint64_t mono_ns,
                      uint64_t* realtime_ns) {
  if (anchors.empty()) return false;
  const ClockAnchor* selected = &anchors.front();
  for (const ClockAnchor& anchor : anchors) {
    if (anchor.mono_ns > mono_ns) break;
    selected = &anchor;
  }
  return MonoToRealtimeNs(*selected, mono_ns, realtime_ns);
}

}  // namespace triggered
}  // namespace thyme
