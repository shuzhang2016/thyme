// trigger_center_main.cpp — P3/P4 host: assembles TriggerCenter (socket + FSM
// + quotas) with a TriggeredRecorder (capture engine), IdleDetector,
// rate-limited drain, and RetentionManager. TriggeredRecorder stays
// thread-free. RecorderDrain is the only thread that calls Tick or any other
// recorder method; the control loop posts that work through Call.
//
// Usage: trigger_center [mode] [--in-process] [--shm-pid N]
//         --record-manifest generated/record_manifest.h
//         <config.ini> | <base.ini> <policy.ini>
// The default forks a recorder process. Hooks cross the page in
// recorder_ipc.h and wake that process with one inherited eventfd.
// --in-process keeps center and recorder in this process.
// `--shm-pid` is the supervisor pid that owns `/c9_<pid>_rec`. It replaces
// `%pid` in shm names. It is not this process's pid.
// Signals: SIGHUP reloads trigger_sources/quotas (5.2) through the same
// legacy/layered loader as startup; SIGINT/SIGTERM exit.
//
// P4 wiring (design 6.2/6.3/6.4/6.5):
//   * ioprio: the drain thread starts at IOPRIO_CLASS_IDLE and rises to
//     best-effort while a crash capture is in flight (6.3). cgroup v2
//     io.max is deployment-side (config placeholder only).
//   * idle flush: IdleDetector with a DEMO predicate (always reports
//     condition-met — production reads the /can_chassis mirror:
//     park_brk engaged && speed < 0.1 m/s). idle_fallback and the 80%
//     pending-pressure override come from IdleDetector.
//   * retention: startup MarkIncomplete (marking incomplete packages),
//     Enforce() after each completed capture and every 5 min (6.4).
//   * ENOSPC: engine sets disk_full; host runs retention once, then
//     ClearDiskFull — the next trigger retries; a second failure re-sets
//     disk_full and the Center rejects low-priority triggers (kQuotaExceeded)
//     until retention actually frees space.

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "thyme/app/systemd_notify.h"
#include "thyme/record/idle_detector.h"
#include "thyme/record/io_priority.h"
#include "thyme/record/record_control.h"
#include "thyme/record/recorder_drain.h"
#include "thyme/record/recorder_ipc.h"
#include "thyme/record/retention.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_config.h"
#include "thyme/record/trigger_config_schema_check.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/log.h"
#include "thyme/runtime/resource_check.h"

namespace {

volatile sig_atomic_t g_stop = 0;
volatile sig_atomic_t g_reload = 0;

void OnSignal(int sig) {
  if (sig == SIGHUP)
    g_reload = 1;
  if (sig == SIGINT || sig == SIGTERM)
    g_stop = 1;
}

uint64_t MonoNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

thyme::record_control::Config ControlConfig(
    const thyme::trigger_cfg::CenterConfig &cfg) {
  thyme::record_control::Config control;
  control.allowed_uids = cfg.allowed_uids;
  control.allow_root = cfg.allow_root;
  control.require_peer_pid = cfg.require_peer_pid;
  control.allow_legacy_control = cfg.allow_legacy_control;
  control.audit_log_path = cfg.control_audit_log_path;
  control.audit_max_bytes = cfg.audit_max_bytes;
  control.audit_rotate_keep = cfg.audit_rotate_keep;
  control.audit_fsync_interval_ns = cfg.audit_fsync_interval_ns;
  return control;
}

void SleepNs(uint64_t ns) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
  nanosleep(&ts, nullptr);
}

// DEMO idle predicate: always reports the vehicle-stationary condition. Used
// only when no [chassis] section is configured; with one, the real
// chassis-mirror predicate below drives the gate.
bool DemoIdlePredicate(void * /*ctx*/, bool *source_ok) {
  *source_ok = true;
  return true;
}

struct HostCtx {
  thyme::triggered::TriggeredRecorder *rec = nullptr;
  thyme::RecorderDrain *drain = nullptr;
};

struct PayloadWatch {
  bool armed = false;
  uint64_t base = 0;
  uint64_t hook_ns = 0;
};

PayloadWatch g_payload;
std::atomic<uint64_t> g_inproc_delta_ns{0};

struct DrainTickCtx {
  thyme::triggered::TriggeredRecorder *rec = nullptr;
  thyme::recorder_ipc::Page *page = nullptr;
  bool idle_set = false;
  bool crash_boost = false;
};

void DrainTick(void *p) {
  auto *d = static_cast<DrainTickCtx *>(p);
  if (!d->idle_set) {
    thyme::io::SetIoPriorityIdle();
    d->idle_set = true;
  }
  d->rec->Tick();
  if (g_payload.armed &&
      d->rec->stats().frames_written > g_payload.base) {
    const uint64_t now = MonoNs();
    if (d->page != nullptr) {
      d->page->first_payload_mono_ns.store(now, std::memory_order_release);
    } else if (g_payload.hook_ns != 0 && now >= g_payload.hook_ns) {
      g_inproc_delta_ns.store(now - g_payload.hook_ns,
                              std::memory_order_release);
    }
    g_payload.armed = false;
  }
  if (d->rec->capture_high_priority() && !d->crash_boost) {
    thyme::io::SetIoPriorityBestEffort();
    d->crash_boost = true;
  } else if (!d->rec->capture_high_priority() && d->crash_boost) {
    thyme::io::SetIoPriorityIdle();
    d->crash_boost = false;
  }
}

template <class F>
auto WithRec(void *c, F &&fn)
    -> decltype(fn(*static_cast<HostCtx *>(c)->rec)) {
  HostCtx *ctx = static_cast<HostCtx *>(c);
  if (ctx->drain == nullptr) return fn(*ctx->rec);
  return ctx->drain->Call([&] { return fn(*ctx->rec); });
}

bool RecorderReady(void *c) {
  return WithRec(c, [](thyme::triggered::TriggeredRecorder &rec) {
    return rec.ready();
  });
}
bool CaptureActive(void *c) {
  return WithRec(c, [](thyme::triggered::TriggeredRecorder &rec) {
    return rec.phase() != thyme::triggered::Phase::kArmed;
  });
}
bool StartCapture(void *c, uint64_t trigger_id, uint64_t t0, uint64_t before,
                  uint64_t after, bool force_realtime, bool high_priority,
                  const std::vector<std::string> &channels) {
  return WithRec(c, [&](thyme::triggered::TriggeredRecorder &rec) {
    g_payload.base = rec.stats().frames_written;
    g_payload.hook_ns = MonoNs();
    g_payload.armed = true;
    return rec.Trigger(trigger_id, before, after, t0, !force_realtime,
                       high_priority, channels);
  });
}
bool PreemptCapture(void *c) {
  return WithRec(c, [](thyme::triggered::TriggeredRecorder &rec) {
    return rec.Preempt();
  });
}
uint64_t PendingBytes(void *c) {
  return WithRec(c, [](thyme::triggered::TriggeredRecorder &rec) {
    return rec.pending_bytes();
  });
}
uint64_t EvictOldest(void *c, uint64_t bytes) {
  return WithRec(c, [&](thyme::triggered::TriggeredRecorder &rec) {
    return rec.EvictOldest(bytes);
  });
}
bool DiskFull(void *c) {
  return WithRec(c, [](thyme::triggered::TriggeredRecorder &rec) {
    return rec.disk_full();
  });
}

// Retention exclusion list: the in-flight trigger package and the active
// continuous slice must never be deleted mid-write.
std::vector<std::string> ActiveExcludes(
    const thyme::triggered::TriggeredRecorder &rec) {
  std::vector<std::string> out;
  if (rec.phase() != thyme::triggered::Phase::kArmed) {
    out.push_back(rec.last_package_dir());
  }
  if (rec.continuous_armed() && !rec.current_slice_dir().empty()) {
    out.push_back(rec.current_slice_dir());
  }
  return out;
}

void CheckUnixDgramQueue(const thyme::trigger_cfg::CenterConfig &cfg) {
#if defined(__linux__)
  const uint64_t required = thyme::trigger_cfg::TriggerDatagramsPerTick(cfg);
  std::FILE *file = std::fopen("/proc/sys/net/unix/max_dgram_qlen", "re");
  unsigned long long qlen = 0;
  if (file == nullptr || std::fscanf(file, "%llu", &qlen) != 1) {
    if (file != nullptr)
      std::fclose(file);
    std::fprintf(stderr,
                 "trigger_center: WARNING: cannot read "
                 "net.unix.max_dgram_qlen; require >= %llu for configured "
                 "max_trigger_rate_hz x tick_ms\n",
                 static_cast<unsigned long long>(required));
    return;
  }
  std::fclose(file);
  if (qlen < required) {
    std::fprintf(stderr,
                 "trigger_center: WARNING: net.unix.max_dgram_qlen=%llu is "
                 "below trigger queue requirement %llu; persistent drops are "
                 "possible. Suggest: sysctl -w net.unix.max_dgram_qlen=%llu\n",
                 qlen, static_cast<unsigned long long>(required),
                 static_cast<unsigned long long>(required));
  } else {
    std::fprintf(
        stderr,
        "trigger_center: net.unix.max_dgram_qlen=%llu (required %llu)\n", qlen,
        static_cast<unsigned long long>(required));
  }
#else
  (void)cfg;
#endif
}

// OPS-07: startup resource self-check. Returns 0 when the host may start and 3
// when a hard requirement is unmet under the default "enforce" policy. The
// human summary always goes to stderr; `json` adds the machine-readable
// object on stdout (for CI/deployment gates).
int RunResourceCheck(const thyme::trigger_cfg::CenterConfig &cfg, bool json) {
  const auto plan = thyme::resource::PlanFromEnv(
      static_cast<uint32_t>(cfg.recorder_sources.size()),
      cfg.recorder_out_dir.c_str(), cfg.recorder_disk_quota_bytes,
      cfg.recorder_high_reserved_bytes);
  if (!plan.enabled) {
    std::fprintf(stderr, "trigger_center: resource self-check disabled "
                 "(THYME_RESOURCE_CHECK=off)\n");
    if (json)
      std::printf("{\"checked\":false}\n");
    return 0;
  }
  const thyme::resource::Report report = thyme::resource::Check(plan.limits);
  char line[768];
  (void)thyme::resource::FormatSummary(report, line, sizeof(line));
  std::fprintf(stderr, "trigger_center: %s\n", line);
  CheckUnixDgramQueue(cfg);
  if (json) {
    char obj[1024];
    (void)thyme::resource::FormatJson(report, obj, sizeof(obj));
    std::printf("%s\n", obj);
  }
  if (!report.fatal) return 0;
  std::fprintf(
      stderr,
      "trigger_center: resource self-check FAILED: a hard host requirement "
      "is unmet (raise RLIMIT_NOFILE, free disk or /dev/shm, or set "
      "THYME_RESOURCE_CHECK=warn to start anyway); see docs/guides/runbook.md "
      "§7\n");
  return plan.enforce ? 3 : 0;
}

bool LoadHostConfig(const char *base_path, const char *policy_path,
                    uint32_t shm_pid, const char *const *manifest,
                    std::size_t manifest_n,
                    thyme::trigger_cfg::CenterConfig *target, std::string *err,
                    std::string *warn) {
  const bool loaded =
      policy_path != nullptr
          ? thyme::trigger_cfg::LoadLayeredCenterConfig(
                base_path, policy_path, target, err, warn)
          : thyme::trigger_cfg::LoadCenterConfig(base_path, target, err, warn);
  return loaded && thyme::trigger_cfg::BindShmPid(target, shm_pid, err) &&
         thyme::trigger_cfg::ValidateManifestSubset(
             *target, manifest_n == 0 ? nullptr : manifest, manifest_n, err);
}

std::vector<std::string> SplitChannelText(const char *text, uint32_t n) {
  std::vector<std::string> out;
  if (text == nullptr || n == 0) return out;
  const char *begin = text;
  const char *end = text + n;
  for (const char *p = text; p <= end; ++p) {
    if (p == end || *p == '\n') {
      if (p != begin) out.emplace_back(begin, static_cast<std::size_t>(p - begin));
      begin = p + 1;
    }
  }
  return out;
}

int TickMs(uint64_t tick_ns) {
  uint64_t ms = tick_ns / 1000000ull;
  if (ms < 1) ms = 1;
  if (ms > 1000) ms = 1000;
  return static_cast<int>(ms);
}

struct ChildProc {
  pid_t pid = -1;
  bool reaped = false;
  int status = 0;
  ~ChildProc() {
    if (pid <= 0 || reaped) return;
    kill(pid, SIGTERM);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    reaped = true;
  }
};

struct SplitCtx {
  thyme::recorder_ipc::Page *page = nullptr;
  int wake_fd = -1;
  ChildProc *proc = nullptr;
};

bool PostCommand(SplitCtx *ctx, uint32_t op, uint32_t flags, uint64_t id,
                 uint64_t before_ns, uint64_t after_ns, uint64_t t0_ns,
                 uint64_t evict_bytes,
                 const std::vector<std::string> &channels, uint64_t *result) {
  std::string joined;
  for (const std::string &name : channels) {
    if (!joined.empty()) joined.push_back('\n');
    if (joined.size() + name.size() > thyme::recorder_ipc::kChannelBytes) {
      return false;
    }
    joined += name;
  }
  thyme::recorder_ipc::Page *page = ctx->page;
  const uint64_t seq = page->cmd_seq.load(std::memory_order_relaxed) + 1;
  page->cmd_op = op;
  page->cmd_flags = flags;
  page->cmd_trigger_id = id;
  page->cmd_before_ns = before_ns;
  page->cmd_after_ns = after_ns;
  page->cmd_t0_ns = t0_ns;
  page->cmd_evict_bytes = evict_bytes;
  page->cmd_channel_bytes = static_cast<uint32_t>(joined.size());
  if (!joined.empty()) {
    std::memcpy(page->cmd_channels, joined.data(), joined.size());
  }
  const uint64_t stamp = MonoNs();
  page->cmd_stamp_ns.store(stamp, std::memory_order_relaxed);
  if (op == thyme::recorder_ipc::kOpTrigger) {
    page->trigger_hook_mono_ns.store(stamp, std::memory_order_relaxed);
  }
  page->cmd_seq.store(seq, std::memory_order_release);
  const uint64_t one = 1;
  if (write(ctx->wake_fd, &one, sizeof(one)) < 0 && errno != EAGAIN) {
    return false;
  }
  while (page->cmd_done.load(std::memory_order_acquire) != seq) {
    if (g_stop != 0) return false;
    int status = 0;
    pid_t got = -1;
    do {
      got = waitpid(ctx->proc->pid, &status, WNOHANG);
    } while (got < 0 && errno == EINTR);
    if (got == ctx->proc->pid) {
      ctx->proc->status = status;
      ctx->proc->reaped = true;
      return false;
    }
    SleepNs(200000);
  }
  if (result != nullptr) {
    *result = page->cmd_result.load(std::memory_order_acquire);
  }
  return true;
}

bool SplitReady(void *c) {
  return static_cast<SplitCtx *>(c)->page->ready.load(
             std::memory_order_acquire) != 0;
}
bool SplitActive(void *c) {
  return static_cast<SplitCtx *>(c)->page->capture_active.load(
             std::memory_order_acquire) != 0;
}
bool SplitStart(void *c, uint64_t trigger_id, uint64_t t0, uint64_t before,
                uint64_t after, bool force_realtime, bool high_priority,
                const std::vector<std::string> &channels) {
  uint32_t flags = 0;
  if (force_realtime) flags |= thyme::recorder_ipc::kFlagRealtime;
  if (high_priority) flags |= thyme::recorder_ipc::kFlagHigh;
  uint64_t result = 0;
  if (!PostCommand(static_cast<SplitCtx *>(c), thyme::recorder_ipc::kOpTrigger,
                   flags, trigger_id, before, after, t0, 0, channels,
                   &result)) {
    return false;
  }
  return result != 0;
}
bool SplitPreempt(void *c) {
  uint64_t result = 0;
  if (!PostCommand(static_cast<SplitCtx *>(c), thyme::recorder_ipc::kOpPreempt,
                   0, 0, 0, 0, 0, 0, {}, &result)) {
    return false;
  }
  return result != 0;
}
uint64_t SplitPending(void *c) {
  return static_cast<SplitCtx *>(c)->page->pending_bytes.load(
      std::memory_order_acquire);
}
uint64_t SplitEvict(void *c, uint64_t bytes) {
  uint64_t result = 0;
  if (!PostCommand(static_cast<SplitCtx *>(c), thyme::recorder_ipc::kOpEvict, 0,
                   0, 0, 0, 0, bytes, {}, &result)) {
    return 0;
  }
  return result;
}
bool SplitDisk(void *c) {
  return static_cast<SplitCtx *>(c)->page->disk_full.load(
             std::memory_order_acquire) != 0;
}

thyme::recorder_ipc::Page *CreateIpc() {
  char name[32];
  std::snprintf(name, sizeof(name), "/tcipc_%ld", static_cast<long>(getpid()));
  shm_unlink(name);
  const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return nullptr;
  const off_t bytes = static_cast<off_t>(sizeof(thyme::recorder_ipc::Page));
  if (ftruncate(fd, bytes) != 0) {
    close(fd);
    shm_unlink(name);
    return nullptr;
  }
  void *mem = mmap(nullptr, sizeof(thyme::recorder_ipc::Page),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  shm_unlink(name);
  if (mem == MAP_FAILED) return nullptr;
  return new (mem) thyme::recorder_ipc::Page();
}

void ApplyCommand(thyme::recorder_ipc::Page *page, thyme::RecorderDrain *drain,
                  thyme::triggered::TriggeredRecorder *rec) {
  const uint64_t seq = page->cmd_seq.load(std::memory_order_acquire);
  if (seq == page->cmd_done.load(std::memory_order_relaxed)) return;
  const uint32_t op = page->cmd_op;
  const uint32_t flags = page->cmd_flags;
  const uint64_t id = page->cmd_trigger_id;
  const uint64_t before_ns = page->cmd_before_ns;
  const uint64_t after_ns = page->cmd_after_ns;
  const uint64_t t0_ns = page->cmd_t0_ns;
  const uint64_t evict_bytes = page->cmd_evict_bytes;
  uint32_t nbytes = page->cmd_channel_bytes;
  if (nbytes > thyme::recorder_ipc::kChannelBytes) nbytes = 0;
  char channel_buf[thyme::recorder_ipc::kChannelBytes];
  if (nbytes != 0) std::memcpy(channel_buf, page->cmd_channels, nbytes);
  const std::vector<std::string> channels = SplitChannelText(channel_buf, nbytes);
  uint64_t result = 0;
  drain->Call([&] {
    if (op == thyme::recorder_ipc::kOpTrigger) {
      g_payload.base = rec->stats().frames_written;
      g_payload.hook_ns = 0;
      g_payload.armed = true;
      page->first_payload_mono_ns.store(0, std::memory_order_relaxed);
      const bool ok = rec->Trigger(id, before_ns, after_ns, t0_ns,
                                   (flags & thyme::recorder_ipc::kFlagRealtime) == 0,
                                   (flags & thyme::recorder_ipc::kFlagHigh) != 0,
                                   channels);
      result = ok ? 1 : 0;
      if (!ok) g_payload.armed = false;
    } else if (op == thyme::recorder_ipc::kOpPreempt) {
      result = rec->Preempt() ? 1 : 0;
    } else if (op == thyme::recorder_ipc::kOpEvict) {
      result = rec->EvictOldest(evict_bytes);
    }
    const bool active = rec->phase() != thyme::triggered::Phase::kArmed;
    page->capture_active.store(active ? 1u : 0u, std::memory_order_release);
    page->pending_bytes.store(rec->pending_bytes(), std::memory_order_relaxed);
    page->disk_full.store(rec->disk_full() ? 1u : 0u, std::memory_order_relaxed);
  });
  page->cmd_result.store(result, std::memory_order_relaxed);
  page->cmd_done.store(seq, std::memory_order_release);
}

int RecorderProcess(thyme::trigger_cfg::CenterConfig *cfg,
                    thyme::recorder_ipc::Page *page, int wake_fd,
                    const char *base_path, const char *policy_path,
                    uint32_t shm_pid, const char *const *manifest,
                    std::size_t manifest_n) {
  thyme::triggered::Config rcfg;
  rcfg.sources = cfg->recorder_sources;
  rcfg.out_dir = cfg->recorder_out_dir;
  rcfg.pending_bytes_quota = cfg->recorder_pending_quota;
  rcfg.io_rate_limit_bps = cfg->recorder_io_rate_limit_bps;
  rcfg.signing_key_path = cfg->signing_key_path;
  rcfg.manifest_mode = cfg->manifest_mode;
  thyme::triggered::TriggeredRecorder rec;
  if (!rec.Init(rcfg)) {
    if (rec.init_lease_result() == thyme::SessionLeaseResult::kBusy) {
      std::fprintf(stderr, "trigger_center: recorder Init failed: session "
                           "lease kBusy\n");
    } else {
      std::fprintf(stderr,
                   "trigger_center: recorder Init failed (sources up?)\n");
    }
    return 1;
  }
  const int rrc = RunResourceCheck(*cfg, false);
  if (rrc != 0) {
    rec.Shutdown();
    return rrc;
  }

  thyme::retention::Manager retention;
  thyme::retention::Config retain_cfg;
  retain_cfg.out_dir = cfg->recorder_out_dir;
  retain_cfg.disk_quota_bytes = cfg->recorder_disk_quota_bytes;
  retain_cfg.high_reserved_bytes = cfg->recorder_high_reserved_bytes;
  thyme::retention::ParseIncompletePolicy(cfg->recorder_incomplete_policy,
                                          &retain_cfg.incomplete_policy);
  retain_cfg.incomplete_max_age_ns = cfg->recorder_incomplete_max_age_ns;
  retain_cfg.signing_key_path = cfg->signing_key_path;
  retain_cfg.manifest_policy.manifest_mode = rec.integrity_mode();
  for (const thyme::triggered::SourceConfig &source : cfg->recorder_sources) {
    if (source.kind != thyme::triggered::SourceConfig::Kind::kPinnedSlot) continue;
    retain_cfg.manifest_policy.bulk_modes.emplace_back(
        source.channel_name.empty() ? "channel" : source.channel_name,
        thyme::triggered::EffectiveManifestMode(source, cfg->manifest_mode));
  }
  if (retention.Init(retain_cfg)) {
    const uint32_t marked = retention.MarkIncomplete();
    if (marked > 0) {
      std::fprintf(stderr,
                   "trigger_center: startup recovery: %u incomplete "
                   "package(s) marked\n",
                   marked);
    }
  }

  thyme::idle::Config idle_cfg;
  idle_cfg.idle_hold_ns = cfg->recorder_idle_hold_ns;
  idle_cfg.fallback_assume_idle = cfg->recorder_idle_fallback_idle;
  thyme::idle::ChassisMirrorPredicate chassis_pred;
  thyme::idle::Predicate pred_fn = DemoIdlePredicate;
  void *pred_ctx = nullptr;
  if (cfg->chassis_present) {
    if (!chassis_pred.Init(cfg->chassis)) {
      std::fprintf(stderr,
                   "trigger_center: chassis mirror %s unreadable; idle "
                   "predicate reports source_ok=false (idle_fallback "
                   "applies)\n",
                   cfg->chassis.page_shm.c_str());
    }
    pred_fn = thyme::idle::ChassisMirrorPredicate::Pred;
    pred_ctx = &chassis_pred;
  }
  thyme::idle::IdleDetector idle(idle_cfg, pred_fn, pred_ctx);

  thyme::record_control::Server control;
  thyme::triggered::ContinuousConfig ccfg;
  ccfg.channels = cfg->cont_channels;
  ccfg.slice_bytes = cfg->cont_slice_bytes;
  ccfg.slice_ns = cfg->cont_slice_ns;
  ccfg.wait_idle = cfg->cont_wait_idle;
  if (!control.Open(cfg->recorder_socket.c_str(), ControlConfig(*cfg))) {
    std::fprintf(stderr, "trigger_center: control socket %s failed\n",
                 cfg->recorder_socket.c_str());
    rec.Shutdown();
    return 1;
  }
  auto validate_plan = [&](bool at_startup) {
    if (!cfg->cont_section_present) return true;
    thyme::trigger_cfg::ContinuousPlanReport plan;
    std::string plan_warn, perr;
    if (!thyme::trigger_cfg::ValidateContinuousPlan(*cfg, &plan, &plan_warn,
                                                    &perr)) {
      std::fprintf(stderr, "trigger_center: continuous plan: %s\n",
                   perr.c_str());
      return false;
    }
    std::fprintf(stderr,
                 "trigger_center: continuous plan (%s): sustained %llu B/s "
                 "(%llu MB/h), quota coverage %.2f h\n",
                 at_startup ? "startup" : "arm",
                 static_cast<unsigned long long>(plan.sustained_bps),
                 static_cast<unsigned long long>(plan.disk_bytes_per_hour /
                                                 (1024 * 1024)),
                 plan.quota_coverage_hours);
    if (!plan_warn.empty()) {
      std::fprintf(stderr, "trigger_center: WARNING: %s\n", plan_warn.c_str());
    }
    return true;
  };
  if (cfg->cont_auto_arm) {
    if (!validate_plan(true) || !rec.ArmContinuous(ccfg)) {
      std::fprintf(stderr, "trigger_center: continuous auto_arm failed\n");
      rec.Shutdown();
      return 1;
    }
    std::fprintf(stderr, "trigger_center: continuous mode armed\n");
  }
  if (!cfg->log_service_file.empty()) {
    thyme::log::FileSinkConfig log_cfg;
    log_cfg.path = cfg->log_service_file.c_str();
    log_cfg.max_bytes = cfg->log_service_max_bytes;
    log_cfg.keep = cfg->log_service_rotate_keep;
    log_cfg.account = true;
    if (!thyme::log::OpenFileSink(log_cfg)) {
      std::fprintf(stderr, "trigger_center: service log %s is not writable\n",
                   cfg->log_service_file.c_str());
      rec.Shutdown();
      return 1;
    }
    THYME_LOG_INFO("trigger_center: service log configured");
  }

  DrainTickCtx drain_ctx;
  drain_ctx.rec = &rec;
  drain_ctx.page = page;
  thyme::RecorderDrain drain(&DrainTick, &drain_ctx, cfg->tick_ns);
  drain.Start();
  page->ready.store(1, std::memory_order_release);
  std::fprintf(stderr, "trigger_center: recorder attached\n");

  uint64_t last_capture_done_count = 0;
  uint64_t last_retention_ns = MonoNs();
  while (g_stop == 0) {
    if (g_reload != 0) {
      g_reload = 0;
      thyme::trigger_cfg::CenterConfig fresh;
      std::string rerr, rwarn;
      if (LoadHostConfig(base_path, policy_path, shm_pid, manifest, manifest_n,
                         &fresh, &rerr, &rwarn)) {
        if (!rwarn.empty()) {
          std::fprintf(stderr, "trigger_center: WARNING: %s\n", rwarn.c_str());
        }
        if (thyme::trigger_cfg::ReloadRequiresRestart(*cfg, fresh)) {
          std::fprintf(stderr,
                       "trigger_center: reload rejected: continuous geometry, "
                       "[recorder] policy, channel layout/params or [chassis] "
                       "change requires restart\n");
        } else if (!control.ReloadConfig(ControlConfig(fresh))) {
          std::fprintf(stderr,
                       "trigger_center: reload rejected: control audit "
                       "settings require restart\n");
        } else {
          *cfg = fresh;
          std::fprintf(stderr, "trigger_center: config reloaded\n");
        }
      } else {
        std::fprintf(stderr, "trigger_center: reload rejected: %s\n",
                     rerr.c_str());
      }
    }

    bool busy = false;
    thyme::record_control::StatusSnapshot snapshot;
    drain.Call([&] {
      const thyme::triggered::Stats &stats = rec.stats();
      const bool capturing = rec.phase() != thyme::triggered::Phase::kArmed;
      busy = capturing || rec.continuous_armed();
      page->capture_active.store(capturing ? 1u : 0u, std::memory_order_release);
      page->pending_bytes.store(rec.pending_bytes(), std::memory_order_relaxed);
      page->disk_full.store(rec.disk_full() ? 1u : 0u, std::memory_order_relaxed);
      page->phase.store(static_cast<uint32_t>(rec.phase()),
                        std::memory_order_relaxed);
      page->frames_written.store(stats.frames_written, std::memory_order_relaxed);
      page->captures_done.store(stats.captures_done, std::memory_order_relaxed);
      snapshot.captures_done = stats.captures_done;
      snapshot.slices_done = rec.continuous_stats().slices_done;
      snapshot.pending_bytes = rec.pending_bytes();
      snapshot.disk_full = rec.disk_full();
      snapshot.integrity_mode = rec.integrity_mode();
      snapshot.triggers_rejected = stats.triggers_rejected;
      snapshot.descriptors_queued = stats.descriptors_queued;
      snapshot.pending_evicted = stats.pending_evicted;
      snapshot.overrun_before_queue = stats.overrun_before_queue;
      snapshot.samples_written = stats.samples_written;
      snapshot.frames_written = stats.frames_written;
      snapshot.holes_written = stats.holes_written;
      snapshot.torn_reads = stats.torn_reads;
      snapshot.harvest_retries = stats.harvest_retries;
      snapshot.session_reattaches = stats.session_reattaches;
      snapshot.source_errors = stats.source_errors;
      snapshot.disk_errors = stats.disk_errors;
      snapshot.sync_errors = stats.sync_errors;
    });
    snapshot.audit_drops =
        page->audit_drops.load(std::memory_order_relaxed);
    snapshot.fsm_event_drops =
        page->fsm_event_drops.load(std::memory_order_relaxed);
    snapshot.service_records_written = thyme::log::records_written();
    snapshot.service_records_suppressed = thyme::log::records_suppressed();
    snapshot.service_records_dropped = thyme::log::records_dropped();
    snapshot.service_records_truncated = thyme::log::records_truncated();
    snapshot.service_rotations = thyme::log::rotations();
    snapshot.service_sink_open = thyme::log::FileSinkOpen();
    control.SetStatusSnapshot(snapshot);

    thyme::record_control::ControlCmd cmd;
    while (control.Poll(cmd)) {
      if (cmd == thyme::record_control::ControlCmd::kArm) {
        const bool armed = drain.Call([&] { return rec.continuous_armed(); });
        if (!armed && validate_plan(false) &&
            drain.Call([&] { return rec.ArmContinuous(ccfg); })) {
          std::fprintf(stderr, "trigger_center: continuous armed\n");
        }
      } else if (cmd == thyme::record_control::ControlCmd::kDisarm) {
        uint64_t slices = 0;
        const bool was_armed = drain.Call([&] {
          if (!rec.continuous_armed()) return false;
          rec.DisarmContinuous();
          slices = rec.continuous_stats().slices_done;
          return true;
        });
        if (was_armed) {
          std::fprintf(stderr,
                       "trigger_center: continuous disarmed, %llu slices\n",
                       static_cast<unsigned long long>(slices));
        }
      } else if (cmd == thyme::record_control::ControlCmd::kStatus) {
        uint64_t captures = 0;
        uint64_t slices = 0;
        uint64_t pending = 0;
        uint64_t sync_errors = 0;
        bool disk_full = false;
        drain.Call([&] {
          captures = rec.stats().captures_done;
          slices = rec.continuous_stats().slices_done;
          pending = rec.pending_bytes();
          disk_full = rec.disk_full();
          sync_errors = rec.stats().sync_errors;
        });
        std::fprintf(stderr,
                     "trigger_center: captures=%llu slices=%llu pending=%llu "
                     "disk_full=%d sync_errors=%llu\n",
                     static_cast<unsigned long long>(captures),
                     static_cast<unsigned long long>(slices),
                     static_cast<unsigned long long>(pending),
                     disk_full ? 1 : 0,
                     static_cast<unsigned long long>(sync_errors));
      }
    }

    drain.Call([&] {
      const double ratio =
          cfg->recorder_pending_quota > 0
              ? static_cast<double>(rec.pending_bytes()) /
                    static_cast<double>(cfg->recorder_pending_quota)
              : 0.0;
      const bool flush_now = idle.ShouldFlush(MonoNs(), ratio);
      if (rec.phase() == thyme::triggered::Phase::kSettle && flush_now) {
        rec.AllowFlush();
      }
      rec.SetContinuousGate(flush_now);
    });

    std::vector<std::string> disk_excludes;
    const bool disk_full = drain.Call([&] {
      if (!rec.disk_full()) return false;
      disk_excludes = ActiveExcludes(rec);
      return true;
    });
    if (disk_full) {
      std::fprintf(stderr, "trigger_center: disk full during drain; running "
                           "retention and re-arming\n");
      retention.Enforce(disk_excludes);
      drain.Call([&] { rec.ClearDiskFull(); });
    }

    const uint64_t now = MonoNs();
    std::vector<std::string> retain_excludes;
    bool run_retention = false;
    drain.Call([&] {
      const uint64_t done = rec.stats().captures_done;
      if (done != last_capture_done_count ||
          now - last_retention_ns >= 300000000000ull) {
        last_capture_done_count = done;
        run_retention = true;
        retain_excludes = ActiveExcludes(rec);
      }
    });
    if (run_retention) {
      last_retention_ns = now;
      const uint64_t freed = retention.Enforce(retain_excludes);
      if (freed > 0) {
        std::fprintf(stderr, "trigger_center: retention freed %llu bytes\n",
                     static_cast<unsigned long long>(freed));
      }
    }

    pollfd pfd {};
    pfd.fd = wake_fd;
    pfd.events = POLLIN;
    const int timeout_ms = busy ? TickMs(cfg->tick_ns) : 100;
    int pr = 0;
    do {
      pr = poll(&pfd, 1, timeout_ms);
    } while (pr < 0 && errno == EINTR && g_stop == 0);
    if (pr > 0) {
      const uint64_t seen = MonoNs();
      uint64_t count = 0;
      if (read(wake_fd, &count, sizeof(count)) < 0 && errno != EAGAIN) {
        std::fprintf(stderr, "trigger_center: eventfd read failed\n");
      }
      const uint64_t cmd_seq = page->cmd_seq.load(std::memory_order_acquire);
      const uint64_t done = page->cmd_done.load(std::memory_order_relaxed);
      const uint64_t stamp = page->cmd_stamp_ns.load(std::memory_order_relaxed);
      if (cmd_seq != done && page->cmd_op == thyme::recorder_ipc::kOpTrigger &&
          stamp != 0 && seen >= stamp) {
        page->wake_last_ns.store(seen - stamp, std::memory_order_relaxed);
      }
    }
    ApplyCommand(page, &drain, &rec);
  }

  drain.Stop();
  control.Close();
  if (rec.continuous_armed()) rec.DisarmContinuous();
  rec.Shutdown();
  thyme::log::CloseFileSink();
  return 0;
}

int ChildStatusCode(int status) {
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 1;
}

int RunSplit(const thyme::trigger_cfg::CenterConfig &cfg_in,
             const char *base_path, const char *policy_path, uint32_t shm_pid,
             const char *const *manifest, std::size_t manifest_n) {
  thyme::trigger_cfg::CenterConfig cfg = cfg_in;
  thyme::recorder_ipc::Page *page = CreateIpc();
  if (page == nullptr) {
    std::fprintf(stderr, "trigger_center: recorder ipc page failed\n");
    return 1;
  }
  const int wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd < 0) {
    std::fprintf(stderr, "trigger_center: eventfd failed\n");
    munmap(page, sizeof(*page));
    return 1;
  }
  struct sigaction sa {};
  sa.sa_handler = OnSignal;
  sigaction(SIGHUP, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  ChildProc proc;
  proc.pid = fork();
  if (proc.pid < 0) {
    std::fprintf(stderr, "trigger_center: fork failed\n");
    close(wake_fd);
    munmap(page, sizeof(*page));
    return 1;
  }
  if (proc.pid == 0) {
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() == 1) _exit(1);
    const int code = RecorderProcess(&cfg, page, wake_fd, base_path,
                                     policy_path, shm_pid, manifest, manifest_n);
    _exit(code);
  }
  std::fprintf(stderr, "trigger_center: split child %ld\n",
               static_cast<long>(proc.pid));

  SplitCtx ctx;
  ctx.page = page;
  ctx.wake_fd = wake_fd;
  ctx.proc = &proc;
  thyme::trigger_center::Hooks hooks;
  hooks.ctx = &ctx;
  hooks.recorder_ready = SplitReady;
  hooks.capture_active = SplitActive;
  hooks.start_capture = SplitStart;
  hooks.preempt_capture = SplitPreempt;
  hooks.pending_bytes = SplitPending;
  hooks.evict_oldest = SplitEvict;
  hooks.disk_full = SplitDisk;

  thyme::trigger_center::Center center;
  if (!center.Init(cfg, hooks)) {
    std::fprintf(stderr, "trigger_center: center Init failed (%s)\n",
                 cfg.socket_path.c_str());
    close(wake_fd);
    munmap(page, sizeof(*page));
    return 1;
  }

  const uint64_t ready_deadline = MonoNs() + 10000000000ULL;
  while (page->ready.load(std::memory_order_acquire) == 0) {
    int status = 0;
    pid_t got = -1;
    do {
      got = waitpid(proc.pid, &status, WNOHANG);
    } while (got < 0 && errno == EINTR);
    if (got == proc.pid) {
      proc.status = status;
      proc.reaped = true;
      close(wake_fd);
      munmap(page, sizeof(*page));
      return ChildStatusCode(status);
    }
    if (MonoNs() >= ready_deadline) {
      std::fprintf(stderr,
                   "trigger_center: recorder process did not become ready\n");
      close(wake_fd);
      munmap(page, sizeof(*page));
      return 1;
    }
    SleepNs(1000000);
  }

  std::fprintf(stderr,
               "trigger_center: listening on %s, out %s (cgroup io.max: "
               "deployment-side, not configured here)\n",
               cfg.socket_path.c_str(), cfg.recorder_out_dir.c_str());
  if (thyme::app::SystemdNotifyEnabled()) {
    (void)thyme::app::SystemdNotifyReady();
  }
  uint64_t last_watchdog_ns = 0;
  bool was_active = false;
  while (g_stop == 0) {
    if (!proc.reaped) {
      int status = 0;
      pid_t got = -1;
      do {
        got = waitpid(proc.pid, &status, WNOHANG);
      } while (got < 0 && errno == EINTR);
      if (got == proc.pid) {
        proc.status = status;
        proc.reaped = true;
        g_stop = 1;
        break;
      }
    }
    if (g_reload != 0) {
      g_reload = 0;
      thyme::trigger_cfg::CenterConfig fresh;
      std::string rerr, rwarn;
      if (LoadHostConfig(base_path, policy_path, shm_pid, manifest, manifest_n,
                         &fresh, &rerr, &rwarn)) {
        if (!rwarn.empty()) {
          std::fprintf(stderr, "trigger_center: WARNING: %s\n", rwarn.c_str());
        }
        if (thyme::trigger_cfg::ReloadRequiresRestart(cfg, fresh)) {
          std::fprintf(stderr,
                       "trigger_center: reload rejected: continuous geometry, "
                       "[recorder] policy, channel layout/params or [chassis] "
                       "change requires restart\n");
        } else {
          center.ReloadConfig(fresh);
          cfg = fresh;
          std::fprintf(stderr, "trigger_center: config reloaded\n");
          if (!proc.reaped) kill(proc.pid, SIGHUP);
        }
      } else {
        std::fprintf(stderr, "trigger_center: reload rejected: %s\n",
                     rerr.c_str());
      }
    }
    center.Tick();
    const bool active =
        page->capture_active.load(std::memory_order_acquire) != 0;
    if (was_active && !active) {
      const uint64_t wake = page->wake_last_ns.load(std::memory_order_acquire);
      const uint64_t first =
          page->first_payload_mono_ns.load(std::memory_order_acquire);
      const uint64_t hook =
          page->trigger_hook_mono_ns.load(std::memory_order_acquire);
      if (first > hook) {
        std::fprintf(stderr,
                     "trigger_center: split wake %llu ns "
                     "first_payload_delta %llu ns\n",
                     static_cast<unsigned long long>(wake),
                     static_cast<unsigned long long>(first - hook));
      }
    }
    was_active = active;
    page->audit_drops.store(center.stats().audit_drops,
                            std::memory_order_relaxed);
    page->fsm_event_drops.store(center.stats().fsm_event_drops,
                                std::memory_order_relaxed);
    const uint64_t wd_now = MonoNs();
    if (wd_now - last_watchdog_ns >= 1000000000ULL) {
      last_watchdog_ns = wd_now;
      (void)thyme::app::SystemdNotifyWatchdog();
    }
    SleepNs(cfg.tick_ns);
  }

  (void)thyme::app::SystemdNotifySend("STOPPING=1");
  if (!proc.reaped) {
    kill(proc.pid, SIGTERM);
    while (waitpid(proc.pid, &proc.status, 0) < 0 && errno == EINTR) {
    }
    proc.reaped = true;
  }
  center.Shutdown();
  const int code = ChildStatusCode(proc.status);
  close(wake_fd);
  munmap(page, sizeof(*page));
  return code;
}

}  // namespace

int main(int argc, char **argv) {
  std::setvbuf(stderr, nullptr, _IOLBF, 0);
  bool in_process = false;
  std::vector<char *> filtered;
  filtered.reserve(static_cast<std::size_t>(argc) + 1);
  for (int i = 0; i < argc; ++i) {
    if (i > 0 && std::strcmp(argv[i], "--in-process") == 0) {
      in_process = true;
      continue;
    }
    filtered.push_back(argv[i]);
  }
  filtered.push_back(nullptr);
  argc = static_cast<int>(filtered.size()) - 1;
  argv = filtered.data();
  bool check_only = false;
  bool dump_only = false;
  bool resources_only = false;
  bool resources_json = false;
  int32_t first_config = 1;
  if (argc > 1 && std::strcmp(argv[1], "--check-config") == 0) {
    check_only = true;
    first_config = 2;
  } else if (argc > 1 && std::strcmp(argv[1], "--dump-config") == 0) {
    dump_only = true;
    first_config = 2;
  } else if (argc > 1 && std::strcmp(argv[1], "--check-resources") == 0) {
    resources_only = true;
    first_config = 2;
  }
  uint32_t shm_pid = 0;
  auto take_shm_pid = [&](int32_t &idx) -> bool {
    if (idx >= argc || std::strcmp(argv[idx], "--shm-pid") != 0) return true;
    if (idx + 1 >= argc) {
      std::fprintf(stderr, "trigger_center: --shm-pid needs a supervisor pid\n");
      return false;
    }
    char *end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(argv[idx + 1], &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || value == 0 ||
        value > 0xfffffffful) {
      std::fprintf(stderr, "trigger_center: bad --shm-pid\n");
      return false;
    }
    shm_pid = static_cast<uint32_t>(value);
    idx += 2;
    return true;
  };
  if (!take_shm_pid(first_config)) return 2;
  const char *manifest_path = nullptr;
  if (first_config + 1 < argc &&
      std::strcmp(argv[first_config], "--record-manifest") == 0) {
    manifest_path = argv[first_config + 1];
    first_config += 2;
    if (!take_shm_pid(first_config)) return 2;
  }
  int32_t config_count = static_cast<int32_t>(argc) - first_config;
  if (resources_only && config_count > 1 &&
      std::strcmp(argv[argc - 1], "--json") == 0) {
    resources_json = true;
    --config_count;
  }
  if (manifest_path == nullptr || config_count < 1 || config_count > 2 ||
      (argc > 1 && argv[1][0] == '-' && first_config == 1)) {
    std::fprintf(
        stderr,
        "usage: trigger_center [--check-config|--dump-config|--check-resources] "
        "[--in-process] [--shm-pid N] --record-manifest "
        "generated/record_manifest.h <config.ini> | "
        "<base.ini> <policy.ini> [--json]\n");
    return 2;
  }
  std::vector<std::string> manifest_channels;
  std::string manifest_err;
  if (!thyme::trigger_cfg::LoadRecordManifest(manifest_path, &manifest_channels,
                                              &manifest_err)) {
    std::fprintf(stderr, "trigger_center: record manifest: %s\n", manifest_err.c_str());
    return 2;
  }
  std::vector<const char *> manifest_table;
  manifest_table.reserve(manifest_channels.size());
  for (const std::string &channel : manifest_channels) {
    manifest_table.push_back(channel.c_str());
  }
  const char *base_path = argv[first_config];
  const char *policy_path = config_count == 2 ? argv[first_config + 1] : nullptr;
  auto load_config = [&](thyme::trigger_cfg::CenterConfig *target,
                         std::string *load_err, std::string *load_warn) {
    const bool loaded = policy_path != nullptr
                            ? thyme::trigger_cfg::LoadLayeredCenterConfig(
                                  base_path, policy_path, target, load_err, load_warn)
                            : thyme::trigger_cfg::LoadCenterConfig(
                                  base_path, target, load_err, load_warn);
    return loaded &&
           thyme::trigger_cfg::BindShmPid(target, shm_pid, load_err) &&
           thyme::trigger_cfg::ValidateManifestSubset(
                         *target, manifest_table.empty() ? nullptr : manifest_table.data(),
                         manifest_table.size(), load_err);
  };
  thyme::trigger_cfg::CenterConfig cfg;
  std::string err;
  std::string warn;
  if (!load_config(&cfg, &err, &warn)) {
    std::fprintf(stderr, "trigger_center: config: %s\n", err.c_str());
    return 2;
  }
  if (!warn.empty()) {
    std::fprintf(stderr, "trigger_center: WARNING: %s\n", warn.c_str());
  }
  if (cfg.recorder_out_dir.empty() || cfg.recorder_sources.empty()) {
    std::fprintf(stderr,
                 "trigger_center: config needs [recorder] output_dir and at "
                 "least one [channel.*] section\n");
    return 2;
  }

  // 6.6 startup accounting / accounting on every Arm: rate_check runs for both
  // automatic and manual Arm. Returns false when rate_check=enforce rejects
  // the plan.
  auto validate_continuous_plan = [&cfg, check_only,
                                   dump_only](bool at_startup) {
    thyme::trigger_cfg::ContinuousPlanReport plan;
    std::string plan_warn, perr;
    if (!thyme::trigger_cfg::ValidateContinuousPlan(cfg, &plan, &plan_warn,
                                                    &perr)) {
      std::fprintf(stderr, "trigger_center: continuous plan: %s\n",
                   perr.c_str());
      return false;
    }
    std::fprintf(stderr,
                 "trigger_center: continuous plan%s: sustained %llu B/s "
                 "(%llu MB/h), quota coverage %.2f h\n",
                 (check_only || dump_only)
                     ? " (check)"
                     : (at_startup ? " (startup)" : " (arm)"),
                 static_cast<unsigned long long>(plan.sustained_bps),
                 static_cast<unsigned long long>(plan.disk_bytes_per_hour /
                                                 (1024 * 1024)),
                 plan.quota_coverage_hours);
    if (!plan_warn.empty()) {
      std::fprintf(stderr, "trigger_center: WARNING: %s\n", plan_warn.c_str());
    }
    return true;
  };

  // Validate the continuous plan whenever a [continuous] section exists (not
  // only on auto_arm): --check-config must reject a bad selector/slice even
  // for a manually armed deployment. rate_check runs again on every Arm.
  if (cfg.cont_section_present && !validate_continuous_plan(true))
    return 2;

  // OPS-07: deployment gate — read-only host capacity check without starting
  // anything. Exit 0 pass, 2 config, 3 a hard requirement is unmet.
  if (resources_only)
    return RunResourceCheck(cfg, resources_json);

  if (check_only) {
    // ARCH-13: the effective [trigger_center] key set must be declared by the
    // generated schema; a parser key added without the schema fails here.
    std::string schema_err;
    if (!thyme::trigger_cfg::ValidateConfigSchema(cfg, &schema_err)) {
      std::fprintf(stderr, "trigger_center: %s\n", schema_err.c_str());
      return 2;
    }
  }

  if (check_only || dump_only) {
    if (dump_only)
      std::fputs(thyme::trigger_cfg::DumpConfig(cfg).c_str(), stdout);
    std::printf("trigger_center: config OK\n");
    return 0;
  }

  if (!in_process) {
    return RunSplit(cfg, base_path, policy_path, shm_pid,
                    manifest_table.empty() ? nullptr : manifest_table.data(),
                    manifest_table.size());
  }

  thyme::triggered::Config rcfg;
  rcfg.sources = cfg.recorder_sources;
  rcfg.out_dir = cfg.recorder_out_dir;
  rcfg.pending_bytes_quota = cfg.recorder_pending_quota;
  rcfg.io_rate_limit_bps = cfg.recorder_io_rate_limit_bps;
  rcfg.signing_key_path = cfg.signing_key_path;  // SEC-03 (empty = unsigned)
  rcfg.manifest_mode = cfg.manifest_mode;
  thyme::triggered::TriggeredRecorder rec;
  if (!rec.Init(rcfg)) {
    if (rec.init_lease_result() == thyme::SessionLeaseResult::kBusy) {
      std::fprintf(stderr, "trigger_center: recorder Init failed: session "
                           "lease kBusy\n");
    } else {
      std::fprintf(stderr,
                   "trigger_center: recorder Init failed (sources up?)\n");
    }
    return 1;
  }

  // OPS-07: the output directory exists now (recorder Init created it), so
  // the disk leg is checkable. A fatal result stops the host before it binds
  // sockets or arms continuous capture; the default policy enforces.
  {
    const int rrc = RunResourceCheck(cfg, false);
    if (rrc != 0) {
      rec.Shutdown();
      return rrc;
    }
  }

  thyme::retention::Manager retention;
  thyme::retention::Config retain_cfg;
  retain_cfg.out_dir = cfg.recorder_out_dir;
  retain_cfg.disk_quota_bytes = cfg.recorder_disk_quota_bytes;
  retain_cfg.high_reserved_bytes = cfg.recorder_high_reserved_bytes;
  // .incomplete policy (the parser already validates the value).
  thyme::retention::ParseIncompletePolicy(cfg.recorder_incomplete_policy,
                                          &retain_cfg.incomplete_policy);
  retain_cfg.incomplete_max_age_ns = cfg.recorder_incomplete_max_age_ns;
  // REC-18: startup-recovery partial manifests are signed with the same key
  // as sealed packages (empty = unsigned).
  retain_cfg.signing_key_path = cfg.signing_key_path;
  retain_cfg.manifest_policy.manifest_mode = rec.integrity_mode();
  for (const thyme::triggered::SourceConfig &source : cfg.recorder_sources) {
    if (source.kind != thyme::triggered::SourceConfig::Kind::kPinnedSlot)
      continue;
    retain_cfg.manifest_policy.bulk_modes.emplace_back(
        source.channel_name.empty() ? "channel" : source.channel_name,
        thyme::triggered::EffectiveManifestMode(source, cfg.manifest_mode));
  }
  if (retention.Init(retain_cfg)) {
    const uint32_t marked = retention.MarkIncomplete();
    if (marked > 0) {
      std::fprintf(stderr,
                   "trigger_center: startup recovery: %u incomplete "
                   "package(s) marked\n",
                   marked);
    }
  }

  HostCtx ctx{&rec};
  thyme::trigger_center::Hooks hooks;
  hooks.ctx = &ctx;
  hooks.recorder_ready = RecorderReady;
  hooks.capture_active = CaptureActive;
  hooks.start_capture = StartCapture;
  hooks.preempt_capture = PreemptCapture;
  hooks.pending_bytes = PendingBytes;
  hooks.evict_oldest = EvictOldest;
  hooks.disk_full = DiskFull;

  thyme::trigger_center::Center center;
  if (!center.Init(cfg, hooks)) {
    std::fprintf(stderr, "trigger_center: center Init failed (%s)\n",
                 cfg.socket_path.c_str());
    return 1;
  }

  thyme::idle::Config idle_cfg;
  idle_cfg.idle_hold_ns = cfg.recorder_idle_hold_ns;
  idle_cfg.fallback_assume_idle = cfg.recorder_idle_fallback_idle;
  // If a [chassis] section is configured, use the real chassis-mirror
  // predicate; otherwise the demo predicate is always true.
  thyme::idle::ChassisMirrorPredicate chassis_pred;
  thyme::idle::Predicate pred_fn = DemoIdlePredicate;
  void *pred_ctx = nullptr;
  if (cfg.chassis_present) {
    if (!chassis_pred.Init(cfg.chassis)) {
      std::fprintf(stderr,
                   "trigger_center: chassis mirror %s unreadable; idle "
                   "predicate reports source_ok=false (idle_fallback "
                   "applies)\n",
                   cfg.chassis.page_shm.c_str());
      // Still attach the real predicate: fallback semantics are handled by
      // IdleDetector.
    }
    pred_fn = thyme::idle::ChassisMirrorPredicate::Pred;
    pred_ctx = &chassis_pred;
  }
  thyme::idle::IdleDetector idle(idle_cfg, pred_fn, pred_ctx);

  // 6.6 control plane: record_control keeps its existing Arm/Disarm/Status
  // semantics and is mounted on the recorder socket (separate from the
  // trigger socket), driving continuous capture.
  thyme::record_control::Server control;
  thyme::triggered::ContinuousConfig ccfg;
  ccfg.channels = cfg.cont_channels;
  ccfg.slice_bytes = cfg.cont_slice_bytes;
  ccfg.slice_ns = cfg.cont_slice_ns;
  ccfg.wait_idle = cfg.cont_wait_idle;
  if (!control.Open(cfg.recorder_socket.c_str(), ControlConfig(cfg))) {
    std::fprintf(stderr, "trigger_center: control socket %s failed\n",
                 cfg.recorder_socket.c_str());
    return 1;
  }
  if (cfg.cont_auto_arm) {
    // rate_check already ran at startup (validate_continuous_plan(true)).
    if (!rec.ArmContinuous(ccfg)) {
      std::fprintf(stderr, "trigger_center: continuous auto_arm failed\n");
      return 1;
    }
    std::fprintf(stderr, "trigger_center: continuous mode armed\n");
  }

  if (!cfg.log_service_file.empty()) {
    thyme::log::FileSinkConfig log_cfg;
    log_cfg.path = cfg.log_service_file.c_str();
    log_cfg.max_bytes = cfg.log_service_max_bytes;
    log_cfg.keep = cfg.log_service_rotate_keep;
    log_cfg.account = true;
    if (!thyme::log::OpenFileSink(log_cfg)) {
      std::fprintf(stderr, "trigger_center: service log %s is not writable\n",
                   cfg.log_service_file.c_str());
      return 1;
    }
    THYME_LOG_INFO("trigger_center: service log configured");
  }

  struct sigaction sa {};
  sa.sa_handler = OnSignal;
  sigaction(SIGHUP, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  std::fprintf(stderr,
               "trigger_center: listening on %s, out %s (cgroup io.max: "
               "deployment-side, not configured here)\n",
               cfg.socket_path.c_str(), cfg.recorder_out_dir.c_str());

  // OPS-11: the unit is ready once both sockets are bound, the recorder has
  // attached every configured source and continuous mode (if any) is armed.
  // Sending READY before that would make Type=notify start consumers against
  // a center that still rejects triggers.
  if (thyme::app::SystemdNotifyEnabled()) {
    (void)thyme::app::SystemdNotifyReady();
  }
  uint64_t last_watchdog_ns = 0;

  uint64_t last_capture_done_count = 0;
  uint64_t last_retention_ns = MonoNs();
  // 6.3: Tick, ioprio, and every other recorder call run on the drain thread.
  // drain.Call posts the job and waits; the control thread does not enter it.
  DrainTickCtx drain_ctx;
  drain_ctx.rec = &rec;
  thyme::RecorderDrain drain(&DrainTick, &drain_ctx, cfg.tick_ns);
  ctx.drain = &drain;
  drain.Start();
  while (g_stop == 0) {
    if (g_reload != 0) {
      g_reload = 0;
      thyme::trigger_cfg::CenterConfig fresh;
      std::string rerr, rwarn;
      if (load_config(&fresh, &rerr, &rwarn)) {
        if (!rwarn.empty()) {
          std::fprintf(stderr, "trigger_center: WARNING: %s\n", rwarn.c_str());
        }
        if (thyme::trigger_cfg::ReloadRequiresRestart(cfg, fresh)) {
          std::fprintf(stderr,
                       "trigger_center: reload rejected: continuous geometry, "
                       "[recorder] policy, channel layout/params or [chassis] "
                       "change requires restart\n");
        } else if (!control.ReloadConfig(ControlConfig(fresh))) {
          std::fprintf(stderr,
                       "trigger_center: reload rejected: control audit "
                       "settings require restart\n");
        } else {
          center.ReloadConfig(fresh);
          cfg = fresh;
          std::fprintf(stderr, "trigger_center: config reloaded\n");
        }
      } else {
        std::fprintf(stderr, "trigger_center: reload rejected: %s\n",
                     rerr.c_str());
      }
    }
    center.Tick();

    // OPS-11: feed systemd's watchdog from the main loop, at most once per
    // second (the loop itself runs at ~1 kHz). A wedged loop stops feeding
    // and systemd's SIGABRT/restart path takes over; a broken NOTIFY_SOCKET
    // is not fatal (the shim contract: lose the notification, not the data).
    const uint64_t wd_now = MonoNs();
    if (wd_now - last_watchdog_ns >= 1000000000ULL) {
      last_watchdog_ns = wd_now;
      (void)thyme::app::SystemdNotifyWatchdog();
    }
    const uint64_t inproc_delta =
        g_inproc_delta_ns.exchange(0, std::memory_order_acquire);
    if (inproc_delta != 0) {
      std::fprintf(stderr,
                   "trigger_center: inproc first_payload_delta %llu ns\n",
                   static_cast<unsigned long long>(inproc_delta));
    }

    // Continuous-capture control plane (record_control Arm/Disarm/Status; the
    // Center does not get involved).
    {
      thyme::record_control::StatusSnapshot snapshot;
      drain.Call([&] {
        const thyme::triggered::Stats &stats = rec.stats();
        snapshot.captures_done = stats.captures_done;
        snapshot.slices_done = rec.continuous_stats().slices_done;
        snapshot.pending_bytes = rec.pending_bytes();
        snapshot.disk_full = rec.disk_full();
        snapshot.integrity_mode = rec.integrity_mode();
        snapshot.triggers_rejected = stats.triggers_rejected;
        snapshot.descriptors_queued = stats.descriptors_queued;
        snapshot.pending_evicted = stats.pending_evicted;
        snapshot.overrun_before_queue = stats.overrun_before_queue;
        snapshot.samples_written = stats.samples_written;
        snapshot.frames_written = stats.frames_written;
        snapshot.holes_written = stats.holes_written;
        snapshot.torn_reads = stats.torn_reads;
        snapshot.harvest_retries = stats.harvest_retries;
        snapshot.session_reattaches = stats.session_reattaches;
        snapshot.source_errors = stats.source_errors;
        snapshot.disk_errors = stats.disk_errors;
        snapshot.sync_errors = stats.sync_errors;
      });
      snapshot.audit_drops = center.stats().audit_drops;
      snapshot.fsm_event_drops = center.stats().fsm_event_drops;
      snapshot.service_records_written = thyme::log::records_written();
      snapshot.service_records_suppressed = thyme::log::records_suppressed();
      snapshot.service_records_dropped = thyme::log::records_dropped();
      snapshot.service_records_truncated = thyme::log::records_truncated();
      snapshot.service_rotations = thyme::log::rotations();
      snapshot.service_sink_open = thyme::log::FileSinkOpen();
      control.SetStatusSnapshot(snapshot);

      thyme::record_control::ControlCmd cmd;
      while (control.Poll(cmd)) {
        if (cmd == thyme::record_control::ControlCmd::kArm) {
          const bool armed =
              drain.Call([&] { return rec.continuous_armed(); });
          if (!armed && validate_continuous_plan(false) &&
              drain.Call([&] { return rec.ArmContinuous(ccfg); })) {
            std::fprintf(stderr, "trigger_center: continuous armed\n");
          }
        } else if (cmd == thyme::record_control::ControlCmd::kDisarm) {
          uint64_t slices = 0;
          const bool was_armed = drain.Call([&] {
            if (!rec.continuous_armed()) return false;
            rec.DisarmContinuous();
            slices = rec.continuous_stats().slices_done;
            return true;
          });
          if (was_armed) {
            std::fprintf(stderr,
                         "trigger_center: continuous disarmed, "
                         "%llu slices\n",
                         static_cast<unsigned long long>(slices));
          }
        } else if (cmd == thyme::record_control::ControlCmd::kStatus) {
          uint64_t captures = 0;
          uint64_t slices = 0;
          uint64_t pending = 0;
          uint64_t sync_errors = 0;
          bool disk_full = false;
          drain.Call([&] {
            captures = rec.stats().captures_done;
            slices = rec.continuous_stats().slices_done;
            pending = rec.pending_bytes();
            disk_full = rec.disk_full();
            sync_errors = rec.stats().sync_errors;
          });
          std::fprintf(
              stderr,
              "trigger_center: captures=%llu slices=%llu "
              "pending=%llu disk_full=%d sync_errors=%llu\n",
              static_cast<unsigned long long>(captures),
              static_cast<unsigned long long>(slices),
              static_cast<unsigned long long>(pending), disk_full ? 1 : 0,
              static_cast<unsigned long long>(sync_errors));
        }
      }
    }

    // Idle flush gate: deferred trigger captures (sticky AllowFlush) and the
    // continuous drain (level-driven SetContinuousGate, when wait_idle=true).
    drain.Call([&] {
      const double ratio =
          cfg.recorder_pending_quota > 0
              ? static_cast<double>(rec.pending_bytes()) /
                    static_cast<double>(cfg.recorder_pending_quota)
              : 0.0;
      const bool flush_now = idle.ShouldFlush(MonoNs(), ratio);
      if (rec.phase() == thyme::triggered::Phase::kSettle && flush_now) {
        rec.AllowFlush();
      }
      rec.SetContinuousGate(flush_now);
    });

    // 6.5: disk_full -> retention once -> allow the next trigger to retry.
    std::vector<std::string> disk_excludes;
    const bool disk_full = drain.Call([&] {
      if (!rec.disk_full()) return false;
      disk_excludes = ActiveExcludes(rec);
      return true;
    });
    if (disk_full) {
      std::fprintf(stderr, "trigger_center: disk full during drain; running "
                   "retention and re-arming\n");
      retention.Enforce(disk_excludes);
      drain.Call([&] { rec.ClearDiskFull(); });
    }

    // 6.4: retention after each completed capture + every 5 min; the
    // in-flight package and the active slice are never deletion candidates.
    const uint64_t now = MonoNs();
    std::vector<std::string> retain_excludes;
    bool run_retention = false;
    drain.Call([&] {
      const uint64_t done = rec.stats().captures_done;
      if (done != last_capture_done_count ||
          now - last_retention_ns >= 300000000000ull) {
        last_capture_done_count = done;
        run_retention = true;
        retain_excludes = ActiveExcludes(rec);
      }
    });
    if (run_retention) {
      last_retention_ns = now;
      const uint64_t freed = retention.Enforce(retain_excludes);
      if (freed > 0) {
        std::fprintf(stderr, "trigger_center: retention freed %llu bytes\n",
                     static_cast<unsigned long long>(freed));
      }
    }
    SleepNs(cfg.tick_ns);
  }

  // Join the drain thread before Shutdown. Tick must not race the seal.
  drain.Stop();
  // OPS-11: a planned stop is not a failure. Tell systemd we are leaving on
  // purpose before sealing the in-flight capture, so `Restart=on-failure`
  // stays quiet on `systemctl stop`.
  (void)thyme::app::SystemdNotifySend("STOPPING=1");
  center.Shutdown();
  control.Close();
  if (rec.continuous_armed())
    rec.DisarmContinuous(); // seal on shutdown
  rec.Shutdown();
  thyme::log::CloseFileSink();
  return 0;
}
