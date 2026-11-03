#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace thyme {

// OPS-02: safe reclamation of stale POSIX-shm segments.
//
// thyme SHM names embed the owning session's pid (MpSupervisorConfig::
// shm_prefix_fmt), so after a crash the /dev/shm nodes can outlive every
// process that knew about them. Reclaiming them must never pull the object
// out from under a live peer: a segment is only unlinked when its liveness
// can be *proved* absent. Anything else is reported as kUnknown /
// kInconclusive and left alone.
//
// Proof strategy (Linux):
//   1. Scan /proc/<pid>/fd for an fd whose target is /dev/shm/<name>, and
//      /proc/<pid>/maps for a live mapping (a mapping can outlive its fd).
//      A hit is kLive. An unreadable /proc entry makes the scan incomplete.
//   2. Take an exclusive file lease (F_SETLEASE / F_WRLCK) on the segment.
//      The kernel grants it only when no other process holds the object
//      open or mapped; success is conclusive proof of staleness, EAGAIN is
//      a live holder. If leases are unavailable (e.g. non-Linux), the
//      /proc verdict is used only when that scan was complete.
//   3. Refuse to touch a segment not owned by the current euid (a crashed
//      root run leaves objects only root can safely reclaim).
//
// Residual race: a brand-new opener can still enter between the liveness
// proof and shm_unlink (POSIX shm has no unlink-if-ino). The window is one
// syscall wide and the lease is held across the unlink; document it, do not
// pretend it is atomic.

// Liveness verdict for an existing POSIX shm object name (leading '/', as
// passed to shm_open).
enum class ShmLiveness : uint8_t {
  kAbsent = 0,  // no /dev/shm node with this name
  kStale,       // proved: no live process holds it open or mapped
  kLive,        // a live process holds it open or mapped
  kUnknown,     // not proved either way; callers must leave it alone
};

// Outcome of SweepStaleShmSegment().
enum class ShmSweepResult : uint8_t {
  kSwept = 0,     // stale segment was unlinked
  kNotPresent,    // nothing was there
  kLive,          // left in place: a live process holds it
  kInconclusive,  // left in place: liveness could not be proved absent
};

// Per-call counters (all start at zero; the callee increments).
struct ShmSweepStats {
  uint64_t examined = 0;
  uint64_t swept = 0;
  uint64_t skipped_live = 0;
  uint64_t inconclusive = 0;
};

// Process-wide counters for observability/tests. Lock-free atomics.
struct ShmSweepCounters {
  std::atomic<uint64_t> examined{0};
  std::atomic<uint64_t> swept{0};
  std::atomic<uint64_t> skipped_live{0};
  std::atomic<uint64_t> inconclusive{0};
};

ShmSweepCounters& ShmSweepGlobalCounters();

// Non-destructive liveness probe. The name must be the shm_open name
// ("/thyme_mp_123_c"); invalid names report kUnknown.
ShmLiveness ProbeShmSegmentLiveness(const char* name);

// Probe and, only when the result is kStale, unlink the segment. `stats` (if
// non-null) is incremented in place; the process-wide counters are always
// incremented. The return value distinguishes "swept" from every refusal.
// Never unlinks a segment whose liveness is not proved.
ShmSweepResult SweepStaleShmSegment(const char* name,
                                    ShmSweepStats* stats = nullptr);

const char* ShmLivenessName(ShmLiveness v);
const char* ShmSweepResultName(ShmSweepResult r);

}  // namespace thyme
