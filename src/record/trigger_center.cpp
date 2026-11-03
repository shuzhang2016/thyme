#include "thyme/record/trigger_center.h"

#include "thyme/runtime/log.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace thyme {
namespace trigger_center {

namespace {

constexpr uint64_t kQuotaWindowNs = 3600ull * 1000000000ull;  // 1 h

const char* RejectReason(trigger_proto::TriggerCode code) {
  using trigger_proto::TriggerCode;
  switch (code) {
    case TriggerCode::kAccepted: return "ok";
    case TriggerCode::kMuted: return "muted";
    case TriggerCode::kQuotaExceeded: return "quota_exceeded";
    case TriggerCode::kUnknownSource: return "unknown_source";
    case TriggerCode::kPreemptedByHigher: return "preempted_by_higher";
    case TriggerCode::kRecorderOffline: return "recorder_offline";
    case TriggerCode::kBusy: return "busy";
    case TriggerCode::kInvalidTimestamp: return "invalid_timestamp";
    case TriggerCode::kUnsupportedVersion: return "unsupported_version";
    case TriggerCode::kReplayRejected: return "replay_rejected";
    case TriggerCode::kAuthRejected: return "auth_rejected";
    case TriggerCode::kMalformedRequest: return "malformed_request";
    case TriggerCode::kTransportTimeout: return "transport_timeout";
    case TriggerCode::kCancelled: return "cancelled";
    case TriggerCode::kUnknownTrigger: return "unknown_trigger";
    case TriggerCode::kCaptureSealed: return "capture_sealed";
    case TriggerCode::kStartFailed: return "start_failed";
    case TriggerCode::kPreemptFailed: return "preempt_failed";
  }
  return "unknown";
}

void FillResponse(trigger_proto::TriggerResponseV4* resp, uint16_t version,
                  trigger_proto::TriggerCode code, uint64_t trigger_id,
                  int32_t effective_priority, uint32_t flags,
                  uint64_t request_id = 0) {
  if (resp == nullptr) return;
  resp->magic = trigger_proto::kTriggerMagic;
  resp->version = version;
  resp->code = static_cast<uint16_t>(code);
  resp->trigger_id = trigger_id;
  resp->effective_priority = effective_priority;
  resp->flags = flags;
  resp->reserved0 = 0;
  resp->reserved1 = 0;
  resp->request_id = request_id;
  resp->crc32 = 0;
  if (version == trigger_proto::kTriggerVersion) {
    trigger_proto::StampResponseCrc(resp);
  }
}

std::size_t SerializeResponse(const trigger_proto::TriggerResponseV4 &resp,
                              uint8_t *out) {
  if (resp.version == trigger_proto::kTriggerVersionLegacy) {
    trigger_proto::TriggerResponseV1 legacy{};
    legacy.magic = resp.magic;
    legacy.version = resp.version;
    legacy.code = resp.code;
    legacy.trigger_id = resp.trigger_id;
    std::memcpy(out, &legacy, sizeof(legacy));
    return sizeof(legacy);
  }
  if (resp.version == trigger_proto::kTriggerVersionV2) {
    trigger_proto::TriggerResponseV2 legacy{};
    legacy.magic = resp.magic;
    legacy.version = resp.version;
    legacy.code = resp.code;
    legacy.trigger_id = resp.trigger_id;
    legacy.effective_priority = resp.effective_priority;
    legacy.flags = resp.flags;
    std::memcpy(out, &legacy, sizeof(legacy));
    return sizeof(legacy);
  }
  if (resp.version == trigger_proto::kTriggerVersionV3) {
    trigger_proto::TriggerResponse legacy{};
    legacy.magic = resp.magic;
    legacy.version = resp.version;
    legacy.code = resp.code;
    legacy.trigger_id = resp.trigger_id;
    legacy.effective_priority = resp.effective_priority;
    legacy.flags = resp.flags;
    trigger_proto::StampResponseCrc(&legacy);
    std::memcpy(out, &legacy, sizeof(legacy));
    return sizeof(legacy);
  }
  if (resp.version == trigger_proto::kTriggerVersion) {
    std::memcpy(out, &resp, sizeof(resp));
    return sizeof(resp);
  }
  return 0;
}

}  // namespace

Center::Center() = default;
Center::~Center() { Shutdown(); }

uint64_t Center::MonoNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

bool Center::RealtimeNs(uint64_t* out) {
  if (out == nullptr) return false;
  struct timespec ts {};
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec <= 0) return false;
  *out = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
  return true;
}

const trigger_cfg::SourceRule* Center::FindSource(const std::string& name) const {
  for (const trigger_cfg::SourceRule& r : cfg_.sources) {
    if (r.name == name) return &r;
  }
  return nullptr;
}

uint32_t Center::QuotaLimit(const std::string& quota_class) const {
  for (const auto& kv : cfg_.quota_limits) {
    if (kv.first == quota_class) return kv.second;
  }
  return 0;  // unlisted = unlimited
}

uint32_t Center::QuotaUsedInWindow(const std::string& quota_class) const {
  const auto it = quota_window_.find(quota_class);
  if (it == quota_window_.end()) return 0;
  return static_cast<uint32_t>(it->second.size());
}

bool Center::NoteQuotaUse(const std::string& quota_class, uint64_t now) {
  if (quota_state_.enabled() && QuotaLimit(quota_class) > 0) {
    uint64_t realtime_ns = 0;
    if (!RealtimeNs(&realtime_ns) ||
        !quota_state_.Append(quota_class, realtime_ns)) {
      return false;
    }
  }
  quota_window_[quota_class].push_back(now);
  return true;
}

void Center::GcQuotaWindows(uint64_t now) {
  for (auto& kv : quota_window_) {
    std::deque<uint64_t>& q = kv.second;
    while (!q.empty() && now - q.front() >= kQuotaWindowNs) q.pop_front();
  }
}

bool Center::Init(const trigger_cfg::CenterConfig& cfg, const Hooks& hooks) {
  Shutdown();
  if (hooks.recorder_ready == nullptr || hooks.capture_active == nullptr ||
      hooks.start_capture == nullptr || hooks.preempt_capture == nullptr) {
    return false;
  }
  // Every quota_class a source references must exist in quotas — a typo like
  // "saftey" used to turn silently into "unlimited", which violates the
  // config's unknown-key-is-an-error philosophy.
  for (const trigger_cfg::SourceRule& r : cfg.sources) {
    bool found = false;
    for (const auto& kv : cfg.quota_limits) {
      if (kv.first == r.quota_class) found = true;
    }
    if (!found) {
      THYME_LOG_ERROR("trigger_center: source %s references unknown "
                      "quota_class %s (refusing to start)",
                      r.name.c_str(), r.quota_class.c_str());
      return false;
    }
  }
  cfg_ = cfg;
  hooks_ = hooks;
  dedup_.assign(cfg_.dedup_window_entries, DedupEntry{});
  // OPS-05: one process-wide log budget covering audit + fsm + perf writers
  // (packages keep [recorder].disk_quota_mb). 0 = per-file caps only.
  logging::LogBudget::Configure(cfg_.log_total_budget_bytes);
  if (!cfg_.quota_state_path.empty()) {
    for (const auto& quota : cfg_.quota_limits) {
      if (quota.second > 0 && quota.first.size() > 63) {
        THYME_LOG_ERROR("trigger_center: persistent quota class exceeds 63 bytes");
        return false;
      }
    }
    uint64_t realtime_ns = 0;
    if (!RealtimeNs(&realtime_ns) ||
        !quota_state_.Open(cfg_.quota_state_path, realtime_ns, MonoNs(),
                           &quota_window_)) {
      THYME_LOG_ERROR("trigger_center: cannot restore quota state %s",
                      cfg_.quota_state_path.c_str());
      quota_state_.Close();
      return false;
    }
  }
#if !defined(_WIN32)
  // Stale-path handling mirrors record_control::Server::Open.
  struct stat st {};
  if (stat(cfg_.socket_path.c_str(), &st) == 0) {
    if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid())
      return false;
    unlink(cfg_.socket_path.c_str());
  }
  fd_ = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd_ < 0)
    return false;
  const uint64_t rcvbuf64 = trigger_cfg::TriggerDatagramsPerTick(cfg_) *
                            sizeof(trigger_proto::TriggerRequestV4);
  if (rcvbuf64 == 0 ||
      rcvbuf64 > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    close(fd_);
    fd_ = -1;
    return false;
  }
  const int rcvbuf = static_cast<int>(rcvbuf64);
  if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) != 0) {
    THYME_LOG_ERROR("trigger_center: SO_RCVBUF=%d failed: %s", rcvbuf,
                    std::strerror(errno));
    close(fd_);
    fd_ = -1;
    return false;
  }
#if defined(__linux__)
  {
    // SCM_CREDENTIALS is the datagram equivalent of SO_PEERCRED: with
    // SO_PASSCRED the kernel stamps every received datagram with the sender's
    // real pid/uid/gid, and a sender cannot spoof them. Without it every
    // request would be rejected (auth_no_credentials), so refuse to start.
    const int passcred = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_PASSCRED, &passcred, sizeof(passcred)) !=
        0) {
      THYME_LOG_ERROR("trigger_center: SO_PASSCRED failed: %s",
                      std::strerror(errno));
      close(fd_);
      fd_ = -1;
      return false;
    }
  }
#endif
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (cfg_.socket_path.size() >= sizeof(addr.sun_path)) {
    close(fd_);
    fd_ = -1;
    return false;
  }
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s",
                cfg_.socket_path.c_str());
  if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd_);
    fd_ = -1;
    return false;
  }
  // ARCH-05 / SEC-02: the control socket is a security boundary. A silent
  // chmod/fcntl failure would leave it world-accessible or block the receive
  // path, so both are fail-closed.
  if (chmod(cfg_.socket_path.c_str(), S_IRUSR | S_IWUSR) != 0) {
    THYME_LOG_ERROR("trigger_center: chmod %s failed: %s",
                    cfg_.socket_path.c_str(), std::strerror(errno));
    close(fd_);
    fd_ = -1;
    unlink(cfg_.socket_path.c_str());
    return false;
  }
  const int flags = fcntl(fd_, F_GETFL, 0);
  if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) != 0) {
    THYME_LOG_ERROR("trigger_center: fcntl O_NONBLOCK on %s failed: %s",
                    cfg_.socket_path.c_str(), std::strerror(errno));
    close(fd_);
    fd_ = -1;
    unlink(cfg_.socket_path.c_str());
    return false;
  }
  bound_path_ = cfg_.socket_path;

  // SEC-02: the audit log is mandatory. A center that cannot record its
  // decisions refuses to start (fail closed) instead of running unlogged.
  trigger_audit::Config acfg;
  acfg.path = EffectiveAuditPath();
  acfg.max_bytes = cfg_.audit_max_bytes;
  acfg.rotate_keep = cfg_.audit_rotate_keep;
  acfg.fsync_interval_ns = cfg_.audit_fsync_interval_ns;
  if (!audit_.Open(acfg)) {
    THYME_LOG_ERROR("trigger_center: audit log %s is not writable",
                    acfg.path.c_str());
    close(fd_);
    fd_ = -1;
    unlink(bound_path_.c_str());
    bound_path_.clear();
    return false;
  }
  auto audit_integrity_mode = [this](const char* source, const char* mode) {
    trigger_audit::Record rec;
    rec.peer_pid = getpid();
    rec.peer_uid = geteuid();
    rec.source = source;
    rec.op = "config";
    rec.accepted = true;
    rec.decision = "configured";
    rec.reason = "integrity_downgrade";
    rec.integrity_mode = mode;
    return audit_.Append(rec);
  };
  if (cfg_.manifest_mode != integrity::ManifestMode::kFull &&
      !audit_integrity_mode("security",
                            integrity::ManifestModeName(cfg_.manifest_mode))) {
    Shutdown();
    return false;
  }
  for (const triggered::SourceConfig& source : cfg_.recorder_sources) {
    if (source.manifest != triggered::SourceConfig::ManifestPolicy::kMetadata &&
        source.manifest != triggered::SourceConfig::ManifestPolicy::kOff) {
      continue;
    }
    if (!audit_integrity_mode(triggered::ChannelSourceName(source).c_str(),
                              triggered::ManifestPolicyName(source.manifest))) {
      Shutdown();
      return false;
    }
  }
  // FSM event dump file (<socket>.fsm, JSONL). Best-effort — the observability
  // surface is not fail-closed like the audit; a failure to open only warns on
  // stderr. Rotation follows [logging].fsm_max_bytes / fsm_rotate_keep.
  {
    logging::RotateConfig rcfg;
    rcfg.path = cfg_.socket_path + ".fsm";
    rcfg.max_bytes = cfg_.log_fsm_max_bytes;
    rcfg.keep = cfg_.log_fsm_rotate_keep;
    rcfg.mandatory = false;  // budget may drop FSM events, never audit records
    rcfg.account = true;
    if (!fsm_file_.Open(rcfg)) {
      THYME_LOG_WARN("trigger_center: fsm event log %s is not writable "
                     "(continuing without it)",
                     rcfg.path.c_str());
    }
  }
#endif
  state_ = State::kIdle;
  return true;
}

void Center::Shutdown() {
#if !defined(_WIN32)
  if (fd_ >= 0)
    close(fd_);
  fd_ = -1;
  if (!bound_path_.empty())
    unlink(bound_path_.c_str());
  bound_path_.clear();
#endif
  fsm_file_.Close();
  audit_.Close();
  quota_state_.Close();
  replay_.fill(ReplayEntry{});
  dedup_.clear();
}

int Center::SocketReceiveBufferBytes() const {
#if !defined(_WIN32)
  int value = 0;
  socklen_t len = sizeof(value);
  if (fd_ >= 0 && getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &value, &len) == 0) {
    return value;
  }
#endif
  return -1;
}

std::string Center::EffectiveAuditPath() const {
  if (!cfg_.audit_log_path.empty())
    return cfg_.audit_log_path;
  return cfg_.socket_path + ".audit";
}

void Center::ReloadConfig(const trigger_cfg::CenterConfig& cfg) {
  const int fd = fd_;
  const std::string bound = bound_path_;
  const State state = state_;
  const uint64_t next_id = next_trigger_id_;
  // Audit geometry is startup-only (the file is open, rotation paths are
  // fixed); keep the running values and tell the operator if they changed.
  const std::string old_audit_path = cfg_.audit_log_path;
  const std::string old_audit_effective = EffectiveAuditPath();
  const uint64_t old_audit_max = cfg_.audit_max_bytes;
  const uint32_t old_audit_keep = cfg_.audit_rotate_keep;
  const uint64_t old_audit_fsync = cfg_.audit_fsync_interval_ns;
  // OPS-05: [logging] rotation geometry and the process budget are bound at
  // startup (the writers are already open); keep them and warn if changed.
  const uint64_t old_fsm_max = cfg_.log_fsm_max_bytes;
  const uint32_t old_fsm_keep = cfg_.log_fsm_rotate_keep;
  const uint64_t old_perf_max = cfg_.log_perf_max_bytes;
  const uint32_t old_perf_keep = cfg_.log_perf_rotate_keep;
  const uint64_t old_budget = cfg_.log_total_budget_bytes;
  const std::string old_signing_key = cfg_.signing_key_path;
  const integrity::ManifestMode old_manifest_mode = cfg_.manifest_mode;
  const uint64_t old_tick_ns = cfg_.tick_ns;
  const uint64_t old_poll_budget_ns = cfg_.socket_poll_budget_ns;
  const uint32_t old_max_rate = cfg_.max_trigger_rate_hz;
  const uint32_t old_dedup_entries = cfg_.dedup_window_entries;
  const uint64_t old_dedup_ns = cfg_.dedup_window_ns;
  const uint64_t old_pending_quota = cfg_.recorder_pending_quota;
  const std::string old_quota_state = cfg_.quota_state_path;
  const std::string new_effective = cfg.audit_log_path.empty()
                                        ? cfg.socket_path + ".audit"
                                 : cfg.audit_log_path;
  if (new_effective != old_audit_effective) {
    THYME_LOG_WARN("trigger_center: reload: [security].audit_log change "
                   "requires restart; keeping %s",
                   old_audit_effective.c_str());
  } else if (cfg.audit_max_bytes != old_audit_max ||
             cfg.audit_rotate_keep != old_audit_keep ||
             cfg.audit_fsync_interval_ns != old_audit_fsync) {
    THYME_LOG_WARN("trigger_center: reload: audit rotation/sync settings "
                   "require restart; keeping the running values");
  }
  if (cfg.signing_key_path != old_signing_key) {
    THYME_LOG_WARN("trigger_center: reload: [security].signing_key change "
                   "requires restart; keeping %s",
                   old_signing_key.empty() ? "<unsigned>"
                                           : old_signing_key.c_str());
  }
  if (cfg.log_fsm_max_bytes != old_fsm_max ||
      cfg.log_fsm_rotate_keep != old_fsm_keep ||
      cfg.log_perf_max_bytes != old_perf_max ||
      cfg.log_perf_rotate_keep != old_perf_keep ||
      cfg.log_total_budget_bytes != old_budget) {
    THYME_LOG_WARN("trigger_center: reload: [logging] rotation/budget settings "
                   "require restart; keeping the running values");
  }
  if (cfg.tick_ns != old_tick_ns ||
      cfg.socket_poll_budget_ns != old_poll_budget_ns ||
      cfg.max_trigger_rate_hz != old_max_rate ||
      cfg.dedup_window_entries != old_dedup_entries ||
      cfg.dedup_window_ns != old_dedup_ns) {
    THYME_LOG_WARN("trigger_center: reload: ingress/dedup settings require "
                   "restart; keeping the running values");
  }
  if (cfg.quota_state_path != old_quota_state) {
    THYME_LOG_WARN("trigger_center: reload: quota_state requires restart; "
                   "keeping the running state file");
  }
  cfg_ = cfg;
  fd_ = fd;
  bound_path_ = bound;
  state_ = state;
  next_trigger_id_ = next_id;
  cfg_.socket_path = bound;
  cfg_.audit_log_path = old_audit_path;
  cfg_.audit_max_bytes = old_audit_max;
  cfg_.audit_rotate_keep = old_audit_keep;
  cfg_.audit_fsync_interval_ns = old_audit_fsync;
  cfg_.signing_key_path = old_signing_key;
  cfg_.manifest_mode = old_manifest_mode;
  cfg_.log_fsm_max_bytes = old_fsm_max;
  cfg_.log_fsm_rotate_keep = old_fsm_keep;
  cfg_.log_perf_max_bytes = old_perf_max;
  cfg_.log_perf_rotate_keep = old_perf_keep;
  cfg_.log_total_budget_bytes = old_budget;
  cfg_.tick_ns = old_tick_ns;
  cfg_.socket_poll_budget_ns = old_poll_budget_ns;
  cfg_.max_trigger_rate_hz = old_max_rate;
  cfg_.dedup_window_entries = old_dedup_entries;
  cfg_.dedup_window_ns = old_dedup_ns;
  cfg_.recorder_pending_quota = old_pending_quota;
  cfg_.quota_state_path = old_quota_state;
  // Security policy itself (allowlist, root, pid requirement, hint bound,
  // legacy peers) IS hot-reloadable: cfg_ now carries the new values.
}

SubmitResult Center::Submit(const std::string& source, int32_t priority_hint,
                            uint64_t timestamp_ns, bool has_timestamp,
                            bool force) {
  using trigger_proto::TriggerCode;
  SubmitResult out;

  const trigger_cfg::SourceRule* rule = FindSource(source);
  std::string used = source;
  if (rule == nullptr && !cfg_.default_source.empty()) {
    rule = FindSource(cfg_.default_source);
    used = cfg_.default_source;
  }
  if (rule == nullptr) {
    out.code = TriggerCode::kUnknownSource;
    stats_.rejected_unknown += 1;
    return out;
  }
  out.source_used = used;

  // priority_hint can only lower priority: no same-uid process may elevate
  // itself above the source's configured priority (that would punch through
  // the mute/quota/preemption rules). SEC-02 additionally clamps the
  // requested hint to [0, max_priority_hint] before that rule, so one
  // deployment-wide bound exists even for a source whose rule priority is
  // high. A negative hint means "use the rule's priority".
  int32_t prio = rule->priority;
  if (priority_hint >= 0) {
    int32_t hinted = priority_hint;
    if (hinted > cfg_.max_priority_hint) {
      hinted = cfg_.max_priority_hint;
      out.priority_clamped = true;
    }
    if (hinted > rule->priority) {
      hinted = rule->priority;
      out.priority_clamped = true;
    }
    prio = hinted;
  }
  if (out.priority_clamped)
    stats_.priority_clamps += 1;
  out.priority_used = prio;
  const bool crash = prio >= cfg_.crash_priority;

  // Bounds check for a supplied timestamp — too old makes the pre window all
  // holes, beyond the future tolerance pushes the post deadline far out and
  // holds the FSM for a long time; out of bounds is rejected explicitly.
  if (has_timestamp && timestamp_ns != 0) {
    const uint64_t now0 = MonoNs();
    if (timestamp_ns > now0 + cfg_.timestamp_future_tolerance_ns ||
        timestamp_ns + rule->before_ns < now0) {
      out.code = TriggerCode::kInvalidTimestamp;
      stats_.rejected_bad_timestamp += 1;
      return out;
    }
  }

  if (!hooks_.recorder_ready(hooks_.ctx)) {
    out.code = TriggerCode::kRecorderOffline;
    stats_.rejected_offline += 1;
    return out;
  }
  // 6.5: disk full — reject low priority as quota_exceeded; crash level is
  // let through (high/ reserved zone; the recorder may still refuse ->
  // kRecorderOffline).
  if (!crash && hooks_.disk_full != nullptr && hooks_.disk_full(hooks_.ctx)) {
    out.code = TriggerCode::kQuotaExceeded;
    stats_.rejected_quota += 1;
    return out;
  }

  // Queue full (the previous trigger has not started / preemption is running)
  // -> kBusy, distinct from offline/mute; checked before the FSM gate because
  // the priority of a capture that has not started cannot be evaluated.
  if (pending_trigger_) {
    out.code = TriggerCode::kBusy;
    stats_.rejected_busy += 1;
    return out;
  }

  // FSM gate (design 5.3). Preemption is decided here but EXECUTED at Tick
  // (T2 async): the reply goes out immediately.
  bool need_preempt = false;
  if (state_ == State::kCapturing) {
    need_preempt =
        prio > current_priority_ || (force && prio >= current_priority_);
    if (!need_preempt) {
      out.code = TriggerCode::kMuted;
      stats_.rejected_muted += 1;
      return out;
    }
  } else if (state_ == State::kMuted && !crash) {
    out.code = TriggerCode::kMuted;
    stats_.rejected_muted += 1;
    return out;
  }

  // Quotas (5.4): count window per class; crash level overrides but is
  // accounted.
  const uint64_t now = MonoNs();
  GcQuotaWindows(now);
  const uint32_t limit = QuotaLimit(rule->quota_class);
  if (limit > 0 && QuotaUsedInWindow(rule->quota_class) >= limit && !crash) {
    out.code = TriggerCode::kQuotaExceeded;
    stats_.rejected_quota += 1;
    return out;
  }
  if (limit > 0 && QuotaUsedInWindow(rule->quota_class) >= limit && crash) {
    stats_.quota_overridden += 1;
    out.quota_overridden = true;  // TRG-12: audited as quota_override=1
  }
  if (cfg_.recorder_pending_quota > 0 && hooks_.pending_bytes != nullptr) {
    const uint64_t pending = hooks_.pending_bytes(hooks_.ctx);
    if (pending > cfg_.recorder_pending_quota) {
      if (!crash) {
        out.code = TriggerCode::kQuotaExceeded;
        stats_.rejected_quota += 1;
        return out;
      }
      if (hooks_.evict_oldest != nullptr) {
        hooks_.evict_oldest(hooks_.ctx, pending);
        stats_.memory_evictions += 1;
      }
    }
  }

  if (!NoteQuotaUse(rule->quota_class, now)) {
    stats_.quota_state_errors += 1;
    stats_.rejected_offline += 1;
    out.code = TriggerCode::kRecorderOffline;
    return out;
  }

  const uint64_t t0 = (has_timestamp && timestamp_ns != 0) ? timestamp_ns : now;
  const uint64_t id = next_trigger_id_++;
  // Snapshot for rollback (T6), then enqueue and answer Accepted at once.
  prev_state_ = state_;
  prev_trigger_id_ = current_trigger_id_;
  prev_priority_ = current_priority_;
  prev_mute_ns_ = current_mute_ns_;
  prev_mute_until_ns_ = mute_until_ns_;
  pending_trigger_ = true;
  pending_preempt_ = need_preempt;
  pending_force_rt_ =
      crash || rule->flush_realtime || need_preempt; // preemption forces realtime
  pending_high_priority_ = crash;
  pending_before_ns_ = rule->before_ns;
  pending_after_ns_ = rule->after_ns;
  pending_channels_ = rule->channels;  // TRG-18 per-source channel filter
  pending_t0_ = t0;
  // TRG-12: preempt-record attribution. Reset here so a direct (in-process)
  // Submit never inherits the previous request's peer credentials; the
  // socket path overwrites these in ProcessRequest right after Submit.
  pending_source_ = used;
  pending_peer_pid_ = 0;
  pending_peer_uid_ = 0;
  pending_proto_ = 0;
  NoteFsmEvent(id, state_, State::kCapturing, FsmReason::kSubmitAccept, now);
  state_ = State::kCapturing;
  current_trigger_id_ = id;
  current_priority_ = prio;
  mute_until_ns_ = 0;
  current_mute_ns_ = rule->mute_ns;
  out.code = TriggerCode::kAccepted;
  out.trigger_id = id;
  stats_.accepted += 1;
  return out;
}

trigger_proto::TriggerCode Center::Cancel(uint64_t trigger_id) {
  using trigger_proto::TriggerCode;
  if (trigger_id == 0 || trigger_id >= next_trigger_id_) {
    stats_.cancels_unknown += 1;
    return TriggerCode::kUnknownTrigger;
  }
  const uint64_t now = MonoNs();
  if (pending_trigger_ && current_trigger_id_ == trigger_id) {
    // Accepted but never started: drop the queue entry and roll the FSM back
    // to the pre-Submit snapshot (the same fields StartPendingTrigger
    // restores). The quota window entry is kept — the trigger was admitted.
    pending_trigger_ = false;
    NoteFsmEvent(trigger_id, state_, prev_state_, FsmReason::kCancelPending,
                 now);
    state_ = prev_state_;
    current_trigger_id_ = prev_trigger_id_;
    current_priority_ = prev_priority_;
    current_mute_ns_ = prev_mute_ns_;
    mute_until_ns_ = prev_mute_until_ns_;
    stats_.cancels_applied += 1;
    PushOutcome(trigger_id, TriggerCode::kCancelled, now);
    return TriggerCode::kCancelled;
  }
  if (state_ == State::kCapturing && current_trigger_id_ == trigger_id) {
    // Running capture: cancel is a preempt-to-idle through the same hook —
    // what is already written is truncated+sealed, exactly like a preempt.
    // (While a preempt is pending, only the pending trigger is cancellable;
    // the old capture is about to get its kPreemptedByHigher outcome anyway.)
    if (!hooks_.preempt_capture(hooks_.ctx)) {
      stats_.cancel_failures += 1;
      return TriggerCode::kBusy;  // capture keeps running; retry
    }
    stats_.cancels_applied += 1;
    // Like a completed capture: the source's mute window still applies.
    if (current_mute_ns_ > 0) {
      NoteFsmEvent(trigger_id, State::kCapturing, State::kMuted,
                   FsmReason::kCancelRunning, now);
      state_ = State::kMuted;
      mute_until_ns_ = now + current_mute_ns_;
    } else {
      NoteFsmEvent(trigger_id, State::kCapturing, State::kIdle,
                   FsmReason::kCancelRunning, now);
      state_ = State::kIdle;
    }
    PushOutcome(trigger_id, TriggerCode::kCancelled, now);
    return TriggerCode::kCancelled;
  }
  // Unknown, or already completed (incl. sealed but still in the mute window).
  stats_.cancels_unknown += 1;
  return TriggerCode::kUnknownTrigger;
}

bool Center::MatchOutcomeRing(uint64_t filter_id, OutcomeEntry* out) const {
  for (uint32_t i = 0; i < outcomes_count_; ++i) {
    // Newest first: the most recent write sits just below outcomes_head_.
    const uint32_t idx =
        (outcomes_head_ + kOutcomeRingSize - 1 - i) % kOutcomeRingSize;
    const OutcomeEntry& e = outcomes_[idx];
    if (filter_id == 0 || e.trigger_id == filter_id) {
      if (out != nullptr) *out = e;
      return true;
    }
  }
  return false;
}

void Center::PushOutcome(uint64_t trigger_id, trigger_proto::TriggerCode code,
                         uint64_t now) {
  OutcomeEntry& e = outcomes_[outcomes_head_];
  e.trigger_id = trigger_id;
  e.code = static_cast<uint16_t>(code);
  e.mono_ns = now;
  outcomes_head_ = (outcomes_head_ + 1) % kOutcomeRingSize;
  if (outcomes_count_ < kOutcomeRingSize) {
    outcomes_count_ += 1;
  } else {
    stats_.notify_outcome_drops += 1;  // oldest outcome overwritten
  }
  stats_.notify_outcomes += 1;

  // Resolve deferred waiters whose filter matches this outcome.
  for (WaiterEntry& w : waiters_) {
    if (!w.used) continue;
    if (w.filter_id != 0 && w.filter_id != trigger_id) continue;
    w.used = false;
#if !defined(_WIN32)
    if (fd_ >= 0 && w.reply_path[0] != '\0') {
      trigger_proto::TriggerResponseV4 resp{};
      FillResponse(&resp, w.proto_version, code, trigger_id, 0,
                   trigger_proto::kRespFlagOutcomeNotification, w.request_id);
      uint8_t wire[sizeof(trigger_proto::TriggerResponseV4)];
      const std::size_t wire_size = SerializeResponse(resp, wire);
      sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", w.reply_path);
      if (wire_size > 0 &&
          sendto(fd_, wire, wire_size, 0,
                 reinterpret_cast<const sockaddr*>(&addr),
                 sizeof(addr)) == static_cast<ssize_t>(wire_size)) {
        stats_.notify_delivered += 1;
      } else {
        stats_.notify_send_failures += 1;
        THYME_LOG_WARN_RL("trigger_center: outcome notify sendto failed: %s",
                          std::strerror(errno));
      }
    }
#endif
    trigger_audit::Record rec;
    rec.peer_pid = w.pid;
    rec.peer_uid = w.uid;
    rec.proto_version = w.proto_version;
    rec.request_id = w.request_id;
    rec.op = "wait";
    rec.accepted = true;
    rec.reason = "ok";
    rec.code = code;
    rec.trigger_id = trigger_id;
    if (!audit_.Append(rec)) stats_.audit_drops += 1;
  }
}

void Center::ExpireWaiters(uint64_t now) {
  for (WaiterEntry& w : waiters_) {
    if (!w.used || now < w.deadline_ns) continue;
    // No reply: the client enforces the same deadline and reports
    // kTransportTimeout (the server never sends that code).
    w.used = false;
    stats_.notify_waiter_expired += 1;
  }
}

void Center::StartPendingTrigger() {
  if (!pending_trigger_)
    return;
  if (!hooks_.recorder_ready(hooks_.ctx)) {
    // Accepted answered but the recorder went offline: roll back to the
    // pre-enqueue state.
    stats_.start_failures += 1;
    PushOutcome(current_trigger_id_, trigger_proto::TriggerCode::kStartFailed,
                MonoNs());
    NoteFsmEvent(current_trigger_id_, State::kCapturing, prev_state_,
                 FsmReason::kStartFailed, MonoNs());
    pending_trigger_ = false;
    state_ = prev_state_;
    current_trigger_id_ = prev_trigger_id_;
    current_priority_ = prev_priority_;
    current_mute_ns_ = prev_mute_ns_;
    mute_until_ns_ = prev_mute_until_ns_;
    return;
  }
  if (pending_preempt_) {
    if (!hooks_.preempt_capture(hooks_.ctx)) {
      // Preemption failed — roll back to the original capture and drop the new
      // trigger (counted; the client already received Accepted = queued, and
      // whether the trigger finally produces a package is determined by the
      // package directory/notification).
      stats_.preempt_failures += 1;
      PushOutcome(current_trigger_id_,
                  trigger_proto::TriggerCode::kPreemptFailed, MonoNs());
      NoteFsmEvent(current_trigger_id_, State::kCapturing, prev_state_,
                   FsmReason::kPreemptFailed, MonoNs());
      pending_trigger_ = false;
      state_ = prev_state_;
      current_trigger_id_ = prev_trigger_id_;
      current_priority_ = prev_priority_;
      current_mute_ns_ = prev_mute_ns_;
    mute_until_ns_ = prev_mute_until_ns_;
      return;
    }
    stats_.preemptions += 1;
    // TRG-12: the preemption is now irreversible (old capture truncated+
    // sealed) — persist the decision. trigger_id is the preempted capture,
    // `by` the preempting trigger; pid/uid/src are the preempting request's
    // attribution stashed at accept time.
    {
      trigger_audit::Record rec;
      rec.peer_pid = pending_peer_pid_;
      rec.peer_uid = pending_peer_uid_;
      rec.proto_version = pending_proto_;
      rec.source = pending_source_.c_str();
      rec.op = "preempt";
      rec.decision = "preempted";
      rec.accepted = true;
      rec.reason = "preempted_by_higher";
      rec.code = trigger_proto::TriggerCode::kPreemptedByHigher;
      rec.hint_effective = current_priority_;
      rec.trigger_id = prev_trigger_id_;
      rec.by_trigger_id = current_trigger_id_;
      if (!audit_.Append(rec))
        stats_.audit_drops += 1;
    }
    // The truncated+sealed old capture gets its outcome (response code 4).
    PushOutcome(prev_trigger_id_,
                trigger_proto::TriggerCode::kPreemptedByHigher, MonoNs());
    // The FSM stays in CAPTURING, but the old capture ended and the new one
    // took over — the event carries the old trigger id.
    NoteFsmEvent(prev_trigger_id_, State::kCapturing, State::kCapturing,
                 FsmReason::kPreempted, MonoNs());
  }
  if (!hooks_.start_capture(hooks_.ctx, current_trigger_id_, pending_t0_,
                            pending_before_ns_, pending_after_ns_,
                            pending_force_rt_, pending_high_priority_,
                            pending_channels_)) {
    stats_.start_failures += 1;
    PushOutcome(current_trigger_id_, trigger_proto::TriggerCode::kStartFailed,
                MonoNs());
    pending_trigger_ = false;
    if (pending_preempt_) {
      // The old capture was truncated+sealed and the new one did not start:
      // treat it as a completed old capture (mute still applies).
      const State to = prev_mute_ns_ > 0 ? State::kMuted : State::kIdle;
      NoteFsmEvent(current_trigger_id_, State::kCapturing, to,
                   FsmReason::kStartFailed, MonoNs());
      state_ = to;
      mute_until_ns_ = prev_mute_ns_ > 0 ? MonoNs() + prev_mute_ns_ : 0;
    } else {
      NoteFsmEvent(current_trigger_id_, State::kCapturing, prev_state_,
                   FsmReason::kStartFailed, MonoNs());
      state_ = prev_state_;
      current_trigger_id_ = prev_trigger_id_;
      current_priority_ = prev_priority_;
      current_mute_ns_ = prev_mute_ns_;
    mute_until_ns_ = prev_mute_until_ns_;
    }
    return;
  }
  pending_trigger_ = false;
  // The queued trigger actually starts (asynchronous start; the FSM itself
  // stays in CAPTURING).
  NoteFsmEvent(current_trigger_id_, State::kCapturing, State::kCapturing,
               FsmReason::kStarted, MonoNs());
}

void Center::CompleteCaptureIfDone(uint64_t now) {
  if (state_ != State::kCapturing)
    return;
  if (pending_trigger_)
    return; // accepted but not started: not a completion
  if (hooks_.capture_active(hooks_.ctx))
    return;
  PushOutcome(current_trigger_id_, trigger_proto::TriggerCode::kCaptureSealed,
              now);
  if (current_mute_ns_ > 0) {
    NoteFsmEvent(current_trigger_id_, State::kCapturing, State::kMuted,
                 FsmReason::kCompleted, now);
    state_ = State::kMuted;
    mute_until_ns_ = now + current_mute_ns_;
  } else {
    NoteFsmEvent(current_trigger_id_, State::kCapturing, State::kIdle,
                 FsmReason::kCompleted, now);
    state_ = State::kIdle;
  }
}

const char* Center::AuthRejectReason(const PeerCred& peer) const {
#if defined(__linux__)
  // No kernel credentials attached: impossible on the real socket path with
  // SO_PASSCRED, but a direct caller could pass this. Fail closed.
  if (peer.uid == static_cast<uid_t>(-1))
    return "auth_no_credentials";
  if (cfg_.require_peer_pid && peer.pid <= 0)
    return "auth_invalid_pid";
#elif defined(__APPLE__)
  // The datagram path does not attach LOCAL_PEERCRED credentials here; keep
  // the historical accept-any-peer behaviour when they are absent.
  if (peer.uid == static_cast<uid_t>(-1))
    return nullptr;
#endif
#if defined(__linux__) || defined(__APPLE__)
  // A non-empty allowlist replaces the default (own euid) entirely; an empty
  // one keeps the historical single-user behaviour.
  if (!cfg_.allowed_uids.empty()) {
    for (uint32_t uid : cfg_.allowed_uids) {
      if (peer.uid == static_cast<uid_t>(uid))
        return nullptr;
    }
    if (peer.uid == 0 && cfg_.allow_root)
      return nullptr;
    return "auth_uid_not_allowed";
  }
  if (peer.uid == geteuid())
    return nullptr;
  if (peer.uid == 0 && cfg_.allow_root)
    return nullptr;
  return "auth_uid_not_allowed";
#else
  (void)peer;  // no peer-credential API: pre-SEC-02 behaviour
  return nullptr;
#endif
}

bool Center::CheckReplay(const PeerCred& peer, uint64_t sequence,
                         uint64_t now) {
  ReplayEntry* entry = nullptr;
  for (ReplayEntry& e : replay_) {
    if (e.used && e.uid == peer.uid && e.pid == peer.pid) {
      entry = &e;
      break;
    }
  }
  if (entry == nullptr) {
    // Take a free slot, else evict the least-recently-seen peer. The table is
    // fixed-size so a peer storm cannot grow memory.
    ReplayEntry* oldest = &replay_[0];
    for (ReplayEntry& e : replay_) {
      if (!e.used) {
        oldest = &e;
        break;
      }
      if (e.last_ns < oldest->last_ns)
        oldest = &e;
    }
    entry = oldest;
    entry->used = true;
    entry->uid = peer.uid;
    entry->pid = peer.pid;
    entry->max_seq = sequence;
    entry->bitmap = 1;
    entry->last_ns = now;
    return true;
  }
  if (sequence > entry->max_seq) {
    const uint64_t delta = sequence - entry->max_seq;
    entry->bitmap =
        delta >= kReplayWindow ? 0 : (entry->bitmap << delta);
    entry->bitmap |= 1;
    entry->max_seq = sequence;
    entry->last_ns = now;
    return true;
  }
  const uint64_t delta = entry->max_seq - sequence;
  if (delta >= kReplayWindow) return false;  // too old
  const uint64_t bit = 1ull << delta;
  if ((entry->bitmap & bit) != 0) return false;  // exact duplicate
  entry->bitmap |= bit;
  entry->last_ns = now;
  return true;
}

bool Center::FindDedup(const PeerCred& peer, uint64_t request_id,
                       uint32_t request_crc, uint64_t now,
                       trigger_proto::TriggerResponseV4* response,
                       bool* conflict) {
  *conflict = false;
  for (DedupEntry& entry : dedup_) {
    if (entry.used && now >= entry.expires_ns) entry.used = false;
    if (!entry.used || entry.uid != peer.uid || entry.pid != peer.pid ||
        entry.request_id != request_id) {
      continue;
    }
    entry.last_ns = now;
    if (entry.request_crc != request_crc) {
      *conflict = true;
      return false;
    }
    *response = entry.response;
    response->flags |= trigger_proto::kRespFlagDeduplicated;
    trigger_proto::StampResponseCrc(response);
    return true;
  }
  return false;
}

void Center::RememberDedup(
    const PeerCred& peer, uint64_t request_id, uint32_t request_crc,
    uint64_t now, const trigger_proto::TriggerResponseV4& response) {
  DedupEntry* slot = nullptr;
  for (DedupEntry& entry : dedup_) {
    if (entry.used && now >= entry.expires_ns) entry.used = false;
    if (!entry.used) {
      slot = &entry;
      break;
    }
    if (slot == nullptr || entry.last_ns < slot->last_ns) slot = &entry;
  }
  if (slot == nullptr) return;
  slot->used = true;
  slot->uid = peer.uid;
  slot->pid = peer.pid;
  slot->request_id = request_id;
  slot->request_crc = request_crc;
  slot->expires_ns = now + cfg_.dedup_window_ns;
  slot->last_ns = now;
  slot->response = response;
}

bool Center::ProcessRequest(const uint8_t* buf, std::size_t n,
                            const PeerCred& peer,
                            trigger_proto::TriggerResponseV4* resp,
                            const char* reply_path) {
  using trigger_proto::TriggerCode;
  const uint64_t now = MonoNs();
  trigger_audit::Record rec;
  rec.peer_pid = peer.pid;
  rec.peer_uid = peer.uid;
  uint64_t request_id = 0;

  auto append_audit = [this, &rec]() {
    if (!audit_.Append(rec)) stats_.audit_drops += 1;
  };
  auto reject_now = [&](uint16_t version, const char* reason, TriggerCode code,
                        bool reply) -> bool {
    rec.proto_version = version;
    rec.accepted = false;
    rec.reason = reason;
    rec.code = code;
    rec.trigger_id = 0;
    rec.hint_effective = 0;
    append_audit();
    if (reply)
      FillResponse(resp, version, code, 0, 0, 0, request_id);
    return reply;
  };

  // SEC-02: authenticate before parsing, so an unauthorized peer learns
  // nothing about the accepted layouts and its garbage is only counted.
  const char* auth_reason = AuthRejectReason(peer);
  if (auth_reason != nullptr) {
    stats_.auth_rejects += 1;
    return reject_now(0, auth_reason, TriggerCode::kAuthRejected, false);
  }

  int32_t hint = -1;
  uint64_t timestamp_ns = 0;
  bool has_timestamp = false;
  bool force = false;
  bool cancel_req = false;  // TRG-07 kFlagCancel (V3 only)
  bool wait_req = false;    // TRG-07 kFlagWaitOutcome (V3 only)
  uint32_t req_timeout_ms = trigger_proto::kDefaultResponseTimeoutMs;
  uint64_t sequence = 0;
  char source[sizeof(trigger_proto::TriggerRequest::source)] = {};
  uint16_t proto_version = trigger_proto::kTriggerVersion;
  uint16_t response_version = trigger_proto::kTriggerVersion;
  uint32_t request_crc = 0;

  if (n == sizeof(trigger_proto::TriggerRequestV4)) {
    trigger_proto::TriggerRequestV4 req;
    std::memcpy(&req, buf, sizeof(req));
    if (req.magic != trigger_proto::kTriggerMagic ||
        req.version != trigger_proto::kTriggerVersion) {
      stats_.malformed_datagrams += 1;
      return reject_now(trigger_proto::kTriggerVersion,
                        "malformed_magic_version",
                        TriggerCode::kMalformedRequest, false);
    }
    if (req.reserved0 != 0 || req.crc32 != trigger_proto::RequestCrcV4(req)) {
      stats_.malformed_datagrams += 1;
      stats_.crc_rejects += 1;
      return reject_now(trigger_proto::kTriggerVersion, "bad_crc",
                        TriggerCode::kMalformedRequest, false);
    }
    if (req.response_timeout_ms < trigger_proto::kMinResponseTimeoutMs ||
        req.response_timeout_ms > trigger_proto::kMaxResponseTimeoutMs) {
      stats_.malformed_datagrams += 1;
      return reject_now(trigger_proto::kTriggerVersion, "bad_timeout",
                        TriggerCode::kMalformedRequest, false);
    }
    if ((req.flags & ~(trigger_proto::kFlagHasTimestamp |
                       trigger_proto::kFlagForce | trigger_proto::kFlagCancel |
                       trigger_proto::kFlagWaitOutcome)) != 0) {
      stats_.malformed_datagrams += 1;
      return reject_now(trigger_proto::kTriggerVersion, "bad_flags",
                        TriggerCode::kMalformedRequest, false);
    }
    req.source[sizeof(req.source) - 1] = '\0';
    request_id = req.request_id;
    if (req.sequence == 0 || request_id == 0) {
      stats_.replay_rejects += 1;
      return reject_now(trigger_proto::kTriggerVersion, "bad_sequence_or_id",
                        TriggerCode::kReplayRejected, true);
    }
    req_timeout_ms = req.response_timeout_ms;
    hint = req.priority_hint;
    timestamp_ns = req.timestamp_ns;
    has_timestamp = (req.flags & trigger_proto::kFlagHasTimestamp) != 0;
    force = (req.flags & trigger_proto::kFlagForce) != 0;
    sequence = req.sequence;
    request_crc = req.crc32;
    std::memcpy(source, req.source, sizeof(source));
    cancel_req = (req.flags & trigger_proto::kFlagCancel) != 0;
    wait_req = (req.flags & trigger_proto::kFlagWaitOutcome) != 0;
    if ((cancel_req || wait_req) &&
        (cancel_req == wait_req || !has_timestamp)) {
      stats_.malformed_datagrams += 1;
      return reject_now(trigger_proto::kTriggerVersion, "bad_flags",
                        TriggerCode::kMalformedRequest, false);
    }
  } else if (n == sizeof(trigger_proto::TriggerRequest)) {
    trigger_proto::TriggerRequest req;
    std::memcpy(&req, buf, sizeof(req));
    if (req.magic != trigger_proto::kTriggerMagic ||
        req.version != trigger_proto::kTriggerVersionV3) {
      stats_.malformed_datagrams += 1;
      return reject_now(trigger_proto::kTriggerVersionV3,
                        "malformed_magic_version",
                        TriggerCode::kMalformedRequest, false);
    }
    if (req.crc32 != trigger_proto::RequestCrc(req)) {
      stats_.malformed_datagrams += 1;
      stats_.crc_rejects += 1;
      return reject_now(trigger_proto::kTriggerVersionV3, "bad_crc",
                        TriggerCode::kMalformedRequest, false);
    }
    if (req.response_timeout_ms < trigger_proto::kMinResponseTimeoutMs ||
        req.response_timeout_ms > trigger_proto::kMaxResponseTimeoutMs) {
      stats_.malformed_datagrams += 1;
      return reject_now(trigger_proto::kTriggerVersionV3, "bad_timeout",
                        TriggerCode::kMalformedRequest, false);
    }
    if ((req.flags & ~(trigger_proto::kFlagHasTimestamp |
                       trigger_proto::kFlagForce | trigger_proto::kFlagCancel |
                       trigger_proto::kFlagWaitOutcome)) != 0) {
      stats_.malformed_datagrams += 1;
      return reject_now(trigger_proto::kTriggerVersionV3, "bad_flags",
                        TriggerCode::kMalformedRequest, false);
    }
    req.source[sizeof(req.source) - 1] = '\0';
    if (req.sequence == 0 || !CheckReplay(peer, req.sequence, now)) {
      stats_.replay_rejects += 1;
      return reject_now(trigger_proto::kTriggerVersionV3, "replay_duplicate",
                        TriggerCode::kReplayRejected, true);
    }
    response_version = trigger_proto::kTriggerVersionV3;
    proto_version = trigger_proto::kTriggerVersionV3;
    req_timeout_ms = req.response_timeout_ms;
    hint = req.priority_hint;
    timestamp_ns = req.timestamp_ns;
    has_timestamp = (req.flags & trigger_proto::kFlagHasTimestamp) != 0;
    force = (req.flags & trigger_proto::kFlagForce) != 0;
    sequence = req.sequence;
    std::memcpy(source, req.source, sizeof(source));
    cancel_req = (req.flags & trigger_proto::kFlagCancel) != 0;
    wait_req = (req.flags & trigger_proto::kFlagWaitOutcome) != 0;
    if ((cancel_req || wait_req) &&
        (cancel_req == wait_req || !has_timestamp)) {
      stats_.malformed_datagrams += 1;
      return reject_now(trigger_proto::kTriggerVersionV3, "bad_flags",
                        TriggerCode::kMalformedRequest, false);
    }
  } else if (n == sizeof(trigger_proto::TriggerRequestV2)) {
    trigger_proto::TriggerRequestV2 req;
    std::memcpy(&req, buf, sizeof(req));
    if (req.magic != trigger_proto::kTriggerMagic ||
        req.version != trigger_proto::kTriggerVersionV2) {
      stats_.malformed_datagrams += 1;
      return reject_now(0, "malformed_magic_version",
                        TriggerCode::kMalformedRequest, false);
    }
    // TRG-07 flag bits are V3-only opcodes: a legacy datagram carrying them
    // is rejected rather than misread as a plain trigger.
    if ((req.flags & ~(trigger_proto::kFlagHasTimestamp |
                       trigger_proto::kFlagForce)) != 0) {
      stats_.malformed_datagrams += 1;
      return reject_now(0, "bad_flags", TriggerCode::kMalformedRequest, false);
    }
    if (!cfg_.allow_legacy_v2) {
      // Authorized peer, but V2 has no CRC: fail closed unless the explicit
      // compat flag was set. Reply with the V2 layout so the old client sees
      // kUnsupportedVersion instead of timing out.
      stats_.malformed_datagrams += 1;
      return reject_now(trigger_proto::kTriggerVersionV2, "legacy_v2_disabled",
                        TriggerCode::kUnsupportedVersion, true);
    }
    req.source[sizeof(req.source) - 1] = '\0';
    if (req.sequence == 0 || !CheckReplay(peer, req.sequence, now)) {
      stats_.replay_rejects += 1;
      return reject_now(trigger_proto::kTriggerVersionV2, "replay_rejected",
                        TriggerCode::kReplayRejected, true);
    }
    response_version = trigger_proto::kTriggerVersionV2;
    proto_version = trigger_proto::kTriggerVersionV2;
    stats_.legacy_v2_requests += 1;
    hint = req.priority_hint;
    timestamp_ns = req.timestamp_ns;
    has_timestamp = (req.flags & trigger_proto::kFlagHasTimestamp) != 0;
    force = (req.flags & trigger_proto::kFlagForce) != 0;
    sequence = req.sequence;
    std::memcpy(source, req.source, sizeof(source));
  } else if (n == sizeof(trigger_proto::TriggerRequestV1)) {
    trigger_proto::TriggerRequestV1 req;
    std::memcpy(&req, buf, sizeof(req));
    if (req.magic != trigger_proto::kTriggerMagic ||
        req.version != trigger_proto::kTriggerVersionLegacy) {
      stats_.malformed_datagrams += 1;
      return reject_now(0, "malformed_magic_version",
                        TriggerCode::kMalformedRequest, false);
    }
    if ((req.flags & ~(trigger_proto::kFlagHasTimestamp |
                       trigger_proto::kFlagForce)) != 0) {
      stats_.malformed_datagrams += 1;
      return reject_now(0, "bad_flags", TriggerCode::kMalformedRequest, false);
    }
    if (!cfg_.allow_legacy_unsequenced) {
      // Authorized peer, but an unsequenced request cannot be replay-checked.
      stats_.malformed_datagrams += 1;
      return reject_now(trigger_proto::kTriggerVersionLegacy, "legacy_disabled",
                        TriggerCode::kUnsupportedVersion, true);
    }
    req.source[sizeof(req.source) - 1] = '\0';
    response_version = trigger_proto::kTriggerVersionLegacy;
    proto_version = trigger_proto::kTriggerVersionLegacy;
    stats_.legacy_requests += 1;
    hint = req.priority_hint;
    timestamp_ns = req.timestamp_ns;
    has_timestamp = (req.flags & trigger_proto::kFlagHasTimestamp) != 0;
    force = (req.flags & trigger_proto::kFlagForce) != 0;
    std::memcpy(source, req.source, sizeof(source));
  } else {
    stats_.malformed_datagrams += 1;
    return reject_now(0, "malformed_size", TriggerCode::kMalformedRequest,
                      false);
  }

  rec.proto_version = proto_version;
  rec.sequence = sequence;
  rec.request_id = request_id;
  rec.source = source;
  rec.hint_raw = hint;

  if (proto_version == trigger_proto::kTriggerVersion) {
    trigger_proto::TriggerResponseV4 cached{};
    bool conflict = false;
    if (FindDedup(peer, request_id, request_crc, now, &cached, &conflict)) {
      stats_.dedup_hits += 1;
      stats_.retry_requests += 1;
      rec.retry = true;
      rec.dedup = true;
      rec.decision = "deduplicated";
      rec.reason = "dedup_hit";
      rec.code = static_cast<TriggerCode>(cached.code);
      rec.trigger_id = cached.trigger_id;
      rec.hint_effective = cached.effective_priority;
      append_audit();
      if (resp != nullptr)
        *resp = cached;
      return true;
    }
    if (conflict) {
      stats_.dedup_conflicts += 1;
      stats_.retry_requests += 1;
      stats_.replay_rejects += 1;
      rec.retry = true;
      return reject_now(response_version, "request_id_conflict",
                        TriggerCode::kReplayRejected, true);
    }
    if (!CheckReplay(peer, sequence, now)) {
      stats_.replay_rejects += 1;
      return reject_now(response_version, "replay_duplicate",
                        TriggerCode::kReplayRejected, true);
    }
  }

  // TRG-07: cancel a pending/running capture by trigger_id (carried in
  // timestamp_ns). Same auth/replay/CRC gauntlet as a trigger, audited with
  // req=cancel; the reply is immediate and explicit.
  auto finish_response = [&]() {
    if (proto_version == trigger_proto::kTriggerVersion && resp != nullptr) {
      RememberDedup(peer, request_id, request_crc, now, *resp);
    }
    return true;
  };

  if (cancel_req) {
    rec.op = "cancel";
    const TriggerCode code = Cancel(timestamp_ns);
    rec.accepted = code == TriggerCode::kCancelled;
    rec.reason = RejectReason(code);
    rec.code = code;
    rec.trigger_id = timestamp_ns;
    append_audit();
    if (resp != nullptr) {
      FillResponse(resp, response_version, code, timestamp_ns, 0, 0,
                   request_id);
    }
    return finish_response();
  }

  if (wait_req) {
    rec.op = "wait";
    const uint64_t filter =
        timestamp_ns == trigger_proto::kWaitFilterAny ? 0 : timestamp_ns;
    OutcomeEntry match;
    if (MatchOutcomeRing(filter, &match)) {
      const TriggerCode code = static_cast<TriggerCode>(match.code);
      rec.accepted = true;
      rec.reason = "ok";
      rec.code = code;
      rec.trigger_id = match.trigger_id;
      append_audit();
      if (resp != nullptr) {
        FillResponse(resp, response_version, code, match.trigger_id, 0,
                     trigger_proto::kRespFlagOutcomeNotification, request_id);
      }
      return finish_response();
    }
    if (reply_path == nullptr || reply_path[0] == '\0') {
      // No return address (in-process caller or abstract socket): the wait
      // cannot be deferred.
      rec.accepted = false;
      rec.reason = "wait_no_channel";
      rec.code = TriggerCode::kBusy;
      append_audit();
      if (resp != nullptr) {
        FillResponse(resp, response_version, TriggerCode::kBusy, 0, 0, 0,
                     request_id);
      }
      return finish_response();
    }
    WaiterEntry* slot = nullptr;
    for (WaiterEntry& w : waiters_) {
      if (!w.used) {
        slot = &w;
        break;
      }
    }
    if (slot == nullptr) {
      stats_.notify_waiter_overflows += 1;
      rec.accepted = false;
      rec.reason = "notify_waiter_overflow";
      rec.code = TriggerCode::kBusy;
      append_audit();
      if (resp != nullptr) {
        FillResponse(resp, response_version, TriggerCode::kBusy, 0, 0, 0,
                     request_id);
      }
      return finish_response();
    }
    slot->used = true;
    slot->uid = peer.uid;
    slot->pid = peer.pid;
    slot->filter_id = filter;
    slot->deadline_ns =
        now + static_cast<uint64_t>(req_timeout_ms) * 1000000ull;
    slot->request_id = request_id;
    slot->proto_version = proto_version;
    std::snprintf(slot->reply_path, sizeof(slot->reply_path), "%s", reply_path);
    rec.accepted = true;
    rec.reason = "wait_registered";
    rec.code = TriggerCode::kAccepted;
    rec.trigger_id = filter;
    append_audit();
    return false;
  }

  const SubmitResult r =
      Submit(source, hint, timestamp_ns, has_timestamp, force);
  rec.hint_effective = r.priority_used;
  rec.priority_clamped = r.priority_clamped;
  rec.quota_override = r.quota_overridden;
  rec.accepted = r.code == TriggerCode::kAccepted;
  rec.reason = RejectReason(r.code);
  rec.code = r.code;
  rec.trigger_id = r.trigger_id;
  if (rec.accepted) {
    pending_peer_pid_ = peer.pid;
    pending_peer_uid_ = peer.uid;
    pending_proto_ = proto_version;
  }
  append_audit();

  if (resp != nullptr) {
    FillResponse(resp, response_version, r.code, r.trigger_id, r.priority_used,
                 r.priority_clamped ? trigger_proto::kRespFlagPriorityClamped
                                    : 0u,
                 request_id);
  }
  return finish_response();
}

bool Center::ProcessRequest(const uint8_t *buf, std::size_t n,
                            const PeerCred &peer,
                            trigger_proto::TriggerResponse *resp,
                            const char *reply_path) {
  if (n == sizeof(trigger_proto::TriggerRequestV4))
    return false;
  trigger_proto::TriggerResponseV4 wide{};
  if (!ProcessRequest(buf, n, peer, &wide, reply_path))
    return false;
  resp->magic = wide.magic;
  resp->version = wide.version;
  resp->code = wide.code;
  resp->trigger_id = wide.trigger_id;
  resp->effective_priority = wide.effective_priority;
  resp->flags = wide.flags;
  resp->reserved0 = 0;
  resp->crc32 = 0;
  if (wide.version == trigger_proto::kTriggerVersionV3) {
    trigger_proto::StampResponseCrc(resp);
  }
  return true;
}

void Center::PollSocket() {
#if !defined(_WIN32)
  if (fd_ < 0)
    return;
  const uint64_t started_ns = MonoNs();
  for (uint32_t count = 0; count < kSocketSafetyCap; ++count) {
    uint8_t buf[512];
    PeerCred peer;
#if defined(__linux__)
    char cmsgbuf[CMSG_SPACE(sizeof(struct ucred))];
    sockaddr_un peer_addr{};
    iovec iov{};
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);
    msghdr msg{};
    msg.msg_name = &peer_addr;
    msg.msg_namelen = sizeof(peer_addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);
    const ssize_t n = recvmsg(fd_, &msg, MSG_DONTWAIT);
    if (n < 0) return;  // EAGAIN: drained
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level == SOL_SOCKET &&
          cmsg->cmsg_type == SCM_CREDENTIALS &&
          cmsg->cmsg_len == CMSG_LEN(sizeof(struct ucred))) {
        const struct ucred* cred =
            reinterpret_cast<const struct ucred*>(CMSG_DATA(cmsg));
        peer.uid = cred->uid;
        peer.pid = cred->pid;
        break;
      }
    }
#else
    sockaddr_un peer_addr{};
    socklen_t peer_len = sizeof(peer_addr);
    const ssize_t n = recvfrom(fd_, buf, sizeof(buf), MSG_DONTWAIT,
                               reinterpret_cast<sockaddr*>(&peer_addr),
                               &peer_len);
    if (n < 0) return;
#endif
    stats_.socket_datagrams += 1;
    trigger_proto::TriggerResponseV4 resp{};
    const char* reply_path =
        peer_addr.sun_path[0] != '\0' ? peer_addr.sun_path : nullptr;
    const bool reply = ProcessRequest(buf, static_cast<std::size_t>(n), peer,
                                      &resp, reply_path);
    if (reply && reply_path != nullptr) {
      uint8_t wire[sizeof(trigger_proto::TriggerResponseV4)];
      const std::size_t resp_size = SerializeResponse(resp, wire);
      const ssize_t sent =
          sendto(fd_, wire, resp_size, 0,
                 reinterpret_cast<const sockaddr*>(&peer_addr),
                 sizeof(peer_addr));
      if (sent != static_cast<ssize_t>(resp_size)) {
        stats_.reply_send_failures += 1;
        THYME_LOG_WARN_RL("trigger_center: reply sendto failed: %s",
                          std::strerror(errno));
      }
    }
    if (MonoNs() - started_ns >= cfg_.socket_poll_budget_ns) break;
  }
  uint8_t probe = 0;
  if (recv(fd_, &probe, sizeof(probe), MSG_PEEK | MSG_DONTWAIT) >= 0) {
    stats_.socket_backlog_events += 1;
  }
#endif
}

void Center::Tick() {
  const uint64_t now = MonoNs();
  PollSocket();
  StartPendingTrigger();  // T2: deferred start/preempt of accepted triggers
  CompleteCaptureIfDone(now);
  if (state_ == State::kMuted && now >= mute_until_ns_) {
    NoteFsmEvent(current_trigger_id_, State::kMuted, State::kIdle,
                 FsmReason::kMuteExpired, now);
    state_ = State::kIdle;
  }
  ExpireWaiters(now);  // TRG-07: drop waiters past their declared deadline
  GcQuotaWindows(now);
  audit_.MaybeSync(now);
  DumpFsmEventsIfDue(now);  // periodically dump FSM events
}

// OBS-05 -----------------------------------------------------------------

void Center::NoteFsmEvent(uint64_t trigger_id, State from, State to,
                          FsmReason reason, uint64_t now) {
  FsmEvent& e = fsm_events_[fsm_idx_ % kFsmEventRingSize];
  e.mono_ns = now;
  e.trigger_id = trigger_id;
  e.from = static_cast<uint8_t>(from);
  e.to = static_cast<uint8_t>(to);
  e.reason = static_cast<uint8_t>(reason);
  e.rsvd = 0;
  if (fsm_idx_ >= kFsmEventRingSize) {
    stats_.fsm_event_drops += 1;  // ring full: overwrite the oldest
  }
  ++fsm_idx_;
  ++stats_.fsm_events;
}

uint32_t Center::CopyFsmEvents(FsmEvent* out, uint32_t cap) const {
  const uint64_t avail = fsm_idx_ < kFsmEventRingSize
                             ? fsm_idx_
                             : kFsmEventRingSize;
  const uint32_t n = avail < cap ? static_cast<uint32_t>(avail) : cap;
  for (uint32_t i = 0; i < n; ++i) {
    out[i] = fsm_events_[(fsm_idx_ - avail + i) % kFsmEventRingSize];
  }
  return n;
}

bool Center::DumpNewFsmEvents() {
  if (!fsm_file_.enabled())
    return false;
  const uint64_t avail = fsm_idx_ - fsm_dumped_;
  if (avail == 0)
    return false;
  const uint64_t begin =
      avail > kFsmEventRingSize ? fsm_idx_ - kFsmEventRingSize : fsm_dumped_;
  uint64_t i = begin;
  for (; i < fsm_idx_; ++i) {
    const FsmEvent& e = fsm_events_[i % kFsmEventRingSize];
    char line[128];
    const int n = std::snprintf(
        line, sizeof(line),
        "{\"ts\":%llu,\"id\":%llu,\"from\":%u,\"to\":%u,\"why\":%u}\n",
        static_cast<unsigned long long>(e.mono_ns),
        static_cast<unsigned long long>(e.trigger_id),
        static_cast<unsigned>(e.from), static_cast<unsigned>(e.to),
        static_cast<unsigned>(e.reason));
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(line))
      break;
    // OPS-05: one unbuffered write() (the perf-dumper contract): the crash
    // window is at most the line being written, never an interval of buffered
    // events, and a rotation happens inside this call at most once.
    if (fsm_file_.Append(line, static_cast<std::size_t>(n)) !=
        static_cast<std::ptrdiff_t>(n)) {
      break;  // keep the watermark, retry next period
    }
  }
  // No fflush: each line is already a completed write() into the page cache.
  // O_APPEND guarantees a truncated tail can only be the last line, which
  // parsers stop at.
  fsm_dumped_ = i;
  stats_.fsm_rotations = fsm_file_.rotations();
  stats_.fsm_bytes = fsm_file_.bytes_written();
  stats_.fsm_write_drops = fsm_file_.drops();
  return i > begin;
}

void Center::DumpFsmEventsIfDue(uint64_t now) {
  if (!fsm_file_.enabled())
    return;
  if (fsm_last_dump_ns_ != 0 &&
      now - fsm_last_dump_ns_ < fsm_dump_interval_ns_) {
    return;
  }
  if (DumpNewFsmEvents())
    fsm_last_dump_ns_ = now;
}

}  // namespace trigger_center
}  // namespace thyme
