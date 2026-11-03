# Triggered acquisition

Triggered acquisition consists of two halves: `trigger_center` is the single
trigger entry point, responsible for authentication, the FSM, mute, priority,
quota, and audit; `TriggeredRecorder` harvests the sender cache per window,
queues descriptors, persists them, and seals packages. The current
implementation assembles both in one process, driven directly through `Hooks`,
while the control plane remains a separate Unix datagram socket.

## Components and call chain

```text
business module / triggerctl
        │  Unix datagram: TriggerRequest V3 (source, priority_hint, timestamp)
        ▼
  trigger_center ── FSM / mute / quota / audit / FSM log
        │  Hooks::start_capture(trigger_id, t0, before, after,
        │                        force_realtime, high_priority, channels)
        ▼
  TriggeredRecorder ── pre harvest → post continuation → Settle → seal
        │
        ▼
<output_dir>/<trigger_id>_<t0>/   or   <output_dir>/high/<trigger_id>_<t0>/
```

- The client is the header-only `trigger_client`: `Trigger()`, `Cancel()`,
  `WaitOutcome()`. Business threads fire and forget; results can be queried
  asynchronously.
- Trigger messages are fixed-size V3 (with `response_timeout_ms` and a CRC32
  trailer); V1/V2 are rejected by default and only allowed when
  `[security].allow_legacy_*` explicitly permits them.
- `max_concurrent_captures` supports only `1`; a value `> 1` is a parse error
  because the FSM cannot run several captures in parallel.

## Trigger FSM

Three states: `IDLE(0)`, `CAPTURING(1)`, `MUTED(2)`.

| Current state | Condition | Result |
| --- | --- | --- |
| `IDLE` | any priority | accept |
| `CAPTURING` | `priority > current`, or `force && priority >= current` | preempt (truncate and seal the old capture, start the new one immediately) |
| `CAPTURING` | otherwise | reject, `code = kMuted` |
| `MUTED` | `priority >= crash_priority` | accept (crash level is not subject to mute) |
| `MUTED` | otherwise | reject, `code = kMuted` |

`force` is a manual confirmation of preemption (`triggerctl <source> --force`);
without it, equal priority cannot preempt.

### Two-phase accept: Accepted means queued

This is the semantics most easily misread. `Submit()` **only decides and
enqueues**, then immediately returns `kAccepted`; the actual `start_capture()`
happens in `StartPendingTrigger()` on the next `Tick()`. Therefore:

- `Accepted` means "the request passed authentication, mute, priority, and
  quota checks and entered the pending queue"; it **does not** mean the capture
  has started, still less that it will produce a package.
- An acknowledged but not yet started trigger can be rolled back with `Cancel`;
  a failure during startup is reported as `kStartFailed` through an outcome
  notification.
- Only one pending trigger can exist in the queue at a time; if another arrives
  before the previous one starts, it returns `kBusy` (not `kMuted`; the two have
  different causes).
- Preemption is decided in `Submit` but executed in `Tick`; the reply is sent
  first so the socket path is not blocked by disk I/O.

### FSM transitions and reason codes

Every transition appends an FSM event; `reason` takes these values:

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | `kSubmitAccept` | Trigger acknowledged and enqueued |
| 1 | `kStarted` | A queued trigger actually started |
| 2 | `kStartFailed` | Recorder offline or `start_capture` failed; rolled back |
| 3 | `kPreempted` | Old capture truncated and sealed; `id` records the old trigger |
| 4 | `kPreemptFailed` | Preemption failed; the original capture was restored |
| 5 | `kCompleted` | Capture completed and sealed |
| 6 | `kMuteExpired` | Mute expired, back to IDLE |
| 7 | `kCancelPending` | An acknowledged but unstarted trigger was cancelled; the FSM rolled back |
| 8 | `kCancelRunning` | A running capture was cancelled (truncate + seal, then mute) |

### Preemption

Preemption runs in `Tick` and proceeds as "truncate the old capture → seal what
was already written → start a new capture with the new trigger parameters":

1. Call `Hooks::preempt_capture()`; on failure, roll back to the old capture,
   discard the new trigger, set the outcome to `kPreemptFailed`, and record
   `kPreemptFailed` in the FSM.
2. On success, the old package is sealed with what was harvested,
   `CaptureStatsRecord::flags` gets `kCaptureFlagTruncated` (bit0), and the post
   window is abandoned.
3. The new capture's `flush_policy` is forced to realtime; crash-level packages
   go to the `high/` reserved area.
4. Only at this point (irreversible) is the audit line
   `req=preempt decision=preempted` written, with `trigger_id` being the
   preempted capture and `by` the preemptor; a `kPreemptedByHigher` (response
   code 4) outcome is also pushed to the old capture.

### Cancellation

`Cancel(trigger_id)` goes through the same `[security]` authentication and
audit as a trigger (`req=cancel`):

| Target state | Behavior | Return |
| --- | --- | --- |
| Acknowledged, not started | Discard the queue entry; the FSM rolls back to the pre-Submit snapshot (the quota window is retained because the request was already admitted) | `kCancelled` |
| Running | Truncate + seal through the same preempt hook; the written portion is kept and the source's `mute_s` still applies | `kCancelled` |
| Unknown / completed / a completed id under mute | Never silently succeeds | `kUnknownTrigger` |
| Preemption in progress | Only the pending trigger can be cancelled; the old capture will shortly receive `kPreemptedByHigher` | Depends on the target |

If the preempt hook fails while cancelling a running capture, it returns
`kBusy`, the capture continues, and the caller may retry.

### Mute

After a capture completes or is cancelled, the source's configured `mute_s`
takes effect and the FSM enters `MUTED`; `mute_s = 0` returns directly to
`IDLE`. Mute blocks only ordinary priority; crash level
(`priority >= crash_priority`) always passes. Starting a preemption clears the
current mute, and the window is recomputed from the new trigger time.

## Windows: before / after / post_grace

Windows come from `[source.*]`:

| Key | Default | Meaning |
| --- | --- | --- |
| `before_s` | 0 | Pre window, fractions allowed |
| `after_s` | 0 | Post window, fractions allowed |
| `mute_s` | 0 | Quiet window after a capture completes |

Determining t0:

- When the request carries `timestamp_ns` (`kFlagHasTimestamp`), that time is
  used; otherwise the server uses `CLOCK_MONOTONIC`.
- A supplied timestamp must pass boundary checks: it returns
  `kInvalidTimestamp` if it is more than `now + 5 s` (an internal `CenterConfig`
  constant, not a config key) or if `timestamp + before < now`. The former would
  push the post deadline far out and occupy the FSM for a long time; the latter
  would only yield a pre window full of holes, so it is explicitly rejected.

Timeline on the recorder side:

1. `Trigger()` records t0, snapshots the post cursor, then runs
   `HarvestWindow(t0 - before, t0)` to harvest the pre window.
2. It enters `kPost` and keeps collecting new samples/frames in
   `(t0, t0 + after]` from the cursor.
3. Only when `now >= post_deadline + post_grace_ns` **and** a complete scan
   finds no new data does it enter `kSettle`.

`post_grace_ns` defaults to 20 ms and is not configurable. It covers the window
where "the producer already has a mono timestamp but has not yet written the
sample into the ring" — a preempted thread on a loaded machine can stall there
for more than 1 ms, and the earlier 2 ms grace was too tight. Increasing it only
delays sealing; it does not make a package more complete.

The Settle phase drains at most `settle_batch = 256` descriptors per Tick, with
a wall-clock cap of `drain_tick_budget_ns = 5 ms`, so a long settle cannot
starve the trigger socket.

## Quota and priority

| Resource | Config | Ordinary priority | Crash level |
| --- | --- | --- | --- |
| Trigger count | `[quota.<class>].max_triggers_per_hour` (0 = unlimited) | Over limit: `kQuotaExceeded` | Overridable; audit records `quota_override=1` and `CenterStats::quota_overridden` increments |
| Pending memory | `[recorder].pending_bytes_quota` (default 64 MiB) | The center checks through the `pending_bytes` hook and rejects over limit | Calls `evict_oldest()` to evict the oldest low-priority data first, then admits |
| Disk | `[recorder].disk_quota_mb` + `high_reserved_mb` | Retention deletes the oldest low-priority packages | Writes to `high/`; low-priority cleanup does not intrude |

The trigger quota is a **sliding 1-hour window per `quota_class`**, accounted in
`Submit`, where GC also runs. `quota_class` must exist in `[quota.*]`; a typo is
rejected during `Init` instead of silently becoming "unlimited".

Priority rules:

- `[source.*].priority` is an **upper bound**. The requested `priority_hint` is
  first clamped to `[0, max_priority_hint]` and can only be lowered from there;
  no process with the same uid can grant itself a higher priority, which would
  otherwise penetrate mute, quota, and preemption rules.
- The clamp is written to the response (`kRespFlagPriorityClamped`,
  `effective_priority`) and the audit (`hint_raw` / `hint_eff` / `clamp`).
- `priority_hint < 0` means "use the source's configured priority".
- `crash_priority` defaults to 100: `priority >= 100` is crash level, with
  preemption, quota override, mute bypass, and the `high/` reserved area.

When the disk is full (`disk_full` latch set), non-crash triggers are rejected
directly as `kQuotaExceeded`; crash level still tries, backed by `high/`.

## Asynchronous results and notifications

Every finished capture pushes an outcome to a fixed 64-entry ring; `code` is:

| code | Meaning |
| --- | --- |
| `kCaptureSealed` | Sealed normally |
| `kPreemptedByHigher` | Truncated by a higher priority (response code 4) |
| `kStartFailed` | Enqueued but failed to start |
| `kPreemptFailed` | Preemption failed; the old capture was restored |
| `kCancelled` | Cancelled |

There are two ways to collect results:

- **Poll/immediate match**: `WaitOutcome(socket, trigger_id, timeout)` first
  checks the outcome ring and returns immediately on a hit; otherwise it
  registers a deferred waiter.
- **Deferred waiter**: 8 fixed slots, deadline set from `response_timeout_ms`,
  resolved by `Tick` and sent back to the client's reply path with `sendto`. The
  response carries `kRespFlagOutcomeNotification`; when the waiter table is full
  it returns `kBusy` immediately.
- After the deadline the client reports `kTransportTimeout` itself (the server
  never sends this code). `response_timeout_ms` ranges 1..60000 ms and the
  client times it with `CLOCK_MONOTONIC`.

Counters: `notify_outcomes`, `notify_outcome_drops` (ring overwrote the oldest),
`notify_delivered`, `notify_send_failures`, `notify_waiter_overflows`,
`notify_waiter_expired`, plus the cancellation side's `cancels_applied` /
`cancels_unknown` / `cancel_failures`. These are process-local and are not
exported to `/status`.

## Audit and FSM logs

### `.audit`

One `key=value` line per request, with a fixed field order:

```text
ts=<RFC3339> ts_ns=<u64> mono_ns=<u64> pid= uid= req=trigger|cancel|wait|preempt \
proto= seq= src= hint_raw= hint_eff= clamp= decision=accepted|rejected reason= \
code= trigger_id= quota_override= by=
```

| Field | Meaning |
| --- | --- |
| `ts` / `ts_ns` | Decision time (UTC / epoch ns) |
| `mono_ns` | `CLOCK_MONOTONIC` decision time, for alignment with `.fsm` and perf data |
| `pid` / `uid` | Peer credentials from kernel `SCM_CREDENTIALS` (0 for in-process direct calls) |
| `proto` / `seq` | Message version actually accepted / anti-replay sequence |
| `src` | Source name, escaped as `%HH` so it is always a single token |
| `hint_raw` / `hint_eff` / `clamp` | Requested hint, effective priority, whether it was clamped |
| `decision` | `accepted` / `rejected` / `preempted` |
| `reason` / `code` | snake_case reason and `TriggerCode` name, such as `quota_exceeded`, `preempted_by_higher` |
| `trigger_id` | The trigger this record refers to; 0 when the request never received an id |
| `quota_override` | 1 means this accept overrode the count quota only by being crash level |
| `by` | In a preemption execution record, the preemptor's trigger id; 0 otherwise |

The audit file must be writable, otherwise the center refuses to start (fail
closed). Write failures are only counted (`CenterStats::audit_drops`) and the
request still gets its normal decision. The file rotates by `audit_max_bytes`,
so **line counts are not a monotonic counter**.

### `.fsm`

JSONL, one line each:

```json
{"ts":1761205953123456789,"id":7,"from":1,"to":2,"why":5}
```

`ts` is mono ns, `from`/`to` are `State` values, and `why` is `FsmReason`. Fixed
256-entry ring, flushing pending events once per second, rotating to
`<socket>.fsm.1..N` by `[logging].fsm_max_bytes`. A crash loses at most one dump
period; parsers tolerate a truncated last line. Overwriting the oldest entry
counts as `fsm_event_drops`, but that counter is not in `/status`.

## trigger→seal SLO and measurement

**Definition**: the difference between two monotonic anchors of the same
`trigger_id`.

| Anchor | Location | Persistence |
| --- | --- | --- |
| start | FSM `kSubmitAccept` (`reason=0`), stamped inside `Submit` | `<socket>.fsm`; in-process `CopyFsmEvents()` |
| end | Full seal completion: all package files fsynced, WAL seal row written, manifest written, package directory fsynced | In-process `last_seal_mono_ns()` + `last_seal_trigger_id()` |
| durable | The seal row in `wal.log`; its `mono_ns` field is the journal commit time | In the package, paired with the WAL header's `trigger_id` |

Note two things that are not the SLO: `integrity.manifest`'s `seal_time_ns` is a
realtime clock (wall-clock evidence, not monotonic); the FSM's `kCompleted` is
when the center **observes** completion one Tick after sealing, a little later
than the true end.

**Measurement**: `bench_trigger_seal` runs the real Center → TriggeredRecorder
pipeline (one `RecordMirrorRingPage` struct plus a small `PinnedSlotWriter`
channel, `flush_realtime`, 20 ms pre / 50 ms post), fires N triggers, and prints
percentiles.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4 --target bench_trigger_seal
./build/bench_trigger_seal --iters 200 --json bench_trigger_seal.json
# or via ctest (RUN_SERIAL, 64 iterations, writes build/bench_trigger_seal.json)
ctest --test-dir build -R bench_trigger_seal --output-on-failure
```

The JSON contains `trigger_to_seal_ns: {n, min, p50, p90, p99, p999, max, mean}`.
ctest gates only the measurement pipeline (accept/seal pair up, stamps are
nonzero, packages are complete); it does not assert latency magnitude.

Reference values (x86_64, i5-12490F, Release, 64 iterations, 20+50 ms window,
reference only):

| Statistic | trigger→seal |
| --- | --- |
| min | 76.7 ms |
| p50 | 78.0 ms |
| p90 | 81.5 ms |
| p99 | 82.4 ms |
| max | 82.4 ms |

The structural floor is roughly `after_ns + post_grace_ns` (50 + 20 ms).
Subtracting the window floor, the seal path (post tail collection + drain +
fsync + manifest + dir sync) is about 12 ms p99 on packages of this kind (about
500 small samples + 70 1 KB frames).

SLO budget (p99, reference hardware):

- trigger→seal for `realtime` captures: `p99 <= after_ns + post_grace_ns + 50 ms`.
- The seal path beyond the window floor: `p99 <= 50 ms` while the package stays
  under the default `pending_bytes_quota` (64 MiB).

Captures with `flush_policy = idle` are **not** covered by the SLO: by design
their seal waits for the idle detector.

## End-to-end example

Goal: stand up a producer, fire one `manual` trigger, then fire a crash-level
trigger, and inspect the package, audit, FSM, and evidence chain.

```ini
# /tmp/thyme_trigger_demo.ini
[trigger_center]
socket = /tmp/thyme_trigger.sock
default_source = manual
crash_priority = 100

[recorder]
output_dir = /tmp/thyme_trigger_out
socket = /tmp/thyme_recorder.sock
flush_policy = realtime
disk_quota_mb = 512
high_reserved_mb = 128
pending_bytes_quota = 33554432

[security]
allow_root = false
require_peer_pid = true
max_priority_hint = 1000

[channel./thyme_recr_can]
kind = mirror_ring
name = can_chassis
rate_hz = 100
avg_frame_bytes = 16

[channel./h264_front]
kind = h264_stream
name = h264_front
bitrate_kbps = 4000

[source.manual]
channels = /thyme_recr_can, /h264_front
before_s = 3
after_s = 1
priority = 10
mute_s = 0
quota_class = manual

[source.crash]
channels = /thyme_recr_can, /h264_front
before_s = 5
after_s = 2
priority = 100
mute_s = 0
flush_policy = realtime
quota_class = safety

[quota.manual]
max_triggers_per_hour = 10

[quota.safety]
max_triggers_per_hour = 0
```

```bash
mkdir -p /tmp/thyme_trigger_out
build/tools/apps/trigger_center/trigger_center --check-config \
    --record-manifest /etc/thyme/record_manifest.h /tmp/thyme_trigger_demo.ini
build/tools/apps/trigger_center/trigger_center --dump-config \
    --record-manifest /etc/thyme/record_manifest.h /tmp/thyme_trigger_demo.ini

# Producer: mirror page /thyme_recr_can + big-frame channel /h264_front, 100 Hz
build/tools/apps/trigger_center/demo_producer \
    /thyme_recr_can /thyme_demo_ring /h264_front 100 &
PROD=$!

# Trigger center
build/tools/apps/trigger_center/trigger_center \
    --record-manifest /etc/thyme/record_manifest.h /tmp/thyme_trigger_demo.ini &
TC=$!
sleep 2

# First shot: manual, ordinary priority
build/tools/apps/trigger_center/triggerctl manual --json
# → {"source":"manual","socket":"/tmp/thyme_trigger.sock","code":"accepted",
#    "transport_ok":true,"trigger_id":1,"priority":10,"priority_clamped":false}

# Wait for the post window + settle
sleep 6
ls -1 /tmp/thyme_trigger_out
```

You should see `1_<t0>/`. The package should contain `meta.tbin`, `small.tbin`,
`frames/h264_front.raw`, `frames/h264_front.index.tbin`, `wal.log`,
`integrity.manifest`, and `schema.json`.

```bash
# Second shot: fire a crash while the manual capture is in progress
# (priority 100 > 10, so it triggers preemption)
build/tools/apps/trigger_center/triggerctl manual --json
sleep 1
build/tools/apps/trigger_center/triggerctl crash --priority 100 --json
# The crash-level package goes to the high/ reserved area; the preempted manual
# package is sealed truncated at what was written
sleep 6

ls -1 /tmp/thyme_trigger_out/high
tail -n 8 /tmp/thyme_trigger.sock.fsm
grep 'req=preempt' /tmp/thyme_trigger.sock.audit
grep 'decision=accepted' /tmp/thyme_trigger.sock.audit | tail -n 3
```

The FSM should show `why=0` (queued), `why=1` (started), and `why=5`
(completed); when preemption happens `why=3` appears, and bit0 of
`CaptureStatsRecord::flags` in the preempted package's `meta.tbin` (the `2_*`
example) is set. In the audit, the `req=preempt` line has the preempted trigger
in `trigger_id` and the preemptor in `by`.

```bash
# Per-package validation: seal state + evidence manifest
PKG=$(ls -d /tmp/thyme_trigger_out/1_* | head -1)
build/tools/apps/trigger_center/triggerctl verify "$PKG" --json
build/tools/replay/thyme-replay/thyme-replay verify "$PKG"
build/tools/replay/thyme-replay/thyme-replay integrity "$PKG" --key /etc/thyme/signing.key
build/tools/replay/thyme-replay/thyme-replay schema "$PKG"

# In-package statistics (drain counts, holes, evictions)
build/tools/replay/thyme-replay/thyme-replay info "$PKG" --json
```

Cancel and wait-result live in `trigger_client`, not in `triggerctl`'s command
table:

```text
// header-only API (illustrative)
Result r = trigger_client::Trigger(socket, "manual", /*hint=*/-1, /*timeout_ms=*/100);
Result c = trigger_client::Cancel(socket, r.trigger_id, 100);   // kCancelled / kUnknownTrigger / kBusy
Result o = trigger_client::WaitOutcome(socket, r.trigger_id, 2000); // outcome notification
```

`Cancel` on a running capture means "truncate + seal", not discarding what was
written; `WaitOutcome` timeouts are decided by the client, which reports
`kTransportTimeout` after the deadline — the server never sends it
retroactively.

## Related documentation

- [`acquisition.md`](acquisition.md) — continuous recording, slice rolling, and retention
- [`config-deploy.md`](config-deploy.md) — all config sections, hot reload, and systemd deployment
- [`perf.md`](perf.md) — `trigger_to_seal` baseline and metric export
- [`replay-cli.md`](replay-cli.md) — package structure validation and replay
