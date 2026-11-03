#pragma once

// retention.h — disk quota + retention for capture packages (design 6.4/6.5).
//
// Layout under out_dir:
//   <out>/<trigger_id>_<t0>/          low-priority packages
//   <out>/high/<trigger_id>_<t0>/     crash-level packages (exclusive subdir)
//
// Rules:
//   * total bytes over disk_quota_bytes -> delete OLDEST low-priority
//     packages until under quota; high/ is never touched by that sweep
//     (low-priority retention must not intrude into the reserved zone);
//   * high/ over high_reserved_bytes -> delete oldest crash packages inside;
//   * packages are identified as DIRECTORIES by mtime, never by file suffix
//     (design 6.4: avoid the class of bug where old ROS2 mcap packages went
//     stale);
//   * startup recovery: a valid non-terminal WAL is first committed to
//     aborted(crash_recovery), then renamed to <name>.incomplete; malformed
//     journals are still renamed fail-closed. .incomplete packages are
//     handled only by Config::incomplete_policy (kept by default: a partial
//     package is incident evidence), and the quota sweep never deletes them;
//   * the rename is followed by a best-effort PARTIAL
//     integrity.manifest over the surviving files (kind from the leaf name,
//     session id recovered from the wal header), so crash-leftover evidence
//     is verifiable like an engine-invalidated package.
//
// Hardening:
//   * lstat everywhere: symbolic links are never followed. A symlink met
//     during a walk is skipped and counted (symlinks_skipped()); RemoveTree
//     only unlinks the link itself and never deletes outside the directory;
//   * recursion depth cap kMaxTreeDepth: packages are flat directories, so
//     exceeding the cap counts as corruption/attack — DirBytes stops
//     descending and RemoveTree refuses to delete (returns false, package
//     kept);
//   * active-package exclusion is enforced at the API layer: Enforce requires
//     the caller to pass the active set explicitly (no default argument) and
//     re-checks the seal state internally — a directory that is not sealed
//     (PackageComplete()==false) and not marked .incomplete is treated as
//     being written and is never deleted even when absent from `exclude`.
//
// Instantiable, no global state; the host calls MarkIncomplete() once at
// startup and Enforce() after each capture + on a 5 min tick (6.4).

#include <cstdint>
#include <string>
#include <vector>

#include "thyme/record/package_integrity.h"

namespace thyme {
namespace retention {

// A package is a flat directory (pkg/<files>, at most one or two subdirectory
// levels); anything deeper counts as corruption or an attack.
constexpr uint32_t kMaxTreeDepth = 8;

// Policy for .incomplete packages. The quota sweep never deletes .incomplete;
// only this governs them.
enum class IncompletePolicy : uint8_t {
  kKeep = 0,              // safe default: never delete (incident evidence)
  kDeleteAfterAge = 1,    // delete only once older than incomplete_max_age_ns
  kDeleteImmediately = 2, // delete on Enforce (ignore quota pressure)
};

// Config string -> policy ("keep" / "delete_after_age" / "delete_immediately").
bool ParseIncompletePolicy(const std::string& s, IncompletePolicy* out);
const char* IncompletePolicyName(IncompletePolicy p);

struct Config {
  std::string out_dir;
  uint64_t disk_quota_bytes = 0;      // 0 = unlimited
  std::string high_dir = "high";      // crash subdir name under out_dir
  uint64_t high_reserved_bytes = 0;   // high/'s own reservation (0 = never clean high/)
  IncompletePolicy incomplete_policy = IncompletePolicy::kKeep;
  uint64_t incomplete_max_age_ns = 0;  // only used by kDeleteAfterAge
  // REC-18: when set (the deployment signs its packages), partial manifests
  // written by MarkIncomplete are HMAC-signed with the same key. Empty =
  // unsigned partial manifests.
  std::string signing_key_path;
  integrity::PackageInfo manifest_policy;
};

struct PackageInfo {
  std::string dir;      // absolute path
  uint64_t bytes = 0;   // recursive size
  int64_t mtime_ns = 0;
  bool high = false;        // inside the high/ zone
  bool incomplete = false;  // name carries the .incomplete suffix
};

class Manager {
 public:
  bool Init(const Config& cfg);

  // All packages (low zone + high/ zone), oldest first.
  std::vector<PackageInfo> Scan() const;
  // Startup recovery: rename unsealed packages to <name>.incomplete and
  // write a best-effort partial manifest over each renamed package (REC-18).
  // Returns how many were marked.
  uint32_t MarkIncomplete();
  // Quota enforcement (see header comment). `exclude` is the active set
  // declared by the host (in-flight triggered packages + active continuous
  // slices) and is mandatory — an empty list is allowed, but the internal seal
  // re-check still backs it up: a directory that is not sealed and not marked
  // .incomplete is always skipped. Returns freed bytes.
  uint64_t Enforce(const std::vector<std::string>& exclude);

  // Number of symbolic links skipped during walks (cumulative, for alerts).
  uint64_t symlinks_skipped() const { return symlinks_skipped_; }

  static uint64_t DirBytes(const std::string& dir);
  static bool RemoveTree(const std::string& dir);

 private:
  std::vector<PackageInfo> ScanZone(const std::string& zone_dir,
                                    bool high) const;
  Config cfg_;
  bool ok_ = false;
  mutable uint64_t symlinks_skipped_ = 0;
};

}  // namespace retention
}  // namespace thyme
