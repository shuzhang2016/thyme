#include "thyme/record/record_control.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

#include "thyme/runtime/log.h"

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#include <sys/types.h>
#endif
#if defined(__APPLE__)
#include <sys/ucred.h>
#endif
#endif

namespace thyme {
namespace record_control {
namespace {

const char *PhaseName(RecorderPhase phase) {
  switch (phase) {
    case RecorderPhase::kArmed:
      return "armed";
    case RecorderPhase::kPost:
      return "post";
    case RecorderPhase::kSettle:
      return "settle";
  }
  return "unknown";
}

std::string JsonEscape(const char *s) {
  std::string out;
  for (const char *p = s; p != nullptr && *p != '\0'; ++p) {
    switch (*p) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += *p;
    }
  }
  return out;
}

uint64_t MonoNs() {
#if defined(_WIN32)
  return 0;
#else
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
#endif
}

bool CommandOp(const char *buf, ControlOp *op) {
  if (std::strcmp(buf, "trigger") == 0) {
    *op = ControlOp::kTrigger;
  } else if (std::strcmp(buf, "arm") == 0) {
    *op = ControlOp::kArm;
  } else if (std::strcmp(buf, "disarm") == 0) {
    *op = ControlOp::kDisarm;
  } else if (std::strcmp(buf, "status") == 0) {
    *op = ControlOp::kStatus;
  } else if (std::strcmp(buf, "status-json") == 0) {
    *op = ControlOp::kStatusJson;
  } else {
    return false;
  }
  return true;
}

ControlCmd ToCommand(ControlOp op) {
  switch (op) {
    case ControlOp::kTrigger:
      return ControlCmd::kTrigger;
    case ControlOp::kArm:
      return ControlCmd::kArm;
    case ControlOp::kDisarm:
      return ControlCmd::kDisarm;
    case ControlOp::kStatus:
    case ControlOp::kStatusJson:
      return ControlCmd::kStatus;
  }
  return ControlCmd::kNone;
}

const char *OpName(ControlOp op) {
  switch (op) {
    case ControlOp::kTrigger:
      return "trigger";
    case ControlOp::kArm:
      return "arm";
    case ControlOp::kDisarm:
      return "disarm";
    case ControlOp::kStatus:
      return "status";
    case ControlOp::kStatusJson:
      return "status-json";
  }
  return "unknown";
}

#if !defined(_WIN32)
// Captures the sender address and kernel-supplied peer credentials.
ssize_t RecvCommand(int fd, uint8_t *buf, std::size_t cap, uid_t *peer_uid,
                    pid_t *peer_pid, sockaddr_un *from, socklen_t *from_len) {
  *peer_uid = static_cast<uid_t>(-1);
  *peer_pid = 0;
#if defined(__linux__)
  char cmsgbuf[CMSG_SPACE(sizeof(struct ucred))];
  iovec iov{};
  iov.iov_base = buf;
  iov.iov_len = cap;
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof(cmsgbuf);
  msg.msg_name = from;
  msg.msg_namelen = *from_len;
  const ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT);
  if (n < 0) return n;
  *from_len = msg.msg_namelen;

  for (cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
       cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS &&
        cmsg->cmsg_len == CMSG_LEN(sizeof(struct ucred))) {
      const struct ucred *cred =
          reinterpret_cast<const struct ucred *>(CMSG_DATA(cmsg));
      *peer_uid = cred->uid;
      *peer_pid = cred->pid;
      break;
    }
  }
  return n;
#elif defined(__APPLE__)
  *from_len = sizeof(*from);
  const ssize_t n = recvfrom(fd, buf, cap, MSG_DONTWAIT,
                             reinterpret_cast<sockaddr *>(from), from_len);
  xucred cred{};
  socklen_t cred_len = sizeof(cred);
  if (n >= 0 &&
      getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &cred, &cred_len) == 0) {
    *peer_uid = cred.cr_uid;
  }
  return n;
#else
  (void)from;
  *from_len = 0;
  return recv(fd, buf, cap, MSG_DONTWAIT);
#endif
}
#endif

}  // namespace

Server::~Server() { Close(); }

bool Server::Open(const char *path, const Config &cfg) {
#if defined(_WIN32)
  (void)path;
  (void)cfg;
  return false;
#else
  if (path == nullptr || path[0] == '\0') return false;
  // Re-entry: close and unlink the previously bound socket instead of
  // leaking its fd.
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
    if (path_[0] != '\0') unlink(path_);
  }
  if (wake_fd_ >= 0) {
    close(wake_fd_);
    wake_fd_ = -1;
  }
  std::snprintf(path_, sizeof(path_), "%s", path);

  struct stat st {};
  if (stat(path_, &st) == 0) {
    if (!S_ISSOCK(st.st_mode)) {
      THYME_LOG_ERROR(
          "recorder: control socket path exists and is not a "
          "socket: %s",
          path_);
      return false;
    }
    if (st.st_uid != geteuid()) {
      THYME_LOG_ERROR("recorder: control socket %s owned by uid %u (euid %u)",
                      path_, static_cast<unsigned>(st.st_uid),
                      static_cast<unsigned>(geteuid()));
      return false;
    }
    unlink(path_);
  }

  fd_ = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd_ < 0) return false;

#if defined(__linux__)
  {
    const int passcred = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_PASSCRED, &passcred, sizeof(passcred)) !=
        0) {
      THYME_LOG_ERROR("recorder: SO_PASSCRED failed: %s", std::strerror(errno));
      close(fd_);
      fd_ = -1;
      return false;
    }
  }
#endif

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path_);
  if (bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    close(fd_);
    fd_ = -1;
    return false;
  }
  // ARCH-05 / SEC-02: control socket is a security boundary — no silent
  // permission or blocking-mode downgrade.
  if (chmod(path_, S_IRUSR | S_IWUSR) != 0) {
    THYME_LOG_ERROR("recorder: chmod %s failed: %s", path_,
                    std::strerror(errno));
    close(fd_);
    fd_ = -1;
    unlink(path_);
    return false;
  }
  const int flags = fcntl(fd_, F_GETFL, 0);
  if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) != 0) {
    THYME_LOG_ERROR("recorder: fcntl O_NONBLOCK on %s failed: %s", path_,
                    std::strerror(errno));
    close(fd_);
    fd_ = -1;
    unlink(path_);
    return false;
  }
  cfg_ = cfg;
  if (cfg_.audit_log_path.empty())
    cfg_.audit_log_path = std::string(path_) + ".audit";
  trigger_audit::Config audit_cfg;
  audit_cfg.path = cfg_.audit_log_path;
  audit_cfg.max_bytes = cfg_.audit_max_bytes;
  audit_cfg.rotate_keep = cfg_.audit_rotate_keep;
  audit_cfg.fsync_interval_ns = cfg_.audit_fsync_interval_ns;
  if (!audit_.Open(audit_cfg)) {
    THYME_LOG_ERROR("recorder: control audit %s open failed",
                    cfg_.audit_log_path.c_str());
    close(fd_);
    fd_ = -1;
    unlink(path_);
    return false;
  }
  if (cfg_.allow_legacy_control) {
    THYME_LOG_WARN(
        "recorder: WARNING allow_legacy_control=true accepts "
        "replayable V1 text commands");
  }
#if defined(__linux__)
  wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd_ < 0) {
    THYME_LOG_ERROR("recorder: eventfd failed: %s", std::strerror(errno));
    audit_.Close();
    close(fd_);
    fd_ = -1;
    unlink(path_);
    return false;
  }
#endif
  phase_.store(static_cast<int>(RecorderPhase::kArmed),
               std::memory_order_release);
  return true;
#endif
}

bool Server::ReloadConfig(const Config &cfg) {
  const std::string effective_path = cfg.audit_log_path.empty()
                                         ? std::string(path_) + ".audit"
                                         : cfg.audit_log_path;
  if (effective_path != cfg_.audit_log_path ||
      cfg.audit_max_bytes != cfg_.audit_max_bytes ||
      cfg.audit_rotate_keep != cfg_.audit_rotate_keep ||
      cfg.audit_fsync_interval_ns != cfg_.audit_fsync_interval_ns) {
    return false;
  }
  cfg_ = cfg;
  cfg_.audit_log_path = effective_path;
  if (cfg_.allow_legacy_control) {
    THYME_LOG_WARN(
        "recorder: WARNING allow_legacy_control=true accepts "
        "replayable V1 text commands");
  }
  return true;
}

void Server::Close() {
#if !defined(_WIN32)
  if (fd_ >= 0) close(fd_);
  fd_ = -1;
  if (wake_fd_ >= 0) close(wake_fd_);
  wake_fd_ = -1;
  pending_trigger_ = false;
  if (path_[0] != '\0') unlink(path_);
  path_[0] = '\0';
#endif
  audit_.Close();
}

bool Server::TakeTrigger(TriggerParams *params, std::string *channels) {
  if (!pending_trigger_) return false;
  pending_trigger_ = false;
  if (params != nullptr) *params = pending_;
  if (channels != nullptr) {
    channels->assign(pending_channels_, pending_channel_bytes_);
  }
  return true;
}

int Server::WakeFd() const { return wake_fd_; }

void Server::SetPhase(RecorderPhase phase) {
  phase_.store(static_cast<int>(phase), std::memory_order_release);
}

uint64_t Server::NextTriggerId() {
  return next_id_.fetch_add(1, std::memory_order_relaxed);
}

bool Server::IsArmed() const { return armed_.load(std::memory_order_acquire); }

RecorderPhase Server::CurrentPhase() const {
  return static_cast<RecorderPhase>(phase_.load(std::memory_order_acquire));
}

void Server::PrintStatus() const {
  THYME_LOG_INFO_FIELDS(
      "recorder: status",
      log::Fields()
          .Bool("armed", IsArmed())
          .Str("phase", PhaseName(CurrentPhase()))
          .U64("next_id", next_id_.load(std::memory_order_acquire))
          .U64("auth_rejects", auth_rejects_.load(std::memory_order_acquire)));
}

uint64_t Server::AuthRejectCount() const {
  return auth_rejects_.load(std::memory_order_acquire);
}

uint64_t Server::ReplayRejectCount() const {
  return replay_rejects_.load(std::memory_order_acquire);
}

uint64_t Server::CrcRejectCount() const {
  return crc_rejects_.load(std::memory_order_acquire);
}

uint64_t Server::LegacyRequestCount() const {
  return legacy_requests_.load(std::memory_order_acquire);
}

uint64_t Server::ControlAuditDropCount() const {
  return control_audit_drops_.load(std::memory_order_acquire);
}

void Server::SetStatusSnapshot(const StatusSnapshot &snapshot) {
  captures_done_.store(snapshot.captures_done, std::memory_order_relaxed);
  slices_done_.store(snapshot.slices_done, std::memory_order_relaxed);
  pending_bytes_.store(snapshot.pending_bytes, std::memory_order_relaxed);
  disk_full_.store(snapshot.disk_full, std::memory_order_relaxed);
  triggers_rejected_.store(snapshot.triggers_rejected,
                           std::memory_order_relaxed);
  descriptors_queued_.store(snapshot.descriptors_queued,
                            std::memory_order_relaxed);
  pending_evicted_.store(snapshot.pending_evicted, std::memory_order_relaxed);
  overrun_before_queue_.store(snapshot.overrun_before_queue,
                              std::memory_order_relaxed);
  samples_written_.store(snapshot.samples_written, std::memory_order_relaxed);
  frames_written_.store(snapshot.frames_written, std::memory_order_relaxed);
  holes_written_.store(snapshot.holes_written, std::memory_order_relaxed);
  torn_reads_.store(snapshot.torn_reads, std::memory_order_relaxed);
  harvest_retries_.store(snapshot.harvest_retries, std::memory_order_relaxed);
  session_reattaches_.store(snapshot.session_reattaches,
                            std::memory_order_relaxed);
  source_errors_.store(snapshot.source_errors, std::memory_order_relaxed);
  disk_errors_.store(snapshot.disk_errors, std::memory_order_relaxed);
  sync_errors_.store(snapshot.sync_errors, std::memory_order_relaxed);
  audit_drops_.store(snapshot.audit_drops, std::memory_order_relaxed);
  fsm_event_drops_.store(snapshot.fsm_event_drops, std::memory_order_relaxed);
  service_records_written_.store(snapshot.service_records_written,
                                 std::memory_order_relaxed);
  service_records_suppressed_.store(snapshot.service_records_suppressed,
                                    std::memory_order_relaxed);
  service_records_dropped_.store(snapshot.service_records_dropped,
                                 std::memory_order_relaxed);
  service_records_truncated_.store(snapshot.service_records_truncated,
                                   std::memory_order_relaxed);
  service_rotations_.store(snapshot.service_rotations,
                           std::memory_order_relaxed);
  service_sink_open_.store(snapshot.service_sink_open,
                           std::memory_order_relaxed);
  integrity_mode_.store(static_cast<uint8_t>(snapshot.integrity_mode),
                        std::memory_order_relaxed);
}

std::string Server::StatusText() const {
  char buf[512];
  std::snprintf(
      buf, sizeof(buf),
      "socket=%s armed=%s phase=%s next_trigger_id=%llu auth_rejects=%llu "
      "captures_done=%llu slices_done=%llu pending_bytes=%llu disk_full=%s "
      "integrity_mode=%s",
      path_, IsArmed() ? "true" : "false", PhaseName(CurrentPhase()),
      static_cast<unsigned long long>(next_id_.load(std::memory_order_acquire)),
      static_cast<unsigned long long>(
          auth_rejects_.load(std::memory_order_acquire)),
      static_cast<unsigned long long>(
          captures_done_.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          slices_done_.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          pending_bytes_.load(std::memory_order_relaxed)),
      disk_full_.load(std::memory_order_relaxed) ? "true" : "false",
      integrity::ManifestModeName(static_cast<integrity::ManifestMode>(
          integrity_mode_.load(std::memory_order_relaxed))));
  return buf;
}

std::string Server::StatusJson() const {
  std::string out =
      "{\"schema\":1,\"kind\":\"thyme-recorder-status\","
      "\"ok\":true,\"socket\":\"";
  out += JsonEscape(path_);
  out += "\",\"armed\":";
  out += IsArmed() ? "true" : "false";
  out += ",\"phase\":\"";
  out += PhaseName(CurrentPhase());
  out += "\",\"next_trigger_id\":";
  out += std::to_string(next_id_.load(std::memory_order_acquire));
  out += ",\"auth_rejects\":";
  out += std::to_string(auth_rejects_.load(std::memory_order_acquire));
  out += ",\"captures_done\":";
  out += std::to_string(captures_done_.load(std::memory_order_relaxed));
  out += ",\"slices_done\":";
  out += std::to_string(slices_done_.load(std::memory_order_relaxed));
  out += ",\"pending_bytes\":";
  out += std::to_string(pending_bytes_.load(std::memory_order_relaxed));
  out += ",\"disk_full\":";
  out += disk_full_.load(std::memory_order_relaxed) ? "true" : "false";
  out += ",\"integrity_mode\":\"";
  out += integrity::ManifestModeName(static_cast<integrity::ManifestMode>(
      integrity_mode_.load(std::memory_order_relaxed)));
  out += "\"";
  out += ",\"stats\":{\"captures_done\":";
  out += std::to_string(captures_done_.load(std::memory_order_relaxed));
  const std::pair<const char *, const std::atomic<uint64_t> *> stats[] = {
      {"triggers_rejected", &triggers_rejected_},
      {"descriptors_queued", &descriptors_queued_},
      {"pending_evicted", &pending_evicted_},
      {"overrun_before_queue", &overrun_before_queue_},
      {"samples_written", &samples_written_},
      {"frames_written", &frames_written_},
      {"holes_written", &holes_written_},
      {"torn_reads", &torn_reads_},
      {"harvest_retries", &harvest_retries_},
      {"session_reattaches", &session_reattaches_},
      {"source_errors", &source_errors_},
      {"disk_errors", &disk_errors_},
      {"sync_errors", &sync_errors_},
  };
  for (const auto &stat : stats) {
    out += ",\"";
    out += stat.first;
    out += "\":";
    out += std::to_string(stat.second->load(std::memory_order_relaxed));
  }
  out += "},\"trigger\":{\"audit_drops\":";
  out += std::to_string(audit_drops_.load(std::memory_order_relaxed));
  out += ",\"fsm_event_drops\":";
  out += std::to_string(fsm_event_drops_.load(std::memory_order_relaxed));
  out += "},\"logging\":{\"service_sink_open\":";
  out += service_sink_open_.load(std::memory_order_relaxed) ? "true" : "false";
  out += ",\"service_records_written\":";
  out += std::to_string(service_records_written_.load(std::memory_order_relaxed));
  out += ",\"service_records_suppressed\":";
  out += std::to_string(
      service_records_suppressed_.load(std::memory_order_relaxed));
  out += ",\"service_records_dropped\":";
  out += std::to_string(service_records_dropped_.load(std::memory_order_relaxed));
  out += ",\"service_records_truncated\":";
  out += std::to_string(
      service_records_truncated_.load(std::memory_order_relaxed));
  out += ",\"service_rotations\":";
  out += std::to_string(service_rotations_.load(std::memory_order_relaxed));
  out += "},\"control\":{\"protocol_version\":2,\"auth_rejects\":";
  out += std::to_string(AuthRejectCount());
  out += ",\"replay_rejects\":";
  out += std::to_string(ReplayRejectCount());
  out += ",\"crc_rejects\":";
  out += std::to_string(CrcRejectCount());
  out += ",\"legacy_requests\":";
  out += std::to_string(LegacyRequestCount());
  out += ",\"audit_drops\":";
  out += std::to_string(ControlAuditDropCount());
  out += "}}";
  return out;
}

const char *Server::AuthRejectReason(uid_t peer_uid, pid_t peer_pid) const {
#if defined(__linux__)
  if (peer_uid == static_cast<uid_t>(-1)) return "auth_no_credentials";
  if (cfg_.require_peer_pid && peer_pid <= 0) return "auth_invalid_pid";
#elif defined(__APPLE__)
  if (peer_uid == static_cast<uid_t>(-1)) return nullptr;
#else
  (void)peer_pid;
#endif
#if defined(__linux__) || defined(__APPLE__)
  if (!cfg_.allowed_uids.empty()) {
    for (uint32_t uid : cfg_.allowed_uids) {
      if (peer_uid == static_cast<uid_t>(uid)) return nullptr;
    }
    if (peer_uid == 0 && cfg_.allow_root) return nullptr;
    return "auth_uid_not_allowed";
  }
  if (peer_uid == geteuid() || (peer_uid == 0 && cfg_.allow_root))
    return nullptr;
  return "auth_uid_not_allowed";
#else
  (void)peer_uid;
  return nullptr;
#endif
}

bool Server::CheckReplay(uid_t peer_uid, pid_t peer_pid, uint64_t sequence,
                         uint64_t now_ns) {
  ReplayEntry *entry = nullptr;
  for (ReplayEntry &candidate : replay_) {
    if (candidate.used && candidate.uid == peer_uid &&
        candidate.pid == peer_pid) {
      entry = &candidate;
      break;
    }
  }
  if (entry == nullptr) {
    ReplayEntry *oldest = &replay_[0];
    for (ReplayEntry &candidate : replay_) {
      if (!candidate.used) {
        oldest = &candidate;
        break;
      }
      if (candidate.last_ns < oldest->last_ns) oldest = &candidate;
    }
    entry = oldest;
    entry->used = true;
    entry->uid = peer_uid;
    entry->pid = peer_pid;
    entry->max_seq = sequence;
    entry->bitmap = 1;
    entry->last_ns = now_ns;
    return true;
  }
  if (sequence > entry->max_seq) {
    const uint64_t delta = sequence - entry->max_seq;
    entry->bitmap = delta >= 64 ? 0 : (entry->bitmap << delta);
    entry->bitmap |= 1;
    entry->max_seq = sequence;
    entry->last_ns = now_ns;
    return true;
  }
  const uint64_t delta = entry->max_seq - sequence;
  if (delta >= 64) return false;
  const uint64_t bit = 1ull << delta;
  if ((entry->bitmap & bit) != 0) return false;
  entry->bitmap |= bit;
  entry->last_ns = now_ns;
  return true;
}

bool Server::AppendAudit(uid_t peer_uid, pid_t peer_pid, const char *op,
                         uint16_t version, uint64_t sequence, bool accepted,
                         const char *reason, ControlCode code) {
  trigger_audit::Record record;
  record.peer_uid = peer_uid;
  record.peer_pid = peer_pid;
  record.proto_version = version;
  record.sequence = sequence;
  record.op = op;
  record.accepted = accepted;
  record.reason = reason;
  switch (code) {
    case ControlCode::kAccepted:
      record.code = trigger_proto::TriggerCode::kAccepted;
      break;
    case ControlCode::kUnsupportedVersion:
      record.code = trigger_proto::TriggerCode::kUnsupportedVersion;
      break;
    case ControlCode::kReplayRejected:
      record.code = trigger_proto::TriggerCode::kReplayRejected;
      break;
    case ControlCode::kAuthRejected:
      record.code = trigger_proto::TriggerCode::kAuthRejected;
      break;
    case ControlCode::kMalformedRequest:
      record.code = trigger_proto::TriggerCode::kMalformedRequest;
      break;
  }
  if (audit_.Append(record)) return true;
  control_audit_drops_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

bool Server::ProcessCommand(const char *buf, uid_t peer_uid, ControlCmd &cmd) {
#if defined(_WIN32)
  (void)buf;
  (void)peer_uid;
  cmd = ControlCmd::kNone;
  return false;
#else
  if (buf == nullptr) {
    cmd = ControlCmd::kNone;
    return false;
  }
  ControlResponse response{};
  std::string payload;
  return ProcessRequest(reinterpret_cast<const uint8_t *>(buf),
                        std::strlen(buf), peer_uid, getpid(), cmd, &response,
                        &payload);
#endif
}

bool Server::ProcessRequest(const uint8_t *buf, std::size_t size,
                            uid_t peer_uid, pid_t peer_pid, ControlCmd &cmd,
                            ControlResponse *response, std::string *payload) {
  cmd = ControlCmd::kNone;
  if (response != nullptr) *response = ControlResponse{};
  if (response != nullptr) response->magic = 0;
  if (payload != nullptr) payload->clear();
  if (buf == nullptr || response == nullptr || payload == nullptr) return false;

  ControlOp op = ControlOp::kStatus;
  uint64_t sequence = 0;
  uint16_t version = kControlVersion;
  bool legacy = false;
  const char *auth_reason = AuthRejectReason(peer_uid, peer_pid);
  if (auth_reason != nullptr) {
    auth_rejects_.fetch_add(1, std::memory_order_relaxed);
    AppendAudit(peer_uid, peer_pid, "unknown", 0, 0, false, auth_reason,
                ControlCode::kAuthRejected);
    THYME_LOG_WARN_RL("recorder: rejected control datagram from uid %u",
                      static_cast<unsigned>(peer_uid));
    return false;
  }

  auto reject = [&](const char *reason, ControlCode code, bool reply) {
    AppendAudit(peer_uid, peer_pid, OpName(op), version, sequence, false,
                reason, code);
    if (reply) {
      response->magic = kControlMagic;
      response->version = kControlVersion;
      response->code = static_cast<uint16_t>(code);
      response->sequence = sequence;
      StampResponseCrc(response);
    }
    return false;
  };

  TriggerParams trigger_params{};
  std::string trigger_channels;
  bool has_trigger_params = false;
  if (size >= sizeof(ControlRequest)) {
    ControlRequest request;
    std::memcpy(&request, buf, sizeof(request));
    op = static_cast<ControlOp>(request.op);
    sequence = request.sequence;
    if (request.magic != kControlMagic || request.version != kControlVersion) {
      return reject("malformed_magic_version", ControlCode::kMalformedRequest,
                    false);
    }
    if (request.crc32 != RequestCrc(request)) {
      crc_rejects_.fetch_add(1, std::memory_order_relaxed);
      return reject("bad_crc", ControlCode::kMalformedRequest, false);
    }
    if (request.reserved0 != 0 ||
        request.op < static_cast<uint16_t>(ControlOp::kTrigger) ||
        request.op > static_cast<uint16_t>(ControlOp::kStatusJson)) {
      return reject("malformed_request", ControlCode::kMalformedRequest, false);
    }
    if (request.sequence == 0) {
      replay_rejects_.fetch_add(1, std::memory_order_relaxed);
      return reject("bad_sequence", ControlCode::kReplayRejected, true);
    }
    if (!CheckReplay(peer_uid, peer_pid, request.sequence, MonoNs())) {
      replay_rejects_.fetch_add(1, std::memory_order_relaxed);
      return reject("replay_duplicate", ControlCode::kReplayRejected, true);
    }
    if (size > sizeof(ControlRequest)) {
      if (op != ControlOp::kTrigger) {
        return reject("malformed_request", ControlCode::kMalformedRequest,
                      false);
      }
      const std::size_t tail = size - sizeof(ControlRequest);
      if (tail < sizeof(TriggerParams) ||
          tail > sizeof(TriggerParams) + kMaxTriggerChannels) {
        return reject("malformed_request", ControlCode::kMalformedRequest,
                      false);
      }
      std::memcpy(&trigger_params, buf + sizeof(ControlRequest),
                  sizeof(trigger_params));
      if (trigger_params.magic != kTriggerParamsMagic ||
          trigger_params.version != kTriggerParamsVersion ||
          sizeof(TriggerParams) + trigger_params.channel_bytes != tail ||
          trigger_params.channel_bytes > kMaxTriggerChannels) {
        return reject("malformed_request", ControlCode::kMalformedRequest,
                      false);
      }
      const char *channels = reinterpret_cast<const char *>(
          buf + sizeof(ControlRequest) + sizeof(TriggerParams));
      if (trigger_params.crc32 !=
          TriggerParamsCrc(trigger_params, channels)) {
        crc_rejects_.fetch_add(1, std::memory_order_relaxed);
        return reject("bad_crc", ControlCode::kMalformedRequest, false);
      }
      if (trigger_params.channel_bytes != 0) {
        trigger_channels.assign(channels, trigger_params.channel_bytes);
      }
      has_trigger_params = true;
    }
  } else {
    legacy = true;
    version = kControlVersionLegacy;
    char text[128];
    if (size == 0 || size >= sizeof(text) ||
        std::memchr(buf, '\0', size) != nullptr) {
      return reject("malformed_size", ControlCode::kMalformedRequest, false);
    }
    std::memcpy(text, buf, size);
    text[size] = '\0';
    if (!CommandOp(text, &op)) {
      return reject("malformed_command", ControlCode::kMalformedRequest, false);
    }
    if (!cfg_.allow_legacy_control) {
      return reject("legacy_control_disabled", ControlCode::kUnsupportedVersion,
                    true);
    }
    legacy_requests_.fetch_add(1, std::memory_order_relaxed);
    THYME_LOG_WARN_RL(
        "recorder: accepted legacy control command; replay "
        "protection is disabled");
  }

  cmd = ToCommand(op);
  if (cmd == ControlCmd::kArm) {
    armed_.store(true, std::memory_order_release);
  } else if (cmd == ControlCmd::kDisarm) {
    armed_.store(false, std::memory_order_release);
  }
  if (op == ControlOp::kStatus) *payload = StatusText();
  if (op == ControlOp::kStatusJson) *payload = StatusJson();
  if (has_trigger_params) {
    pending_ = trigger_params;
    pending_channel_bytes_ = trigger_params.channel_bytes;
    if (pending_channel_bytes_ != 0) {
      std::memcpy(pending_channels_, trigger_channels.data(),
                  pending_channel_bytes_);
    }
    pending_trigger_ = true;
#if defined(__linux__)
    if (wake_fd_ >= 0) {
      const uint64_t one = 1;
      if (write(wake_fd_, &one, sizeof(one)) < 0 && errno != EAGAIN) {
        THYME_LOG_WARN_RL("recorder: eventfd write failed: %s",
                          std::strerror(errno));
      }
    }
#endif
  }
  AppendAudit(peer_uid, peer_pid, OpName(op), version, sequence, true, "ok",
              ControlCode::kAccepted);
  response->magic = kControlMagic;
  response->version = kControlVersion;
  response->code = static_cast<uint16_t>(ControlCode::kAccepted);
  response->sequence = sequence;
  response->payload_size = static_cast<uint32_t>(payload->size());
  StampResponseCrc(response, payload->data());
  (void)legacy;
  return true;
}

bool Server::Poll(ControlCmd &cmd) {
#if defined(_WIN32)
  cmd = ControlCmd::kNone;
  return false;
#else
  cmd = ControlCmd::kNone;
  if (fd_ < 0) return false;
  audit_.MaybeSync(MonoNs());

  uint8_t buf[sizeof(ControlRequest) + sizeof(TriggerParams) +
              kMaxTriggerChannels];
  uid_t peer_uid = static_cast<uid_t>(-1);
  pid_t peer_pid = 0;
  sockaddr_un from{};
  socklen_t from_len = sizeof(from);
  const ssize_t size = RecvCommand(fd_, buf, sizeof(buf), &peer_uid, &peer_pid,
                                   &from, &from_len);
  if (size < 0) return false;

  ControlResponse response{};
  std::string payload;
  const bool ok = ProcessRequest(buf, static_cast<std::size_t>(size), peer_uid,
                                 peer_pid, cmd, &response, &payload);
  bool binary = false;
  if (static_cast<std::size_t>(size) >= sizeof(ControlRequest)) {
    ControlRequest peek{};
    std::memcpy(&peek, buf, sizeof(peek));
    binary = peek.magic == kControlMagic && peek.version == kControlVersion;
  }
  const bool legacy = !binary;
  if (response.magic == kControlMagic && from_len > 0 &&
      from.sun_family == AF_UNIX) {
    if (legacy) {
      std::string legacy_reply;
      if (ok) {
        legacy_reply = payload;
      } else if (cmd == ControlCmd::kNone && size > 0) {
        const bool json = static_cast<std::size_t>(size) == 11 &&
                          std::memcmp(buf, "status-json", 11) == 0;
        legacy_reply =
            json ? "{\"schema\":1,\"kind\":\"thyme-recorder-status\",\"ok\":"
                   "false,"
                   "\"error\":\"unsupported_version: record_control v2 "
                   "required\"}"
                 : "error=unsupported_version record_control_v2_required=true";
      }
      if (!legacy_reply.empty()) {
        sendto(fd_, legacy_reply.data(), legacy_reply.size(), 0,
               reinterpret_cast<sockaddr *>(&from), from_len);
      }
    } else {
      uint8_t reply[sizeof(ControlResponse) + kMaxControlPayload + 1] = {};
      std::memcpy(reply, &response, sizeof(response));
      if (!payload.empty()) {
        std::copy(payload.begin(), payload.end(), reply + sizeof(response));
      }
      sendto(fd_, reply, sizeof(response) + payload.size(), 0,
             reinterpret_cast<sockaddr *>(&from), from_len);
    }
  }
  return ok;
#endif
}

const char *ControlCodeName(ControlCode code) {
  switch (code) {
    case ControlCode::kAccepted:
      return "accepted";
    case ControlCode::kUnsupportedVersion:
      return "unsupported_version";
    case ControlCode::kReplayRejected:
      return "replay_rejected";
    case ControlCode::kAuthRejected:
      return "auth_rejected";
    case ControlCode::kMalformedRequest:
      return "malformed_request";
  }
  return "unknown";
}

uint64_t NextSequence() {
  static std::atomic<uint64_t> sequence{MonoNs()};
  const uint64_t next = sequence.fetch_add(1, std::memory_order_relaxed);
  return next == 0 ? sequence.fetch_add(1, std::memory_order_relaxed) : next;
}

bool SendClientCommand(const char *socket_path, const char *command) {
#if defined(_WIN32)
  (void)socket_path;
  (void)command;
  return false;
#else
  ControlOp op{};
  if (socket_path == nullptr || socket_path[0] == '\0' || command == nullptr ||
      !CommandOp(command, &op)) {
    return false;
  }
  ControlRequest request;
  request.op = static_cast<uint16_t>(op);
  request.sequence = NextSequence();
  StampRequestCrc(&request);
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
  const ssize_t sent =
      sendto(fd, &request, sizeof(request), 0,
             reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  close(fd);
  return sent == static_cast<ssize_t>(sizeof(request));
#endif
}

Result ExecuteCommand(const char *socket_path, const char *command,
                      uint32_t timeout_ms) {
  Result result;
#if defined(_WIN32)
  (void)socket_path;
  (void)command;
  (void)timeout_ms;
  result.error = "control sockets are not supported";
  return result;
#else
  ControlOp op{};
  if (socket_path == nullptr || socket_path[0] == '\0' || command == nullptr ||
      timeout_ms == 0 || !CommandOp(command, &op)) {
    result.error = "invalid socket path, command, or timeout";
    return result;
  }
  static std::atomic<uint32_t> reply_sequence{0};
  char self_path[108];
  std::snprintf(self_path, sizeof(self_path), "/tmp/thyme_control_%ld_%u.sock",
                static_cast<long>(getpid()),
                reply_sequence.fetch_add(1, std::memory_order_relaxed));
  unlink(self_path);
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) {
    result.error = "cannot create reply socket";
    return result;
  }
  sockaddr_un self{};
  self.sun_family = AF_UNIX;
  std::snprintf(self.sun_path, sizeof(self.sun_path), "%s", self_path);
  if (bind(fd, reinterpret_cast<sockaddr *>(&self), sizeof(self)) != 0 ||
      chmod(self_path, S_IRUSR | S_IWUSR) != 0) {
    close(fd);
    unlink(self_path);
    result.error = "cannot bind private reply socket";
    return result;
  }

  ControlRequest request;
  request.op = static_cast<uint16_t>(op);
  request.sequence = NextSequence();
  StampRequestCrc(&request);
  sockaddr_un target{};
  target.sun_family = AF_UNIX;
  std::snprintf(target.sun_path, sizeof(target.sun_path), "%s", socket_path);
  const ssize_t sent =
      sendto(fd, &request, sizeof(request), 0,
             reinterpret_cast<sockaddr *>(&target), sizeof(target));
  if (sent != static_cast<ssize_t>(sizeof(request))) {
    close(fd);
    unlink(self_path);
    result.error =
        std::string("control socket ") + socket_path + " unreachable";
    return result;
  }

  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN;
  int ready = 0;
  do {
    ready = poll(&pfd, 1, static_cast<int>(timeout_ms));
  } while (ready < 0 && errno == EINTR);
  if (ready <= 0) {
    close(fd);
    unlink(self_path);
    result.error = "timed out after " + std::to_string(timeout_ms) +
                   " ms waiting for " + socket_path;
    return result;
  }

  uint8_t wire[sizeof(ControlResponse) + kMaxControlPayload];
  const ssize_t size = recv(fd, wire, sizeof(wire), 0);
  close(fd);
  unlink(self_path);
  if (size < static_cast<ssize_t>(sizeof(ControlResponse))) {
    result.error = "short or empty response from recorder";
    return result;
  }
  ControlResponse response;
  std::memcpy(&response, wire, sizeof(response));
  const std::size_t expected = sizeof(response) + response.payload_size;
  if (response.magic != kControlMagic || response.version != kControlVersion ||
      response.sequence != request.sequence ||
      response.payload_size > kMaxControlPayload ||
      expected != static_cast<std::size_t>(size)) {
    result.error = "malformed response from recorder";
    return result;
  }
  const char *body = reinterpret_cast<const char *>(wire + sizeof(response));
  if (response.crc32 != ResponseCrc(response, body)) {
    result.error = "bad response CRC from recorder";
    return result;
  }
  result.transport_ok = true;
  result.code = static_cast<ControlCode>(response.code);
  result.sequence = response.sequence;
  result.payload.assign(body, response.payload_size);
  if (result.code == ControlCode::kReplayRejected) {
    result.error =
        "replay_rejected: duplicate or old control sequence; retry "
        "with a new sequence";
  } else if (result.code == ControlCode::kUnsupportedVersion) {
    result.error = "unsupported_version: server requires record_control v2";
  } else if (result.code != ControlCode::kAccepted) {
    result.error = ControlCodeName(result.code);
  }
  return result;
#endif
}

Result SendTrigger(const char *socket_path, TriggerParams params,
                   const char *channels, uint32_t timeout_ms) {
  Result result;
#if defined(_WIN32)
  (void)socket_path;
  (void)params;
  (void)channels;
  (void)timeout_ms;
  result.error = "control sockets are not supported";
  return result;
#else
  if (socket_path == nullptr || socket_path[0] == '\0' || timeout_ms == 0 ||
      params.channel_bytes > kMaxTriggerChannels ||
      (params.channel_bytes != 0 && channels == nullptr)) {
    result.error = "invalid trigger datagram";
    return result;
  }
  params.magic = kTriggerParamsMagic;
  params.version = kTriggerParamsVersion;
  StampTriggerParams(&params, channels);
  uint8_t datagram[sizeof(ControlRequest) + sizeof(TriggerParams) +
                   kMaxTriggerChannels];
  ControlRequest request;
  request.op = static_cast<uint16_t>(ControlOp::kTrigger);
  request.sequence = NextSequence();
  StampRequestCrc(&request);
  std::memcpy(datagram, &request, sizeof(request));
  std::memcpy(datagram + sizeof(request), &params, sizeof(params));
  if (params.channel_bytes != 0) {
    std::memcpy(datagram + sizeof(request) + sizeof(params), channels,
                params.channel_bytes);
  }
  const std::size_t datagram_bytes =
      sizeof(request) + sizeof(params) + params.channel_bytes;

  static std::atomic<uint32_t> reply_sequence{0};
  char self_path[108];
  std::snprintf(self_path, sizeof(self_path), "/tmp/thyme_control_%ld_%u.sock",
                static_cast<long>(getpid()),
                reply_sequence.fetch_add(1, std::memory_order_relaxed));
  unlink(self_path);
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) {
    result.error = "cannot create reply socket";
    return result;
  }
  sockaddr_un self{};
  self.sun_family = AF_UNIX;
  std::snprintf(self.sun_path, sizeof(self.sun_path), "%s", self_path);
  if (bind(fd, reinterpret_cast<sockaddr *>(&self), sizeof(self)) != 0 ||
      chmod(self_path, S_IRUSR | S_IWUSR) != 0) {
    close(fd);
    unlink(self_path);
    result.error = "cannot bind private reply socket";
    return result;
  }
  sockaddr_un target{};
  target.sun_family = AF_UNIX;
  std::snprintf(target.sun_path, sizeof(target.sun_path), "%s", socket_path);
  result.sent_mono_ns = MonoNs();
  const ssize_t sent =
      sendto(fd, datagram, datagram_bytes, 0,
             reinterpret_cast<sockaddr *>(&target), sizeof(target));
  if (sent != static_cast<ssize_t>(datagram_bytes)) {
    close(fd);
    unlink(self_path);
    result.error =
        std::string("control socket ") + socket_path + " unreachable";
    return result;
  }
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN;
  int ready = 0;
  do {
    ready = poll(&pfd, 1, static_cast<int>(timeout_ms));
  } while (ready < 0 && errno == EINTR);
  if (ready <= 0) {
    close(fd);
    unlink(self_path);
    result.error = "timed out after " + std::to_string(timeout_ms) +
                   " ms waiting for " + socket_path;
    return result;
  }
  uint8_t wire[sizeof(ControlResponse) + kMaxControlPayload];
  const ssize_t size = recv(fd, wire, sizeof(wire), 0);
  close(fd);
  unlink(self_path);
  if (size < static_cast<ssize_t>(sizeof(ControlResponse))) {
    result.error = "short or empty response from recorder";
    return result;
  }
  ControlResponse response;
  std::memcpy(&response, wire, sizeof(response));
  const std::size_t expected = sizeof(response) + response.payload_size;
  if (response.magic != kControlMagic || response.version != kControlVersion ||
      response.sequence != request.sequence ||
      response.payload_size > kMaxControlPayload ||
      expected != static_cast<std::size_t>(size)) {
    result.error = "malformed response from recorder";
    return result;
  }
  const char *body = reinterpret_cast<const char *>(wire + sizeof(response));
  if (response.crc32 != ResponseCrc(response, body)) {
    result.error = "bad response CRC from recorder";
    return result;
  }
  result.transport_ok = true;
  result.code = static_cast<ControlCode>(response.code);
  result.sequence = response.sequence;
  result.payload.assign(body, response.payload_size);
  if (result.code != ControlCode::kAccepted) {
    result.error = ControlCodeName(result.code);
  }
  return result;
#endif
}

bool QueryRecorderStatus(const char *socket_path, bool json,
                         uint32_t timeout_ms, std::string *reply,
                         std::string *error) {
  if (reply != nullptr) reply->clear();
  if (error != nullptr) error->clear();
  Result result =
      ExecuteCommand(socket_path, json ? "status-json" : "status", timeout_ms);
  if (!result.transport_ok || result.code != ControlCode::kAccepted) {
    if (error != nullptr) *error = result.error;
    return false;
  }
  if (reply != nullptr) *reply = result.payload;
  return true;
}

}  // namespace record_control
}  // namespace thyme
