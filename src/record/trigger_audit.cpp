#include "thyme/record/trigger_audit.h"

#include <cstdio>
#include <ctime>

#if !defined(_WIN32)
#include <time.h>
#endif

namespace thyme {
namespace trigger_audit {
namespace {

#if !defined(_WIN32)
uint64_t MonoNsNow() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}
#endif

const char* CodeName(trigger_proto::TriggerCode code) {
  using trigger_proto::TriggerCode;
  switch (code) {
    case TriggerCode::kAccepted: return "accepted";
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

// Wire `source` is attacker-controlled: keep the line one token by escaping
// anything outside a conservative identifier set as %HH.
void AppendEscapedSource(char* dst, std::size_t cap, const char* src) {
  std::size_t o = 0;
  for (std::size_t i = 0; src != nullptr && src[i] != '\0' && o + 4 < cap; ++i) {
    const unsigned char c = static_cast<unsigned char>(src[i]);
    const bool plain = (c > 0x20 && c < 0x7f && c != '%' && c != '=');
    if (plain) {
      dst[o++] = static_cast<char>(c);
    } else {
      const int w = std::snprintf(dst + o, cap - o, "%%%02X", c);
      if (w <= 0) break;
      o += static_cast<std::size_t>(w);
    }
  }
  if (o == 0) dst[o++] = '-';
  dst[o] = '\0';
}

}  // namespace

Log::~Log() { Close(); }

bool Log::Open(const Config& cfg) {
  Close();
  if (cfg.path.empty()) return false;
  fsync_interval_ns_ = cfg.fsync_interval_ns;
  records_ = 0;
  drops_ = 0;
#if defined(_WIN32)
  return false;
#else
  logging::RotateConfig rcfg;
  rcfg.path = cfg.path;
  rcfg.max_bytes = cfg.max_bytes;
  rcfg.keep = cfg.rotate_keep;
  // SEC-02: the audit trail is mandatory. The process budget never drops an
  // audit record — it may only suppress non-mandatory observability writers.
  rcfg.mandatory = true;
  rcfg.account = true;
  last_sync_ns_ = MonoNsNow();
  return file_.Open(rcfg);
#endif
}

bool Log::Append(const Record& r) {
#if defined(_WIN32)
  (void)r;
  return false;
#else
  if (!file_.enabled()) {
    drops_ += 1;
    return false;
  }

  struct timespec rt {};
  clock_gettime(CLOCK_REALTIME, &rt);
  struct tm tmv {};
  gmtime_r(&rt.tv_sec, &tmv);
  char ts[64];
  std::snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02d.%09ldZ",
                tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                tmv.tm_min, tmv.tm_sec, rt.tv_nsec);
  const uint64_t ts_ns = static_cast<uint64_t>(rt.tv_sec) * 1000000000ULL +
      static_cast<uint64_t>(rt.tv_nsec);

  char src[256];
  AppendEscapedSource(src, sizeof(src), r.source);

  char line[768];
  const char *decision = r.decision != nullptr
                             ? r.decision
                             : (r.accepted ? "accepted" : "rejected");
  const int n = std::snprintf(
      line, sizeof(line),
      "ts=%s ts_ns=%llu mono_ns=%llu pid=%d uid=%u req=%s proto=%u "
      "seq=%llu request_id=%llu retry=%d dedup=%d src=%s hint_raw=%d "
      "hint_eff=%d clamp=%d decision=%s reason=%s code=%s trigger_id=%llu "
      "quota_override=%d by=%llu",
      ts, static_cast<unsigned long long>(ts_ns),
      static_cast<unsigned long long>(MonoNsNow()),
      static_cast<int>(r.peer_pid), static_cast<unsigned>(r.peer_uid),
      r.op != nullptr ? r.op : "trigger",
      static_cast<unsigned>(r.proto_version),
      static_cast<unsigned long long>(r.sequence),
      static_cast<unsigned long long>(r.request_id), r.retry ? 1 : 0,
      r.dedup ? 1 : 0, src, static_cast<int>(r.hint_raw),
      static_cast<int>(r.hint_effective), r.priority_clamped ? 1 : 0, decision,
      r.reason != nullptr ? r.reason : "unknown", CodeName(r.code),
      static_cast<unsigned long long>(r.trigger_id), r.quota_override ? 1 : 0,
      static_cast<unsigned long long>(r.by_trigger_id));
  if (n <= 0) {
    drops_ += 1;
    return false;
  }
  std::size_t len = static_cast<std::size_t>(n);
  if (len < sizeof(line)) {
    const int suffix = std::snprintf(
        line + len, sizeof(line) - len,
                                     r.integrity_mode != nullptr ? " integrity_mode=%s\n" : "\n",
                                     r.integrity_mode);
    if (suffix > 0)
      len += static_cast<std::size_t>(suffix);
  }
  if (len >= sizeof(line)) {
    // Should not happen with the bounded fields above, but never lose the
    // record delimiter: a long line must stay one parseable line.
    line[sizeof(line) - 2] = '\n';
    line[sizeof(line) - 1] = '\0';
    len = sizeof(line) - 1;
  }
  // One unbuffered write(): the rotation check + budget check + write happen
  // inside RotatingFile, so a slow/full disk stalls the control path for at
  // most this one small record.
  const std::ptrdiff_t wrote = file_.Append(line, len);
  if (wrote == static_cast<std::ptrdiff_t>(len)) {
    records_ += 1;
    return true;
  }
  // RotatingFile counts the failed/partial write itself (drops() adds its
  // counter); only line-formatting failures are counted here.
  if (wrote > 0 && static_cast<std::size_t>(wrote) < len) {
    // Terminate a partially written record so the next line stays parseable.
    const char nl = '\n';
    (void)file_.Append(&nl, 1);
  }
  return false;
#endif
}

void Log::MaybeSync(uint64_t mono_ns) {
#if defined(_WIN32)
  (void)mono_ns;
#else
  if (!file_.enabled() || fsync_interval_ns_ == 0)
    return;
  if (last_sync_ns_ != 0 && mono_ns - last_sync_ns_ < fsync_interval_ns_) {
    return;
  }
  last_sync_ns_ = mono_ns;
  (void)file_.Sync();
#endif
}

void Log::Close() {
  if (file_.enabled())
    (void)file_.Sync();
  file_.Close();
}

}  // namespace trigger_audit
}  // namespace thyme
