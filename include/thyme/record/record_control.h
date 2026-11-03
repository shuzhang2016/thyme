#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "thyme/record/package_integrity.h"
#include "thyme/record/record_control_protocol.h"
#include "thyme/record/trigger_audit.h"

#if !defined(_WIN32)
#include <sys/types.h>
#else
using uid_t = unsigned int;
#endif

namespace thyme {
namespace record_control {

enum class ControlCmd {
  kNone,
  kTrigger,
  kArm,
  kDisarm,
  kStatus,
};

enum class RecorderPhase {
  kArmed,
  kPost,
  kSettle,
};

// Optional recorder counters carried by `status`/`status-json` replies.
// trigger_center refreshes this each loop; a bare Server keeps the zeros.
struct StatusSnapshot {
  uint64_t captures_done = 0;
  uint64_t slices_done = 0;
  uint64_t pending_bytes = 0;
  bool disk_full = false;
  uint64_t triggers_rejected = 0;
  uint64_t descriptors_queued = 0;
  uint64_t pending_evicted = 0;
  uint64_t overrun_before_queue = 0;
  uint64_t samples_written = 0;
  uint64_t frames_written = 0;
  uint64_t holes_written = 0;
  uint64_t torn_reads = 0;
  uint64_t harvest_retries = 0;
  uint64_t session_reattaches = 0;
  uint64_t source_errors = 0;
  uint64_t disk_errors = 0;
  uint64_t sync_errors = 0;
  uint64_t audit_drops = 0;
  uint64_t fsm_event_drops = 0;
  uint64_t service_records_written = 0;
  uint64_t service_records_suppressed = 0;
  uint64_t service_records_dropped = 0;
  uint64_t service_records_truncated = 0;
  uint64_t service_rotations = 0;
  bool service_sink_open = false;
  integrity::ManifestMode integrity_mode = integrity::ManifestMode::kFull;
};

struct Config {
  std::vector<uint32_t> allowed_uids;
  bool allow_root = false;
  bool require_peer_pid = true;
  bool allow_legacy_control = false;
  std::string audit_log_path;
  uint64_t audit_max_bytes = 4ull * 1024 * 1024;
  uint32_t audit_rotate_keep = 2;
  uint64_t audit_fsync_interval_ns = 0;
};

struct ReplayEntry {
  bool used = false;
  uid_t uid = 0;
  pid_t pid = 0;
  uint64_t max_seq = 0;
  uint64_t bitmap = 0;
  uint64_t last_ns = 0;
};

// Unix datagram control socket server (default /tmp/thyme_recorder.sock).
// V2 requests are authenticated, audited and replay-checked. The legacy text
// protocol is rejected unless allow_legacy_control is explicitly enabled.
class Server {
 public:
  Server() = default;
  ~Server();
  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;

  bool Open(const char *path, const Config &cfg = Config{});
  bool ReloadConfig(const Config &cfg);
  void Close();
  bool Poll(ControlCmd &cmd);

  void SetPhase(RecorderPhase phase);
  uint64_t NextTriggerId();
  bool IsArmed() const;
  RecorderPhase CurrentPhase() const;
  void PrintStatus() const;
  uint64_t AuthRejectCount() const;
  uint64_t ReplayRejectCount() const;
  uint64_t CrcRejectCount() const;
  uint64_t LegacyRequestCount() const;
  uint64_t ControlAuditDropCount() const;

  void SetStatusSnapshot(const StatusSnapshot &snapshot);
  std::string StatusText() const;
  std::string StatusJson() const;

  // Set by Poll when a kTrigger datagram carries TriggerParams. The caller
  // that Polls is the one that Takes. Frame payloads are not stored here.
  bool TakeTrigger(TriggerParams *params, std::string *channels);
  int WakeFd() const;

  // Legacy text entry point, retained for source compatibility. It is subject
  // to the same default-deny V1 policy as the socket path.
  bool ProcessCommand(const char *buf, uid_t peer_uid, ControlCmd &cmd);
  bool ProcessRequest(const uint8_t *buf, std::size_t size, uid_t peer_uid,
                      pid_t peer_pid, ControlCmd &cmd,
                      ControlResponse *response, std::string *payload);

 private:
  const char *AuthRejectReason(uid_t peer_uid, pid_t peer_pid) const;
  bool CheckReplay(uid_t peer_uid, pid_t peer_pid, uint64_t sequence,
                   uint64_t now_ns);
  bool AppendAudit(uid_t peer_uid, pid_t peer_pid, const char *op,
                   uint16_t version, uint64_t sequence, bool accepted,
                   const char *reason, ControlCode code);

  int fd_ = -1;
  int wake_fd_ = -1;
  bool pending_trigger_ = false;
  TriggerParams pending_{};
  char pending_channels_[kMaxTriggerChannels] = {};
  uint32_t pending_channel_bytes_ = 0;
  char path_[108] = {};
  Config cfg_;
  trigger_audit::Log audit_;
  std::array<ReplayEntry, 64> replay_{};
  std::atomic<uint64_t> replay_rejects_{0};
  std::atomic<uint64_t> crc_rejects_{0};
  std::atomic<uint64_t> legacy_requests_{0};
  std::atomic<uint64_t> control_audit_drops_{0};
  std::atomic<bool> armed_{true};
  std::atomic<uint64_t> next_id_{1};
  std::atomic<int> phase_{static_cast<int>(RecorderPhase::kArmed)};
  std::atomic<uint64_t> auth_rejects_{0};
  std::atomic<uint64_t> captures_done_{0};
  std::atomic<uint64_t> slices_done_{0};
  std::atomic<uint64_t> pending_bytes_{0};
  std::atomic<bool> disk_full_{false};
  std::atomic<uint64_t> triggers_rejected_{0};
  std::atomic<uint64_t> descriptors_queued_{0};
  std::atomic<uint64_t> pending_evicted_{0};
  std::atomic<uint64_t> overrun_before_queue_{0};
  std::atomic<uint64_t> samples_written_{0};
  std::atomic<uint64_t> frames_written_{0};
  std::atomic<uint64_t> holes_written_{0};
  std::atomic<uint64_t> torn_reads_{0};
  std::atomic<uint64_t> harvest_retries_{0};
  std::atomic<uint64_t> session_reattaches_{0};
  std::atomic<uint64_t> source_errors_{0};
  std::atomic<uint64_t> disk_errors_{0};
  std::atomic<uint64_t> sync_errors_{0};
  std::atomic<uint64_t> audit_drops_{0};
  std::atomic<uint64_t> fsm_event_drops_{0};
  std::atomic<uint64_t> service_records_written_{0};
  std::atomic<uint64_t> service_records_suppressed_{0};
  std::atomic<uint64_t> service_records_dropped_{0};
  std::atomic<uint64_t> service_records_truncated_{0};
  std::atomic<uint64_t> service_rotations_{0};
  std::atomic<bool> service_sink_open_{false};
  std::atomic<uint8_t> integrity_mode_{0};
};

// Fire-and-forget V2 send retained for recorderctl/trigger_relay callers.
bool SendClientCommand(const char *socket_path, const char *command);

struct Result {
  bool transport_ok = false;
  ControlCode code = ControlCode::kMalformedRequest;
  uint64_t sequence = 0;
  uint64_t sent_mono_ns = 0;
  std::string payload;
  std::string error;
};

// Sends arm/disarm/trigger/status/status-json and waits for the V2 response.
Result ExecuteCommand(const char *socket_path, const char *command,
                      uint32_t timeout_ms);

// V2 kTrigger plus a TriggerParams tail. `channels` is the raw selector text
// (channel_bytes long); it is not a frame payload.
Result SendTrigger(const char *socket_path, TriggerParams params,
                   const char *channels, uint32_t timeout_ms);
const char *ControlCodeName(ControlCode code);
uint64_t NextSequence();

// Compatibility wrapper: preserves the status/status-json API and reply body.
bool QueryRecorderStatus(const char *socket_path, bool json,
                         uint32_t timeout_ms, std::string *reply,
                         std::string *error);

}  // namespace record_control
}  // namespace thyme
