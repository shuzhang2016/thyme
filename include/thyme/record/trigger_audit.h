#pragma once

// trigger_audit.h — append-only audit log for the trigger control socket
// (SEC-02).
//
// One newline-terminated record per request, in the order the center handled
// it. Format (line-oriented `key=value` pairs, ASCII, single spaces):
//
//   ts=2026-10-23T09:12:33.123456789Z ts_ns=1761205953123456789 mono_ns=...
//   pid=4242 uid=1000 req=trigger proto=4 seq=1761205953123456000
//   request_id=1761205953123456001 retry=1 dedup=1
//   src=manual hint_raw=80 hint_eff=50 clamp=1 decision=deduplicated
//   reason=dedup_hit code=accepted trigger_id=7 quota_override=0 by=0
//
// `req` is the operation: `trigger` (submit), `cancel` or `wait` (TRG-07),
// `preempt` (TRG-12), or startup `config` for REC-23 integrity downgrades.
// Config records carry decision=configured, reason=integrity_downgrade and
// integrity_mode=metadata|off. A `preempt` record is emitted when a decided
// preemption actually executes (the old capture is truncated+sealed):
// `trigger_id` is the preempted capture, `by` the preempting trigger,
// `decision=preempted`, and pid/uid/src carry the preempting request's
// attribution (0/0/- for a direct in-process Submit, which has no peer
// credentials). On every other record `by` is 0. `quota_override=1` marks an
// accept that only passed because crash-level priority overrode the per-class
// count quota (the same event CenterStats::quota_overridden counts). `proto` is
// the request version actually accepted (1/2/3); malformed and auth-rejected
// datagrams are audited with the version they claimed (0 when the size/magic
// did not identify a known layout).
//
// Durability: O_APPEND + exactly one write() per record. A crash can lose the
// tail (or leave a partial last line, which parsers skip) but never reorders
// or interleaves records. fdatasync runs on Shutdown and, when configured,
// every audit_fsync_interval_s in Tick(); there is no fsync per record (that
// would put the disk's latency on the control path).
//
// Bounded: the file is rotated at audit_max_bytes into <path>.1 .. <path>.N
// (oldest deleted first). Rotation happens in the control path before the
// oversized record is written, so a file overshoots the bound by at most one
// record.
//
// Slow/full disk: Append never retries and never blocks beyond one small
// write. A failed/partial write increments drops(); the caller exposes that
// counter and the control path continues.
//
// Thread-safety: the Center is single-threaded by contract (Tick/Submit), so
// the log is not internally locked.

#include "thyme/record/trigger_protocol.h"
#include "thyme/runtime/rotating_file.h"

#include <cstdint>
#include <string>

#if !defined(_WIN32)
#include <sys/types.h>
#else
using uid_t = unsigned int;
using pid_t = int;
#endif

namespace thyme {
namespace trigger_audit {

struct Config {
  std::string path;                 // empty = disabled (caller derives one)
  uint64_t max_bytes = 4ull * 1024 * 1024;
  uint32_t rotate_keep = 2;         // rotated files kept (>= 1)
  uint64_t fsync_interval_ns = 0;   // 0 = only at Shutdown
};

struct Record {
  pid_t peer_pid = 0;
  uid_t peer_uid = 0;
  uint16_t proto_version = 0;
  uint64_t sequence = 0;
  // TRG-19: V4 logical request identity. Duplicate cached responses set both
  // retry and dedup; an id conflict sets retry only. Legacy keeps all zero.
  uint64_t request_id = 0;
  bool retry = false;
  bool dedup = false;
  const char* source = "";          // NUL-terminated, raw wire bytes
  const char* op = "trigger";       // trigger | cancel | wait (TRG-07) | preempt (TRG-12)
  int32_t hint_raw = -1;
  int32_t hint_effective = 0;
  bool priority_clamped = false;
  bool accepted = false;
  const char* reason = "ok";        // snake_case, no spaces
  trigger_proto::TriggerCode code = trigger_proto::TriggerCode::kAccepted;
  uint64_t trigger_id = 0;
  // TRG-12. quota_override: this accept overrode the count quota (crash
  // level). by_trigger_id: on a preempt record, the preempting trigger's id
  // (0 otherwise). decision: when non-null, emitted verbatim instead of the
  // accepted/rejected rendering — internal constant strings only (e.g.
  // "preempted" for the preempt-execution record).
  bool quota_override = false;
  uint64_t by_trigger_id = 0;
  const char* decision = nullptr;
  // REC-23: set only on startup integrity-downgrade configuration records.
  const char* integrity_mode = nullptr;
};

class Log {
 public:
  Log() = default;
  ~Log();
  Log(const Log&) = delete;
  Log& operator=(const Log&) = delete;

  // false -> the audit file could not be created; callers that treat audit
  // as mandatory (trigger_center) refuse to start.
  bool Open(const Config& cfg);
  void Close();

  // Appends one record with a single write(). Returns false when the record
  // was dropped (open failed, write error, short write).
  bool Append(const Record& r);

  // fdatasync when cfg.fsync_interval_ns elapsed. Cheap no-op when 0.
  void MaybeSync(uint64_t mono_ns);

  bool enabled() const { return file_.enabled(); }
  const std::string& path() const { return file_.path(); }
  uint64_t records() const { return records_; }
  uint64_t drops() const { return drops_ + file_.drops(); }
  uint64_t rotations() const { return file_.rotations(); }
  uint64_t bytes_written() const { return file_.bytes_written(); }

 private:
  // OPS-05: the audit writer is the mandatory RotatingFile (never dropped by
  // the process log budget; still counted in it). SEC-02's guarantees —
  // O_APPEND, one write() per record, rename-based rotation — are the shared
  // implementation's contract.
  logging::RotatingFile file_;
  uint64_t fsync_interval_ns_ = 0;
  uint64_t last_sync_ns_ = 0;
  uint64_t records_ = 0;
  uint64_t drops_ = 0;
};

}  // namespace trigger_audit
}  // namespace thyme
