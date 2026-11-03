#pragma once

// ARCH-14 test helper: a TriggeredRecorder keeps its session-lease page after
// Shutdown on purpose, so a restarted recorder can reattach the same page.
// Production uses one fixed out_dir, so the page is reused; tests use
// pid-scoped out_dirs, which would leave one `/dev/shm/tsess_*` page per run
// and trip check_shm_leaks. Tests that capture into a temporary tree must
// unlink the lease for every out_dir they used before removing the tree.
#include "thyme/runtime/session_lease.h"
#include "thyme/runtime/shm_region.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace thyme_test {

inline void UnlinkSessionLeaseFor(const std::string& out_dir) {
  char name[64];
  if (thyme::SessionLeaseNameForRoot(out_dir.c_str(), name, sizeof(name))) {
    thyme::ShmRegion::UnlinkName(name);
  }
}

// Walks the temporary tree and unlinks the lease of every directory (each
// capture used its own out_dir, so each has its own page name).
inline void UnlinkSessionLeasesUnder(const std::filesystem::path& root) {
  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) return;
  UnlinkSessionLeaseFor(root.string());
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(root, ec)) {
    if (entry.is_directory(ec)) UnlinkSessionLeaseFor(entry.path().string());
  }
}

}  // namespace thyme_test
