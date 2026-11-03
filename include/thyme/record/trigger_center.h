#pragma once

// trigger_center.h — trigger center library (design doc 5.1/5.3/5.4).
// Thread-free and instantiable: the host main loop calls Tick(), the socket
// datagrams and the recorder are both driven through it. The recorder itself
// is injected via Hooks (the host wires them to a TriggeredRecorder), so the
// FSM is testable without any shm.
//
// State machine (design doc 5.3; max_concurrent_captures = 1 this phase):
//   IDLE      --accept--> CAPTURING --(capture done)--> MUTED(mute_ns>0)
//   MUTED     --mute expires--> IDLE ;  CAPTURING done & mute_ns==0 -> IDLE
//   CAPTURING: priority >  current -> preempt (truncate+seal old, new capture
//              starts immediately, flush forced realtime); with force (manual
//              confirmation) priority >= current is enough to preempt;
//              anything else -> kMuted
//   MUTED:     crash level (priority >= crash_priority) -> accept; else
//              -> kMuted
// Quotas (5.4): per-quota_class sliding 1h window count (0 = unlimited; crash
// level may override and counts quota_overridden); when recorder_pending_quota
// is exceeded only low priority is rejected, while a crash-level trigger first
// calls hooks.evict_oldest and then proceeds.
//
// TRG-07 cancel + outcome notification:
//   Cancel(trigger_id) drops a pending (accepted, not yet started) trigger —
//   rolling the FSM back to the pre-Submit snapshot — or cancels the running
//   capture through the same preempt_capture hook a preempt uses
//   (truncate+seal what is already written, then apply the source's mute
//   window like a completed capture). Cancelling an unknown/completed id is
//   kUnknownTrigger, never silent success.
//   Every capture end (sealed / preempted / start_failed / preempt_failed /
//   cancelled) is pushed into a fixed 64-entry outcome ring; a kFlagWaitOutcome
//   request either matches the ring immediately or parks a deferred waiter
//   (fixed 8-entry table, deadline = response_timeout_ms) that Tick resolves
//   without any allocation. Ring/table overflow and waiter expiry are counted.
//
// Structured FSM event log: every FSM transition (submit/accept/start/
// preempt/rollback/complete/cancel) appends one
// {mono_ns, trigger_id, from, to, reason} record to a fixed 256-entry ring; on
// overflow the oldest is dropped and stats.fsm_event_drops is counted. Every
// fsm_dump_interval_ns_ (default 1s) Tick appends the undumped events to an
// O_APPEND JSONL file (<socket_path>.fsm, in the same directory as the audit
// file); each record is one unbuffered write() and the file follows the
// [logging] rotation policy, so a crash loses at most one dump period of
// events; the reader tolerates a truncated last line. The ring and the dump
// path perform no allocation after Init.

// Storm observability: every received datagram counts socket_datagrams;
// PollSocket stops at EAGAIN, the default 200us time budget or the fixed safety
// cap, whichever comes first. socket_backlog_events is counted only when the
// budget/cap ran out AND MSG_PEEK confirms data is still pending; consuming
// exactly the available datagrams is not backlog.

#include "thyme/record/quota_state.h"
#include "thyme/record/trigger_audit.h"
#include "thyme/record/trigger_config.h"
#include "thyme/record/trigger_protocol.h"
#include "thyme/runtime/rotating_file.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/types.h>
#else
using uid_t = unsigned int;
using pid_t = int;
#endif

namespace thyme {
namespace trigger_center {

enum class State : uint8_t { kIdle = 0, kCapturing = 1, kMuted = 2 };

// Reason codes for structured FSM transition events (FsmEvent::reason).
enum class FsmReason : uint8_t {
  kSubmitAccept = 0,   // IDLE/MUTED -> CAPTURING: trigger accepted and queued
  kStarted = 1,        // CAPTURING -> CAPTURING: queued trigger started
  kStartFailed = 2,    // rollback: recorder offline / start_capture failed
  kPreempted = 3,      // CAPTURING -> CAPTURING: old capture truncated+sealed
  kPreemptFailed = 4,  // rollback: preemption failed, original capture restored
  kCompleted = 5,      // CAPTURING -> MUTED/IDLE: capture completed and sealed
  kMuteExpired = 6,    // MUTED -> IDLE: mute window expired
  kCancelPending = 7,  // accepted, not yet started trigger cancelled; FSM rollback
  kCancelRunning = 8,  // running capture cancelled (truncate+seal + mute)
};

// One FSM transition record. from/to are State values, reason is a FsmReason
// value.
struct FsmEvent {
  uint64_t mono_ns = 0;    // CLOCK_MONOTONIC
  uint64_t trigger_id = 0;
  uint8_t from = 0;
  uint8_t to = 0;
  uint8_t reason = 0;
  uint8_t rsvd = 0;
};

struct Hooks {
  void* ctx = nullptr;
  // false -> kRecorderOffline.
  bool (*recorder_ready)(void* ctx) = nullptr;
  // Center polls this to detect capture completion (CAPTURING -> MUTED/IDLE).
  bool (*capture_active)(void* ctx) = nullptr;
  // Start a capture; force_realtime_flush overrides the source's idle policy
  // (crash level/preemption); high_priority sends the package to the reserved
  // high/ zone (crash level, 6.4). channels is the rule's
  // [source.*].channels filter (empty = all). Returns false if the recorder
  // refused (offline).
  bool (*start_capture)(void* ctx, uint64_t trigger_id, uint64_t t0_ns,
                        uint64_t before_ns, uint64_t after_ns,
                        bool force_realtime_flush, bool high_priority,
                        const std::vector<std::string>& channels) = nullptr;
  // Truncate + seal the in-flight capture. Returns false when none active.
  bool (*preempt_capture)(void* ctx) = nullptr;
  // Live pending-queue bytes (tied to the memory quota). nullptr -> not
  // counted against the quota.
  uint64_t (*pending_bytes)(void* ctx) = nullptr;
  // Evict oldest pending data (crash-level memory override). Returns freed bytes.
  uint64_t (*evict_oldest)(void* ctx, uint64_t bytes) = nullptr;
  // 6.5: disk full after a failed drain. Non-crash triggers are rejected with
  // kQuotaExceeded while this holds; crash level still tries (high/ reserved
  // zone).
  // nullptr -> never full.
  bool (*disk_full)(void* ctx) = nullptr;
};

struct SubmitResult {
  trigger_proto::TriggerCode code = trigger_proto::TriggerCode::kUnknownSource;
  uint64_t trigger_id = 0;
  int32_t priority_used = 0;
  // True when the requested hint was not used verbatim: either the
  // [security] max_priority_hint clamp or the "hint may only lower the rule"
  // rule adjusted it. Surfaced in the V2 response and the audit record.
  bool priority_clamped = false;
  // TRG-12: this accept only passed because crash-level priority overrode
  // the per-class count quota (mirrors CenterStats::quota_overridden for this
  // request). Surfaced in the audit record as quota_override=1.
  bool quota_overridden = false;
  std::string source_used;  // after default_source fallback
};

// Kernel-supplied peer identity. On Linux the datagram path gets this from
// SCM_CREDENTIALS (SO_PASSCRED is enabled on the socket) — the datagram
// equivalent of SO_PEERCRED, and equally unspoofable: the kernel fills it and
// overwrites anything the sender supplied. Direct in-process callers pass the
// values explicitly (tests pass geteuid()/getpid()).
struct PeerCred {
  uid_t uid = static_cast<uid_t>(-1);
  pid_t pid = 0;
};

struct CenterStats {
  uint64_t accepted = 0;
  uint64_t rejected_muted = 0;
  uint64_t rejected_quota = 0;
  uint64_t rejected_unknown = 0;
  uint64_t rejected_offline = 0;
  uint64_t rejected_busy = 0;      // previous trigger not started / preempting
  uint64_t rejected_bad_timestamp = 0;  // timestamp_ns out of bounds
  uint64_t preemptions = 0;
  uint64_t quota_overridden = 0;  // crash-level count-quota overrides (alerting)
  uint64_t quota_state_errors = 0;  // T5: durable-state admission failures
  uint64_t memory_evictions = 0;  // crash-level memory evictions
  uint64_t auth_rejects = 0;
  uint64_t malformed_datagrams = 0;
  uint64_t crc_rejects = 0;       // V3 datagrams with a bad CRC (SEC-04)
  uint64_t replay_rejects = 0;    // duplicate/old sequence (SEC-02)
  uint64_t priority_clamps = 0;   // hint adjusted by [security] clamp/rules
  uint64_t legacy_requests = 0;   // V1 datagrams accepted (unsequenced)
  uint64_t legacy_v2_requests = 0;  // V2 datagrams accepted (no CRC)
  uint64_t audit_drops = 0;       // audit records that could not be written
  uint64_t start_failures = 0;    // Accepted answered but start_capture failed
  uint64_t preempt_failures = 0;  // preemption failed (FSM already rolled back)
  // TRG-07.
  uint64_t cancels_applied = 0;   // pending dropped / running capture cancelled
  uint64_t cancels_unknown = 0;   // cancel of an unknown/completed trigger id
  uint64_t cancel_failures = 0;   // running-cancel preempt hook failed (retry)
  uint64_t notify_outcomes = 0;   // outcomes pushed into the ring
  uint64_t notify_outcome_drops = 0;   // ring full: oldest outcome overwritten
  uint64_t notify_delivered = 0;       // outcome notifications sent to waiters
  uint64_t notify_send_failures = 0;   // outcome notification sendto failed
  uint64_t notify_waiter_overflows = 0;  // wait request over the 8-slot table
  uint64_t notify_waiter_expired = 0;    // waiters dropped at their deadline
  // ARCH-05: direct replies that the kernel refused (EAGAIN/ENOBUFS/...).
  // The request was already processed; the client will time out.
  uint64_t reply_send_failures = 0;
  // OBS-05.
  uint64_t fsm_events = 0;        // total FSM events (including overwritten ones)
  uint64_t fsm_event_drops = 0;   // times a full ring overwrote the oldest event
  // Rotation/byte counters for <socket>.fsm (per process; they work with
  // LogBudget's process budget, see [logging] in
  // docs/guides/trigger-config.md).
  uint64_t fsm_rotations = 0;
  uint64_t fsm_bytes = 0;
  uint64_t fsm_write_drops = 0;   // write failures / budget drops (watermark kept, retried)
  // TRG-09/TRG-19.
  uint64_t socket_datagrams = 0;
  uint64_t socket_backlog_events = 0; // budget/safety cap exhausted, peek still has data
  uint64_t dedup_hits = 0;      // V4 retry hit the first result; not executed twice
  uint64_t dedup_conflicts = 0; // same peer/request_id but a different datagram
  uint64_t retry_requests = 0;  // V4 requests audited with retry=1
};

class Center {
 public:
  Center();
  ~Center();
  Center(const Center&) = delete;
  Center& operator=(const Center&) = delete;

  bool Init(const trigger_cfg::CenterConfig& cfg, const Hooks& hooks);
  void Shutdown();

  // Socket poll + capture completion + mute expiry + quota window GC.
  void Tick();

  // Direct submit path (used by the socket handler and by tests).
  SubmitResult Submit(const std::string& source, int32_t priority_hint,
                      uint64_t timestamp_ns, bool has_timestamp, bool force);

  // TRG-07: cancel the pending (accepted, not yet started) or the running
  // capture. Returns kCancelled when applied, kUnknownTrigger when the id is
  // unknown or already completed (explicit, never silent success), kBusy when
  // the running-cancel rollback hook failed and the capture keeps running
  // (retry). Direct path used by the socket handler and by tests.
  trigger_proto::TriggerCode Cancel(uint64_t trigger_id);

  // SIGHUP path: sources/quotas/default_source/crash_priority are replaced;
  // socket path, FSM state, counters, trigger ids are kept. The recorder
  // pending quota is startup-only because it sizes the recorder rings.
  void ReloadConfig(const trigger_cfg::CenterConfig& cfg);

  State state() const { return state_; }
  const CenterStats& stats() const { return stats_; }
  uint64_t current_trigger_id() const { return current_trigger_id_; }
  const std::string& socket_path() const { return cfg_.socket_path; }
  int SocketReceiveBufferBytes() const;
  static constexpr uint32_t kSocketSafetyCap = 4096;

  // V4 handler; PollSocket serializes V1/V2/V3 to their exact legacy layouts.
  bool ProcessRequest(const uint8_t* buf, std::size_t n, const PeerCred& peer,
                      trigger_proto::TriggerResponseV4* resp,
                      const char* reply_path = nullptr);
  // Source-compatible V3 adapter for legacy tests/callers.
  bool ProcessRequest(const uint8_t* buf, std::size_t n, const PeerCred& peer,
                      trigger_proto::TriggerResponse* resp,
                      const char* reply_path = nullptr);

  // Test introspection: sliding-window usage of a quota class.
  uint32_t QuotaUsedInWindow(const std::string& quota_class) const;

  // Copy the retained FSM events to out in time order (at most cap entries);
  // returns the count.
  uint32_t CopyFsmEvents(FsmEvent* out, uint32_t cap) const;
  // Append the undumped FSM events to <socket_path>.fsm (O_APPEND JSONL, one
  // write() per record, [logging] rotation; a crash loses at most the events
  // after the most recent dump). Returns whether anything was written. Events
  // overwritten in the ring before being dumped are silently lost (counted in
  // fsm_event_drops) — the same convention as PerfSampler's FlushNew
  // watermark.
  bool DumpNewFsmEvents();
  // Tick's periodic dump interval (default 1s); tests may shrink it.
  void set_fsm_dump_interval_ns(uint64_t ns) { fsm_dump_interval_ns_ = ns; }
  static constexpr uint32_t kFsmEventRingSize = 256;

 private:
  // SEC-02 anti-replay entry: sliding window of 64 sequence values per
  // (uid, pid). Bounded array; the least-recently-seen peer is evicted.
  struct ReplayEntry {
    bool used = false;
    uid_t uid = 0;
    pid_t pid = 0;
    uint64_t max_seq = 0;  // bit 0 of bitmap
    uint64_t bitmap = 0;   // bit i = max_seq - i already seen
    uint64_t last_ns = 0;
  };
  static constexpr std::size_t kReplayEntries = 64;
  static constexpr uint64_t kReplayWindow = 64;

  // TRG-07 outcome ring and deferred waiters. Both are fixed-size and
  // zero-allocation after Init; overflow/expiry are counted, never grown.
  struct OutcomeEntry {
    uint64_t trigger_id = 0;
    uint16_t code = 0;  // TriggerCode outcome value
    uint64_t mono_ns = 0;
  };
  static constexpr uint32_t kOutcomeRingSize = 64;
  struct WaiterEntry {
    bool used = false;
    uid_t uid = 0;
    pid_t pid = 0;
    uint64_t filter_id = 0;
    uint64_t deadline_ns = 0;
    uint64_t request_id = 0;
    uint16_t proto_version = 0;
    char reply_path[108] = {};
  };
  static constexpr uint32_t kMaxOutcomeWaiters = 8;

  struct DedupEntry {
    bool used = false;
    uid_t uid = 0;
    pid_t pid = 0;
    uint64_t request_id = 0;
    uint32_t request_crc = 0;
    uint64_t expires_ns = 0;
    uint64_t last_ns = 0;
    trigger_proto::TriggerResponseV4 response;
  };

  void PushOutcome(uint64_t trigger_id, trigger_proto::TriggerCode code,
                   uint64_t now);
  bool MatchOutcomeRing(uint64_t filter_id, OutcomeEntry* out) const;
  void ExpireWaiters(uint64_t now);
  bool FindDedup(const PeerCred& peer, uint64_t request_id,
                 uint32_t request_crc, uint64_t now,
                 trigger_proto::TriggerResponseV4* response, bool* conflict);
  void RememberDedup(const PeerCred& peer, uint64_t request_id,
                     uint32_t request_crc, uint64_t now,
                     const trigger_proto::TriggerResponseV4& response);

  // Append one FSM transition event (zero allocation; on a full ring the
  // oldest event is overwritten).
  void NoteFsmEvent(uint64_t trigger_id, State from, State to,
                    FsmReason reason, uint64_t now);
  void DumpFsmEventsIfDue(uint64_t now);

  const char* AuthRejectReason(const PeerCred& peer) const;
  bool CheckReplay(const PeerCred& peer, uint64_t sequence, uint64_t now);
  std::string EffectiveAuditPath() const;

  const trigger_cfg::SourceRule* FindSource(const std::string& name) const;
  uint32_t QuotaLimit(const std::string& quota_class) const;
  bool NoteQuotaUse(const std::string& quota_class, uint64_t now);
  void GcQuotaWindows(uint64_t now);
  static uint64_t MonoNs();
  static bool RealtimeNs(uint64_t* out);
  void PollSocket();
  void CompleteCaptureIfDone(uint64_t now);
  // Asynchronous admission: an Accepted trigger actually starts (including
  // preemption) in Tick.
  void StartPendingTrigger();

  trigger_cfg::CenterConfig cfg_;
  Hooks hooks_;
  trigger_audit::Log audit_;
  std::array<ReplayEntry, kReplayEntries>
      replay_; // zero-initialised (used=false)
  std::vector<DedupEntry> dedup_;
  State state_ = State::kIdle;
  CenterStats stats_;
  uint64_t next_trigger_id_ = 1;
  uint64_t current_trigger_id_ = 0;
  int32_t current_priority_ = 0;
  uint64_t current_mute_ns_ = 0;
  uint64_t mute_until_ns_ = 0;
  std::map<std::string, std::deque<uint64_t>> quota_window_;
  QuotaStateStore quota_state_;
  int fd_ = -1;
  std::string bound_path_;
  // Pending (accepted, not yet started) trigger — the queue depth is 1 while
  // max_concurrent_captures is 1; a second admission gets kBusy.
  bool pending_trigger_ = false;
  bool pending_preempt_ = false;
  bool pending_force_rt_ = false;
  bool pending_high_priority_ = false;
  uint64_t pending_before_ns_ = 0;
  uint64_t pending_after_ns_ = 0;
  uint64_t pending_t0_ = 0;
  std::vector<std::string> pending_channels_;  // TRG-18 rule filter
  // TRG-12: attribution of the accepted pending trigger, stashed so the
  // preempt-execution audit record (emitted from StartPendingTrigger, after
  // ProcessRequest returned) can name the preempting source and peer. Peer
  // credentials are 0/0 for the direct in-process Submit path.
  std::string pending_source_;
  pid_t pending_peer_pid_ = 0;
  uid_t pending_peer_uid_ = 0;
  uint16_t pending_proto_ = 0;
  // Pre-Submit FSM snapshot for rollback when the deferred start/preempt
  // fails (T6).
  State prev_state_ = State::kIdle;
  uint64_t prev_trigger_id_ = 0;
  int32_t prev_priority_ = 0;
  uint64_t prev_mute_ns_ = 0;
  uint64_t prev_mute_until_ns_ = 0;
  // TRG-07.
  std::array<OutcomeEntry, kOutcomeRingSize> outcomes_{};
  uint32_t outcomes_head_ = 0;   // next write position
  uint32_t outcomes_count_ = 0;  // valid entries (<= kOutcomeRingSize)
  std::array<WaiterEntry, kMaxOutcomeWaiters> waiters_{};
  // FSM event ring and periodic dump. fsm_idx_ is the absolute write sequence,
  // fsm_dumped_ the dump watermark; the file is opened best-effort as
  // <socket_path>.fsm in Init (a failure only warns, no fail-closed — this is
  // the observability surface, not the audit surface). Rotation follows
  // [logging] fsm_max_bytes/fsm_rotate_keep (single write(), unbuffered).
  std::array<FsmEvent, kFsmEventRingSize> fsm_events_{};
  uint64_t fsm_idx_ = 0;
  uint64_t fsm_dumped_ = 0;
  logging::RotatingFile fsm_file_;
  uint64_t fsm_dump_interval_ns_ = 1000000000ull;
  uint64_t fsm_last_dump_ns_ = 0;
};

}  // namespace trigger_center
}  // namespace thyme
