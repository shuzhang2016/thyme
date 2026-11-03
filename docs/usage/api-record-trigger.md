# Recording and trigger API

This page covers the three library components of triggered recording plus the
package-level tool APIs:

- `thyme::triggered::TriggeredRecorder`: the capture engine (window, swap queue, disk
  writes, sealing)
- `thyme::trigger_proto` / `thyme::trigger_center`: the control-plane protocol and the
  trigger FSM
- `thyme::retention` / `thyme::integrity` / `thyme::schema`: retention policy, the
  evidence manifest and the record schema

All of these components are instantiable and hold no global state. `TriggeredRecorder`
and `Center` are both driven by the host's main loop: the library creates no threads of
its own, and `Tick()` is the only point where work advances.

## 1. `TriggeredRecorder`

`include/thyme/record/triggered_recorder.h`, namespace `thyme::triggered`.

### 1.1 `Config`

| Field | Default | Notes |
|---|---|---|
| `sources` | empty | list of `SourceConfig`; `Init()` attaches strictly and fails if any attach fails |
| `out_dir` | empty | package root directory, created if missing |
| `signing_key_path` | empty | raw HMAC-SHA256 key file; empty = no signing. If configured but unreadable/empty, `Init()` fails |
| `pending_bytes_quota` | 64 MiB | byte budget for pending swap descriptors; overflow evicts the oldest entries and counts them in `Stats::pending_evicted` |
| `reattach_scan_ns` | 200 ms | scan period for detecting session/shared-memory replacement |
| `attach_timeout_ns` | 2 s | attach timeout |
| `record_manifest` | `nullptr` | host-generated `schema::RecordManifest`; when non-null the package carries a manifest block and `schema.json` |
| `post_grace_ns` | 20 ms | keep sweeping after the post deadline until a whole round yields no new data and this grace has elapsed |
| `settle_batch` | 256 | number of descriptors drained per `Tick()` |
| `drain_tick_budget_ns` | 5 ms | wall-clock budget for the drain in each `Tick()` |
| `io_rate_limit_bps` | 0 | rate limit for idle-flush packages only; 0 = unlimited |
| `io_burst_bytes` | 4 MiB | burst of the rate-limit token bucket |
| `gop_roll_timeout_ns` | 5 s | hard upper bound on H.264 GOP-aligned slices in continuous mode |
| `fault_ctx` / `fault_inject_write` / `fault_inject_seal` / `test_hook_after_t0` | empty | test injection points; stay empty on the production path |

`SourceConfig`:

| Field | Default | Notes |
|---|---|---|
| `kind` | `kMirrorRing` | `kMirrorRing` or `kPinnedSlot` |
| `shm_name` | empty | shared-memory name of the mirror page or the large-frame channel |
| `channel_name` | empty | `kPinnedSlot` only: the name behind `frames/<channel_name>.*` |
| `h264_stream` | `false` | GOP-aligned slicing, no frame-rate reduction |
| `rate_hz` / `avg_frame_bytes` / `bitrate_kbps` | 0 | bitrate accounting parameters for continuous mode |
| `crc` | `kDefault` | `kDefault` / `kPackage` / `kFrame` / `kOff` |

The effective `CrcPolicy` is decided by `EffectiveCrcPolicy(source)`: `kDefault` resolves
to `kPackage` for bulk pinned_slot/h264 (per-frame transport CRC off, the whole-package
manifest is the integrity boundary) and to `kFrame` for small-struct mirror rings
(per-record CRC kept). `CrcPolicyName()` / `ParseCrcPolicy()` convert to and from the
strings `package` / `frame` / `off`.

Note: **the window is not part of `Config`**. `before_ns` / `after_ns` are arguments to
each `Trigger()` call; `channels` is likewise a per-trigger filter (or comes from
`[source.*].channels`).

### 1.2 Lifecycle

| Phase | Entered by | Notes |
|---|---|---|
| `kArmed` | after a successful `Init()` | waiting for a trigger; `Trigger()` is only valid in this phase |
| `kPost` | `Trigger(...)` | captures the post window; the pre window has already been backfilled from `t0 - before_ns` |
| `kSettle` | end of the post window | drains descriptors, writes the WAL, seals the package, then returns to `kArmed` |

Key methods:

| Method | Signature | Notes |
|---|---|---|
| `Init` | `bool Init(const Config& cfg)` | attaches every source; returns `false` on failure |
| `Trigger` | `bool Trigger(uint64_t trigger_id, uint64_t before_ns, uint64_t after_ns, uint64_t t0_mono_ns = 0, bool defer_settle = false, bool high_priority = false, const std::vector<std::string>& channels = {})` | `t0_mono_ns == 0` means "now"; outside `kArmed` it counts into `triggers_rejected` and returns `false` |
| `Tick` | `void Tick()` | host-driven: rescans sources while Armed, captures the post window while Post, bounded drain while Settle |
| `AllowFlush` | `void AllowFlush()` | idle-flush gate for `defer_settle`; only meaningful in `kSettle` |
| `Preempt` | `bool Preempt()` | truncates the in-flight capture, immediately drains and seals what is already queued, and sets `kCaptureFlagTruncated` in `CaptureStatsRecord::flags`; returns `false` in `kArmed` |
| `Shutdown` | `void Shutdown()` | releases sources and file handles |
| `phase` / `stats` / `last_package_dir` | read-only | current phase, cumulative statistics, most recent package directory |
| `last_seal_mono_ns` / `last_seal_trigger_id` | read-only | `CLOCK_MONOTONIC` time and trigger id of the most recent complete seal |
| `disk_full` / `ClearDiskFull` | `bool` / `void` | set after a drain write fails (for example ENOSPC); `Trigger()` rejects new captures until it is cleared |
| `ready` | `bool` | every source is currently attached (drives the center's `kRecorderOffline`) |
| `PackageComplete` | `static bool PackageComplete(const std::string& package_dir)` | `wal.log` parses and its final lifecycle state is `sealed`; older packages are still judged by their last seal/abort row |
| `ReadPackageLifecycle` | `bool ReadPackageLifecycle(const std::string& package_dir, PackageLifecycleState* out)` | reads the persistent lifecycle, reason code and dual-clock timestamps from the package WAL; returns `false` if corrupted/truncated |
| `AbortPackageAfterCrash` | `bool AbortPackageAfterCrash(const std::string& package_dir)` | for startup recovery: appends `aborted(crash_recovery)` to a parsable non-terminal WAL; idempotent on terminal states |

The package lifecycle is `armed → capturing → sealing → sealed`; explicit invalidation
or startup crash recovery moves it to `aborted`. Reason codes are `none`, `unspecified`
(old abort rows), `write_failure`, `seal_failure` and `crash_recovery`. The lifecycle
uses a dedicated op on the existing 24 B `WalRow`, and the trailing legacy seal/abort
row is still written. A WAL without lifecycle information is returned with
`legacy=true` and a derived state, and an unsealed old package can only be derived as
`capturing`. `PackageLifecycleName()` and `PackageAbortReasonName()` provide stable text
names for auditing and alerting.

Triggered mode has **no** `Arm()` / `Disarm()` methods: `kArmed` is the FSM's initial
phase, `Trigger()` is the only post-arm action, and `Shutdown()` ends the lifecycle. For
continuous recording use `ArmContinuous()` / `DisarmContinuous()` from the next section.

### 1.3 `Stats`

| Field | Meaning |
|---|---|
| `captures_done` | triggered captures that sealed successfully |
| `triggers_rejected` | `Trigger()` calls outside `kArmed` |
| `descriptors_queued` | total swap descriptors queued |
| `pending_evicted` | evictions caused by queue quota/ring capacity (oldest first) |
| `overrun_before_queue` | samples already evicted from shared memory before the post sweep queued them |
| `samples_written` | small-struct data records |
| `frames_written` | large-frame raw segments + index records |
| `holes_written` | GAP records (overwritten before the drain) |
| `torn_reads` | seqlock tears observed during harvest |
| `harvest_retries` | rescans of the pre window caused by a new tear |
| `session_reattaches` | successful re-attaches after shared-memory replacement |
| `source_errors` | failed re-attaches (configuration drift, for example) |
| `disk_errors` | failed drain writes |
| `sync_errors` | failed abort row / fsync / directory fsync |

### 1.4 Package layout

```text
<out_dir>/<trigger_id>_<t0>/
├── meta.tbin                    header + TRIGGER entry + TrigMeta + manifest + stats
├── small.tbin                   small-struct samples (including GAPs), ascending by mono_ns
├── frames/<channel>.raw         raw bytes of large frames
├── frames/<channel>.index.tbin  FrameIndexRecord rows (offset/len/flags/payload_crc)
├── wal.log                      drain journal; a seal row means the package is complete
├── schema.json                  record-contract sidecar
├── integrity.manifest           evidence manifest
└── integrity.hmac               detached HMAC-SHA256 when a signing key is configured
```

`FrameIndexRecord` is `{uint64_t offset; uint32_t len; uint32_t flags;
uint32_t payload_crc;}`, where `payload_crc` is the CRC32 of the raw segment, recomputed
on demand by `thyme-replay verify`. `wal.log` first writes a `WalHeader` with magic
`TRIGWAL1` (`kWalMagic`), followed by fixed 24-byte `WalRow`s; `kWalOpSeal` means done
and `kWalOpAbort` means the seal failed midway. `kWalOpClockAnchor` is a standalone time
anchor row: `index` is the `CLOCK_MONOTONIC` value and `mono_ns` the corresponding
`CLOCK_REALTIME` value. Triggered packages and continuous slices write anchors at
creation, once per second while active, and before sealing; anchors never enter the
replay data stream.

`include/thyme/record/clock_anchor.h` provides cold-path reading and conversion:

```cpp
std::vector<thyme::triggered::ClockAnchor> anchors;
if (thyme::triggered::ReadPackageClockAnchors(package_dir, &anchors) &&
    !anchors.empty()) {
  uint64_t realtime_ns = 0;
  if (thyme::triggered::MonoToRealtimeNs(anchors, record_mono_ns,
                                         &realtime_ns)) {
    // realtime_ns is the estimated wall-clock time for record_mono_ns.
  }
}
```

The conversion picks the closest anchor before the target (records older than the first
anchor use the first anchor), keeps the monotonic delta and never interpolates; overflow
returns `false`. Old packages without anchors return an empty list, while a truncated
WAL, illegal fields or a mono regression in the anchors return an error. The error bound
and the rejection matrix are in
[clock-conversion.md](../reference/clock-conversion.md).

Swap descriptors carry header-level information only (`source` / `kind` / `struct_id` /
`index` / `mono_ns` / `len` / `slot_seq`). Small-struct payloads are copied during
Settle, only after a seqlock + `slot_seq` + CRC re-check; large frames are written from
the lease-pinned slot view, so the bytes on disk equal what the producer committed. A
slot overwritten before the drain becomes a GAP with reason `kGapReasonOverrun`.

```cpp
#include "thyme/record/triggered_recorder.h"

thyme::triggered::Config cfg;
cfg.out_dir = "/data/thyme_captures";
cfg.pending_bytes_quota = 64ull * 1024 * 1024;

thyme::triggered::SourceConfig cam;
cam.kind = thyme::triggered::SourceConfig::Kind::kPinnedSlot;
cam.shm_name = "/h264_cam0";
cam.channel_name = "cam0";
cam.h264_stream = true;
cfg.sources.push_back(cam);

thyme::triggered::TriggeredRecorder recorder;
if (!recorder.Init(cfg)) return -1;

// host loop
while (running) {
  if (trigger_pending) {
    recorder.Trigger(trigger_id++, 5ull * 1000000000ull,
                     3ull * 1000000000ull);  // 5 s before, 3 s after
  }
  recorder.Tick();
}
```

## 2. Trigger protocol

`include/thyme/record/trigger_protocol.h`, namespace `thyme::trigger_proto`. The wire
format is little-endian with fixed structures and magic `kTriggerMagic` (`"TRIG"`).

### 2.1 Versions and layout

| Version | Constant | Request | Response | Notes |
|---|---|---|---|---|
| V1 | `kTriggerVersionLegacy == 1` | 88 B | 16 B | no sequence, no CRC |
| V2 | `kTriggerVersionV2 == 2` | 96 B | 24 B | adds `sequence`, still no CRC |
| V3 | `kTriggerVersion == 3` | 104 B | 32 B | adds `response_timeout_ms` + `crc32`; the only version accepted by default |

`TriggerRequest` fields: `magic`, `version`, `flags`, `priority_hint` (negative =
use the configured default), `timestamp_ns` (only valid with `kFlagHasTimestamp`),
`source[64]` (NUL-terminated), `sequence`, `response_timeout_ms`, `crc32`.
`TriggerResponse` fields: `magic`, `version`, `code`, `trigger_id`,
`effective_priority`, `flags`, `reserved0`, `crc32`.

Each version is a pure append over the previous one: the common prefix offsets are
pinned by `static_assert`, so a server can select the structure by datagram size and
then validate `version`.

### 2.2 flags

| Constant | Value | Notes |
|---|---|---|
| `kFlagHasTimestamp` | `1u << 0` | `timestamp_ns` is valid |
| `kFlagForce` | `1u << 1` | operator-confirmed preemption; `priority >= current` suffices to preempt |
| `kFlagCancel` | `1u << 2` | cancels the trigger id given by `timestamp_ns` (must also set `kFlagHasTimestamp`) |
| `kFlagWaitOutcome` | `1u << 3` | wait for the capture outcome; `timestamp_ns` is the trigger-id filter and `kWaitFilterAny` means any |

`kFlagCancel` / `kFlagWaitOutcome` are V3-only; older servers reject them with
`kInvalidTimestamp`. Any **unknown** flag bit is rejected as malformed on every version —
a server never records a trigger for a request whose semantics it does not understand.

Response flags: `kRespFlagPriorityClamped` (the priority was clamped or lowered by
policy) and `kRespFlagOutcomeNotification` (this is the deferred outcome
notification).

### 2.3 CRC and replay protection

CRC32 (IEEE 802.3, the same `Crc32()` as `FrameHeader`) covers every byte before the
`crc32` field. Helper functions:

```cpp
uint32_t RequestCrc(const TriggerRequest& r);
void StampRequestCrc(TriggerRequest* r);
uint32_t ResponseCrc(const TriggerResponse& r);
void StampResponseCrc(TriggerResponse* r);
```

The server validates the request CRC **before** sequence/priority handling; a bad CRC is
treated as `kMalformedRequest` (audited, never answered). The client validates the
response CRC. `sequence` is seeded from `CLOCK_MONOTONIC` and incremented per call; the
server keeps a 64-entry sliding window per `(uid, pid)` and rejects 0, duplicates and
stale values (`kReplayRejected`).

`response_timeout_ms` ranges from 1 to 60000 and defaults to
`kDefaultResponseTimeoutMs == 100`. The server replies synchronously within one `Tick()`;
the client enforces the deadline with `CLOCK_MONOTONIC` and maps expiry to
`kTransportTimeout` (distinct from `kRecorderOffline`).

### 2.4 `TriggerCode`

| Value | Name | Meaning |
|---|---|---|
| 0 | `kAccepted` | accepted |
| 1 | `kMuted` | rejected during mute, or because the requester was low/equal priority |
| 2 | `kQuotaExceeded` | count or memory quota |
| 3 | `kUnknownSource` | unknown trigger source |
| 4 | `kPreemptedByHigher` | outcome notification only: a running capture was preempted by a higher priority |
| 5 | `kRecorderOffline` | recorder unavailable (also used for client socket/setup failure) |
| 6 | `kBusy` | an accepted but not yet started trigger occupies the queue, or a preemption is in progress; retryable |
| 7 | `kInvalidTimestamp` | `timestamp_ns` out of range |
| 8 | `kUnsupportedVersion` | V1/V2 request but the legacy peer is not enabled |
| 9 | `kReplayRejected` | duplicate/stale sequence |
| 10 | `kAuthRejected` | audit only: peer uid/pid not allowed (no reply) |
| 11 | `kMalformedRequest` | audit only: bad size/magic/version/CRC (no reply) |
| 12 | `kTransportTimeout` | client got no response before the deadline (never sent by the server) |
| 13 | `kCancelled` | Cancel took effect, or outcome notification that the capture was cancelled |
| 14 | `kUnknownTrigger` | Cancel target unknown or already complete |
| 15 | `kCaptureSealed` | outcome notification: the capture completed and sealed |
| 16 | `kStartFailed` | outcome notification: deferred start failed (rolled back) |
| 17 | `kPreemptFailed` | outcome notification: preemption failed and the accepted trigger was dropped |

## 3. `TriggerCenter`

`include/thyme/record/trigger_center.h`, namespace `thyme::trigger_center`.

### 3.1 Initialization and call surface

```cpp
bool Center::Init(const trigger_cfg::CenterConfig& cfg, const Hooks& hooks);
void Center::Tick();
SubmitResult Center::Submit(const std::string& source,
                            int32_t priority_hint,
                            uint64_t timestamp_ns,
                            bool has_timestamp,
                            bool force);
trigger_proto::TriggerCode Center::Cancel(uint64_t trigger_id);
void Center::ReloadConfig(const trigger_cfg::CenterConfig& cfg);
```

`Init()` binds the control socket, the audit log and the FSM file; `Tick()` handles
socket polling, capture-completion detection, mute expiry, quota-window GC, deferred
start/preemption execution and outcome-waiter timeouts.
`ProcessRequest(buf, n, peer, resp, reply_path = nullptr)` is the public entry point of
the socket handler: when it returns `true`, the caller sends a 16/24/32-byte response
according to `resp->version`.

`Hooks` injects the recorder into the FSM (all optional; unset hooks count as
unavailable):

| Hook | Signature | Purpose |
|---|---|---|
| `recorder_ready` | `bool (*)(void*)` | `false` → `kRecorderOffline` |
| `capture_active` | `bool (*)(void*)` | detect capture completion |
| `start_capture` | `bool (*)(void*, uint64_t trigger_id, uint64_t t0_ns, uint64_t before_ns, uint64_t after_ns, bool force_realtime_flush, bool high_priority, const std::vector<std::string>& channels)` | start a capture |
| `preempt_capture` | `bool (*)(void*)` | truncate + seal the in-flight capture |
| `pending_bytes` | `uint64_t (*)(void*)` | memory-quota coupling; empty = not consulted |
| `evict_oldest` | `uint64_t (*)(void*, uint64_t bytes)` | crash-level memory override |
| `disk_full` | `bool (*)(void*)` | when the disk is full, non-crash triggers are rejected with `kQuotaExceeded` |

### 3.2 FSM, priority and quota

`State`: `kIdle` / `kCapturing` / `kMuted`. `max_concurrent_captures` supports only 1
for now (values greater than 1 are a configuration error).

- `kIdle` / `kMuted` receiving a trigger → accept and enter `kCapturing`.
- `kCapturing` receiving a higher-priority trigger → preempt: the old capture is
  truncated and sealed, the new one starts immediately with a forced realtime flush;
  with `kFlagForce`, `priority >= current` suffices to preempt.
- Otherwise a lower or equal priority trigger returns `kMuted`.
- While `kMuted`, triggers with `priority >= crash_priority` (default 100) get through;
  everything else is `kMuted`.
- The count quota is tracked per `quota_class` over a 1-hour sliding window (0 =
  unlimited); crash level may override it and counts into `quota_overridden`.
- When the recorder's `pending_bytes_quota` is exceeded, the Center checks through the
  `pending_bytes` hook and rejects only low-priority triggers; crash level first calls
  `hooks.evict_oldest` and then proceeds, counting into `memory_evictions`.
- An accepted but not yet started trigger occupies a depth-1 queue, so a second
  admission gets `kBusy`.

`SubmitResult`: `code`, `trigger_id`, `priority_used`, `priority_clamped`,
`quota_overridden`, `source_used` (the name after `default_source` fallback).

### 3.3 `FsmEvent` ring

Every FSM transition appends one `FsmEvent{mono_ns, trigger_id, from, to, reason}` to a
fixed 256-entry ring buffer (`kFsmEventRingSize`). When the ring is full it overwrites
the oldest entry and counts into `stats.fsm_event_drops`.

| `FsmReason` | Value | Meaning |
|---|---|---|
| `kSubmitAccept` | 0 | trigger answered and queued |
| `kStarted` | 1 | a queued trigger actually started |
| `kStartFailed` | 2 | start failed, rolled back |
| `kPreempted` | 3 | old capture truncated and sealed |
| `kPreemptFailed` | 4 | preemption failed, original capture restored |
| `kCompleted` | 5 | capture completed and sealed |
| `kMuteExpired` | 6 | mute expired |
| `kCancelPending` | 7 | a not-yet-started trigger was cancelled |
| `kCancelRunning` | 8 | a running capture was cancelled |

`CopyFsmEvents(FsmEvent* out, uint32_t cap)` copies events in time order (at most `cap`)
and returns the count. `DumpNewFsmEvents()` appends the events not yet on disk to
`<socket_path>.fsm` (O_APPEND JSONL, one `write()` per event, rotated per `[logging]`);
`Tick()` dumps once per second by default, adjustable via
`set_fsm_dump_interval_ns()`. A crash loses at most one dump period of events, and
readers should tolerate a truncated final line.

### 3.4 `CenterStats`

Admission and rejection:

| Field | Meaning |
|---|---|
| `accepted` | accepted triggers |
| `rejected_muted` / `rejected_quota` / `rejected_unknown` / `rejected_offline` | the corresponding rejection reasons |
| `rejected_busy` | previous trigger not started yet, or a preemption in progress |
| `rejected_bad_timestamp` | `timestamp_ns` out of range |
| `preemptions` | number of preemptions |
| `quota_overridden` / `memory_evictions` | crash-level overrides / memory evictions |

Security and protocol:

| Field | Meaning |
|---|---|
| `auth_rejects` | uid/pid not allowed |
| `malformed_datagrams` | bad size/magic/version |
| `crc_rejects` | V3 CRC errors |
| `replay_rejects` | duplicate/stale sequence |
| `priority_clamps` | hints clamped or lowered by policy |
| `legacy_requests` / `legacy_v2_requests` | accepted V1 / V2 datagrams |
| `audit_drops` | audit records that could not be written |
| `start_failures` / `preempt_failures` | answered but failed to start / preemption execution failed |
| `reply_send_failures` | direct replies refused by the kernel (EAGAIN/ENOBUFS and so on) |

Outcomes and observability:

| Field | Meaning |
|---|---|
| `cancels_applied` / `cancels_unknown` / `cancel_failures` | Cancel results |
| `notify_outcomes` / `notify_outcome_drops` | entries pushed into the 64-entry outcome ring / overwrites when it is full |
| `notify_delivered` / `notify_send_failures` | outcome notifications sent / failed |
| `notify_waiter_overflows` / `notify_waiter_expired` | overflow of the 8-slot waiter table / discarded on expiry |
| `fsm_events` / `fsm_event_drops` | total FSM events (including overwritten ones) / ring-full overwrites |
| `fsm_rotations` / `fsm_bytes` / `fsm_write_drops` | `<socket>.fsm` rotations, bytes, failed/budget-dropped writes |
| `socket_datagrams` / `socket_backlog_events` | cumulative datagrams / ticks that hit the 16-datagram receive budget |

### 3.5 Audit log

`include/thyme/record/trigger_audit.h`, namespace `thyme::trigger_audit`. One line per
request, appended with `O_APPEND` + a single `write()`, in space-separated `key=value`
ASCII:

```text
ts=2026-10-23T09:12:33.123456789Z ts_ns=1761205953123456789 mono_ns=...
pid=4242 uid=1000 req=trigger proto=3 seq=1761205953123456000
src=manual hint_raw=80 hint_eff=50 clamp=1 decision=accepted reason=ok
code=accepted trigger_id=7 quota_override=0 by=0
```

| Key | Source | Notes |
|---|---|---|
| `ts` / `ts_ns` | `CLOCK_REALTIME` | ISO-8601 UTC / nanoseconds |
| `mono_ns` | `CLOCK_MONOTONIC` | processing instant |
| `pid` / `uid` | `PeerCred` (SCM_CREDENTIALS) | in-process calls pass 0/0 |
| `req` | `Record::op` | `trigger` / `cancel` / `wait` / `preempt` |
| `proto` / `seq` | request | accepted version / replay-protection sequence |
| `src` | request | trigger source name (bounded escaping) |
| `hint_raw` / `hint_eff` / `clamp` | priority handling | raw hint / effective value / whether it was adjusted |
| `decision` | `Record::decision` | `accepted` or `rejected` by default; preempt records as `preempted` |
| `reason` / `code` | decision | snake_case reason / `TriggerCode` name |
| `trigger_id` | request/FSM | the trigger id concerned |
| `quota_override` / `by` | admission | crash-level override flag / preempting trigger id (0 in other records) |

`Config`: `path` (empty = disabled), `max_bytes = 4 MiB`, `rotate_keep = 2`,
`fsync_interval_ns = 0` (fsync only in `Shutdown`). Files rotate by `max_bytes` into
`<path>.1 .. <path>.N`; when the disk is slow or full, `Append()` neither retries nor
blocks, and the failure counts into `drops()`. Under the Center's single-threaded
contract the log needs no internal locking.

```cpp
#include "thyme/record/trigger_center.h"

thyme::trigger_center::Hooks hooks;
hooks.ctx = &recorder;
hooks.recorder_ready = [](void* c) {
  return static_cast<thyme::triggered::TriggeredRecorder*>(c)->ready();
};
hooks.start_capture = [](void* c, uint64_t id, uint64_t t0, uint64_t before,
                         uint64_t after, bool force_rt, bool high,
                         const std::vector<std::string>& channels) {
  return static_cast<thyme::triggered::TriggeredRecorder*>(c)->Trigger(
      id, before, after, t0, /*defer_settle=*/!force_rt, high, channels);
};

thyme::trigger_cfg::CenterConfig cfg;
cfg.socket_path = "/tmp/thyme_trigger.sock";
cfg.default_source = "manual";
cfg.crash_priority = 100;

thyme::trigger_center::Center center;
if (!center.Init(cfg, hooks)) return -1;

while (running) {
  center.Tick();
  if (manual_request) {
    const auto r = center.Submit("manual", 50, 0, false, false);
    if (r.code != thyme::trigger_proto::TriggerCode::kAccepted) {
      // kMuted / kQuotaExceeded / kBusy / ...
    }
  }
}
```

## 4. Continuous recording

Continuous mode reuses `TriggeredRecorder` but with its own descriptor bucket and an
unbounded window. `ContinuousConfig`:

| Field | Default | Notes |
|---|---|---|
| `slice_bytes` | 512 MiB | byte limit per slice |
| `slice_ns` | 0 | time limit per slice; 0 = do not roll on time |
| `wait_idle` | `false` | share the same idle gate; not recommended together with large frames (frames are overwritten while the gate is held and count into `pinned_slot_drops`) |
| `channels` | empty = `{"small_struct:*"}` | allow/exclude list |

`channels` shares the `ChannelSelectorRule` syntax with `Trigger(..., channels)`:

- `small_struct:<glob>` / `pinned_slot:<glob>`: select by type; `*` matches any
  character sequence
- a bare `<channel-name-or-shm-glob>`: the original name/shm addressing, any type
- any include entry may carry `@dec=N`: record 1 of every N publish sequence numbers
  for both small structs and large frames; h264_stream cannot be decimated
- the prefix `!` marks an exclusion, exclusions win, and a specific include overrides
  the decimation of a `*` include
- if parsing fails or the final selection is empty, `Trigger` / `ArmContinuous` return
  `false`

`auto_arm` is **not** a field of `ContinuousConfig`: it maps to `[continuous] auto_arm`
in the configuration file, which the host parses into
`trigger_cfg::CenterConfig::cont_auto_arm` to decide whether to call `ArmContinuous()`
at startup. Manual arming goes through the same validation.

| Method | Signature | Notes |
|---|---|---|
| `ArmContinuous` | `bool ArmContinuous(const ContinuousConfig& cfg)` | starts from the **current** heads, no history backfill; callable in any phase and never interrupts a triggered capture |
| `DisarmContinuous` | `void DisarmContinuous()` | seals the current slice |
| `continuous_armed` | `bool` | whether continuous recording is active |
| `continuous_stats` | `const ContinuousStats&` | continuous-mode counters |
| `current_slice_dir` | `const std::string&` | active slice directory (for retention exclusion); empty when not armed |
| `SetContinuousGate` | `void SetContinuousGate(bool open)` | level-driven idle gate used when `wait_idle = true` |
| `ParseContinuousChannels` | `static bool ParseContinuousChannels(const std::vector<std::string>& entries, std::vector<ContinuousChannelRule>* out, std::string* err)` | parse/validate the selector |

`DrainContinuous(uint64_t now)` is a **private** implementation detail called by `Tick()`
→ `TickContinuous()`; the host cannot drain directly. The public contract is only "the
host calls `Tick()` repeatedly".

`ContinuousStats`:

| Field | Meaning |
|---|---|
| `slices_done` | sealed slices |
| `samples_written` / `frames_written` / `holes_written` | data records / large frames / GAPs |
| `queue_evicted` | continuous queue entries evicted under shared quota pressure |
| `torn_reads` | seqlock tears |
| `bytes_written` | cumulative bytes |
| `pinned_slot_drops` | frames skipped as gaps because streaming drain fell behind |
| `roll_forced` | forced slices due to the GOP-alignment timeout |
| `out_of_order` | late samples below the slice watermark (degraded to GAP) |

Continuous slice directories are named `continuous_<seq>_<ts>/`; the
`ContinuousMetaRecord` in `meta.tbin` records `slice_seq` / `begin_mono_ns` /
`end_mono_ns` / `samples` / `frames` / `holes` / `evicted` / `detail` (the IDR mono_ns
when GOP-aligned, otherwise 0). The continuous and triggered queues share
`pending_bytes_quota`, and under pressure continuous data is evicted first — triggered
packages win.

## 5. Retention

`include/thyme/record/retention.h`, namespace `thyme::retention`.

```cpp
struct Config {
  std::string out_dir;
  uint64_t disk_quota_bytes = 0;        // 0 = unlimited
  std::string high_dir = "high";
  uint64_t high_reserved_bytes = 0;     // 0 = never clean up high/
  IncompletePolicy incomplete_policy = IncompletePolicy::kKeep;
  uint64_t incomplete_max_age_ns = 0;   // only for kDeleteAfterAge
  std::string signing_key_path;
};
```

| Member | Signature | Notes |
|---|---|---|
| `Init` | `bool Init(const Config& cfg)` | validates the directory and quotas |
| `Scan` | `std::vector<PackageInfo> Scan() const` | every package in the low-priority area and `high/`, oldest first |
| `MarkIncomplete` | `uint32_t MarkIncomplete()` | startup recovery: renames packages whose wal has no seal to `<name>.incomplete` and best-effort writes a partial manifest; returns the count marked |
| `Enforce` | `uint64_t Enforce(const std::vector<std::string>& exclude)` | quota sweep; `exclude` is mandatory (no default), and the seal state is re-checked internally |
| `symlinks_skipped` | `uint64_t` | cumulative symlinks skipped during traversal |
| `DirBytes` / `RemoveTree` | `static uint64_t` / `static bool` | recursive byte count / tree removal |

Rules:

- When the total exceeds `disk_quota_bytes`, delete the **oldest low-priority
  packages**; `high/` is never touched by that pass.
- When `high/` exceeds `high_reserved_bytes`, delete only the oldest crash packages
  inside `high/`.
- Packages are identified by directory mtime, not by file suffix.
- Everything uses `lstat` and never follows symlinks; `RemoveTree` only unlinks the link
  itself.
- The recursion depth limit is `kMaxTreeDepth == 8`; exceeding it counts as
  corruption/attack: no further descent, and deletion is refused.
- A directory that is not sealed (`PackageComplete() == false`) and not marked
  `.incomplete` counts as "being written" and is never deleted, even when it is absent
  from `exclude`.
- `.incomplete` packages are handled only by `incomplete_policy`: `kKeep` (the default,
  kept forever as incident evidence), `kDeleteAfterAge`, `kDeleteImmediately`. The quota
  sweep never deletes them.

```cpp
#include "thyme/record/retention.h"

thyme::retention::Config rc;
rc.out_dir = "/data/thyme_captures";
rc.disk_quota_bytes = 200ull * 1024 * 1024 * 1024;
rc.high_reserved_bytes = 20ull * 1024 * 1024 * 1024;
rc.incomplete_policy = thyme::retention::IncompletePolicy::kKeep;

thyme::retention::Manager retention;
retention.Init(rc);
retention.MarkIncomplete();                       // once at startup

std::vector<std::string> active{recorder.last_package_dir(),
                                recorder.current_slice_dir()};
retention.Enforce(active);                        // after each seal + a 5 min tick
```

## 6. Package verification and schema

### 6.1 Evidence manifest

`include/thyme/record/package_integrity.h`, namespace `thyme::integrity`.

| Constant / type | Value | Notes |
|---|---|---|
| `kManifestName` | `integrity.manifest` | binary manifest |
| `kSignatureName` | `integrity.hmac` | detached HMAC-SHA256 (hex) |
| `kManifestVersion` | 1 | manifest version |
| `kManifestHeaderBytes` | 120 | fixed header size in bytes |
| `PackageKind` | `kTrigger = 1` / `kContinuous = 2` | package kind |
| `kFlagSigned` / `kFlagPartial` | `1u << 0` / `1u << 1` | signed / surviving files of a partial package |
| `ManifestState` | `kOk` / `kAbsent` / `kInvalid` | parse result |

```cpp
bool WritePackageManifest(const std::string& package_dir,
                          const PackageInfo& info,
                          const std::string& signing_key_path,
                          std::string* err);
VerifyResult VerifyPackageManifest(const std::string& package_dir,
                                   const std::string& signing_key_path);
bool LoadSigningKey(const std::string& path, std::string* key,
                    std::string* err);
```

The manifest covers every regular file in the package (except `integrity.manifest` /
`integrity.hmac` themselves); entries are sorted by relative-path byte order and chained
into a hash chain:

```text
chain[-1] = sha256("thyme-evidence-package-v1")
chain[i]  = sha256(chain[i-1] || path || u64le(size) || sha256(file))
root      = chain[file_count-1]
```

Key `VerifyResult` fields and how they are judged:

| Field | Meaning |
|---|---|
| `state` / `error` | manifest state / error details |
| `info` | kind/session/trigger/slice times from the header |
| `partial` | this is the partial manifest of an `.incomplete` package |
| `signed_manifest` / `signature_present` / `signature_checked` / `signature_ok` | signature state |
| `chain_ok` | whether the chain recomputed from the actual files matches |
| `files_ok` / `files_missing` / `files_mismatch` / `files_unlisted` | per-file results |
| `intact()` | the manifest parses, the chain matches, every listed file exists unmodified, there are no extra files, and the signature (if any) verifies |
| `ok()` | `intact()` and not partial: a fully sealed package |

A signed manifest fails closed when no key is provided (`signature key required`).
`intact()` can be true for a partial manifest (the surviving files are sound), but
`ok()` is always `false`. A missing manifest is reported as `kAbsent`, not as a crash.

### 6.2 Record schema sidecar

`include/thyme/record/record_schema.h`, namespace `thyme::schema`.

| Member | Notes |
|---|---|
| `kFileName` / `kFormat` | `schema.json` / `thyme-record-schema` |
| `kVersion == 3`, `kVersionMin == 1` | current version; v1/v2 sidecars remain readable |
| `PackageSchema` | `version` / `partial` / `manifest_declared` / `manifest_hash` / `manifest_version` / `frame_header_fields` / `channels` / `records` |
| `ChannelDesc` | `{struct_id, channel, record_key, contract_version, payload_size, layout_hash}` |
| `RecordDesc` | record-level `channel` / `record_key` / `layout_hash` / `payload_offset` / `contract_bytes` / `fields` |
| `ReadState` | `kOk` / `kAbsent` (packages predating the record manifest) / `kInvalid` |
| `ComputeLayoutHash` | FNV-1a over the field model; 0 means the layout is unknown, and non-zero values are comparable across builds |

```cpp
ReadState ReadPackageSchema(const std::string& package_dir,
                            PackageSchema* out, std::string* err);
ReadState ReadSchemaFile(const std::string& path,
                         PackageSchema* out, std::string* err);
bool WritePackageSchema(const std::string& package_dir,
                        const ThymeStructDesc* descs, std::size_t count,
                        const RecordManifest* manifest, bool partial,
                        std::string* err);
bool WriteSchemaFile(const std::string& path,
                     const ThymeStructDesc* descs, std::size_t count,
                     const RecordManifest* manifest, bool partial,
                     std::string* err);
std::string RenderSchemaText(const PackageSchema& schema);
```

The write path is atomic (temp + fsync + rename + directory fsync) and runs after the
data files are finalized and before the integrity manifest, so the sidecar is covered by
the package hash chain. Reading is strict: unknown keys, duplicate keys, unknown enums,
out-of-range numbers and trailing bytes are all rejected as `kInvalid`.

Field offsets are offsets **inside the record payload**: for a bare contract record
`payload_offset == 0`; for a `sizeof(Frame<T>)` record it is `align_up(36,
alignof(T))` (`Frame` is not packed). A record with `layout == kUnavailable` guarantees
only `struct_id` / `name` / `contract_version` / `payload_size`, so tools must treat it
as raw bytes — never guess the field layout.

```cpp
#include "thyme/record/package_integrity.h"
#include "thyme/record/record_schema.h"

const auto vr = thyme::integrity::VerifyPackageManifest(pkg_dir, key_path);
if (vr.state == thyme::integrity::ManifestState::kAbsent) {
  // old or partial package: no evidence manifest
} else if (!vr.intact()) {
  // tampered, or files are missing
} else if (!vr.ok()) {
  // partial (.incomplete) package: survivors are intact, but it is not fully sealed
}

thyme::schema::PackageSchema schema;
std::string err;
const auto rs = thyme::schema::ReadPackageSchema(pkg_dir, &schema, &err);
if (rs == thyme::schema::ReadState::kOk) {
  // schema.channels / schema.records can be used for offline decoding
}
```

## Related documents

- [Runtime primitive API](api-runtime.md)
- [Replay API](api-replay.md)
- [Triggered acquisition design](triggered-acquisition.md)
- [Configuration and deployment](config-deploy.md)
