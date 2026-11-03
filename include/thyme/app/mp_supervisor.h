#pragma once
#include "thyme/app/mp_control.h"
#include "thyme/app/mp_shm_kind.h"
#include "thyme/app/span.h"
#include "thyme/app/status_server.h"
#include "thyme/app/time.h"
#include "thyme/record/record_table.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shared_health.h"
#include "thyme/runtime/resource_check.h"
#include "thyme/runtime/shm_region.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <ctime>

namespace thyme {
namespace app {

// Table-driven graph descriptors; the generated graph_ir.h emits the
// per-graph arrays and the project main builds a config from them.
struct MpProcess {
  const char* id;
  const char* role;
};
struct MpNode {
  const char* id;
  const char* type;
  const char* process;
  int hz;
};
struct MpShmChannel {
  const char* suffix;
  int kind;  // index into MpSupervisorConfig::slot_specs (generated enum)
  const char* from_process;
  const char* to_process;
};
struct MpRecordGate {
  const char* process;
  uint16_t slot;
};
// Cross-process frame gate (S-D4): the shm kind `shm_kind` must appear on at
// least one cross-process channel, and MpControl::cross_frames[counter_slot]
// must reach >= 1 by end of run (the consumer role increments the slot per
// received frame). gen_graph emits the table from graph.yaml edges annotated
// `mp_gate: true`; counter_slot indexes MpControl::cross_frames.
struct MpCrossGate {
  int shm_kind;
  uint32_t counter_slot;
};

struct MpSupervisorConfig {
  thyme::span<const MpProcess> processes;
  thyme::span<const MpNode> nodes;
  thyme::span<const MpShmChannel> shm_channels;
  thyme::span<const MpRecordGate> record_gates;
  thyme::span<const MpShmKindSpec> slot_specs;  // per SHM kind index; generated from LatestSlotLayoutModel<T>
  int health_n = 0;
  const char* shm_prefix_fmt = "/c9_%d";
  // Cross-process frame gates: every entry requires its shm kind on a
  // cross-process channel AND cross_frames[counter_slot] >= 1 by end of run.
  // Empty disables the check. A gate whose kind never appears cross-process
  // fails the run — a misconfigured gate must not silently pass.
  thyme::span<const MpCrossGate> cross_gates;
  // Record mirror table; only consulted when Run(record=true).
  const thyme::ThymeRecordTable* record_table = nullptr;
  // Non-null opts this session into the separate RecordMirrorRingPage v4 ABI.
  // Null preserves the legacy RecordMirrorPage v3 path for existing embedders.
  const thyme::RecordMirrorRingSpec* record_ring_specs = nullptr;
  // M4.5: when true, Run fails if ScanHealthPage reports drops or errors.
  bool health_scan_strict = false;
  // ARCH-17: optional graph identity written to `<prefix>_id`. 0 = unknown;
  // roles then skip the graph comparison but still check config_hash/health_n.
  uint64_t graph_hash = 0;
  // Optional per-role executable resolver for SDK isolation. It receives ctx,
  // role, and the default self_exe, and must return a stable executable path.
  // Null preserves the single-binary behavior.
  const char* (*role_executable)(void* ctx, const char* role,
                                 const char* self_exe) = nullptr;
  void* role_executable_ctx = nullptr;

  // ---- OPS-01 restart policy ---------------------------------------------
  // Defaults preserve the pre-OPS-01 behaviour: a role that exits or stalls
  // is *not* restarted (the run's success gate then fails). Opt in with
  // restart_enabled; all timings are CLOCK_MONOTONIC nanoseconds.
  bool restart_enabled = false;
  // Restart budget: at most max_restarts respawns per restart_window_ns. When
  // the budget is exhausted the role enters kGaveUp and is never respawned
  // again (the supervisor stays up and reports it). restart_window_ns == 0
  // means the budget is for the whole run (no window reset).
  uint32_t max_restarts = 3;
  uint64_t restart_window_ns = 60ULL * 1000 * 1000 * 1000;
  // Exponential backoff between respawns of the same role, clamped to
  // [kMinRestartIntervalNs, restart_backoff_max_ns]. A successful restart
  // (the new child is observed making progress) resets it to the initial
  // value; a child that dies immediately still waits the full backoff.
  uint64_t restart_backoff_initial_ns = 100ULL * 1000 * 1000;
  uint64_t restart_backoff_max_ns = 5ULL * 1000 * 1000 * 1000;
  uint32_t restart_backoff_multiplier = 2;
  // Stall policy: SIGTERM, then SIGKILL after this grace period when the
  // role does not exit. restart_stall_timeout_ns == 0 disables stall
  // detection (exit-status restarts still work). A role is stalled when
  // neither its health heartbeat nor its MpControl::proc_loops counter has
  // advanced for longer than the timeout.
  uint64_t restart_kill_grace_ns = 500ULL * 1000 * 1000;
  uint64_t restart_stall_timeout_ns = 0;

  // ---- ARCH-03 abort-path child wait --------------------------------------
  // When ForkRoles fails part-way, already-forked roles are stopped and
  // reaped with this deadline before SIGKILL. Host operational policy: how
  // long a role may take to exit is a deployment property, not a supervisor
  // mechanism.
  uint64_t shutdown_timeout_ns = kMpWaitTimeoutNs;

  // ---- OPS-10 status endpoint --------------------------------------------
  // Read-only JSON over a UNIX stream socket. nullptr consults the
  // THYME_STATUS_SOCKET environment variable (unset/empty = disabled);
  // "none" also disables it. A non-empty value overrides the environment.
  // Prepare() starts it and fails closed when a requested path cannot be
  // bound; Cleanup()/the destructor stop it and unlink the socket.
  const char* status_socket_path = nullptr;
};

// Smallest spacing between respawns of one role, even when the policy asks for
// a zero backoff. Guarantees a role that dies immediately cannot spin the
// supervisor.
inline constexpr uint64_t kMinRestartIntervalNs = 1ULL * 1000 * 1000;

// OPS-01 role lifecycle. kRunning/kTerminating/kWaitingBackoff are the only
// transient states; kGaveUp and kStopped are terminal for the session.
enum class MpRoleState : uint8_t {
  kNotStarted = 0,
  kRunning,         // child forked and not yet reaped
  kTerminating,     // stall detected, SIGTERM sent, SIGKILL after grace
  kWaitingBackoff,  // child reaped, waiting for the next restart slot
  kGaveUp,          // restart budget exhausted; never respawned again
  kStopped,         // exited with restart disabled, or after stop was asked
};

// Read-only per-role restart observability.
struct MpRoleRestartInfo {
  MpRoleState state = MpRoleState::kNotStarted;
  pid_t pid = 0;
  uint32_t restarts_attempted = 0;
  // Restarts after which the new child was observed making progress
  // (heartbeat or proc_loops advanced). Attempts whose child dies before
  // reporting anything are not successes.
  uint32_t restarts_succeeded = 0;
  int last_exit_code = -1;   // WEXITSTATUS; -1 if signaled or never reaped
  int last_term_signal = 0;  // WTERMSIG; 0 if exited normally or never reaped
  uint64_t last_stall_age_ns = 0;  // last observed heartbeat/loop age
  uint64_t last_restart_ns = 0;    // CLOCK_MONOTONIC time of the last respawn
};

// Snapshot of the OPS-01 counters. Plain POD so it can be copied/logged
// without allocation; index order matches cfg.processes.
struct MpRestartStats {
  uint64_t restarts_attempted = 0;
  uint64_t restarts_succeeded = 0;
  uint64_t restart_giveups = 0;
  uint32_t roles = 0;
  MpRoleRestartInfo role[MpControl::kMaxProcSlots]{};
};

// Aggregated worker health after a run (excludes health_reporter nodes).
struct MpHealthScanSummary {
  std::size_t workers_scanned = 0;
  // RT-23: 64-bit so a summed drop count cannot wrap/truncate at 2^32.
  uint64_t total_drops = 0;
  uint32_t total_deadline_miss = 0;
  uint32_t total_errors = 0;
};

// Sum drop/deadline_miss/error_code over graph worker slots (index aligns with
// cfg.nodes / SharedHealthPage::slots). health_reporter nodes are skipped.
MpHealthScanSummary ScanHealthPage(const MpSupervisorConfig& cfg,
                                   const thyme::SharedHealthPage& page);

// Strict gates for supervisor success (deadline_miss is warn-only unless
// strict_deadline_miss). Used by Run when health_scan_strict is set.
bool HealthScanOk(const MpHealthScanSummary& summary, bool strict_drops,
                  bool strict_errors, bool strict_deadline_miss);

// Success gate for the per-process loop counts. For every process i,
// proc_loops[i] must meet that process's own bound (its slowest worker hz *
// seconds / 4). proc_loops[i] aligns with cfg.processes[i]; the supervisor
// reads the per-process counters out of MpControl::proc_loops.
bool LoopsOk(const MpSupervisorConfig& cfg, int seconds,
             thyme::span<const uint64_t> proc_loops);

// Success gate for the cross-process frame counters (S-D4). For every
// configured gate, cross_frames[gate.counter_slot] must be >= 1 AND the
// gate's shm kind must appear on at least one cross-process channel; a gate
// whose kind never crosses processes (or whose slot is out of range) is a
// misconfiguration and must not silently pass. Empty gate table passes.
bool CrossGatesOk(const MpSupervisorConfig& cfg,
                  thyme::span<const uint64_t> cross_frames);

// waitpid with a deadline. Returns the waitpid status (same shape as the
// supervisor's blocking wait), or -1 on waitpid error. If the child has not
// exited within timeout_ns it is SIGKILLed and the wait continues, so a stuck
// child can never deadlock the supervisor; the kill fallback is reported
// through timed_out.
inline int WaitChildTimeout(pid_t pid, uint64_t timeout_ns,
                            bool* timed_out = nullptr) {
  if (timed_out != nullptr) *timed_out = false;
  int status = 0;
  const uint64_t deadline = NowMonoNs() + timeout_ns;
  for (;;) {
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) return status;
    if (r == -1) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (NowMonoNs() >= deadline) break;
    struct timespec req{0, 10L * 1000 * 1000};
    nanosleep(&req, nullptr);
  }
  if (timed_out != nullptr) *timed_out = true;
  (void)kill(pid, SIGKILL);  // ESRCH: exited between deadline check and kill.
  for (;;) {
    const pid_t r = waitpid(pid, &status, 0);
    if (r == pid) return status;
    if (r == -1 && errno == EINTR) continue;
    return -1;
  }
}

// Supervisor *skeleton* for a table-driven multi-process graph — the "how":
// SHM set create/attach (health page + control + per-channel LatestSlot
// regions), the optional record mirror, role fork/exec helpers, cooperative
// stop, child reap with timeout, post-reap counter/health collection, and
// (OPS-01) supervisor-driven restart with backoff and a give-up state.
//
// The "what" (run duration, trigger-relay polling, success-gate composition,
// the printf report, and the exit-code policy) lives in the app-side playbook
// that composes this session (see the generated scaffold's
// src/mp/mp_supervisor_adapter.cpp). thyme itself carries no demo-specific
// exit policy.
/** \brief Supervisor session for a multi-process graph: spawn roles, monitor
 *  health/stall/restart, and reap children.
 *  \note The session owns the child processes and the SHM regions it created
 *        for the run; a caller that supplied a pre-existing SHM layout keeps
 *        owning that memory. The session must be stopped and reaped before
 *        its config references go out of scope. */
class MpSupervisorSession {
 public:
  explicit MpSupervisorSession(const MpSupervisorConfig& cfg);
  ~MpSupervisorSession();

  // App::Prepare + InstallStopSignals, then computes the shm prefix, creates
  // the record mirror (when record_shm_or_null != nullptr) and the SHM set.
  // record_shm_or_null: nullptr = no recording; "" = default mirror name;
  // otherwise the exact mirror name. Returns false on failure; any partially
  // created mirror/SHM is cleaned up and the session must not be reused.
  bool Prepare(const char* record_shm_or_null);

  // Forks one child per configured process and execs self_exe with
  // --role / --shm-prefix / --proc-idx [--record-shm]. Returns false if the
  // process table overflows or a fork fails; already-forked children are
  // stopped and reaped (WaitChildTimeout) so a failure never leaks orphans.
  // The executable path is remembered (fixed-size copy) for respawns.
  // OPS-03: on success, sends READY=1 to systemd when THYME_SD_NOTIFY is on
  // and NOTIFY_SOCKET is set (see thyme/app/systemd_notify.h).
  bool ForkRoles(const char* self_exe);

  // OPS-01 supervisor tick. Reaps exited children (waitpid/WNOHANG), detects
  // stalls from the health heartbeat or proc_loops progress, sends SIGTERM to
  // a stalled role (SIGKILL after restart_kill_grace_ns), and respawns dead
  // roles subject to the restart policy/backoff. Respawning stops as soon as
  // SignalStop()/ctrl->stop is set. Call it from the supervisor's existing
  // poll loop (a few ms to a few tens of ms) — it allocates nothing and never
  // blocks on a child. now_ns == 0 uses NowMonoNs().
  // OPS-03: while THYME_SD_NOTIFY is on, this tick also sends WATCHDOG=1 to
  // systemd at most once per second. A deployment that stops polling therefore
  // stops feeding WatchdogSec (that is the point: a hung supervisor is
  // restarted by systemd, while role-level stalls are still handled here).
  void PollRestarts(uint64_t now_ns = 0);

  // OPS-01 read-only snapshot of restart counters and per-role state.
  MpRestartStats restart_stats() const;

  // OPS-10 read-only status endpoint. StartStatusServer(nullptr) honours
  // MpSupervisorConfig::status_socket_path / THYME_STATUS_SOCKET; a nullptr
  // with neither set disables the endpoint (returns true). Idempotent while
  // running (returns true). PublishStatus() refreshes the snapshot from the
  // current role/health state; PollRestarts() calls it every tick when the
  // server is running, so embedders rarely need to call it directly.
  bool StartStatusServer(const char* socket_path = nullptr);
  void StopStatusServer();
  void PublishStatus(uint64_t now_ns = 0);
  StatusServer* status_server() { return &status_; }

  // The shared control page (for the app's poll/relay loop and stop signal).
  MpControl* ctrl();

  // Cooperative stop: ctrl->stop = 1. Also latches the session's stopping
  // flag so a later PollRestarts never respawns.
  void SignalStop();

  // Reaps every remaining child (wait_child/child_ok) and returns whether all
  // final children exited 0. Roles that already gave up or are waiting for a
  // respawn count as failure; a role already reaped by PollRestarts is judged
  // by the recorded exit status.
  bool ReapChildren();

  // Post-reap counter reads into caller-provided arrays. proc_loops_out must
  // hold kMaxProcSlots entries; cross_frames_out kMaxCrossFrameSlots.
  void Collect(uint64_t* proc_loops_out, uint64_t* cross_frames_out);

  // The health page (may be null if creation failed).
  const thyme::SharedHealthPage* health_page();

  // Mirror name used when recording ("" otherwise).
  const char* mirror_name() const;

  // OPS-02: FNV-1a identity of the config that produced this SHM set (0
  // before Prepare succeeds). Registered process-locally for attach-time
  // comparison; it is NOT stored in the SHM wire format.
  uint64_t config_hash() const { return impl_.config_hash; }

  // ARCH-05: status snapshots skipped because the status thread was copying
  // the active slot. The next tick republishes, so a skip loses no state, but
  // a persistently skipped endpoint is now countable instead of invisible.
  uint64_t status_publish_skips() const {
    return impl_.status_publish_skips;
  }

  // Unmaps + unlinks the SHM set, and shuts down the mirror (when recording).
  void Cleanup();

  const MpSupervisorConfig& config() const { return cfg_; }

 private:
  // Supervisor-side SHM set: _h (SharedHealthPage) and _c (MpControl) are
  // always created; one LatestSlot region per cross-process channel entry.
  // Fixed-size inline member — the session performs no heap allocation.
  struct SupShmSet {
    thyme::ShmRegion h, c, d;
    static constexpr std::size_t kMaxChannels = 16;
    thyme::ShmRegion ch[kMaxChannels];
    char nm_ch[kMaxChannels][64] = {};
    std::size_t n_ch = 0;

    char nm_h[64] = {}, nm_c[64] = {}, nm_d[64] = {};

    bool create_all(const char* prefix, const MpSupervisorConfig& cfg);
    void unlink_all();
    void unmap_all();
  };

  struct Impl {
    // Per-role OPS-01 state. Fixed-size inline array: the restart path never
    // allocates.
    struct Role {
      MpRoleState state = MpRoleState::kNotStarted;
      pid_t pid = 0;
      uint32_t attempts = 0;           // lifetime restart attempts
      uint32_t successes = 0;          // attempts observed healthy
      uint32_t attempts_in_window = 0;
      uint64_t window_start_ns = 0;
      uint64_t backoff_ns = 0;         // spacing for the next respawn
      uint64_t next_restart_ns = 0;
      uint64_t last_fork_ns = 0;
      uint64_t last_progress_ns = 0;   // heartbeat/proc_loops baseline
      uint64_t last_seen_heartbeat_ns = 0;
      uint64_t last_seen_loops = 0;
      uint64_t last_stall_age_ns = 0;
      uint64_t kill_deadline_ns = 0;
      uint64_t last_restart_ns = 0;
      int last_exit_code = -1;
      int last_term_signal = 0;
      bool last_exit_ok = true;        // final wait status was exit 0
      bool success_pending = false;    // a restart awaits its first progress
    };

    SupShmSet shm;
    char prefix[24]{};
    char mirror_name[64]{};
    char self_exe[256]{};
    bool record_mode = false;
    bool stopping = false;
    uint64_t config_hash = 0;
    uint64_t start_ns = 0;
    uint64_t last_watchdog_ns = 0;  // OPS-03 notify rate limit
    Role roles[MpControl::kMaxProcSlots]{};
    int n_procs = 0;
    uint64_t restarts_attempted = 0;
    uint64_t restarts_succeeded = 0;
    uint64_t restart_giveups = 0;
    uint64_t status_publish_skips = 0;  // ARCH-05: Publish() raced a reader
    MpControl* ctrl = nullptr;
    // OPS-07: one startup capacity check, republished by every status tick.
    thyme::resource::Report resources;
    bool resources_checked = false;
  };

  bool ForkRole(std::size_t index);
  void InitRole(std::size_t index, uint64_t now_ns);
  void ObserveRoleProgress(std::size_t index, uint64_t now_ns);
  void HandleRoleExit(std::size_t index, int status, uint64_t now_ns,
                      bool stopping);
  void TryRestartRole(std::size_t index, uint64_t now_ns, bool stopping);
  void ResetRoleHealth(std::size_t index);
  uint64_t RoleHeartbeat(std::size_t index) const;
  uint64_t NextBackoff(std::size_t index) const;

  MpSupervisorConfig cfg_;
  Impl impl_;
  StatusServer status_;
};

}  // namespace app
}  // namespace thyme
