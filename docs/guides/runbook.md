# Operations runbook

This is the incident-handling manual for thyme deployments: four high-frequency
scenarios (disk full, SHM leaks, trigger storms, crash recovery), each with
**symptoms / diagnostics / handling / prevention**. §5 covers service log levels,
rate limiting, and rotation; §6 is the upgrade/rollback checklist; §7 is the
startup resource self-check; §8 is package verification; §9 is the fault matrix.

Commands use binaries from this repository's build tree (`build/tools/...`).
Configuration keys are authoritative in
[`trigger-config.md`](trigger-config.md); deployment/systemd semantics are in
[`../../deploy/README.md`](../../deploy/README.md). Default failure actions and
their costs are in
[`../reference/default-policy-matrix.md`](../reference/default-policy-matrix.md).

Assumed standard layout:

- trigger socket: `/tmp/thyme_trigger.sock` (`[trigger_center].socket`)
- FSM event log: `<socket>.fsm` (JSONL)
- audit log: `<socket>.audit` (`[security].audit_log`)
- recorder control socket: `/tmp/thyme_recorder.sock` (`[recorder].socket`)
- trigger package output: `[recorder].output_dir` (examples use
  `/opt/usr/triggered_data`)
- supervisor status endpoint: `/run/thyme/status.sock` (read-only JSON)

## Quick index

| Symptom / task | Go to |
|---|---|
| Drain write failure, `code=quota_exceeded`, disk full | §1 |
| `/dev/shm` growth, EEXIST, stale segments | §2 |
| Trigger storm, many `code=muted`/`busy`, rapid FSM flips | §3 |
| Process disappeared, `.incomplete` packages, auto-restart | §4 |
| Service log level, rate limiting, rotation | §5 |
| Upgrade / rollback / ABI preflight | §6 (full flow in [`upgrade.md`](upgrade.md)) |
| Startup reports insufficient resources (fd / memlock / disk / `/dev/shm`) | §7 |
| Package verification: per-frame CRC, evidence manifest, schema, record manifest | §8 |
| Fault matrix drills (kill roles / SHM full / disk full / clock jump) | §9 |
| Startup order, dependencies, health checks, watchdog | [`orchestration.md`](orchestration.md) |

## 0. Observability surface

| Tool / file | Location | Purpose |
|---|---|---|
| `thyme_probe` | `build/tools/inspect/thyme_probe/thyme_probe` | Read-only `/dev/shm` view: `list / info / watch / hz / bw / delay / echo`; `abi` is the pre-upgrade compatibility check (`--matrix` prints this build's constants, `--mismatches` lists incompatible pages) |
| `thyme_log_merge` | `build/tools/inspect/thyme_log_merge/thyme_log_merge` | Merge multi-process `.fsm` / JSONL / perf dumps into one timeline; `--hist` merges histograms and prints p50/p90/p99/p999/max per series; `--expect-hash` validates the record manifest (mismatch/legacy → exit 4) |
| `triggerctl` | `build/tools/apps/trigger_center/triggerctl` | Manual trigger / `arm` / `disarm` / `status` |
| `<socket>.fsm` | Same directory as the trigger socket | FSM transition ring (256 entries), appended as JSONL once per second; rotates to `.1..N` |
| `<socket>.audit` | `[security].audit_log` | One `key=value` audit line per request, auto-rotated |
| Status endpoint | `/run/thyme/status.sock` | `curl --unix-socket ... http://localhost/status`, role state and counters |
| Service log | stderr by default (journald under systemd); file sink path when configured | One `key=value` line (`ts/level/pid/tid/site/msg` plus fields); hot paths rate-limit per call site (first 3, then every 1000 with `suppressed=N`) |
| Metrics bridge | `tools/inspect/thyme_metrics_exporter/thyme_metrics_exporter.py` | Translate status/probe/perf histograms/audit/`df` into a Prometheus textfile |
| perf dump | Host-wired `PerfLogDumper`; generated MP apps enable it with `T9_PERF_LOG=<prefix>` | Crash-safe periodic binary append |

`trigger_center`'s in-process `CenterStats` are not exported directly; the
external equivalent is `decision=` / `code=` in audit lines and transitions in
`.fsm`. Recorder-side `Stats` (`disk_errors`, `pending_evicted`,
`overrun_before_queue`, `holes_written`, …) are written as a drain record at the
end of each package's `meta.tbin`.

## 1. Disk full (ENOSPC)

**Symptoms**

- drain write failure; stderr prints
  `trigger_center: disk full during drain; running retention and re-arming`;
- after a second failure in the same drain, non-crash triggers are rejected:
  the client gets `code=quota_exceeded`, and the audit line shows
  `decision=rejected` + `code=quota_exceeded`;
- crash-level triggers (`priority >= [trigger_center].crash_priority`) still
  proceed into the `high/` reserve;
- continuous capture follows the same ENOSPC semantics: the current slice is
  invalidated and stopped until retention frees space and the host re-arms;
- `disk_errors` grows in the package's drain statistics.

**Diagnostics**

```bash
df -h /opt/usr/triggered_data
du -sh /opt/usr/triggered_data/* | sort -h | tail      # low-priority usage
du -sh /opt/usr/triggered_data/high                    # crash reserve
ls /opt/usr/triggered_data | grep '\.incomplete$'
grep code=quota_exceeded /tmp/thyme_trigger.sock.audit | tail
ls -l /tmp/thyme_trigger.sock.audit* /tmp/thyme_trigger.sock.fsm*
du -ch /var/log/thyme/perf_*.bin* | tail -1
```

**Log disk bounds**

Non-package append files have hard bounds
(`include/thyme/runtime/rotating_file.h`):

| File | Configuration | Default bound |
|---|---|---|
| `<socket>.audit` | `[security].audit_max_bytes` × (`audit_rotate_keep`+1) | 4 MiB × 3 ≈ 12 MiB |
| `<socket>.fsm` | `[logging].fsm_max_bytes` × (`fsm_rotate_keep`+1) | 4 MiB × 3 ≈ 12 MiB |
| Each perf dump | `[logging].perf_max_bytes` × (`perf_rotate_keep`+1) | 64 MiB × 3 ≈ 192 MiB |

Going over a single file exceeds by at most one record (one dump block for perf).
`[logging].total_budget_mb` is a **per-process** total (audit + fsm + perf;
0 = per-file limits only). When the budget is exhausted, fsm/perf records are
dropped and counted (`logging.budget_drops` in `/status`); the security audit is
**never dropped**. In multi-process deployments each process enforces its own
budget, so the machine bound is the sum; whole-disk guarantees still need the
filesystem/package quota (`disk_quota_mb` +
`thyme_filesystem_used_percent`). Rotation generations are complete files and
can be passed directly to `thyme_log_merge`:

```bash
build/tools/inspect/thyme_log_merge/thyme_log_merge \
    /var/log/thyme/perf_node_sensor_*.bin.1 \
    /var/log/thyme/perf_node_sensor_*.bin /tmp/thyme_trigger.sock.fsm* --json
```

**Handling**

1. Confirm layout: `<output_dir>/<trigger_id>_<t0>/` is low-priority,
   `<output_dir>/high/<trigger_id>_<t0>/` is crash-level. Packages are **flat
   directories**; age is by mtime.
2. The host automatically runs `retention.Enforce()` then `ClearDiskFull()` and
   retries. If nothing within quota can be deleted (`.incomplete` and active
   packages are never deleted; `high/` is untouched by low-priority sweeps),
   intervene manually.
3. Free space manually: **move/delete only sealed low-priority packages**
   (`wal.log` has a seal row, directory name has no `.incomplete` suffix),
   oldest mtime first. Never delete:
   - packages still being written (the recorder's active set; `Enforce`
     rechecks seal state);
   - `.incomplete` packages (incident evidence; quota sweeps never delete them,
     and deleting them does not help retention);
   - crash packages under `high/` unless already exported.
4. No restart is needed after freeing space: the next trigger retries the drain;
   retention runs `Enforce()` after every capture and on a 5 min tick, printing
   `trigger_center: retention freed N bytes`.

**Prevention**

- Configure `[recorder].disk_quota_mb` and `[recorder].high_reserved_mb`;
- Use `[recorder].io_rate_limit_mbps` + `[continuous].rate_check = enforce` to
  limit drain/continuous rates and avoid burst overrun;
- Limit low-priority source production with
  `[quota.*].max_triggers_per_hour`;
- Set `[recorder].incomplete_policy` explicitly (`keep` is safe but uses disk;
  `delete_after_age` reclaims via `incomplete_max_age_s`);
- Configure `[logging]` file sizes/rotation and `total_budget_mb`;
- Monitor `df` and the rate of `quota_exceeded`; with metrics, watch
  `thyme_filesystem_used_percent` (`ThymeDiskPressure` 85%,
  `ThymeDiskFull` 95%) and log budget metrics
  `thyme_log_budget_used_bytes` / `thyme_log_budget_drops_total`.

## 2. SHM leak (stale `/dev/shm` segments)

thyme embeds the session pid in SHM names: the MP supervisor uses
`/c9_<supervisor-pid>`, recording mirror pages use `/thyme_rec_<pid>`, and
business channels use names such as `/h264_<camera>` and `/thyme_recr_*`. After
a crash these nodes may outlive every process that knows them.

**Symptoms**

- `/dev/shm` usage keeps growing;
- many old-pid segments in `ls /dev/shm`;
- `ShmRegion::Create` reports EEXIST on startup (same-name leftover).

**Diagnostics**

```bash
ls -l /dev/shm
build/tools/inspect/thyme_probe/thyme_probe list
build/tools/inspect/thyme_probe/thyme_probe list --prefix c9_
grep -l '<segment name without leading />' /proc/[0-9]*/maps 2>/dev/null
ls -l /proc/[0-9]*/fd 2>/dev/null | grep '<segment name>'
build/tools/inspect/thyme_probe/thyme_probe info /thyme_rec_1234
```

**Handling**

1. **Use the safe reclamation path; do not `rm` by hand.** The lifecycle code
   (`include/thyme/runtime/shm_lifecycle.h`,
   `src/runtime/shm_lifecycle.cpp`) decides:
   - scans `/proc/<pid>/fd` and `/proc/<pid>/maps` (mappings can outlive fds);
     a hit means `kLive`; unreadable `/proc` makes the scan incomplete;
   - takes an exclusive file lease (`F_SETLEASE`): the kernel grants it only
     when no process holds the object — success proves stale, `EAGAIN` means a
     live holder;
   - refuses segments not owned by the current euid;
   - leaves `kUnknown`/`kInconclusive` untouched.
2. Same-name segments (application restarted with the same session name) need
   no manual action: `ShmRegion::Create()` handles EEXIST by calling
   `SweepStaleShmSegment()` and rebuilding only after proving staleness.
3. For differently named leftovers (old session pid, never reused), confirm no
   live holder, then reclaim through `SweepStaleShmSegment("/name")` or delete;
   `ShmSweepGlobalCounters()` (`examined/swept/skipped_live/inconclusive`) can
   be used to cross-check. **Never** run `rm -rf /dev/shm/thyme_*`: it misses
   most names and can unlink a segment a live peer still maps (POSIX shm has no
   unlink-if-inode). Deployment explicitly forbids shell sweeps in
   `ExecStartPre`.
4. There is an accepted residual race: a one-syscall window between liveness
   proof and `shm_unlink` (unlink while the lease is held). This is documented in
   the header; do not invent another "more atomic" scheme.

**Prevention**

- Keep the session pid in segment names so new sessions never reuse old names;
- Deploy through the systemd/busybox units; a supervisor crash is restarted by
  `Restart=on-failure`, and startup reclaims automatically;
- Monitor `/dev/shm` (`df` or periodic `thyme_probe list | wc -l`). The metrics
  bridge exports `thyme_shm_used_percent` / `thyme_shm_objects`
  (`ThymeShmPressure` 80%, `ThymeShmObjectsGrowth` +100 in 30 min);
  `ThymeShmPageIncompatible` covers segment ABI mismatches.

## 3. Trigger storm

**Symptoms**

- a burst of trigger requests; FSM flips rapidly between IDLE/CAPTURING/MUTED;
- many low-priority rejections: `code=muted` (same/lower priority during
  CAPTURING or inside the mute window), `code=quota_exceeded` (count or memory
  quota), `code=busy` (an accepted but not-yet-started trigger occupies the
  queue or a preemption is in progress);
- `CenterStats` counters rise: `rejected_muted` / `rejected_quota` /
  `rejected_busy` / `priority_clamps`;
- the FSM event ring overflows: `fsm_event_drops > 0` (256-entry ring overwrites
  the oldest; `.fsm` shows a gap);
- high-priority preemption and crash-level quota override leave audit traces
  (never lost with the FSM ring): `req=preempt decision=preempted`
  (`trigger_id=` victim, `by=` initiator) and `quota_override=1`;
- packages accumulate and start consuming `disk_quota_mb` (go to scenario 1).

**Diagnostics**

```bash
tail -100 /tmp/thyme_trigger.sock.audit
grep -c 'decision=accepted' /tmp/thyme_trigger.sock.audit
grep 'decision=rejected' /tmp/thyme_trigger.sock.audit | tail -20
grep 'req=preempt' /tmp/thyme_trigger.sock.audit | tail -20
grep 'quota_override=1' /tmp/thyme_trigger.sock.audit | tail -20
tail -50 /tmp/thyme_trigger.sock.fsm
build/tools/inspect/thyme_probe/thyme_probe hz /thyme_recr_<pid> --window 2000
```

`.fsm` `why` values (`FsmReason`):

| why | Meaning |
|---|---|
| 0 `kSubmitAccept` | Trigger accepted and queued (→ CAPTURING) |
| 1 `kStarted` | Queued trigger actually started |
| 2 `kStartFailed` | Rollback: recorder offline / `start_capture` failed |
| 3 `kPreempted` | Old capture preempted by higher priority (truncated and sealed) |
| 4 `kPreemptFailed` | Preemption failed; old capture restored |
| 5 `kCompleted` | Capture completed and sealed (→ MUTED/IDLE) |
| 6 `kMuteExpired` | Mute window expired (→ IDLE) |
| 7 `kCancelPending` | Pending trigger cancelled; FSM rolled back |
| 8 `kCancelRunning` | Running capture cancelled (truncated, sealed, muted) |

**Handling**

1. **Stop the source:** the audit line's `pid=`/`uid=`/`src=` identifies it
   (kernel `SCM_CREDENTIALS` cannot be forged).
2. **Cancel in-flight triggers:** `trigger_client::Cancel(socket, trigger_id)`
   drops pending triggers and rolls back the FSM; running captures are
   truncated/sealed as with preemption. A reply is `kCancelled`; unknown or
   completed is `kUnknownTrigger` (explicit rejection). `triggerctl` exposes
   trigger/arm/disarm/status only; cancel uses the client API.
3. **Tighten policy online without restart:** `[source.*]` `mute_s`,
   `priority`, `[quota.*].max_triggers_per_hour`, and
   `[security].max_priority_hint` are SIGHUP-reloadable. Increase the offender's
   `mute_s` or lower its quota, then `kill -HUP <trigger_center pid>`.
4. After the storm, confirm rejections stop growing and `.fsm` returns to low
   frequency. With metrics, watch `thyme_trigger_relay_debounced_total` rate
   (`ThymeTriggerStorm`), audit decision distribution, and
   `thyme_shm_page_pinned_skip_total` for real frame loss.

**Prevention**

- Give every low-priority source a `mute_s` and a quota class;
- Use `priority` tiers plus `[security].max_priority_hint` clamping: a hint can
  only **lower** the source-rule priority (`priority_clamps`, `clamp=1` in
  audit);
- Only crash level (`>= crash_priority`) may override quota
  (`quota_overridden`); ordinary sources cannot;
- `[recorder].pending_bytes_quota` is the recorder's only pending-byte quota;
  the center links through the `pending_bytes` hook. Ordinary over-limit
  requests are rejected; crash level may evict the oldest pending data first.

## 4. Crash recovery

**Symptoms**

- supervisor or `trigger_center` disappears; systemd/busybox starts a new
  session;
- `<name>.incomplete` packages appear;
- old session segments remain in `/dev/shm` (go to scenario 2).

**Crash guarantees per log surface**

| Surface | File | Crash loss window |
|---|---|---|
| FSM events | `<socket>.fsm` (O_APPEND JSONL, 1 s dump, one `write()` per record; rotation `[logging].fsm_*`) | Events after the most recent dump; at most 256 ring entries; only the last line may be truncated |
| Audit | `<socket>.audit` (O_APPEND, rotation `audit_max_bytes`/`audit_rotate_keep`) | `fdatasync` on shutdown by default (`audit_fsync_interval_s` configures periodic sync) |
| Service log | stderr/journal by default; `[logging].service_file` for trigger_center (recommended `/var/log/thyme/<role>.log`); refuses to start if the explicit file is unwritable | One `write()` per record; at most the tail record is lost (write failures count `records_dropped()`); rotation `service_max_bytes` × (`service_rotate_keep`+1) |
| perf dump | Host-wired `PerfLogDumper`; generated MP apps use `$T9_PERF_LOG_node_<role>_<i>.bin` / `$T9_PERF_LOG_edge_...`; rotation `[logging].perf_*` | O_APPEND periodic append; at most one dump interval lost; a half-written tail record stops the parser at the first short block; each generation has its own T9P1 magic |

Perf dump is crash-safe by design: each interval appends only samples after the
last `FlushNew` watermark (no duplicates), then resets the source ring
(dump-then-reset). Ring capacity only needs `rate × interval`. There is **no**
signal-handler flush (not async-signal-safe); periodic append already bounds the
loss window.

**Histograms (p999/max)**

Each `PerfSampler` has a fixed-bucket histogram. `Note()` performs one relaxed
atomic bucket increment (about 5–6.5 ns/sample, lock-free, no allocation).
Latency (`perf()`, `perf_edge()`) and per-callback work (`cb_work()`) get
histograms automatically; series bytes are 0/1/2. Each dump window appends one
type-6 block after sample/percentile blocks, from which p50/p90/p99/p999 and
exact max are computed.

- **Resolution:** log-linear, exact 0–15 ns; ≥16 ns uses 8 sub-buckets per
  octave. Percentiles report the bucket midpoint, worst error **+6.25% /
  −5.56%**; `max_ns` is exact. Merging across processes adds bucket counts and
  takes the max.
- **Memory:** fixed **1,332 B/series** (329 × u32 buckets + 8 B max); no growth.
  Values above 2^41 ns saturate into the last bucket (max stays exact).
- **Offline:** timeline mode renders each type-6 block as
  `hist node=.. series=.. n=.. p50_ns=.. p90_ns=.. p99_ns=.. p999_ns=.. max_ns=..`;
  `--hist` merges across processes by (node, series) and prints one line each.

```bash
build/tools/inspect/thyme_log_merge/thyme_log_merge \
    /var/log/thyme/perf_node_*.bin --hist
build/tools/inspect/thyme_log_merge/thyme_log_merge \
    /var/log/thyme/perf_node_*.bin --hist --json --from 1234567890000
```

**Diagnostics**

```bash
tail -20 /tmp/thyme_trigger.sock.fsm
tail -20 /tmp/thyme_trigger.sock.audit
journalctl -u thyme-app --since '1 hour ago'
curl --unix-socket /run/thyme/status.sock http://localhost/status
build/tools/inspect/thyme_log_merge/thyme_log_merge \
    /tmp/thyme_trigger.sock.fsm /var/log/thyme/perf_node_perception_0.bin \
    --json -o /tmp/merged.jsonl
```

**Handling**

1. **Partial packages:** on startup `MarkIncomplete()` renames packages whose
   `wal.log` lacks a seal row to `<name>.incomplete`. The default
   `[recorder].incomplete_policy = keep` treats them as incident evidence;
   **quota sweeps never delete `.incomplete`** — only that policy does
   (`delete_after_age` / `delete_immediately`). Export before disposing. The
   engine also writes a **partial** `integrity.manifest` over surviving files;
   `thyme-replay integrity <pkg>.incomplete` reports `PARTIAL` if survivors are
   intact or `FAILED` if tampered.
2. **Retention self-heals:** the host runs `MarkIncomplete()` at startup, then
   `Enforce()` after every capture and every 5 min. It deletes the oldest
   low-priority packages over quota; `high/` is cleaned separately by
   `high_reserved_mb`. Active packages are never deleted.
3. **SHM:** a new session's `ShmRegion::Create` reclaims same-name stale
   segments automatically.
4. **Role restart:** a single role crash is handled by the supervisor's restart
   policy (SIGTERM → SIGKILL, exponential backoff, budget, then `kGaveUp`).
   A supervisor crash is handled by init's `Restart=on-failure`.
5. **Export evidence:** keep `.incomplete` packages + `.fsm` + `.audit` + perf
   dumps; merge them to locate the first anomalous event.

**Prevention**

- Use the shipped systemd/busybox units and their restart/watchdog settings; do
  not add shell cleanup in `ExecStartPre`;
- Set `audit_fsync_interval_s` nonzero for critical scenes;
- Wire `PerfLogDumper` when perf forensics are needed;
- Configure `[security].signing_key` so sealed packages carry
  `integrity.hmac`;
- Enable the observability alerts (`ThymeStatusEndpointDown`,
  `ThymeRoleGaveUp`, `ThymeRoleStalled`, `ThymeRoleRestartStorm`).

## 5. Service log level, rate limiting, and rotation

**Location**

- Default: stderr. Under systemd, journald collects it (`journalctl -u
  thyme-app`); on busybox/bare metal, redirect stderr to
  `/var/log/thyme/<role>.log`.
- File sink: the host calls `thyme::log::OpenFileSink(...)` before `Start()`;
  if it fails, the logger stays on stderr and startup still succeeds.
  Recommended `max_bytes = 8 MiB`, `keep = 2`, path `/var/log/thyme/<role>.log`.
  Rotation uses rename semantics (`.1`…`.N`, oldest deleted first), so keep the
  rotated generations in addition to `journalctl`/`tail -F`.

**Format**

```
ts=2026-10-24T09:12:33.123Z level=warn pid=4242 tid=4243 \
site=record_mirror.h:274 msg="oversized payload dropped" \
struct_id=0 size=2048 capacity=512 suppressed=0
```

- `level` is `error|warn|info|debug`; runtime threshold via
  `thyme::log::SetLevel()` (default `info`), compile-time cap
  `-DTHYME_LOG_MAX_LEVEL=0..3` (default 3).
- `site=file:line` is the call site; newlines/quotes/tabs in `msg` are escaped,
  so one record is always one line. Overlong records carry `truncated=1`.
- Locate with `grep 'level=error'`, `grep 'trigger_id=7'`, or aggregate by
  `site=`.

**Rate limiting**

- Hot-path/per-message logs use `THYME_LOG_*_RL`: the first 3 per call site are
  always emitted, then one per 1000 with `suppressed=N`. Counters remain on the
  business side (`auth_rejects`, `oversize_drop_cnt`, …); **logs are not a
  counter source**.
- Startup/error paths use the non-`_RL` macros and are not sampled.
- Rate state is per static call site (per process), not per thread; concurrent
  sampling decisions are atomic, but `suppressed=` is approximate.

**Do not**

- Call the logger from a signal handler (sink mutex, `vsnprintf`,
  `clock_gettime`, `rename()` are not async-signal-safe). Use `PerfLogDumper`
  for crash-window evidence.
- Call the logger in a child process after `fork()` in a multithreaded process
  (the sink lock may be held by a vanished thread). The supervisor's `execv`
  failure path keeps raw `fprintf(stderr, ...)` for this reason.
- Log unconditionally per frame/message; use `_RL` and put counts in
  perf/status.

## 6. Upgrade / rollback

Upgrade is a **whole-session** operation: stop producers, stop consumers,
replace with the same build tree, recreate the SHM session. Current
`LatestSlot` v3 also has a super-header; mixed runs fail explicitly at
attach/open. Full steps, mixed-run matrix, and rollback are in
[`upgrade.md`](upgrade.md); minimal checklist:

```bash
# 1) Preflight (read-only): this build's constant matrix + host page versions
build/tools/inspect/thyme_probe/thyme_probe abi --matrix
build/tools/inspect/thyme_probe/thyme_probe abi --prefix thyme_ --prefix c9_ \
    --manifest /etc/thyme/probe.manifest --mismatches
#    exit 0 consistent / 2 no pages (not running) / 3 version mismatch
# 2) Dry-run upgrade (default dry-run: no stop/copy/unlink/package delete)
tools/ops/upgrade/thyme_upgrade.sh --package /opt/packages/thyme-<new> \
    --prefix /opt/thyme --backup-dir /var/backups/thyme/<old> \
    --producer-unit thyme-producers --consumer-unit thyme-consumers \
    --probe build/tools/inspect/thyme_probe/thyme_probe --purge-shm
# 3) Confirm the plan, then --apply
# 4) Post-start recheck: thyme_probe abi → 0; thyme-replay integrity <new pkg> → OK
```

**Order:** preflight → stop producers → stop consumers → replace binaries →
recreate/clean SHM → start producers → start consumers → verify. A single-unit
deployment stops all roles at the "stop producers" step.

**Rollback:** stop producers/consumers → restore the whole prefix with
`--rollback <backup>` → if crossing ABI, recheck with the restored probe and
`--purge-shm` → start producers/consumers → verify. The script never deletes
packages unless `--delete-package` and `--apply` are both given.

**Common traps:**

- `[logging]` file sizes/rotation/total budget are startup-only;
- external cloud/image/h264 drivers and replay/injectors must be rebuilt from
  the same tree; old pinned-slot (v4) or mirror-ring (v6) pages cannot mix;
- hand-built external producers must migrate to
  `FrameInit` → full payload → seal; reserved-bit validation has no legacy
  switch;
- Do not use `thyme-replay --force` / `Create(force_replace)` as an upgrade, and
  do not blindly delete `/dev/shm` segments.

## 7. Startup resource self-check

`trigger_center` runs a **read-only** host-capacity check after `rec.Init` and
before binding sockets: `getrlimit`/`statvfs` plus a read-only check of
`/proc/sys/net/unix/max_dgram_qlen`. It does not change system settings and can
run standalone without starting threads:

```bash
build/tools/apps/trigger_center/trigger_center --check-resources \
    --record-manifest /etc/thyme/record_manifest.h /etc/thyme/trigger.ini
build/tools/apps/trigger_center/trigger_center --check-resources \
    --record-manifest /etc/thyme/record_manifest.h /etc/thyme/trigger.ini --json
```

**Checks and verdicts**

| Item | Requirement | Fatal (exit 3 by default) | Warn (count and continue) |
|---|---|---|---|
| `RLIMIT_NOFILE` | `channels × files_per_channel + baseline` (default 2 × recorded sources + 64) | soft < requirement | unreadable (unverifiable) |
| `RLIMIT_MEMLOCK` | Only when mlockall is active (`THYME_NO_MLOCK` unset); requirement = planned SHM + 64 MiB headroom | never fatal (mlockall failure only degrades latency) | soft < requirement, or unreadable |
| Output disk free | `statvfs(output_dir)` `f_bavail × f_frsize` | free < `disk_reserve_bytes` (default `[recorder].high_reserved_mb`) | free < `[recorder].disk_quota_mb`, or unreadable |
| `/dev/shm` free | `statvfs("/dev/shm")` | free < planned segment size | free < planned + reserve (default 16 MiB), or unreadable |
| `net.unix.max_dgram_qlen` | `ceil(max_trigger_rate_hz × tick_ms / 1000)`; at least 100 for bursts | never fatal | below requirement or procfs unreadable |

Planned segments default to **8 MiB × recorded sources** (one mirror page
≤ 2 MiB; PinnedSlot channels are created by producers and not counted here).
`--json` fields: `checked/worst/fatal/warnings/nofile{...}/memlock{...}/disk{...}/shm{...}`.

**Deployment switches (environment variables, all optional)**

| Variable | Default | Effect |
|---|---|---|
| `THYME_RESOURCE_CHECK` | `enforce` | `off` skips; `warn` only warns; `enforce` makes fatal → exit 3 |
| `THYME_RESOURCE_FILES_PER_CHANNEL` | `2` | Files per channel per package (`.raw` + `.index.tbin`) |
| `THYME_RESOURCE_BASELINE_FILES` | `64` | Baseline fds for sockets/logs/directories |
| `THYME_RESOURCE_SHM_PLANNED_MB` | `8 × channels` | Planned SHM; set explicitly for PinnedSlot-heavy deployments |
| `THYME_RESOURCE_SHM_RESERVE_MB` | `16` | `/dev/shm` headroom |
| `THYME_RESOURCE_MEMLOCK_HEADROOM_MB` | `64` | mlockall stack/arena headroom estimate |
| `THYME_RESOURCE_DISK_RESERVE_MB` | `high_reserved_mb` | Output disk hard floor |
| `THYME_NO_MLOCK` | unset | `1/true/yes/on` disables mlockall; shared with `App::Prepare()` |

**Status surface:** `/status`'s `resources` block (checked/fatal/warnings plus
nofile/shm/disk free/required) is re-sent each tick; top-level `ok` includes
resource fatal. `/health` remains a **role liveness** probe; check resources via
`/status`. The MP supervisor also runs the check in `Prepare()` (no output
directory, so disk is `checked:false`) and publishes it. `checked:false` means
the host did not run the check, not that it passed.

Actual memory hardening results are published per role in `health.memory`:
`flags` is the bitwise OR of worker flags, `workers` gives each worker's
`flags/state`. bit 0 = `mlock_disabled`, bit 1 = `mlock_failed`, bit 2 =
`mallopt_failed`, bit 3 = `unsupported`; success is 0. `state` is
`ok/disabled/degraded/unsupported`; differing states within a role are `mixed`.
`thyme_probe echo <health-page> --count N --json` also reports per-worker
`memory_flags/memory_state`. These bits do not enter `error_code`,
`health.errors`, or top-level `ok`; `disabled` is an explicit deployment policy
and must never be treated as lock failure.

**Remediation**

- `nofile` fatal: raise systemd `LimitNOFILE=`/`ulimit -n`, then rerun the check;
- `memlock` warn: grant `CAP_IPC_LOCK`, raise `LimitMEMLOCK`, or set
  `THYME_NO_MLOCK=1` and accept page-fault jitter. Only
  `1/true/yes/on` disables it; `0`, empty, and other values still run the
  default `mlockall(MCL_CURRENT|MCL_FUTURE)`. Disabling lowers residency but RT
  threads may fault;
- `shm` fatal/warn: reclaim stale segments first, then consider enlarging
  `/dev/shm`;
- `disk` fatal: free space; `high_reserved_mb` is the crash-package reserve, so
  refusing to start below it is expected;
- `max_dgram_qlen` warn: raise it, for example
  `sysctl -w net.unix.max_dgram_qlen=512`, and persist under
  `/etc/sysctl.d/`.

## 8. Package verification

`thyme-replay` provides four read-only commands, all with `--json`:

```bash
# 1) Structure + per-frame CRC + timeline monotonicity + index
build/tools/replay/thyme-replay/thyme-replay verify /opt/usr/triggered_data/<pkg>
# 2) Evidence manifest: per-file sha256 + hash chain + optional HMAC
build/tools/replay/thyme-replay/thyme-replay integrity /opt/usr/triggered_data/<pkg>
build/tools/replay/thyme-replay/thyme-replay integrity /opt/usr/triggered_data/<pkg> \
    --key /etc/thyme/signing.key
# 3) Schema sidecar (contracts + field layouts)
build/tools/replay/thyme-replay/thyme-replay schema /opt/usr/triggered_data/<pkg>
# 4) Record manifest identity
build/tools/replay/thyme-replay/thyme-replay verify /opt/usr/triggered_data/<pkg> \
    --expect-manifest-hash 0x<this build's constant>
# 5) Structure and evidence together
build/tools/replay/thyme-replay/thyme-replay verify /opt/usr/triggered_data/<pkg> \
    --integrity --key /etc/thyme/signing.key
```

Exit codes: **0** declared scope passed; **1** verification failed (or package
unreadable/missing manifest/mismatch); **2** usage error; **3** only `play`
target configuration errors.

`[security].manifest=full` (default) verifies whole-file contents. `metadata`
verifies only `.raw` size and full content elsewhere; `off` verifies only the
file list + size; per-bulk overrides produce `mixed`. Non-full success prints
`OK(metadata)`, `OK(off)`, or `OK(mixed)` with `UNVERIFIED(payload)`, and **must
not be read as proof that payload content is untampered**. Status/JSON
`integrity_mode` and `thyme_recorder_integrity_mode{mode=...}` expose the current
mode; a startup downgrade is audited as
`req=config reason=integrity_downgrade`.

**Failure modes**

| Symptom | Key output | Exit | Handling |
|---|---|---|---|
| Truncated data / frame CRC mismatch | `verify: FAILED`, `crc_bad > 0` | 1 | Preserve for forensics; do not replay; locate the first anomaly via the timeline |
| `mono_ns` regression (clock jump/mixed sessions) | `timeline is not monotonic in mono_ns` | 1 | Split by time window; check for packages from different sessions |
| Missing manifest (pre-SEC-03 package) | `integrity: FAILED`, manifest `absent` | 1 | Old packages only support structural `verify`; new packages should configure `signing_key` |
| Tampered/missing file | `integrity: FAILED`, `files ... mismatched/missing` | 1 | Treat as untrusted; follow evidence-integrity procedure |
| Non-full same-size payload tampering | May still be `OK(metadata/off/mixed)` + `UNVERIFIED(payload)` | 0 | Explicitly degraded scope, not proof; restore full and re-record if content proof is needed |
| `.incomplete` declared scope intact | `integrity: PARTIAL (not sealed; declared scope intact)` | 1 | Export as incident evidence; not a sealed package |
| Unsigned manifest | `signature: absent (unsigned)` | 0 (if rest OK) | Not a failure; add a key per policy |
| Missing `schema.json` (pre-ABI-09) | `schema: FAILED`, `pre-ABI-09 package` | 1 | Use `list/info/verify`; do not guess layouts |
| Record manifest mismatch | `list/info/verify: FAILED`; `play` refuses | 1 | Use the build that produced the package |

`PARTIAL` and `FAILED` exit nonzero; only a full sealed package prints bare
`integrity: OK`. Batch check:

```bash
for pkg in /opt/usr/triggered_data/*/; do
  build/tools/replay/thyme-replay/thyme-replay integrity "$pkg" --json \
      | grep -o '"ok":\(true\|false\)' | sed "s#^#$pkg #"
done
```

## 9. Fault matrix

`tools/ops/fault_matrix/fault_matrix.py` drives the `fault_role` helper; each
case asserts a concrete observable (exit signal, counter, package state,
monotonic invariant) and emits a JSON summary. It is the ctest `fault` label
(nightly/manual, not MR gate):

```bash
ctest --test-dir build -L fault --output-on-failure
python3 tools/ops/fault_matrix/fault_matrix.py \
    --helper build/tools/ops/fault_matrix/fault_role \
    --json /tmp/fault_matrix.json
python3 tools/ops/fault_matrix/fault_matrix.py \
    --helper build/tools/ops/fault_matrix/fault_role \
    --only kill_producer --only disk_full --verbose
```

| Case | Type | Asserted observable |
|---|---|---|
| `kill_producer` | Real `kill -9` | Exit signal 9; reader sees a committed prefix with `bad=torn=0` and `frames+drops=head`; a new producer can recreate the same channel and publish |
| `kill_reader` | Real `kill -9` | Exit signal 9; producer `committed` still grows without consumers; a new reader attaches and validates frames |
| `kill_supervisor` | Real `kill -9` | Exit signal 9; role children with `PR_SET_PDEATHSIG` disappear from `/proc` within 3 s |
| `shm_full` | Simulated (`RLIMIT_FSIZE` restricts the tmpfs segment) | `ShmRegion::Create` fails, no `/dev/shm` residue, same geometry succeeds after restoring the limit |
| `disk_full` | Simulated (ENOSPC injection point, no tmpfs/loop) | `disk_full` set, `disk_errors ≥ 1`, package renamed `.incomplete` and unsealed |
| `clock_jump` | Simulated (control plane injects `t0`) | Both forward/backward jumps give `kInvalidTimestamp`, `rejected_bad_timestamp == 2`; in-window triggers are accepted |

The driver cleans up its children, temp directories, and `/dev/shm` objects;
`residual_shm`/`cleanup_ok` in the summary are cleanup evidence. After a manual
run, `ls /dev/shm | grep thyme_fault_` should be empty.

Real clock jumps are not in the matrix because thyme reads `CLOCK_MONOTONIC`,
which the kernel guarantees monotonic and unprivileged processes cannot change.
External jumps enter only through the caller's `timestamp_ns`, which the matrix
covers. A real RTC drill requires a target account with `CAP_SYS_TIME` and
alignment across `/status`, `.fsm`, and `verify`.

## References

- [`observability-dashboards.md`](observability-dashboards.md)
- [`coding-rules.md`](../reference/coding-rules.md)
- [`trigger-config.md`](trigger-config.md)
- [`upgrade.md`](upgrade.md)
- [`../../deploy/README.md`](../../deploy/README.md)
- [`abi-compatibility.md`](../reference/abi-compatibility.md)
- [`as-built-deviations.md`](../reference/as-built-deviations.md)
- [`security/evidence-integrity.md`](../security/evidence-integrity.md)
- [`eval-chains.md`](eval-chains.md)
- [`test-matrix.md`](test-matrix.md)
- [`onboarding.md`](onboarding.md)
