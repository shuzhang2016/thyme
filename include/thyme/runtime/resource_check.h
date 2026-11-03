#pragma once
// resource_check.h — OPS-07: startup resource self-check.
//
// A host verifies the hard host requirements thyme depends on *before* it
// starts threads, sockets and recorders:
//
//   RLIMIT_NOFILE   channels x files_per_channel + baseline descriptors
//   RLIMIT_MEMLOCK  only when mlockall is configured (THYME_NO_MLOCK unset)
//   free disk       the configured retention quota / crash reserve
//   /dev/shm free   the planned SHM regions (+ reserve)
//
// The check is a pure read: getrlimit()/statvfs(), no allocation, no
// exceptions, no file opens. A caller gets a POD Report and decides what to
// do with it (`fatal` blocks startup, `warnings` are logged counters).
//
// Severity policy (documented in docs/guides/runbook.md §7):
//   * NOFILE below the plan                       -> fatal (cannot open the
//     channels/files the config promises)
//   * /dev/shm free below the planned regions     -> fatal (Create() fails)
//   * free disk below the explicit reserve        -> fatal (not even crash
//     packages can be written)
//   * mlockall configured but RLIMIT_MEMLOCK low  -> warn (mlockall failure is
//     loud but non-fatal; see App::Prepare and PERF-04)
//   * free disk below the configured quota        -> warn (retention will
//     churn; the quota is a cap, not a requirement)
//   * /dev/shm free below planned + reserve       -> warn (headroom)
//   * a limit/filesystem that cannot be read      -> warn (unverifiable)
//
// Deployment knobs are environment variables (same precedent as
// THYME_NO_MLOCK / THYME_STATUS_SOCKET); PlanFromEnv() resolves them:
//   THYME_RESOURCE_CHECK=off|warn|enforce   (default enforce)
//   THYME_RESOURCE_BASELINE_FILES=<n>       (default 64)
//   THYME_RESOURCE_FILES_PER_CHANNEL=<n>    (default 2: .raw + .index.tbin)
//   THYME_RESOURCE_SHM_PLANNED_MB=<n>       (default 8 MiB * channels)
//   THYME_RESOURCE_SHM_RESERVE_MB=<n>       (default 16)
//   THYME_RESOURCE_MEMLOCK_HEADROOM_MB=<n>  (default 64)
//   THYME_RESOURCE_DISK_RESERVE_MB=<n>      (default: the caller's value,
//                                            e.g. [recorder].high_reserved_mb)

#include <cstddef>
#include <cstdint>

namespace thyme {
namespace resource {

enum class Level : uint8_t { kOk = 0, kWarn = 1, kFatal = 2 };

const char* LevelName(Level level) noexcept;

// Returns the more severe of the two levels.
Level Worst(Level a, Level b) noexcept;

// Inputs the host knows from its own configuration. Zero means "not
// configured / not checked" unless noted.
struct Limits {
  // RLIMIT_NOFILE. required = channels * files_per_channel + baseline_files.
  uint32_t channels = 0;
  uint32_t files_per_channel = 2;
  uint32_t baseline_files = 64;
  // RLIMIT_MEMLOCK. Checked only when mlock_configured is true.
  bool mlock_configured = false;
  uint64_t memlock_required_bytes = 0;
  // Free-space checks. A null path (or statvfs failure) is "unverifiable".
  const char* disk_path = nullptr;
  uint64_t disk_quota_bytes = 0;    // configured cap; free < quota = warn
  uint64_t disk_reserve_bytes = 0;  // hard floor; free < reserve = fatal
  const char* shm_path = "/dev/shm";
  uint64_t shm_planned_bytes = 0;   // regions this host will create
  uint64_t shm_reserve_bytes = 0;   // headroom above the plan
};

struct Report {
  Level nofile = Level::kOk;
  Level memlock = Level::kOk;
  Level disk = Level::kOk;
  Level shm = Level::kOk;

  uint64_t nofile_soft = 0;  // UINT64_MAX encodes RLIM_INFINITY
  uint64_t nofile_hard = 0;
  uint64_t nofile_required = 0;
  bool nofile_checked = false;

  bool memlock_configured = false;
  bool memlock_checked = false;
  uint64_t memlock_soft = 0;
  uint64_t memlock_hard = 0;
  uint64_t memlock_required = 0;

  bool disk_checked = false;
  uint64_t disk_free = 0;
  uint64_t disk_required = 0;  // the reserve that would be fatal
  uint64_t disk_quota = 0;

  bool shm_checked = false;
  uint64_t shm_free = 0;
  uint64_t shm_required = 0;  // plan + reserve (warn threshold)
  uint64_t shm_planned = 0;   // plan alone (fatal threshold)

  uint32_t warnings = 0;  // number of warn-level items
  bool fatal = false;     // any fatal-level item

  Level worst() const noexcept {
    return Worst(Worst(nofile, memlock), Worst(disk, shm));
  }
};

// Runs the check. `limits` is read-only for the duration of the call.
Report Check(const Limits& limits) noexcept;

// Resolved plan from the environment (see the knob list above). `channels`
// and the disk inputs come from the host's configuration.
struct Plan {
  Limits limits;
  bool enabled = true;
  bool enforce = true;
};

Plan PlanFromEnv(uint32_t channels, const char* disk_path,
                 uint64_t disk_quota_bytes, uint64_t disk_reserve_bytes,
                 const char* shm_path = "/dev/shm") noexcept;

// THYME_NO_MLOCK truthiness (1/true/yes/on, case-insensitive). Shared with
// App::Prepare so the check and mlockall never disagree about the opt-out.
bool MlockOptOut(const char* value) noexcept;
bool MlockConfiguredFromEnv() noexcept;

// Bounded, allocation-free renderings. FormatSummary is a single line for
// stderr/log sinks; FormatJson is one object for status/CI ingestion. Both
// return false when `cap` was too small (output is NUL-terminated and
// truncated).
bool FormatSummary(const Report& report, char* out, std::size_t cap) noexcept;
bool FormatJson(const Report& report, char* out, std::size_t cap) noexcept;

}  // namespace resource
}  // namespace thyme
