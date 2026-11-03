# Trigger / recorder configuration reference

On-disk format: the minimal INI dialect parsed by
`src/record/trigger_config.cpp` (`#` comments, one `key = value` per line,
comma-separated lists). It maps 1:1 to the YAML schema in
[`triggered-acquisition-design.md`](../design/triggered-acquisition-design.md) chapter 7.

**Every accepted key either takes effect or is a parse error.** There are no
accepted-but-ignored keys. Unknown keys/sections are errors with a `line N:`
prefix; `--check-config` exits non-zero on any error. A repeated
`[trigger_center]` key is a `duplicate trigger_center key` parse error (the
schema-backed slice; other sections are not duplicate-checked yet).
`--check-config` also fails with `config schema: [trigger_center] …` when the
parser's effective key set and the generated schema disagree.

## Field audit

Authoritative per-section key inventory, cross-checked against the parser by
`test_trigger_config_doc` (mr_gate): the test dumps the effective
configuration of a maximal config (`DumpConfig`), parses this table, and
asserts the two key sets match exactly — every implemented key below must
appear in the effective dump, and every key in the dump must be listed here.
`deprecated` keys must still parse (with a warning) but must not appear in the
dump; `rejected`/`future` keys must be parse errors. Adding a key to the
parser without updating this table (or `DumpConfig`) fails the gate.

**Generated `[trigger_center]` metadata.** The `[trigger_center]` table below is
generated from `tools/gen/config_schema/trigger_config.schema.json`
(section/key, status, reload class, type, default, owning contract). The same
schema emits `include/thyme/record/trigger_config_schema.h`, which
`--check-config` and `test_trigger_config_schema` use to cross-check the parser,
`DumpConfig`, the defaults and the SIGHUP matrix. A `--check` CTest fails when a
generated block is stale. The remaining sections in this table are still
hand-maintained; the schema rollout continues section by section.

Status legend: `implemented` = parsed and effective; `deprecated` = old
spelling still accepted with a warning; `rejected` = deliberate parse error
(never accepted-but-ignored); `future` = design-schema field with no parser
support yet (shown only to mark it; setting it is an unknown-key parse error).
Accepted-but-ignored keys: **0**.

<!-- DOC-08-AUDIT-BEGIN -->
| section | key | status | note |
| --- | --- | --- | --- |
| `security` | `allowed_uids` | implemented | uid allowlist (empty = center euid only) |
| `security` | `allow_root` | implemented | accept peer uid 0 |
| `security` | `require_peer_pid` | implemented | require kernel peer pid |
| `security` | `max_priority_hint` | implemented | request hint clamp bound |
| `security` | `allow_legacy_unsequenced` | implemented | accept V1 requests (downgrade, warned) |
| `security` | `allow_legacy_v2` | implemented | accept V2 requests (downgrade, warned) |
| `security` | `allow_legacy_control` | implemented | accept recorder V1 text commands (downgrade, warned) |
| `security` | `signing_key` | implemented | HMAC key file (startup-only) |
| `security` | `manifest` | implemented | `full`/`metadata`/`off` (default `full`, startup-only) |
| `security` | `audit_log` | implemented | trigger audit file path (startup-only) |
| `security` | `control_audit_log` | implemented | recorder-control audit file path (startup-only) |
| `security` | `audit_max_bytes` | implemented | audit rotation threshold (startup-only) |
| `security` | `audit_rotate_keep` | implemented | rotated files kept, 1..32 (startup-only) |
| `security` | `audit_fsync_interval_s` | implemented | audit fsync interval (startup-only) |
| `logging` | `service_file` | implemented | service log file; `stderr` disables the file sink (startup-only) |
| `logging` | `service_max_bytes` | implemented | service log rotation threshold (startup-only) |
| `logging` | `service_rotate_keep` | implemented | service log generations kept, 1..32 (startup-only) |
| `logging` | `fsm_max_bytes` | implemented | `<socket>.fsm` rotation threshold (startup-only) |
| `logging` | `fsm_rotate_keep` | implemented | `<socket>.fsm` generations kept, 1..32 (startup-only) |
| `logging` | `perf_max_bytes` | implemented | perf dump rotation threshold (startup-only) |
| `logging` | `perf_rotate_keep` | implemented | perf dump generations kept, 1..32 (startup-only) |
| `logging` | `total_budget_mb` | implemented | per-process log budget, `0` = per-file caps only (startup-only) |
| `recorder` | `output_dir` | implemented | package root (restart required) |
| `recorder` | `socket` | implemented | continuous-control socket, must differ from `[trigger_center].socket` (restart required) |
| `recorder` | `pending_bytes_quota` | implemented | recorder pending byte quota (restart required) |
| `recorder` | `flush_policy` | implemented | default flush for sources (restart required) |
| `recorder` | `idle_hold_s` | implemented | stationary hold before idle flush (restart required) |
| `recorder` | `idle_fallback` | implemented | `assume_idle`/`always_busy` (restart required) |
| `recorder` | `io_rate_limit_mbps` | implemented | drain rate limit (restart required) |
| `recorder` | `disk_quota_mb` | implemented | retention disk quota (restart required) |
| `recorder` | `high_reserved_mb` | implemented | high-priority reserved zone (restart required) |
| `recorder` | `incomplete_policy` | implemented | `.incomplete` policy (restart required) |
| `recorder` | `incomplete_max_age_s` | implemented | age bound for `delete_after_age` (restart required) |
| `recorder` | `use_cgroup` | rejected | cgroup v2 `io.max` is deployment-side; parse error |
| `recorder` | `high_priority.dir` | future | design schema only; only `reserved_mb` (`high_reserved_mb`) is implemented |
| `channel.*` | `kind` | implemented | `mirror_ring`/`pinned_slot`/`h264_stream` |
| `channel.*` | `name` | implemented | addressable channel name (default: shm name) |
| `channel.*` | `record_channels` | implemented | generated aggregate mirror's manifest structs (restart required) |
| `channel.*` | `rate_hz` | implemented | continuous rate accounting |
| `channel.*` | `avg_frame_bytes` | implemented | continuous rate accounting |
| `channel.*` | `bitrate_kbps` | implemented | `h264_stream` rate accounting |
| `channel.*` | `crc` | implemented | `package`/`frame`/`off` |
| `channel.*` | `manifest` | implemented | bulk-only `full`/`metadata`/`off` override |
| `channel.*` | `gop_s` | implemented | h264 trigger-window IDR slack; `0` disables; `1..60` only on `h264_stream` |
| `channel.*` | `window_cap_s` | future | design schema only; per-channel window cap not implemented |
| `channel.*` | `format` | future | design schema only (e.g. `nv12`); not parsed |
| `channel.*` | `slot_bytes` | future | design schema only; transport geometry comes from contracts |
| `channel.*` | `enable` | future | design schema only; per-channel on/off not implemented |
| `source.*` | `channels` | implemented | per-source channel filter |
| `source.*` | `before_s` | implemented | pre-roll |
| `source.*` | `after_s` | implemented | post-roll |
| `source.*` | `mute_s` | implemented | mute window |
| `source.*` | `priority` | implemented | base priority |
| `source.*` | `flush_policy` | implemented | overrides `[recorder].flush_policy` |
| `source.*` | `quota_class` | implemented | required; names a `[quota.*]` class |
| `source.*` | `name` | rejected | the name comes from the `[source.<name>]` header |
| `quota.*` | `max_triggers_per_hour` | implemented | 0 = unlimited |
| `chassis` | `page_shm` | implemented | chassis mirror page (required if `[chassis]` present) |
| `chassis` | `struct_id` | implemented | page struct id, 0..63 |
| `chassis` | `epb_offset` | implemented | EPB field offset |
| `chassis` | `speed_offset` | implemented | speed field offset |
| `chassis` | `speed_threshold` | implemented | stationary speed threshold |
| `continuous` | `auto_arm` | implemented | arm continuous capture at startup only |
| `continuous` | `channels` | implemented | whitelist/exclusion/decimate grammar (default `small_struct:*`) |
| `continuous` | `slice_mb` | implemented | slice size; `0 < slice_mb <= disk_quota_mb` |
| `continuous` | `slice_s` | implemented | time-based roll; 0 = size only |
| `continuous` | `wait_idle` | implemented | gate drain on idle detector; needs `[chassis]` or `demo_idle` |
| `continuous` | `demo_idle` | implemented | explicit demo-predicate opt-in |
| `continuous` | `rate_check` | implemented | `enforce`/`warn`/`off`, runs on every Arm |
| `continuous` | `enable` | deprecated | old spelling of `auto_arm`; accepted with a warning |

<!-- TRIGGER-CONFIG-SCHEMA-BEGIN
| section | key | status | reload | type | default | contract | note |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `trigger_center` | `socket` | implemented | startup-only (kept silently) | `path` | `/tmp/thyme_trigger.sock` | TRG-17 | trigger datagram socket path (startup-only) |
| `trigger_center` | `max_concurrent_captures` | implemented | fixed at startup | `uint32` | `1` | TRG-11 | only `1` supported; `> 1` is a parse error |
| `trigger_center` | `tick_ms` | implemented | **restart required** | `uint32` | `1` | TRG-19 | host Tick period and socket queue sizing (startup-only) |
| `trigger_center` | `socket_poll_budget_us` | implemented | **restart required** | `uint64` | `200` | TRG-19 | per-Tick receive time budget (startup-only) |
| `trigger_center` | `max_trigger_rate_hz` | implemented | **restart required** | `uint32` | `10000` | TRG-19 | maximum trigger-rate sizing input (startup-only) |
| `trigger_center` | `dedup_window_entries` | implemented | **restart required** | `uint32` | `1024` | TRG-19 | V4 central dedup capacity (startup-only) |
| `trigger_center` | `dedup_window_ms` | implemented | **restart required** | `uint64` | `60000` | TRG-19 | V4 cached-result retention (startup-only) |
| `trigger_center` | `quota_state` | implemented | **restart required** | `enum` | `none` | TRG-04 | durable one-hour quota windows; `none` = process-local (startup-only) |
| `trigger_center` | `default_source` | implemented | hot | `source-name` | `<unset: unknown sources are rejected>` | TRG-17 | fallback rule for unknown sources |
| `trigger_center` | `allow_empty_sources` | implemented | hot | `bool` | `false` | TRG-17 | allow a center with no `[source.*]` |
| `trigger_center` | `crash_priority` | implemented | hot | `int32` | `100` | TRG-12 | crash-level threshold (hot-reloadable) |
| `trigger_center` | `pending_memory_mb` | rejected | n/a (parse error) | `rejected` | `—` | TRG-17 | removed; use `[recorder].pending_bytes_quota` |
<!-- TRIGGER-CONFIG-SCHEMA-END -->
<!-- DOC-08-AUDIT-END -->

Totals: **72 implemented, 1 deprecated, 4 rejected, 5 future, 0 ignored.**

Canonical minimal template: `tools/apps/trigger_center/trigger_config.minimal.ini`.
The standalone host always requires an explicit record manifest. Generated
mirror-only deployments use `generated/record_manifest.h`; legacy PinnedSlot/H264
configs still need an external manifest until those sources are modeled in graph:

```
tools/apps/trigger_center --check-config --record-manifest /etc/thyme/record_manifest.h \
  tools/apps/trigger_center/trigger_config.minimal.ini
tools/apps/trigger_center --dump-config --record-manifest /etc/thyme/record_manifest.h \
  tools/apps/trigger_center/trigger_config.minimal.ini
```

`--dump-config` prints the **effective** configuration: defaults are
materialised, `[source.*].channels` shows the effective channel set, and keys
that are deliberately not configurable (`use_cgroup`) never appear.
`gop_s` is materialised (`0` when unset).
Missing rate metadata stays missing rather than being rendered as zero. The
dump parses back to an equivalent config.

### Generated base + deployment policy

Every configuration command also accepts two files instead of the legacy single
file. The generated base comes first and the deployment policy second:

```bash
tools/apps/trigger_center --check-config --record-manifest generated/record_manifest.h \
  generated/trigger.base.ini trigger.policy.ini
tools/apps/trigger_center --dump-config --record-manifest generated/record_manifest.h \
  generated/trigger.base.ini trigger.policy.ini
tools/apps/trigger_center --check-resources --record-manifest generated/record_manifest.h \
  generated/trigger.base.ini trigger.policy.ini --json
# Normal startup uses the same manifest and two positional files.
tools/apps/trigger_center --record-manifest generated/record_manifest.h \
  generated/trigger.base.ini trigger.policy.ini
```

The base may contain `[channel.*]` and `[chassis]` sections. The policy
contains the recorder, trigger, source/quota, security, logging and continuous
settings; it must not contain `[channel.*]` or `[chassis]`. This is a fixed two-file
composition, not a general include mechanism. The combined configuration goes
through the same defaults and semantic validation as the legacy format.

A generated aggregate mirror uses `record_channels` to associate all its structs
with the manifest. The explicit manifest, not the SHM filename, is the selector
subset authority. An empty generated manifest is a strict empty topology:
select-all or other recording selectors are rejected, not treated as “manifest
checking disabled”. SIGHUP reloads both config files and revalidates them against
the same manifest used at startup.

## `[trigger_center]`

| Key | Default | Effect |
| --- | --- | --- |
| `socket` | `/tmp/thyme_trigger.sock` | Trigger datagram socket path. |
| `max_concurrent_captures` | `1` | Only `1` is implemented. `> 1` is a **parse error** (the FSM cannot run captures in parallel). |
| `tick_ms` | `1` | Host Tick period, `1..1000` ms. Also sizes the socket queue. |
| `socket_poll_budget_us` | `200` | Drain until `EAGAIN`, this time budget, or the fixed 4096-datagram safety cap, whichever comes first. |
| `max_trigger_rate_hz` | `10000` | Maximum trigger-rate sizing input, `1..100000`; this is not a rate limiter. |
| `dedup_window_entries` | `1024` | Central V4 result-cache capacity, `1..16384`; oldest entry is evicted when full. |
| `dedup_window_ms` | `60000` | V4 cached-result retention, `1..600000` ms. Hits do not extend expiry. |
| `quota_state` | `none` (process-local) | Fixed-record state file for limited quota classes. Restores the one-hour window after process restart, atomically compacts expired records at startup, and rejects startup/admission on state corruption or I/O failure. No power-loss durability guarantee. |
| `default_source` | unset (unknown sources rejected) | Fallback rule for unknown source names. Must name a `[source.*]`. |
| `allow_empty_sources` | `false` | Allow a center with no `[source.*]` (read-only/test center). A continuous-only config is exempt automatically. |
| `crash_priority` | `100` | Priority at or above this is crash level (preempt/quota override/mute bypass). |

With `quota_state` enabled, each admission to a limited class is appended before
the trigger enters the FSM. A write failure returns `kRecorderOffline`, increments
`quota_state_errors`, and does not consume a trigger ID or advance the in-memory
window. A crash between the state append and admission can conservatively consume
one quota slot. State CRC detects accidental corruption; it is not a signature
against a user who can already modify the file. The sidecar `.lock` prevents two
centers from sharing one state file. Records survive process crash/restart, but
there is no per-record `fdatasync`, so power-loss durability is not guaranteed.

Pending memory has one owner: `[recorder].pending_bytes_quota` (default 64 MiB)
sizes the recorder rings and bounds its pending queue. Center reads live pending
bytes through the `pending_bytes` hook, rejects ordinary requests over the same
quota, and lets crash-level requests evict oldest pending data before proceeding.
The legacy `[trigger_center].pending_memory_mb` key is a parse error with migration
guidance; it is not an alias and never appears in the effective dump.

Socket sizing uses `N = ceil(max_trigger_rate_hz * tick_ms / 1000)` and
requests `SO_RCVBUF = N * 120` bytes (the V4 request size). Linux may round or
double the value and datagram bookkeeping also has kernel overhead. The queue
**count** must independently satisfy `net.unix.max_dgram_qlen >= N`; a 100-entry
single-Tick burst needs at least 100. `--check-resources` reads the sysctl and
warns when it is too small or unreadable, without adding a fatal gate. A typical
setting for the default rate plus burst headroom is:

```bash
sysctl -w net.unix.max_dgram_qlen=512
# Persist the deployment-specific value in /etc/sysctl.d/*.conf.
```

`socket_backlog_events` increments only when the time/safety budget stops the
poll and `MSG_PEEK` confirms another datagram remains. Exactly filling a budget
is not itself backlog; reaching `EAGAIN` is the normal drained path.

## `[security]`

Control-surface authentication, authorization, anti-replay, priority clamp,
audit, evidence-package signing and the control-message E2E profile. Peer
identity comes from kernel `SCM_CREDENTIALS` (`SO_PASSCRED` on the datagram
socket — the datagram equivalent of `SO_PEERCRED`): the sender cannot forge its
uid/pid. A rejected peer gets **no reply** (it cannot probe the protocol); the
decision is still audited. Threat model and residual risks:
[`security/threat-model.md`](../security/threat-model.md); package manifests:
[`security/evidence-integrity.md`](../security/evidence-integrity.md).

<!-- TRIGGER-CONFIG-SCHEMA-SECURITY-BEGIN
| section | key | status | reload | type | default | contract | note |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `security` | `allowed_uids` | implemented | hot | `string` | `<unset: only the center's euid>` | SEC-02 | comma-separated allowlist; a non-empty list replaces the default |
| `security` | `allow_root` | implemented | hot | `bool` | `false` | SEC-02 | also accept peer uid 0 |
| `security` | `require_peer_pid` | implemented | hot | `bool` | `true` | SEC-02 | reject datagrams without a kernel-supplied peer pid |
| `security` | `max_priority_hint` | implemented | hot | `int32` | `1000` | SEC-02 | priority_hint clamp upper bound (may only lower a rule priority) |
| `security` | `allow_legacy_unsequenced` | implemented | hot | `bool` | `false` | SEC-02 | accept 88-byte V1 requests (no replay protection) |
| `security` | `allow_legacy_v2` | implemented | hot | `bool` | `false` | SEC-04 | accept 96-byte V2 requests (no CRC) |
| `security` | `allow_legacy_control` | implemented | hot | `bool` | `false` | SEC-10 | accept V1 recorder text commands (default deny) |
| `security` | `signing_key` | implemented | startup-only (kept silently) | `path` | `<unset = unsigned>` | SEC-03 | HMAC-SHA256 key for evidence manifests; unreadable key fails closed |
| `security` | `manifest` | implemented | **restart required** | `enum` | `full` | REC-23 | evidence scope: full/metadata/off |
| `security` | `audit_log` | implemented | startup-only (kept silently) | `path` | `<trigger socket>.audit` | SEC-02 | append-only trigger audit path; must differ from both sockets |
| `security` | `control_audit_log` | implemented | startup-only (kept silently) | `path` | `<recorder socket>.audit` | SEC-10 | recorder-control audit path, independent of audit_log |
| `security` | `audit_max_bytes` | implemented | startup-only (kept silently) | `uint64` | `4194304` | SEC-02 | audit rotation threshold (must be > 0) |
| `security` | `audit_rotate_keep` | implemented | startup-only (kept silently) | `uint32` | `2` | SEC-02 | rotated audit generations kept (1..32) |
| `security` | `audit_fsync_interval_s` | implemented | startup-only (kept silently) | `uint64` | `0` | SEC-02 | 0 = fdatasync only at shutdown; otherwise at most once per interval |
<!-- TRIGGER-CONFIG-SCHEMA-SECURITY-END -->

A `[channel.*] manifest=...` override is valid only on bulk `pinned_slot` /
`h264_stream` channels; `mirror_ring` small-struct/control channels reject
`metadata` and `off`. Overrides affect only that channel's `.raw`; different
bulk scopes are reported as `mixed`. Non-full startup records have
`req=config decision=configured reason=integrity_downgrade integrity_mode=...`
in `<socket>.audit`. `thyme-replay integrity` prints `OK(mode)` plus
`UNVERIFIED(payload)` instead of a bare `OK`; recorder status JSON/text and the
`thyme_recorder_integrity_mode` metric expose the same mode.

The audit file must be writable when the center starts; otherwise the center
refuses to start (fail closed). A slow/full disk cannot stall the control loop
beyond one small write: a failed write is counted (`CenterStats::audit_drops`)
and the request still gets its normal decision. `allowed_uids`, `allow_root`,
`require_peer_pid`, `max_priority_hint`, `allow_legacy_unsequenced`,
`allow_legacy_v2` and `allow_legacy_control` are hot-reloadable (SIGHUP);
`manifest`, `audit_log`, `control_audit_log`, the
rotation/sync settings and the `signing_key` path are startup-only (the
recorder is initialised with them; changing the path needs a restart, while
rotating the key file's contents is picked up by the next seal).

Control-message profile: V3 and V4 requests/responses carry a
CRC32 (IEEE) over the bytes before the `crc32` field. CRC is checked before
replay/dedup and priority handling; a bad CRC is audited as `bad_crc` and gets
no reply. `response_timeout_ms` (`1..60000`) is the total client deadline.
`trigger_client` sets `SO_SNDTIMEO` and makes at most three attempts within
that total budget. Timeout means **result unknown**, not failure to execute.

V4 adds nonzero `request_id`. A retry reuses the identical datagram, sequence
and request id; the central bounded window returns the first result with
`kRespFlagDeduplicated` and never executes a second trigger. Reusing an id for
different bytes is `request_id_conflict` / `kReplayRejected`. Once an entry
expires or is evicted, the existing sequence window remains the fail-safe.
Compatibility is deliberately not silently downgraded:

| Version | Server policy | Retry semantics |
| --- | --- | --- |
| V1 (88 B) | Disabled unless `allow_legacy_unsequenced=true`; no sequence/CRC | No idempotency or replay protection; do not retry automatically |
| V2 (96 B) | Disabled unless `allow_legacy_v2=true`; sequence, no CRC | Duplicate sequence is rejected, not deduplicated |
| V3 (104 B) | Accepted; sequence + CRC + deadline | Duplicate sequence is `kReplayRejected`; no result cache |
| V4 (120 B) | Accepted; V3 protections + `request_id` | Bounded identical retries return the cached result |

V4 responses are 48 B. An old server that does not recognize V4 normally stays
silent, so the client reports result-unknown after its bounded attempts.

The recorder-control socket uses a separate V2 layout (24-byte request/response
headers, with a bounded status payload). Its CRC covers the header prefix and
payload; its 64-entry sequence window is keyed by kernel `(uid,pid)`. Bad CRC,
malformed and auth failures get no V2 reply; replay and disabled-V1 requests
get explicit result codes. `allowed_uids`, `allow_root` and `require_peer_pid`
apply to both sockets. Recorder-control decisions use the same audit record
format in `control_audit_log`, not the trigger socket's command namespace.

Audit line format (one record per request, `key=value` tokens):

```
ts=2026-10-23T09:12:33.123456789Z ts_ns=1761205953123456789 mono_ns=... \
pid=4242 uid=1000 req=trigger proto=4 seq=1761205953123456000 \
request_id=1761205953123456001 retry=1 dedup=1 \
src=manual hint_raw=80 hint_eff=50 clamp=1 decision=deduplicated reason=dedup_hit \
code=accepted trigger_id=7 quota_override=0 by=0
```

`req` is the operation: `trigger` (submit), `cancel` (cancel a trigger),
`wait` (wait for an outcome — both the registration and the later delivery are
recorded), or `preempt` (see below). `src` is escaped as `%HH`, so a
hostile source string stays one token. Field reference:

| Field | Meaning |
| --- | --- |
| `ts` / `ts_ns` | Wall-clock decision time (ISO-8601 UTC / epoch ns). |
| `mono_ns` | `CLOCK_MONOTONIC` decision time (correlates with `.fsm` and perf logs). |
| `pid` / `uid` | Kernel-supplied peer credentials (`SCM_CREDENTIALS`); `0`/`-1`-class values only for direct in-process submissions. |
| `proto` / `seq` | Request wire version actually accepted (0 when the layout was not identified) / anti-replay sequence. |
| `request_id` / `retry` / `dedup` | V4 logical request id; duplicate seen; cached result returned. First/legacy requests are all zero. A conflict has `retry=1 dedup=0`. |
| `hint_raw` / `hint_eff` / `clamp` | Requested priority hint, priority actually used, and whether a clamp adjusted it. |
| `decision` | `accepted`, `rejected`, `deduplicated`, or `preempted` (preempt records only). |
| `reason` / `code` | Snake_case reason and the `TriggerCode` name (e.g. `quota_exceeded`, `preempted_by_higher`). |
| `trigger_id` | The trigger this record is about (0 when the request never got one). |
| `quota_override` | `1` when this accept only passed because crash-level priority overrode the per-class count quota (the same event `CenterStats::quota_overridden` counts). |
| `by` | On a `preempt` record, the preempting trigger's id; `0` otherwise. |

Preempt record: when a decided preemption actually executes (the old
capture is truncated+sealed — the point of no return), the center appends a
`req=preempt decision=preempted reason=preempted_by_higher` record whose
`trigger_id` is the **preempted** capture and `by` the preempting trigger;
`pid`/`uid`/`src` carry the preempting request's attribution (`0`/`0`/`-` for
a direct in-process `Submit`, which has no peer credentials). A preemption
that fails to execute rolls back and emits no preempt record (the FSM log
records `kPreemptFailed` instead). The audit file is separate from the
`<socket>.fsm` JSONL ring on purpose: the FSM ring is a bounded 256-entry
drop-oldest observability buffer (and its file is size-rotated, so old events
expire), while the audit trail must not lose decisions silently — it rotates
by size and counts any record it could not write
(`CenterStats::audit_drops`).

## `[logging]`

Rotation and disk budget for the append-only **non-package** files. Packages
are bounded separately by `[recorder].disk_quota_mb`; this section
never touches them.

<!-- TRIGGER-CONFIG-SCHEMA-LOGGING-BEGIN
| section | key | status | reload | type | default | contract | note |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `logging` | `service_file` | implemented | **restart required** | `path` | `stderr` | OPS-05 | service log destination; a path enables the file sink |
| `logging` | `service_max_bytes` | implemented | **restart required** | `uint64` | `8388608` | OPS-05 | service log rotation threshold (must be > 0) |
| `logging` | `service_rotate_keep` | implemented | **restart required** | `uint32` | `2` | OPS-05 | rotated service log generations kept (1..32) |
| `logging` | `fsm_max_bytes` | implemented | **restart required** | `uint64` | `4194304` | OPS-05 | rotation threshold for <socket>.fsm (must be > 0) |
| `logging` | `fsm_rotate_keep` | implemented | **restart required** | `uint32` | `2` | OPS-05 | rotated FSM generations kept (1..32) |
| `logging` | `perf_max_bytes` | implemented | **restart required** | `uint64` | `67108864` | OPS-05 | rotation threshold for each PerfLogDumper file (must be > 0) |
| `logging` | `perf_rotate_keep` | implemented | **restart required** | `uint32` | `2` | OPS-05 | rotated perf generations kept (1..32) |
| `logging` | `total_budget_mb` | implemented | **restart required** | `uint64` | `0` | OPS-05 | per-process cap over service+audit+FSM+perf; 0 disables the aggregate check |
<!-- TRIGGER-CONFIG-SCHEMA-LOGGING-END -->

All eight are startup-only (the writers are open and their rotation paths are
fixed): SIGHUP keeps the running values and warns. The audit keys stay in
`[security]` for backward compatibility but are counted in the same budget.
The trigger center applies `fsm_*` to `<socket>.fsm` directly; a host that
writes binary perf dumps passes `perf_*` when it wires the dumper —
`PerfLogDumper::Open(path, interval_ns, PerfDumpRotation{perf_max_bytes,
perf_rotate_keep})` (the default `Open` overload uses the same defaults).

### Rotation contract

All four files use the same writer (`include/thyme/runtime/rotating_file.h`):
`O_APPEND`, exactly **one `write()` per record** and no user-space buffer. The
size check is one integer comparison on the commit path; rotation is
rename-based (`<file>` → `<file>.1`, oldest `<file>.N` unlinked first) and
happens *before* the oversized record, so a file overshoots its cap by at most
one record (one perf dump block for perf files). A crash can leave a partial
last line/block — parsers stop there — but never interleaves records. Each
perf generation is a complete perf-dump file (it carries the magic), so
`thyme_log_merge` takes the live path and its `.1…N` siblings as independent
inputs:

```bash
thyme_log_merge /var/log/thyme/perf_node_sensor_*.bin.1 \
                /var/log/thyme/perf_node_sensor_*.bin --hist
```

Rotation is never a place to lose audit data: if the rename fails the live
file keeps growing (the record is still appended). The FSM writer is
best-effort observability and drops (counted in `CenterStats::fsm_write_drops`)
only when the process budget is exhausted.

### Disk upper bound (be honest about the arithmetic)

Per file the hard bound is `max_bytes × (keep + 1)` plus one record. The
shipped defaults therefore bound one trigger-center process at:

| writer | default cap × generations | worst case |
| --- | --- | --- |
| `service_file` (when enabled) | 8 MiB × (2+1) | ~24 MiB |
| `<socket>.audit` | 4 MiB × (2+1) | ~12 MiB |
| `<socket>.fsm` | 4 MiB × (2+1) | ~12 MiB |
| each perf dump file | 64 MiB × (2+1) | ~192 MiB |

`total_budget_mb` is a **per-process** belt over those per-file caps: writers
register their live + retained bytes in `LogBudget`; once the budget is
reached, non-mandatory writers (service, FSM, perf) drop and count records
(`thyme::log::records_dropped()`, `CenterStats::fsm_write_drops`,
`LogBudget::drops()`) while the audit trail keeps writing (never trade
auditability for disk). It is not a global filesystem quota: MP role processes
each enforce their own share, so the fleet bound is the per-process
caps/budget summed over the running processes. Pair it with a
filesystem/package quota (`[recorder].disk_quota_mb` + the
`thyme_filesystem_*` metrics) for a whole-disk guarantee.

Status surface: MP `/status` carries
`logging:{budget_limit_bytes,budget_used_bytes,budget_drops,rotations}` for the
process that serves the endpoint. Recorder `status-json` carries
`logging:{service_sink_open,service_records_written,service_records_suppressed,service_records_dropped,service_records_truncated,service_rotations}`;
the exporter maps these to `thyme_service_log_*` and alerts on drops.

## Cancel / outcome notification (no new config keys)

`Cancel(trigger_id)` and the async outcome notification are protocol-level
features of the existing trigger socket; **no configuration keys are added**.
Cancel goes through the same `[security]` allowlist / `SCM_CREDENTIALS`
gauntlet as a trigger and is audited (`req=cancel`). Cancelling a pending
(accepted, not yet started) trigger drops it and rolls the FSM back; cancelling
a running capture is a preempt-to-idle through the recorder's preempt hook
(already-written data is truncated+sealed, the source's `mute_s` still
applies); an unknown or completed id is answered `kUnknownTrigger`. Capture
outcomes (`kCaptureSealed`, `kPreemptedByHigher` — response code 4 —
`kStartFailed`, `kPreemptFailed`, `kCancelled`) are pushed into a bounded
64-entry ring and delivered to at most 8 deferred waiters; overflow and expiry
are counted in `CenterStats` (`cancels_applied`, `cancels_unknown`,
`cancel_failures`, `notify_outcome_drops`, `notify_delivered`,
`notify_waiter_overflows`, `notify_waiter_expired`). Wire details:
[`abi-compatibility.md`](../reference/abi-compatibility.md).

Storm/overflow observability: `socket_datagrams` counts every
received datagram. `socket_backlog_events` counts a time/safety-limited poll
that still has queued data, never an exact-budget false positive. V4 also
exposes `dedup_hits`, `dedup_conflicts` and `retry_requests`. Malformed
datagrams (bad size, magic, version, reserved bits, CRC, timeout range, or
contradictory opcode flags) are counted in `malformed_datagrams`, audited, and
never answered.

## `[recorder]`

<!-- TRIGGER-CONFIG-SCHEMA-RECORDER-BEGIN
| section | key | status | reload | type | default | contract | note |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `recorder` | `output_dir` | implemented | **restart required** | `path` | `<unset>` | OPS-13 | package root; required by the host |
| `recorder` | `socket` | implemented | **restart required** | `path` | `/tmp/thyme_recorder.sock` | TRG-15 | continuous-control (Arm/Disarm/Status) socket |
| `recorder` | `pending_bytes_quota` | implemented | **restart required** | `uint64` | `67108864` | TRG-10 | pending queue byte quota; also sizes the descriptor rings |
| `recorder` | `flush_policy` | implemented | **restart required** | `enum` | `idle` | TRG-17 | default for sources without an explicit flush_policy |
| `recorder` | `idle_hold_s` | implemented | **restart required** | `uint64` | `30` | TRG-17 | vehicle-stationary hold before an idle flush (fractional allowed) |
| `recorder` | `idle_fallback` | implemented | **restart required** | `enum` | `always_busy` | TRG-17 | behaviour when the chassis mirror is unreadable |
| `recorder` | `io_rate_limit_mbps` | implemented | **restart required** | `uint64` | `20` | REC-10 | drain rate limit for idle-flushed captures (decimal MB/s) |
| `recorder` | `use_cgroup` | rejected | n/a (parse error) | `rejected` | `—` | OPS-04 | cgroup v2 io.max is deployment-side; setting it is a parse error |
| `recorder` | `disk_quota_mb` | implemented | **restart required** | `uint64` | `0` | REC-10 | retention disk quota (0 = unlimited) |
| `recorder` | `high_reserved_mb` | implemented | **restart required** | `uint64` | `0` | REC-10 | reserved high-priority zone never reclaimed by low-priority retention |
| `recorder` | `incomplete_policy` | implemented | **restart required** | `enum` | `keep` | REC-10 | retention action for .incomplete packages |
| `recorder` | `incomplete_max_age_s` | implemented | **restart required** | `uint64` | `0` | REC-10 | age threshold for delete_after_age (fractional allowed) |
<!-- TRIGGER-CONFIG-SCHEMA-RECORDER-END -->

Recorder operations (`arm`/`disarm`/`status`/`verify`), their flags, exit
codes and the `status --json` schema are documented in
[recorder-cli.md](recorder-cli.md).

## `[channel.<shm_name>]`

One entry per recorded channel; this is the recorder manifest.

**Manifest subset contract.** `graph.yaml` is the topology source of
truth: an edge with `record: false` (and every channel without any recorded
edge) is absent from the generated `RecordManifest`
(`generated/record_manifest.h`, ordered by channel name). A deployment config
may only describe channels in that manifest. The host that owns the generated
table passes the generated arrays to
`thyme::trigger_cfg::ValidateManifestSubset(cfg, kRecordManifestChannels,
kRecordManifestCount, &err)` after parsing; any `[channel.*]` whose `name` (or
shm suffix with the leading `/` stripped) is not a manifest channel is rejected
at startup with
`channel '<name>' is not recorded in the graph record manifest`. An empty
manifest disables the check (standalone trigger_center without codegen).
Source/continuous selector globs are validated against the configured
`[channel.*]` set and against the manifest.

`gop_s` is the h264 trigger-window slack in whole seconds (`0..60`, default
`0`). `0` keeps `[t0-before, t0+after]`. A positive value is valid only on
`kind = h264_stream`: the pre window starts at the last IDR at or before
`t0-before`, and no earlier than `gop_s`; the post window keeps frames after
`t0+after` until the next IDR and does not include that IDR. If no IDR arrives
within `gop_s`, the cut stays at the slack bound. Continuous slice rolling
still uses its own IDR wait (`gop_roll_timeout_ns`), not this key.

<!-- TRIGGER-CONFIG-SCHEMA-CHANNEL-BEGIN
| section | key | status | reload | type | default | contract | note |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `channel.*` | `kind` | implemented | **restart required** | `enum` | `mirror_ring` | REC-19 | mirror_ring (small structs), pinned_slot, h264_stream |
| `channel.*` | `name` | implemented | **restart required** | `string` | `<shm name>` | TRG-16 | addressable channel name used by source/continuous selectors |
| `channel.*` | `record_channels` | implemented | **restart required** | `string` | `<unset: generated aggregate>` | GEN-12 | record-manifest structs aggregated into one mirror source (normally generated) |
| `channel.*` | `rate_hz` | implemented | **restart required** | `uint64` | `0` | TRG-10 | nominal sample rate for continuous rate accounting |
| `channel.*` | `avg_frame_bytes` | implemented | **restart required** | `uint64` | `0` | TRG-10 | average payload for continuous rate accounting |
| `channel.*` | `bitrate_kbps` | implemented | **restart required** | `uint64` | `0` | TRG-10 | encoded bitrate for h264_stream rate accounting |
| `channel.*` | `crc` | implemented | **restart required** | `enum` | `<kind default>` | REC-21 | package/frame/off per-channel CRC policy |
| `channel.*` | `manifest` | implemented | **restart required** | `enum` | `<inherit [security]>` | REC-23 | bulk .raw override: full/metadata/off |
| `channel.*` | `profile` | implemented | **restart required** | `string` | `none` | REC-27 | opaque big-frame payload decoder id; none = unset; mirror_ring rejects |
| `channel.*` | `profile_version` | implemented | **restart required** | `uint32` | `0` | REC-27 | payload profile version; requires profile |
| `channel.*` | `profile_params` | implemented | **restart required** | `string` | `none` | REC-27 | opaque parameter string hashed into params_digest; none = empty |
| `channel.*` | `gop_s` | implemented | **restart required** | `uint64` | `0` | OPS-13 | h264 trigger window IDR slack in seconds; 0 disables; 1..60 only on h264_stream |
<!-- TRIGGER-CONFIG-SCHEMA-CHANNEL-END -->


### CRC policy (`crc = package|frame|off`)

| Policy | PinnedSlot drain | frame index `payload_crc` | Integrity boundary |
| --- | --- | --- | --- |
| `frame` | per-frame transport CRC verified (`Acquire(verify_crc=true)`); a mismatch degrades the frame to a GAP | recorded (transport CRC, or recomputed when the slot carries none) | per-frame CRC + package manifest |
| `package` | per-frame verification skipped (`verify_crc=false`) | recomputed at drain, so `thyme-replay verify` can re-check on demand | **package `integrity.manifest`** |
| `off` | per-frame verification skipped | `0` (no per-frame anchor; `verify` counts frames as unchecked) | package manifest only |

Defaults by channel kind:

| `kind` | default `crc` | rationale |
| --- | --- | --- |
| `pinned_slot` / `h264_stream` (point-cloud/image/H264 bulk) | `package` | per-frame CRC off on the transport too (`PinnedSlotWriter::Create` now leaves `kPinnedSlotFlagCrcEnabled` clear; `SetCrcEnabled(true)` opts back in); the integrity manifest is the boundary |
| `mirror_ring` (small structs / control `Frame<T>`) | `frame` | per-record CRC kept (`.tbin` rows always carry it); producers may additionally seal the transport `Frame<T>` without CRC (`reserved[0]` bit0 + `crc32=0`) |

`crc = package` on a `mirror_ring` channel is a **parse error** (small-struct
`.tbin` rows always carry per-record CRC, so `package` would be a silent
no-op). Old packages written before the CRC-disable flag existed (no
`reserved[0]` flag) verify exactly as before: `crc32 == 0` without the flag is
treated as a real CRC value, never as "CRC disabled".

## `[source.<name>]`

<!-- TRIGGER-CONFIG-SCHEMA-SOURCE-BEGIN
| section | key | status | reload | type | default | contract | note |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `source.*` | `channels` | implemented | hot | `string` | `<unset: all channels>` | TRG-16 | per-source channel selector list (glob/exclusion/decimate grammar) |
| `source.*` | `before_s` | implemented | hot | `uint64` | `0` | TRG-10 | pre-roll (fractions allowed) |
| `source.*` | `after_s` | implemented | hot | `uint64` | `0` | TRG-10 | post-roll (fractions allowed) |
| `source.*` | `priority` | implemented | hot | `int32` | `0` | TRG-12 | base priority; a submitter hint may only lower it |
| `source.*` | `mute_s` | implemented | hot | `uint64` | `0` | TRG-10 | mute window after the capture completes |
| `source.*` | `flush_policy` | implemented | hot | `enum` | `<inherit [recorder].flush_policy>` | TRG-17 | idle or realtime; an explicit value overrides [recorder] |
| `source.*` | `quota_class` | implemented | hot | `string` | `<required: must match a [quota.*] class>` | TRG-04 | quota class; must exist at center start |
| `source.*` | `name` | rejected | n/a (parse error) | `rejected` | `—` | TRG-17 | the name comes from the [source.<name>] section header |
<!-- TRIGGER-CONFIG-SCHEMA-SOURCE-END -->

`name` in a `[source.*]` body is rejected: the name comes from the section
header.

## `[quota.<class>]`

<!-- TRIGGER-CONFIG-SCHEMA-QUOTA-BEGIN
| section | key | status | reload | type | default | contract | note |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `quota.*` | `max_triggers_per_hour` | implemented | hot | `uint64` | `0` | TRG-04 | sliding one-hour trigger budget; 0 = unlimited |
<!-- TRIGGER-CONFIG-SCHEMA-QUOTA-END -->

## `[continuous]`

Continuous capture is host-side (the center does not participate).

<!-- TRIGGER-CONFIG-SCHEMA-CONTINUOUS-BEGIN
| section | key | status | reload | type | default | contract | note |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `continuous` | `auto_arm` | implemented | **restart required** | `bool` | `false` | TRG-10 | arm continuous capture at startup; does not affect manual Arm |
| `continuous` | `channels` | implemented | **restart required** | `string` | `<unset: all small structs>` | TRG-16 | continuous channel selector grammar |
| `continuous` | `slice_mb` | implemented | **restart required** | `uint64` | `512` | TRG-10 | slice size; must be > 0 and <= disk_quota_mb when set |
| `continuous` | `slice_s` | implemented | **restart required** | `uint64` | `0` | TRG-10 | time-based roll; 0 = size only |
| `continuous` | `wait_idle` | implemented | **restart required** | `bool` | `false` | TRG-17 | gate continuous drain on the idle detector |
| `continuous` | `demo_idle` | implemented | **restart required** | `bool` | `false` | TRG-17 | explicit demo idle predicate when no [chassis] exists |
| `continuous` | `rate_check` | implemented | **restart required** | `enum` | `warn` | TRG-10 | enforce/warn/off; runs on every Arm and --check-config |
| `continuous` | `enable` | deprecated | **restart required** | `bool` | `false` | TRG-17 | old spelling of auto_arm; accepted with a deprecation warning |
<!-- TRIGGER-CONFIG-SCHEMA-CONTINUOUS-END -->

## `[chassis]`

Chassis-mirror idle predicate offsets (normally generated from contracts).
Present only when a real chassis mirror is wired.

<!-- TRIGGER-CONFIG-SCHEMA-CHASSIS-BEGIN
| section | key | status | reload | type | default | contract | note |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `chassis` | `page_shm` | implemented | **restart required** | `path` | `<unset: no chassis predicate>` | TRG-10 | record-mirror page shm name for the chassis mirror |
| `chassis` | `struct_id` | implemented | **restart required** | `uint32` | `0` | TRG-10 | struct id inside the chassis mirror page |
| `chassis` | `epb_offset` | implemented | **restart required** | `uint32` | `0` | TRG-10 | uint8 park-brake status offset (!=0 means engaged) |
| `chassis` | `speed_offset` | implemented | **restart required** | `uint32` | `4` | TRG-10 | float vehicle-speed offset (m/s) |
| `chassis` | `speed_threshold` | implemented | **restart required** | `string` | `0.1` | TRG-10 | speed threshold below which the vehicle is considered stationary |
<!-- TRIGGER-CONFIG-SCHEMA-CHASSIS-END -->

## SIGHUP reload matrix

`trigger_center` treats SIGHUP as a *policy* reload. Anything the host binds
into the recorder/retention/idle objects at startup is rejected with
`reload rejected: … requires restart` — accepting it would print
`config reloaded` while the old value stayed in force.

| Section / key | SIGHUP |
| --- | --- |

All schema-backed rows (`[trigger_center]`, `[recorder]`, `[logging]`,
`[security]`, `[continuous]`, `[chassis]` and dynamic `[quota.*]`/`[source.*]`/
`[channel.*]`) are generated from the configuration schema; the C++ test checks
`ReloadRequiresRestart` against the declared class for every key.

<!-- TRIGGER-CONFIG-RELOAD-MATRIX-BEGIN
| Section / key | SIGHUP |
| --- | --- |
| `[trigger_center]` `socket` | startup-only (kept silently: the listening socket survives the reload) |
| `[trigger_center]` `max_concurrent_captures` | fixed at startup (`1` only; `> 1` is a parse error and there is no reload delta) |
| `[trigger_center]` `tick_ms` | **restart required** (socket geometry and queue sizing are bound at `Init`) |
| `[trigger_center]` `socket_poll_budget_us` | **restart required** (per-Tick poll budget is bound at `Init`) |
| `[trigger_center]` `max_trigger_rate_hz` | **restart required** (socket queue sizing is bound at `Init`) |
| `[trigger_center]` `dedup_window_entries` | **restart required** (the dedup cache is allocated at `Init`) |
| `[trigger_center]` `dedup_window_ms` | **restart required** (the dedup cache is allocated at `Init`) |
| `[trigger_center]` `quota_state` | **restart required** (the state file and its exclusive lock are bound at `Init`) |
| `[trigger_center]` `default_source` | hot (policy reload) |
| `[trigger_center]` `allow_empty_sources` | hot (policy reload) |
| `[trigger_center]` `crash_priority` | hot (policy reload) |
| `[trigger_center]` `pending_memory_mb` | n/a (parse error) |
| `[recorder]` `output_dir` | **restart required** (retention/recorder objects bind the root at Init) |
| `[recorder]` `socket` | **restart required** (the listening socket is bound at Init) |
| `[recorder]` `pending_bytes_quota` | **restart required** (recorder rings are sized at Init) |
| `[recorder]` `flush_policy` | **restart required** (drain/recorder objects are constructed once) |
| `[recorder]` `idle_hold_s` | **restart required** (idle detector is constructed at Init) |
| `[recorder]` `idle_fallback` | **restart required** (idle detector is constructed at Init) |
| `[recorder]` `io_rate_limit_mbps` | **restart required** (the bucket is created at Init) |
| `[recorder]` `use_cgroup` | n/a (parse error) |
| `[recorder]` `disk_quota_mb` | **restart required** (retention is constructed at Init) |
| `[recorder]` `high_reserved_mb` | **restart required** (retention is constructed at Init) |
| `[recorder]` `incomplete_policy` | **restart required** (retention is constructed at Init) |
| `[recorder]` `incomplete_max_age_s` | **restart required** (retention is constructed at Init) |
| `[logging]` `service_file` | **restart required** (the sink is opened at Init) |
| `[logging]` `service_max_bytes` | **restart required** (the writer is opened at Init) |
| `[logging]` `service_rotate_keep` | **restart required** (the writer is opened at Init) |
| `[logging]` `fsm_max_bytes` | **restart required** (the FSM dump writer is opened at Init) |
| `[logging]` `fsm_rotate_keep` | **restart required** (the FSM dump writer is opened at Init) |
| `[logging]` `perf_max_bytes` | **restart required** (host perf writers are opened at Init) |
| `[logging]` `perf_rotate_keep` | **restart required** (host perf writers are opened at Init) |
| `[logging]` `total_budget_mb` | **restart required** (the budget counter is owned by the open writers) |
| `[security]` `allowed_uids` | hot (policy reload) |
| `[security]` `allow_root` | hot (policy reload) |
| `[security]` `require_peer_pid` | hot (policy reload) |
| `[security]` `max_priority_hint` | hot (policy reload) |
| `[security]` `allow_legacy_unsequenced` | hot (policy reload) |
| `[security]` `allow_legacy_v2` | hot (policy reload) |
| `[security]` `allow_legacy_control` | hot (policy reload) |
| `[security]` `signing_key` | startup-only (kept silently: the path is kept; the next seal reads the file contents) |
| `[security]` `manifest` | **restart required** (recorder integrity mode is bound at Init) |
| `[security]` `audit_log` | startup-only (kept silently: the audit file is open; a change warns and keeps the path) |
| `[security]` `control_audit_log` | startup-only (kept silently: the audit file is open; a change keeps the running path) |
| `[security]` `audit_max_bytes` | startup-only (kept silently: rotation geometry is fixed at Init) |
| `[security]` `audit_rotate_keep` | startup-only (kept silently: rotation geometry is fixed at Init) |
| `[security]` `audit_fsync_interval_s` | startup-only (kept silently: the sync cadence is fixed at Init) |
| `[continuous]` `auto_arm` | **restart required** (continuous session state is bound at Init) |
| `[continuous]` `channels` | **restart required** (the recorder continuous plan is built at Init) |
| `[continuous]` `slice_mb` | **restart required** (slice geometry is bound at Init) |
| `[continuous]` `slice_s` | **restart required** (slice geometry is bound at Init) |
| `[continuous]` `wait_idle` | **restart required** (the drain policy is bound at Init) |
| `[continuous]` `demo_idle` | **restart required** (the drain policy is bound at Init) |
| `[continuous]` `rate_check` | **restart required** (the continuous plan is bound at Init) |
| `[continuous]` `enable` | **restart required** (it changes auto_arm, which requires restart) |
| `[chassis]` `page_shm` | **restart required** (the chassis predicate attaches at Init) |
| `[chassis]` `struct_id` | **restart required** (the chassis predicate attaches at Init) |
| `[chassis]` `epb_offset` | **restart required** (the chassis predicate attaches at Init) |
| `[chassis]` `speed_offset` | **restart required** (the chassis predicate attaches at Init) |
| `[chassis]` `speed_threshold` | **restart required** (the chassis predicate attaches at Init) |
| `[quota.*]` `max_triggers_per_hour` | hot (quota policy reload) |
| `[source.*]` `channels` | hot (policy reload) |
| `[source.*]` `before_s` | hot (policy reload) |
| `[source.*]` `after_s` | hot (policy reload) |
| `[source.*]` `priority` | hot (policy reload) |
| `[source.*]` `mute_s` | hot (policy reload) |
| `[source.*]` `flush_policy` | hot (policy reload) |
| `[source.*]` `quota_class` | hot (policy reload) |
| `[source.*]` `name` | n/a (parse error) |
| `[channel.*]` `kind` | **restart required** (recorder sources are built at Init) |
| `[channel.*]` `name` | **restart required** (recorder sources are built at Init) |
| `[channel.*]` `record_channels` | **restart required** (record registry is built at Init) |
| `[channel.*]` `rate_hz` | **restart required** (the continuous plan is built at Init) |
| `[channel.*]` `avg_frame_bytes` | **restart required** (the continuous plan is built at Init) |
| `[channel.*]` `bitrate_kbps` | **restart required** (the continuous plan is built at Init) |
| `[channel.*]` `crc` | **restart required** (the recorder plan is built at Init) |
| `[channel.*]` `manifest` | **restart required** (the recorder plan is built at Init) |
| `[channel.*]` `profile` | **restart required** (the recorder plan is built at Init) |
| `[channel.*]` `profile_version` | **restart required** (the recorder plan is built at Init) |
| `[channel.*]` `profile_params` | **restart required** (the recorder plan is built at Init) |
| `[channel.*]` `gop_s` | **restart required** (the recorder plan is built at Init) |
<!-- TRIGGER-CONFIG-RELOAD-MATRIX-END -->

(`timestamp_future_tolerance_ns` — shown in older revisions of this matrix —
is an internal `CenterConfig` constant, **not a config key**: the parser has
no such key and setting it is an unknown-key parse error.)

## Channel selector semantics

Source and continuous lists use one parser and matcher. Each comma-separated
entry is `[!][small_struct:|pinned_slot:]<glob>[@dec=N]`; the type prefix is
optional for backward-compatible bare channel/shm names. `*` matches any run of
characters, including `/`; all other characters are literal. There is no `?`
or character-class syntax. `@dec=N` accepts `1..1000000` and is invalid on an
exclusion. An `h264_stream` selected with `N > 1` is rejected because cutting a
GOP stream would make it undecodable.

- Exclusions always win, regardless of list order. Among includes, a non-`*`
  rule overrides the decimation of a `*` rule; otherwise the last matching
  include supplies decimation.
- Decimation keeps exactly `publish_index % N == 0` for small structs and
  `frame_index % N == 0` for big frames, in both trigger pre/post windows and
  continuous capture. Indices are producer-global, not restart-relative.
- Empty source channels = all channels with no decimation. Empty continuous
  channels = `small_struct:*`; both defaults are unchanged.
- Every positive source selector must match a configured channel. In both
  contexts an effective zero-channel selection is an error: config/startup
  validation fails, and direct `Trigger`/`ArmContinuous` calls return `false`.
- `[channel.*]` and positive source/continuous selectors must also match the
  generated `RecordManifest`; exclusions cannot broaden that subset.
- Excluded channels are not harvested and do not open raw/index files. A
  mirror-ring selection covers all small structs in that shm page.

## trigger→seal latency SLO

**Metric.** trigger→seal latency is the CLOCK_MONOTONIC delta between two
anchors for the same `trigger_id`:

- **start** — the center's FSM `kSubmitAccept` event (`reason=0`) for the
  trigger, stamped inside `Center::Submit` at accept time. Durable copy:
  the `<socket>.fsm` JSONL ring; in-process: `Center::CopyFsmEvents`.
- **end** — full seal completion in the recorder, stamped after every
  durability step succeeded (all package files fsync'ed, wal seal row
  written, integrity manifest written, package directory sync'ed).
  In-process: `TriggeredRecorder::last_seal_mono_ns()` paired with
  `last_seal_trigger_id()`. Aborted/invalidated captures never move this
  endpoint.
- **durable record** — the package's own `wal.log` seal row carries the
  CLOCK_MONOTONIC journal-commit time in its `mono_ns` field (paired with
  the wal header's `trigger_id`); 0 on older packages means "unknown". The
  journal commit is by construction ≤ the full-seal endpoint (the manifest
  hashes the sealed wal, so the row must be final first). `integrity.manifest`'s
  `seal_time_ns` is realtime clock, not monotonic — it is wall-clock evidence,
  not an SLO input.

The `kCompleted` FSM event is the center *observing* completion one
Tick after the seal — close to, but not, the SLO endpoint.

**Measurement.** `bench_trigger_seal` fires N triggers through the real
Center → TriggeredRecorder pipeline (in-process producer: one mirror-ring
struct + one small PinnedSlot channel, `flush_realtime`, 20 ms pre / 50 ms
post window) and prints p50/p90/p99/max. Its exit code gates only the
measurement plumbing (accept/seal pairing, non-zero stamps, complete
packages); latency magnitudes are never asserted in ctest.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"
./build/bench_trigger_seal --iters 200 --json bench_trigger_seal.json
# or via ctest (RUN_SERIAL, 64 iters, writes build/bench_trigger_seal.json):
ctest --test-dir build -R bench_trigger_seal --output-on-failure
```

**Reference values** (x86_64 dev box, `12th Gen Intel Core i5-12490F`,
Release, 64 iters, 20 ms + 50 ms windows — measured 2026-10-24, runner
`x86_64-local`; these are *reference* numbers for this hardware, not CI gates
and not target-board budgets):

| stat | trigger→seal | note |
| --- | --- | --- |
| min | 76.7 ms | structural floor ≈ `after_ns` + `post_grace_ns` (50 + 20 ms) |
| p50 | 78.0 ms | |
| p90 | 81.5 ms | |
| p99 | 82.4 ms | |
| max | 82.4 ms | |

**SLO budgets (reference hardware).** With the window floor subtracted, the
seal path itself (post-window collection tail + drain + fsync + manifest +
dir sync) costs p99 ≈ 12 ms for this package size (~500 small samples + ~70
× 1 KB frames). Budgets, as p99 over the `bench_trigger_seal` distribution
on reference hardware:

- trigger→seal for a `realtime` capture: **p99 ≤ `after_ns` + `post_grace_ns` + 50 ms**
  (measured headroom on the reference box: >2.5×).
- seal path beyond the window floor: **p99 ≤ 50 ms** for packages up to the
  default `pending_bytes_quota` (64 MiB).

Idle-flushed (`flush_policy = idle`) captures are deliberately excluded from
the SLO: their seal waits on the idle detector by design.
