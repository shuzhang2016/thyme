#include "thyme/app/mp_supervisor.h"

#include "thyme/app/mp_session_identity.h"
#include "thyme/app/record_mirror.h"
#include "thyme/app/systemd_notify.h"
#include "thyme/runtime/app.h"
#include "thyme/runtime/pinned_slot_gate.h"
#include "thyme/runtime/log.h"
#include "thyme/runtime/parent_death.h"
#include "thyme/runtime/rotating_file.h"
#include "thyme/runtime/shm_region.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace thyme {
namespace app {
namespace {

// OPS-03: how often the poll loop feeds systemd's watchdog. The unit sets
// WatchdogSec=30s; one ping per second keeps a wide margin without making
// the 10ms supervisor tick send 100 datagrams per second.
constexpr uint64_t kSdWatchdogIntervalNs = 1000ULL * 1000 * 1000;

// Minimum loop count for a process role: 25% of the slowest worker hz in that
// process times the run seconds.
uint64_t MinLoopsForProcess(const MpSupervisorConfig& cfg, const char* process,
                            int seconds) {
  if (seconds <= 0) seconds = 1;
  uint64_t min_hz = 0;
  bool any = false;
  for (const auto& n : cfg.nodes) {
    if (std::strcmp(n.process, process) != 0) continue;
    if (std::strcmp(n.type, "health_reporter") == 0) continue;
    any = true;
    const uint64_t hz = static_cast<uint64_t>(n.hz > 0 ? n.hz : 1);
    if (min_hz == 0 || hz < min_hz) min_hz = hz;
  }
  if (!any) return 1;
  const uint64_t expected = min_hz * static_cast<uint64_t>(seconds);
  const uint64_t min_loops = expected / 4;
  return min_loops > 1 ? min_loops : 1;
}

}  // namespace

// Success gate for the per-process loop counts (Spec §6: every existing
// process must have looped). Each process is judged against its own bound:
// proc_loops[i] must meet MinLoopsForProcess for that process's id. There is
// no privileged role and no cross-process aggregate; a slow process can no
// longer mask (or be masked by) any other.
// Defined out-of-line in mp_supervisor.cpp so it is testable from
// test_mp_supervisor (thyme::app::LoopsOk).
bool LoopsOk(const MpSupervisorConfig& cfg, int seconds,
             thyme::span<const uint64_t> proc_loops) {
  const std::size_t n = cfg.processes.size();
  if (proc_loops.size() < n) return false;
  for (std::size_t i = 0; i < n; ++i) {
    if (proc_loops[i] <
        MinLoopsForProcess(cfg, cfg.processes[i].id, seconds)) {
      return false;
    }
  }
  return true;
}

// True if shm kind `kind` appears on at least one cross-process channel.
bool KindOnCrossProcessChannel(const MpSupervisorConfig& cfg, int kind) {
  for (const auto& ch : cfg.shm_channels) {
    if (ch.kind != kind) continue;
    if (std::strcmp(ch.from_process, ch.to_process) != 0) return true;
  }
  return false;
}

// Success gate for the cross-process frame counters (S-D4). Every configured
// gate must see >= 1 frame in its slot; a gate whose kind never appears on a
// cross-process channel, or whose slot exceeds the counter array, is a
// misconfiguration and fails instead of silently passing.
// Defined out-of-line in mp_supervisor.cpp so it is testable from
// test_mp_supervisor (thyme::app::CrossGatesOk).
bool CrossGatesOk(const MpSupervisorConfig& cfg,
                  thyme::span<const uint64_t> cross_frames) {
  for (const auto& gate : cfg.cross_gates) {
    if (static_cast<std::size_t>(gate.counter_slot) >= cross_frames.size()) {
      return false;
    }
    if (!KindOnCrossProcessChannel(cfg, gate.shm_kind)) return false;
    if (cross_frames[gate.counter_slot] < 1) return false;
  }
  return true;
}

bool IsHealthReporterNode(const MpNode& n) {
  return std::strcmp(n.type, "health_reporter") == 0;
}

MpHealthScanSummary ScanHealthPage(const MpSupervisorConfig& cfg,
                                   const thyme::SharedHealthPage& page) {
  MpHealthScanSummary out{};
  if (!page.Validate()) return out;
  const uint32_t worker_count = page.worker_count();
  std::size_t n = worker_count > 0 ? static_cast<std::size_t>(worker_count)
                                   : static_cast<std::size_t>(cfg.health_n);
  if (cfg.health_n > 0 &&
      static_cast<std::size_t>(cfg.health_n) < n) {
    n = static_cast<std::size_t>(cfg.health_n);
  }
  if (n > thyme::SharedHealthPage::kMaxWorkers) {
    n = thyme::SharedHealthPage::kMaxWorkers;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (i < cfg.nodes.size() && IsHealthReporterNode(cfg.nodes[i])) continue;
    const thyme::HealthSlot& hs = page.slots[i];
    ++out.workers_scanned;
    out.total_drops += hs.drop_cnt.load(std::memory_order_acquire);
    out.total_deadline_miss +=
        hs.deadline_miss_cnt.load(std::memory_order_acquire);
    if (hs.error_code.load(std::memory_order_acquire) != 0) {
      ++out.total_errors;
    }
  }
  return out;
}

bool HealthScanOk(const MpHealthScanSummary& summary, bool strict_drops,
                  bool strict_errors, bool strict_deadline_miss) {
  if (strict_drops && summary.total_drops > 0) return false;
  if (strict_errors && summary.total_errors > 0) return false;
  if (strict_deadline_miss && summary.total_deadline_miss > 0) return false;
  return true;
}

// Supervisor-side SHM set: _h (SharedHealthPage) and _c (MpControl) are
// always created; one LatestSlot region per cross-process channel entry.
bool MpSupervisorSession::SupShmSet::create_all(
    const char* prefix, const MpSupervisorConfig& cfg) {
  MpName(nm_h, sizeof(nm_h), prefix, "_h");
  MpName(nm_c, sizeof(nm_c), prefix, "_c");
  MpName(nm_d, sizeof(nm_d), prefix, "_id");
  // ARCH-11: the health page has payload initialization after allocation. Use
  // a raw region so Init publishes magic only after header and slots are ready;
  // CreateDescribed would publish a valid header before that initialization.
  if (!h.Create(nm_h, sizeof(thyme::SharedHealthPage))) return false;
  auto* health_page = static_cast<thyme::SharedHealthPage*>(h.data());
  if (!health_page->Init(static_cast<uint32_t>(getpid()),
                         static_cast<uint32_t>(cfg.health_n))) {
    return false;
  }
  if (!c.CreateDescribed(nm_c, MpControl::Spec())) return false;
  auto* ctrl = static_cast<MpControl*>(c.data());
  if (!ctrl->Validate()) return false;
  // ARCH-17: session identity page. Created before roles are forked, so a role
  // can verify the supervisor's config/graph identity cross-process.
  if (!CreateMpSessionIdentityPage(
          d, prefix, MpConfigHash(cfg), cfg.graph_hash,
          static_cast<uint32_t>(getpid()),
          ProducerGateProcessStart(getpid()),
          static_cast<uint32_t>(cfg.health_n))) {
    THYME_LOG_ERROR("supervisor: identity page create failed");
    return false;
  }
  for (const auto& chn : cfg.shm_channels) {
    if (std::strcmp(chn.from_process, chn.to_process) == 0) continue;
    if (n_ch >= kMaxChannels) return false;
    if (chn.kind < 0 ||
        static_cast<std::size_t>(chn.kind) >= cfg.slot_specs.size()) {
      return false;
    }
    const thyme::app::MpShmKindSpec& spec =
        cfg.slot_specs[static_cast<std::size_t>(chn.kind)];
    if (spec.create_page == nullptr || spec.payload_layout_hash == 0 ||
        spec.region_bytes == 0) {
      THYME_LOG_ERROR(
          "supervisor: SHM kind %d has no described LatestSlot page spec",
          chn.kind);
      return false;
    }
    MpName(nm_ch[n_ch], sizeof(nm_ch[n_ch]), prefix, chn.suffix);
    thyme::ShmPageError error;
    if (!spec.create_page(ch[n_ch], nm_ch[n_ch], spec.feature_bits, &error)) {
      THYME_LOG_ERROR("supervisor: create LatestSlot page %s failed: %s",
                      nm_ch[n_ch], ShmPageErrorName(error.code));
      return false;
    }
    ++n_ch;
  }
  return true;
}

void MpSupervisorSession::SupShmSet::unlink_all() {
  thyme::ShmRegion::UnlinkName(nm_h);
  thyme::ShmRegion::UnlinkName(nm_c);
  thyme::ShmRegion::UnlinkName(nm_d);
  for (std::size_t i = 0; i < n_ch; ++i) {
    thyme::ShmRegion::UnlinkName(nm_ch[i]);
  }
}

void MpSupervisorSession::SupShmSet::unmap_all() {
  h.Unmap();
  c.Unmap();
  d.Unmap();
  for (std::size_t i = 0; i < n_ch; ++i) ch[i].Unmap();
}

int wait_child(pid_t pid, const char* who) {
  int status = 0;
  while (true) {
    pid_t r = waitpid(pid, &status, 0);
    if (r == pid) break;
    if (r == -1 && errno == EINTR) continue;
    if (r == -1) {
      THYME_LOG_ERROR("supervisor: waitpid(%s) failed: %s", who,
                      std::strerror(errno));
      return -1;
    }
  }
  return status;
}

bool child_ok(int status) {
  if (status == -1) return false;
  if (WIFSIGNALED(status)) return false;
  if (!WIFEXITED(status)) return false;
  return WEXITSTATUS(status) == 0;
}

void ShutdownRecordMirror(const MpSupervisorConfig& cfg) {
  if (cfg.record_ring_specs != nullptr) {
    record_mirror::ShutdownRing(true);
  } else {
    record_mirror::Shutdown(true);
  }
}

MpSupervisorSession::MpSupervisorSession(const MpSupervisorConfig& cfg)
    : cfg_(cfg) {}

MpSupervisorSession::~MpSupervisorSession() { StopStatusServer(); }

bool MpSupervisorSession::Prepare(const char* record_shm_or_null) {
  thyme::App::Prepare();
  thyme::App::InstallStopSignals();
  impl_.start_ns = NowMonoNs();

  std::snprintf(impl_.prefix, sizeof(impl_.prefix), cfg_.shm_prefix_fmt,
                static_cast<int>(getpid()));

  impl_.record_mode = (record_shm_or_null != nullptr);
  impl_.mirror_name[0] = '\0';
  if (impl_.record_mode) {
    if (record_shm_or_null[0] != '\0') {
      std::snprintf(impl_.mirror_name, sizeof(impl_.mirror_name), "%s",
                    record_shm_or_null);
    } else {
      MpName(impl_.mirror_name, sizeof(impl_.mirror_name), impl_.prefix,
             "_rec");
    }
    if (cfg_.record_table == nullptr) {
      THYME_LOG_ERROR(
          "supervisor: record mode requires a record_table in the config");
      return false;
    }
    const bool created = cfg_.record_ring_specs != nullptr
                             ? record_mirror::CreateRing(impl_.mirror_name, *cfg_.record_table,
                                                         cfg_.record_ring_specs)
                             : record_mirror::Create(impl_.mirror_name, *cfg_.record_table);
    if (!created) {
      THYME_LOG_ERROR("supervisor: record mirror Create(%s) failed",
                      impl_.mirror_name);
      return false;
    }
  }

  // Test hook (test_mp_supervisor): force the create_all failure path.
  const bool force_create_fail =
      std::getenv("T9_MP_SUPERVISOR_FAIL_CREATE") != nullptr;
  if (force_create_fail || !impl_.shm.create_all(impl_.prefix, cfg_)) {
    THYME_LOG_ERROR("supervisor: shm create failed for prefix %s",
                    impl_.prefix);
    impl_.shm.unlink_all();
    if (impl_.record_mode) {
      ShutdownRecordMirror(cfg_);
    }
    return false;
  }

  impl_.ctrl = static_cast<MpControl*>(impl_.shm.c.data());
  // Attach-side identity: record the config hash next to the prefix. Roles
  // are separate processes and cannot read this registry, so their check
  // falls back to the in-header session_id/abi_version validation (see
  // mp_runtime.h). Registering here still makes in-process attaches exact.
  impl_.config_hash = MpConfigHash(cfg_);
  if (!RegisterMpSessionIdentity(impl_.prefix, impl_.config_hash)) {
    // Identity registration is an optimisation for in-process attaches; a
    // failure still falls back to the in-header session_id/abi_version check,
    // but it must not disappear.
    THYME_LOG_WARN("supervisor: could not register session identity for %s",
                   impl_.prefix);
  }

  // OPS-07: one read-only host capacity check at startup (no allocation, no
  // file opens). The supervisor owns no package directory, so the disk leg is
  // unverified here; trigger_center is the reference host that also checks
  // the output filesystem and enforces a fatal result. The result is
  // republished by every PublishStatus and folds into /status `ok`.
  {
    const auto plan = thyme::resource::PlanFromEnv(
        static_cast<uint32_t>(cfg_.shm_channels.size()), nullptr, 0, 0);
    if (plan.enabled) {
      impl_.resources = thyme::resource::Check(plan.limits);
      impl_.resources_checked = true;
      char line[512];
      (void)thyme::resource::FormatSummary(impl_.resources, line,
                                           sizeof(line));
      if (impl_.resources.fatal) {
        THYME_LOG_ERROR("supervisor: %s", line);
      } else if (impl_.resources.warnings != 0) {
        THYME_LOG_WARN("supervisor: %s", line);
      } else {
        THYME_LOG_INFO("supervisor: %s", line);
      }
    }
  }

  if (!StartStatusServer(cfg_.status_socket_path)) {
    THYME_LOG_ERROR("supervisor: status endpoint failed to start");
    impl_.shm.unmap_all();
    impl_.shm.unlink_all();
    if (impl_.record_mode) {
      ShutdownRecordMirror(cfg_);
    }
    return false;
  }
  PublishStatus();
  return true;
}

void MpSupervisorSession::InitRole(std::size_t index, uint64_t now_ns) {
  Impl::Role& r = impl_.roles[index];
  r = Impl::Role{};
  r.state = MpRoleState::kRunning;
  uint64_t backoff = cfg_.restart_backoff_initial_ns;
  if (backoff < kMinRestartIntervalNs) backoff = kMinRestartIntervalNs;
  if (cfg_.restart_backoff_max_ns != 0 &&
      backoff > cfg_.restart_backoff_max_ns) {
    backoff = cfg_.restart_backoff_max_ns;
  }
  r.backoff_ns = backoff;
  r.last_fork_ns = now_ns;
  r.last_progress_ns = now_ns;
}

// Backoff to use after the *next* failure of this incarnation.
uint64_t MpSupervisorSession::NextBackoff(std::size_t index) const {
  const Impl::Role& r = impl_.roles[index];
  uint64_t initial = cfg_.restart_backoff_initial_ns;
  if (initial < kMinRestartIntervalNs) initial = kMinRestartIntervalNs;
  uint64_t cap = cfg_.restart_backoff_max_ns;
  if (cap < initial) cap = initial;
  if (r.backoff_ns == 0) return initial;
  const uint32_t mul = cfg_.restart_backoff_multiplier;
  if (mul <= 1) return r.backoff_ns < cap ? r.backoff_ns : cap;
  uint64_t next = 0;
  if (r.backoff_ns > cap / mul) {
    next = cap;  // saturating multiply
  } else {
    next = r.backoff_ns * mul;
  }
  if (next < initial) next = initial;
  if (next > cap) next = cap;
  return next;
}

uint64_t MpSupervisorSession::RoleHeartbeat(std::size_t index) const {
  if (impl_.shm.h.data() == nullptr) return 0;
  const auto* page =
      static_cast<const thyme::SharedHealthPage*>(impl_.shm.h.data());
  if (!page->Validate()) return 0;
  const uint32_t workers = page->worker_count();
  const char* id = cfg_.processes[index].id;
  uint64_t hb = 0;
  for (std::size_t j = 0; j < cfg_.nodes.size() && j < workers; ++j) {
    if (std::strcmp(cfg_.nodes[j].process, id) != 0) continue;
    const uint64_t v =
        page->slots[j].last_finish_mono_ns.load(std::memory_order_acquire);
    if (v > hb) hb = v;
  }
  return hb;
}

// Clear the health slots owned by `index` before respawning it. A heartbeat
// (or drop/error counter) left behind by the dead incarnation must never make
// the fresh one look alive.
void MpSupervisorSession::ResetRoleHealth(std::size_t index) {
  if (impl_.shm.h.data() == nullptr) return;
  auto* page = static_cast<thyme::SharedHealthPage*>(impl_.shm.h.data());
  if (!page->Validate()) return;
  const uint32_t workers = page->worker_count();
  const char* id = cfg_.processes[index].id;
  for (std::size_t j = 0; j < cfg_.nodes.size() && j < workers; ++j) {
    if (std::strcmp(cfg_.nodes[j].process, id) != 0) continue;
    thyme::HealthSlot& s = page->slots[j];
    s.alive_counter.store(0, std::memory_order_relaxed);
    s.state.store(0, std::memory_order_relaxed);
    s.last_finish_mono_ns.store(0, std::memory_order_relaxed);
    s.output_sequence.store(0, std::memory_order_relaxed);
    s.deadline_miss_cnt.store(0, std::memory_order_relaxed);
    s.drop_cnt.store(0, std::memory_order_relaxed);
    s.error_code.store(0, std::memory_order_relaxed);
    s.memory_flags.store(0, std::memory_order_relaxed);
  }
}

// Fork+exec one role, reusing the args and environment of ForkRoles. The
// caller owns pid/state bookkeeping.
bool MpSupervisorSession::ForkRole(std::size_t index) {
  const auto& p = cfg_.processes[index];
  const pid_t pid = fork();
  if (pid == 0) {
    // Fail closed if the supervisor vanished between fork() and the prctl
    // (PR_SET_PDEATHSIG is cleared by fork and only fires on a future
    // parent death). The setting survives the execv below.
    const ParentDeathSignal pds = InstallParentDeathSignal(SIGTERM);
    if (pds == ParentDeathSignal::kParentGone) _exit(127);
    impl_.shm.unmap_all();
    char proc_idx_buf[16];
    std::snprintf(proc_idx_buf, sizeof(proc_idx_buf), "%zu", index);
    const char* executable = impl_.self_exe;
    if (cfg_.role_executable != nullptr) {
      executable = cfg_.role_executable(cfg_.role_executable_ctx, p.role,
                                        impl_.self_exe);
    }
    // 7 base args + up to 2 record args + trailing nullptr.
    const char* child_argv[10];
    int arg = 0;
    child_argv[arg++] = executable;
    child_argv[arg++] = "--role";
    child_argv[arg++] = p.role;
    child_argv[arg++] = "--shm-prefix";
    child_argv[arg++] = impl_.prefix;
    child_argv[arg++] = "--proc-idx";
    child_argv[arg++] = proc_idx_buf;
    if (impl_.mirror_name[0] != '\0') {
      child_argv[arg++] = "--record-shm";
      child_argv[arg++] = impl_.mirror_name;
    }
    child_argv[arg] = nullptr;
    if (executable == nullptr || executable[0] == '\0') _exit(127);
    execv(executable, const_cast<char* const*>(child_argv));
    // OBS-08: this is a post-fork child of a possibly multi-threaded
    // supervisor (the status server has a thread), so the logger's sink mutex
    // must not be taken here. Raw stderr, then _exit, is the only safe path.
    std::fprintf(stderr, "supervisor: execv %s failed: %s\n", p.role,
                 std::strerror(errno));
    _exit(127);
  }
  if (pid < 0) return false;
  impl_.roles[index].pid = pid;
  return true;
}

bool MpSupervisorSession::ForkRoles(const char* self_exe) {
  if (self_exe == nullptr || self_exe[0] == '\0' ||
      std::strlen(self_exe) >= sizeof(impl_.self_exe)) {
    THYME_LOG_ERROR("supervisor: invalid self exe path");
    return false;
  }
  std::snprintf(impl_.self_exe, sizeof(impl_.self_exe), "%s", self_exe);
  const uint64_t now_ns = NowMonoNs();
  for (std::size_t i = 0; i < cfg_.processes.size(); ++i) {
    if (impl_.n_procs >= static_cast<int>(MpControl::kMaxProcSlots)) {
      THYME_LOG_ERROR("supervisor: too many processes");
      impl_.ctrl->stop.store(1, std::memory_order_release);
      for (int j = 0; j < impl_.n_procs; ++j) {
        bool timed = false;
        const int st = WaitChildTimeout(impl_.roles[j].pid,
                                        cfg_.shutdown_timeout_ns, &timed);
        impl_.roles[j].pid = 0;
        if (st == -1) {
          THYME_LOG_WARN("supervisor: waitpid for %s failed during abort: %s",
                         cfg_.processes[j].role, std::strerror(errno));
        }
      }
      impl_.shm.unmap_all();
      impl_.shm.unlink_all();
      if (impl_.record_mode) {
        ShutdownRecordMirror(cfg_);
      }
      return false;
    }
    InitRole(i, now_ns);
    if (!ForkRole(i)) {
      THYME_LOG_ERROR("supervisor: fork %s failed: %s",
                      cfg_.processes[i].role, std::strerror(errno));
      impl_.ctrl->stop.store(1, std::memory_order_release);
      // Already-forked children must not deadlock the abort: wait with a
      // deadline, then SIGKILL any straggler (WaitChildTimeout).
      for (int j = 0; j < impl_.n_procs; ++j) {
        bool timed = false;
        const int st = WaitChildTimeout(impl_.roles[j].pid,
                                        cfg_.shutdown_timeout_ns, &timed);
        impl_.roles[j].pid = 0;
        if (st == -1) {
          THYME_LOG_WARN("supervisor: waitpid for %s failed during abort: %s",
                         cfg_.processes[j].role, std::strerror(errno));
        }
        if (timed) {
          THYME_LOG_WARN("supervisor: %s did not exit within %llu ns; "
                         "SIGKILLed",
                         cfg_.processes[j].role,
                         static_cast<unsigned long long>(
                             cfg_.shutdown_timeout_ns));
        }
      }
      impl_.roles[i].pid = 0;
      impl_.shm.unmap_all();
      impl_.shm.unlink_all();
      if (impl_.record_mode) {
        ShutdownRecordMirror(cfg_);
      }
      return false;
    }
    ++impl_.n_procs;
  }
  // OPS-03: every role is forked; tell systemd the unit is ready. No-op
  // unless THYME_SD_NOTIFY is enabled and NOTIFY_SOCKET is present.
  SystemdNotifyReady();
  return true;
}

// Record a reaped child's status and decide whether to schedule a respawn.
void MpSupervisorSession::HandleRoleExit(std::size_t index, int status,
                                         uint64_t now_ns, bool stopping) {
  Impl::Role& r = impl_.roles[index];
  r.pid = 0;
  r.kill_deadline_ns = 0;
  if (status >= 0 && WIFEXITED(status)) {
    r.last_exit_code = WEXITSTATUS(status);
    r.last_term_signal = 0;
    r.last_exit_ok = child_ok(status);
  } else if (status >= 0 && WIFSIGNALED(status)) {
    r.last_exit_code = -1;
    r.last_term_signal = WTERMSIG(status);
    r.last_exit_ok = false;
  } else {
    r.last_exit_code = -1;
    r.last_term_signal = 0;
    r.last_exit_ok = false;
  }
  if (!cfg_.restart_enabled || stopping) {
    r.state = MpRoleState::kStopped;
    return;
  }
  // Backoff is measured from the previous fork so a role that dies instantly
  // cannot spin: the next respawn is at least backoff_ns after it started.
  uint64_t earliest = r.last_fork_ns + r.backoff_ns;
  if (earliest < r.last_fork_ns) earliest = UINT64_MAX;  // overflow
  r.next_restart_ns = now_ns > earliest ? now_ns : earliest;
  r.state = MpRoleState::kWaitingBackoff;
}

// A running role makes progress when its health heartbeat or its per-process
// loop counter advances. The first progress after a respawn also credits the
// restart as succeeded and resets the backoff.
void MpSupervisorSession::ObserveRoleProgress(std::size_t index,
                                              uint64_t now_ns) {
  Impl::Role& r = impl_.roles[index];
  bool progressed = false;
  const uint64_t hb = RoleHeartbeat(index);
  if (hb > r.last_seen_heartbeat_ns) {
    r.last_seen_heartbeat_ns = hb;
    progressed = true;
  }
  if (impl_.ctrl != nullptr) {
    const uint64_t loops =
        impl_.ctrl->proc_loops[index].load(std::memory_order_acquire);
    if (loops > r.last_seen_loops) {
      r.last_seen_loops = loops;
      progressed = true;
    }
  }
  if (!progressed) return;
  r.last_progress_ns = now_ns;
  if (r.success_pending) {
    r.success_pending = false;
    ++r.successes;
    ++impl_.restarts_succeeded;
    uint64_t initial = cfg_.restart_backoff_initial_ns;
    if (initial < kMinRestartIntervalNs) initial = kMinRestartIntervalNs;
    if (cfg_.restart_backoff_max_ns != 0 &&
        initial > cfg_.restart_backoff_max_ns) {
      initial = cfg_.restart_backoff_max_ns;
    }
    r.backoff_ns = initial;
  }
}

void MpSupervisorSession::TryRestartRole(std::size_t index, uint64_t now_ns,
                                         bool stopping) {
  Impl::Role& r = impl_.roles[index];
  if (stopping) {
    r.state = MpRoleState::kStopped;
    return;
  }
  if (now_ns < r.next_restart_ns) return;

  // Fixed window: at most max_restarts attempts per restart_window_ns.
  if (cfg_.restart_window_ns > 0 && r.window_start_ns != 0 &&
      now_ns >= r.window_start_ns + cfg_.restart_window_ns) {
    r.window_start_ns = now_ns;
    r.attempts_in_window = 0;
  }
  if (cfg_.max_restarts == 0 ||
      r.attempts_in_window >= cfg_.max_restarts) {
    r.state = MpRoleState::kGaveUp;
    ++impl_.restart_giveups;
    return;
  }
  if (r.window_start_ns == 0) r.window_start_ns = now_ns;
  ++r.attempts_in_window;
  ++r.attempts;
  ++impl_.restarts_attempted;

  // Fresh incarnation: clear the health slots (stale heartbeats from the
  // dead process), rebase progress and grow the backoff for a future failure.
  ResetRoleHealth(index);
  r.last_seen_heartbeat_ns = 0;
  r.last_seen_loops =
      impl_.ctrl != nullptr
          ? impl_.ctrl->proc_loops[index].load(std::memory_order_acquire)
          : 0;
  r.last_progress_ns = now_ns;
  r.last_stall_age_ns = 0;
  r.last_fork_ns = now_ns;
  r.last_restart_ns = now_ns;
  r.backoff_ns = NextBackoff(index);

  if (!ForkRole(index)) {
    r.state = MpRoleState::kWaitingBackoff;
    r.next_restart_ns = now_ns + r.backoff_ns;
    return;
  }
  r.state = MpRoleState::kRunning;
  r.success_pending = true;
}

void MpSupervisorSession::PollRestarts(uint64_t now_ns) {
  if (now_ns == 0) now_ns = NowMonoNs();
  const bool stopping =
      impl_.stopping || thyme::App::stop_requested() ||
      (impl_.ctrl != nullptr &&
       impl_.ctrl->stop.load(std::memory_order_acquire) != 0);
  for (int i = 0; i < impl_.n_procs; ++i) {
    const std::size_t index = static_cast<std::size_t>(i);
    Impl::Role& r = impl_.roles[index];
    if (r.state == MpRoleState::kNotStarted) continue;

    if (r.pid > 0) {
      int status = 0;
      const pid_t w = waitpid(r.pid, &status, WNOHANG);
      if (w == r.pid) {
        HandleRoleExit(index, status, now_ns, stopping);
      } else if (w == -1 && errno == ECHILD) {
        HandleRoleExit(index, -1, now_ns, stopping);
      } else if (w == 0) {
        if (r.state == MpRoleState::kRunning ||
            r.state == MpRoleState::kTerminating) {
          ObserveRoleProgress(index, now_ns);
        }
        if (r.state == MpRoleState::kTerminating) {
          if (r.kill_deadline_ns != 0 && now_ns >= r.kill_deadline_ns) {
            (void)kill(r.pid, SIGKILL);
            r.kill_deadline_ns = 0;  // one escalation per incarnation
          }
        } else if (r.state == MpRoleState::kRunning &&
                   cfg_.restart_enabled &&
                   cfg_.restart_stall_timeout_ns > 0) {
          const uint64_t age =
              now_ns > r.last_progress_ns ? now_ns - r.last_progress_ns : 0;
          r.last_stall_age_ns = age;
          if (age > cfg_.restart_stall_timeout_ns) {
            (void)kill(r.pid, SIGTERM);
            r.state = MpRoleState::kTerminating;
            r.kill_deadline_ns = now_ns + cfg_.restart_kill_grace_ns;
          }
        }
      }
    }

    if (r.state == MpRoleState::kWaitingBackoff) {
      if (stopping) {
        r.state = MpRoleState::kStopped;
      } else if (now_ns >= r.next_restart_ns) {
        TryRestartRole(index, now_ns, stopping);
      }
    }
  }
  // OPS-03: feed systemd's watchdog while the supervisor tick keeps running.
  // Rate-limited so a 10ms tick does not become a notify storm; a hung or
  // stopped poll loop lets WatchdogSec expire, which is exactly the signal
  // systemd needs to restart the whole session.
  if (SystemdNotifyEnabled()) {
    const bool due =
        impl_.last_watchdog_ns == 0 ||
        (now_ns >= impl_.last_watchdog_ns &&
         now_ns - impl_.last_watchdog_ns >= kSdWatchdogIntervalNs);
    if (due) {
      impl_.last_watchdog_ns = now_ns;
      SystemdNotifyWatchdog();
    }
  }
  PublishStatus(now_ns);
}

bool MpSupervisorSession::StartStatusServer(const char* socket_path) {
  if (status_.running()) return true;
  const char* path = socket_path;
  if (path == nullptr) path = std::getenv("THYME_STATUS_SOCKET");
  if (path == nullptr || path[0] == '\0' || std::strcmp(path, "none") == 0) {
    return true;  // disabled: monitoring is opt-in
  }
  StatusServer::Config sc;
  sc.socket_path = path;
  if (!status_.Start(sc)) {
    THYME_LOG_ERROR("supervisor: cannot bind status socket %s", path);
    return false;
  }
  THYME_LOG_INFO("supervisor: status endpoint on %s", path);
  return true;
}

void MpSupervisorSession::StopStatusServer() { status_.Stop(); }

void MpSupervisorSession::PublishStatus(uint64_t now_ns) {
  if (!status_.running()) return;
  if (now_ns == 0) now_ns = NowMonoNs();

  StatusSnapshot snap;
  std::snprintf(snap.build, sizeof(snap.build), "%s", THYME_VERSION_STRING);
  const char* session = impl_.prefix;
  if (session[0] == '/') ++session;
  std::snprintf(snap.session_id, sizeof(snap.session_id), "%s", session);
  snap.uptime_ns =
      now_ns > impl_.start_ns ? now_ns - impl_.start_ns : 0;

  const std::size_t n_roles =
      cfg_.processes.size() > StatusSnapshot::kMaxRoles
          ? StatusSnapshot::kMaxRoles
          : cfg_.processes.size();
  snap.n_roles = static_cast<uint32_t>(n_roles);
  bool healthy = n_roles > 0;

  const thyme::SharedHealthPage* page = nullptr;
  uint32_t workers = 0;
  if (impl_.shm.h.data() != nullptr) {
    const auto* p = static_cast<const thyme::SharedHealthPage*>(
        impl_.shm.h.data());
    if (p->Validate()) {
      page = p;
      workers = p->worker_count();
      if (workers > static_cast<uint32_t>(cfg_.nodes.size())) {
        workers = static_cast<uint32_t>(cfg_.nodes.size());
      }
    }
  }

  for (std::size_t i = 0; i < n_roles; ++i) {
    StatusRoleSnapshot& out = snap.roles[i];
    const MpProcess& proc = cfg_.processes[i];
    std::snprintf(out.id, sizeof(out.id), "%s", proc.id);
    std::snprintf(out.role, sizeof(out.role), "%s", proc.role);
    const Impl::Role& r = impl_.roles[i];
    out.state = static_cast<uint8_t>(r.state);
    out.pid = static_cast<int32_t>(r.pid);
    out.restarts_attempted = r.attempts;
    out.restarts_succeeded = r.successes;
    out.last_exit_code = static_cast<int32_t>(r.last_exit_code);
    out.last_term_signal = static_cast<int32_t>(r.last_term_signal);
    out.last_stall_age_ns = r.last_stall_age_ns;
    out.loops =
        impl_.ctrl != nullptr
            ? impl_.ctrl->proc_loops[i].load(std::memory_order_acquire)
            : 0;
    if (page != nullptr) {
      for (uint32_t j = 0; j < workers; ++j) {
        if (std::strcmp(cfg_.nodes[j].process, proc.id) != 0) continue;
        const thyme::HealthSlot& slot = page->slots[j];
        out.alive += slot.alive_counter.load(std::memory_order_acquire);
        out.deadline_misses +=
            slot.deadline_miss_cnt.load(std::memory_order_acquire);
        const uint64_t drops = slot.drop_cnt.load(std::memory_order_acquire);
        out.drops += drops;
        if (out.n_health_drops < StatusRoleSnapshot::kMaxHealthErrors) {
          StatusHealthDrop& health_drop =
              out.health_drops[out.n_health_drops++];
          std::snprintf(health_drop.edge, sizeof(health_drop.edge), "%s",
                        cfg_.nodes[j].id);
          health_drop.drops = drops;
        }
        out.sched_mismatches +=
            slot.sched_mismatch_cnt.load(std::memory_order_acquire);
        const uint32_t memory_flags =
            slot.memory_flags.load(std::memory_order_acquire);
        out.memory_flags |= memory_flags;
        if (out.n_health_memory < StatusRoleSnapshot::kMaxHealthErrors) {
          StatusHealthMemory& health_memory =
              out.health_memory[out.n_health_memory++];
          std::snprintf(health_memory.node, sizeof(health_memory.node), "%s",
                        cfg_.nodes[j].id);
          health_memory.flags = memory_flags;
        }
        const uint32_t error_code =
            slot.error_code.load(std::memory_order_acquire);
        if (error_code != 0) ++out.errors;
        if (out.n_health_errors < StatusRoleSnapshot::kMaxHealthErrors) {
          StatusHealthError &health_error =
              out.health_errors[out.n_health_errors++];
          std::snprintf(health_error.node, sizeof(health_error.node), "%s",
                        cfg_.nodes[j].id);
          health_error.error_code = error_code;
        }
      }
    }
    if (r.state != MpRoleState::kRunning) healthy = false;
  }
  snap.healthy = healthy;
  snap.has_recorder = impl_.record_mode;
  if (impl_.ctrl != nullptr) {
    snap.trigger_relay_sent =
        impl_.ctrl->trigger_relay_sent.load(std::memory_order_acquire);
    snap.trigger_relay_debounced =
        impl_.ctrl->trigger_relay_debounced.load(std::memory_order_acquire);
    snap.trigger_req_seq =
        impl_.ctrl->trigger_req_seq.load(std::memory_order_acquire);
    snap.trigger_ack_seq =
        impl_.ctrl->trigger_ack_seq.load(std::memory_order_acquire);
  }
  // OPS-05: the supervisor process's own log budget (per-process; roles
  // enforce and account their own).
  snap.log_budget_limit_bytes = logging::LogBudget::limit_bytes();
  snap.log_budget_used_bytes = logging::LogBudget::used_bytes();
  snap.log_budget_drops = logging::LogBudget::drops();
  snap.log_rotations = logging::LogBudget::rotations();
  // OPS-07: startup resource check evidence (checked=false when the check was
  // disabled with THYME_RESOURCE_CHECK=off).
  if (impl_.resources_checked) {
    const thyme::resource::Report& r = impl_.resources;
    snap.resources_checked = true;
    snap.resources_fatal = r.fatal;
    snap.resource_warnings = r.warnings;
    snap.nofile_soft = r.nofile_soft;
    snap.nofile_required = r.nofile_required;
    snap.shm_free_bytes = r.shm_free;
    snap.shm_required_bytes = r.shm_required;
    snap.disk_free_bytes = r.disk_free;
    snap.disk_required_bytes = r.disk_required;
  }
  if (!status_.Publish(snap)) {
    // A reader held the active slot. Bounded/latest-value: the next tick
    // republishes; counted so a starved endpoint is visible.
    impl_.status_publish_skips += 1;
  }
}

MpRestartStats MpSupervisorSession::restart_stats() const {
  MpRestartStats out;
  out.restarts_attempted = impl_.restarts_attempted;
  out.restarts_succeeded = impl_.restarts_succeeded;
  out.restart_giveups = impl_.restart_giveups;
  out.roles = static_cast<uint32_t>(impl_.n_procs);
  for (int i = 0; i < impl_.n_procs; ++i) {
    const Impl::Role& r = impl_.roles[i];
    MpRoleRestartInfo& o = out.role[i];
    o.state = r.state;
    o.pid = r.pid;
    o.restarts_attempted = r.attempts;
    o.restarts_succeeded = r.successes;
    o.last_exit_code = r.last_exit_code;
    o.last_term_signal = r.last_term_signal;
    o.last_stall_age_ns = r.last_stall_age_ns;
    o.last_restart_ns = r.last_restart_ns;
  }
  return out;
}

MpControl* MpSupervisorSession::ctrl() { return impl_.ctrl; }

void MpSupervisorSession::SignalStop() {
  impl_.stopping = true;
  if (impl_.ctrl != nullptr) {
    impl_.ctrl->stop.store(1, std::memory_order_release);
  }
}

bool MpSupervisorSession::ReapChildren() {
  bool all_ok = true;
  for (int i = 0; i < impl_.n_procs; ++i) {
    Impl::Role& r = impl_.roles[i];
    if (r.pid <= 0) {
      // Already reaped by PollRestarts (or never respawned after a death).
      if (r.state == MpRoleState::kWaitingBackoff ||
          r.state == MpRoleState::kGaveUp ||
          r.state == MpRoleState::kTerminating ||
          (r.state == MpRoleState::kStopped && !r.last_exit_ok)) {
        all_ok = false;
      }
      continue;
    }
    // A role already sent SIGTERM by the stall path may ignore it. Bound the
    // final reap: keep the kill-grace contract instead of blocking forever.
    int status = 0;
    if (r.state == MpRoleState::kTerminating) {
      bool timed_out = false;
      status = thyme::app::WaitChildTimeout(
          r.pid, cfg_.restart_kill_grace_ns + kMpWaitTimeoutNs, &timed_out);
    } else {
      status = wait_child(r.pid, cfg_.processes[i].role);
    }
    r.pid = 0;
    r.kill_deadline_ns = 0;
    if (status >= 0 && WIFEXITED(status)) {
      r.last_exit_code = WEXITSTATUS(status);
      r.last_term_signal = 0;
      r.last_exit_ok = child_ok(status);
    } else if (status >= 0 && WIFSIGNALED(status)) {
      r.last_exit_code = -1;
      r.last_term_signal = WTERMSIG(status);
      r.last_exit_ok = false;
    } else {
      r.last_exit_code = -1;
      r.last_term_signal = 0;
      r.last_exit_ok = false;
    }
    r.state = MpRoleState::kStopped;
    if (!r.last_exit_ok) all_ok = false;
  }
  return all_ok;
}

void MpSupervisorSession::Collect(uint64_t* proc_loops_out,
                                  uint64_t* cross_frames_out) {
  for (int i = 0; i < impl_.n_procs; ++i) {
    proc_loops_out[i] =
        impl_.ctrl->proc_loops[i].load(std::memory_order_acquire);
  }
  for (std::size_t i = 0; i < MpControl::kMaxCrossFrameSlots; ++i) {
    cross_frames_out[i] =
        impl_.ctrl->cross_frames[i].load(std::memory_order_acquire);
  }
}

const thyme::SharedHealthPage* MpSupervisorSession::health_page() {
  return static_cast<const thyme::SharedHealthPage*>(impl_.shm.h.data());
}

const char* MpSupervisorSession::mirror_name() const {
  return impl_.mirror_name;
}

void MpSupervisorSession::Cleanup() {
  StopStatusServer();
  impl_.shm.unmap_all();
  impl_.shm.unlink_all();
  if (impl_.record_mode) {
    ShutdownRecordMirror(cfg_);
  }
}

}  // namespace app
}  // namespace thyme
