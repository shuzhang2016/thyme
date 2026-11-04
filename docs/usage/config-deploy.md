# Configuration and deployment

This page is a field-operations guide to configuration and deployment: what the configuration file looks like, which keys can be hot-reloaded, what is checked before startup, how systemd starts and restarts the stack, and in what order to upgrade and roll back. The authoritative per-key table is `docs/guides/trigger-config.md` (including implemented / deprecated / rejected / future status); it is not repeated here — only the parts you actually touch during deployment.

## Configuration file and dialect

`trigger_center` reads a minimal INI dialect (`#` comments, one `key = value` per line, comma-separated lists) with no YAML dependency. There is only one rule, but it is hard:

- **Every accepted key must take effect, otherwise it is a parse error.** Unknown sections, unknown keys and illegal values fail with a `line N:` prefix and make `--check-config` exit non-zero. There are no "accepted but ignored" keys.
- `deprecated` spellings still work but print a warning (for example `[continuous].enable`).
- `rejected` keys (`[recorder].use_cgroup`) are a **deliberate parse error**: accepting one would only turn it into a silent no-op. `[channel.*].gop_s` is effective and only applies to the trigger window of `h264_stream`.
- **I/O isolation**: cgroup v2 `io.max` is configured by the deployment side (systemd slice / container runtime), and `trigger_center` does not parse `[recorder].use_cgroup`. If the target machine has no usable `io.max` (or permissions are insufficient), the code falls back to best-effort `ioprio` plus the `[recorder].io_rate_limit_mbps` token bucket; that fallback provides no hard per-package isolation, which deployment documentation must state explicitly.

```bash
# parse + semantic validation (including [continuous] bitrate accounting)
build/tools/apps/trigger_center/trigger_center --check-config \
    --record-manifest /etc/thyme/record_manifest.h /etc/thyme/trigger-center.ini
# print the materialized effective configuration (defaults expanded, channels shows the effective set)
build/tools/apps/trigger_center/trigger_center --dump-config \
    --record-manifest /etc/thyme/record_manifest.h /etc/thyme/trigger-center.ini
# generated base + user policy two-file form; runtime mode passes the two files in the same order
build/tools/apps/trigger_center/trigger_center --check-config \
    --record-manifest /etc/thyme/record_manifest.h \
    /etc/thyme/trigger.base.ini /etc/thyme/trigger.policy.ini
build/tools/apps/trigger_center/trigger_center --dump-config \
    --record-manifest /etc/thyme/record_manifest.h \
    /etc/thyme/trigger.base.ini /etc/thyme/trigger.policy.ini
```

The two-file form is a fixed two-layer composition; there is no generic include or recursive merge. The base file may contain only `[channel.*]`; the policy file may not contain `[channel.*]` or `[chassis]`, and the original parser still validates the remaining sections. After merging, parsing happens once, and defaults, selector and continuous validation are unchanged. The channel `name` values of the base file (defaulting to `shm_name`) form the selector's manifest; an empty base is a strictly empty topology and does not disable validation. Startup and SIGHUP share the same loader, and a reload re-reads both the base and the policy file.

On success, `--check-config` prints `trigger_center: config OK` and exits 0; a configuration error exits 2. `--check-config` also rejects configurations without `[recorder].output_dir` or without any `[channel.*]`.

## Section overview

| Section | What it controls | Typical keys |
| --- | --- | --- |
| `[trigger_center]` | trigger socket, FSM limits, default source, crash threshold | `socket`, `max_concurrent_captures`, `default_source`, `crash_priority` |
| `[security]` | authentication, priority clamping, audit, evidence signing | `allowed_uids`, `allow_root`, `require_peer_pid`, `max_priority_hint`, `audit_log`, `signing_key` |
| `[logging]` | service log files, rotation of non-package logs and the process-level budget | `service_file`, `service_max_bytes`, `service_rotate_keep`, `fsm_max_bytes`, `perf_max_bytes`, `*_rotate_keep`, `total_budget_mb` |
| `[recorder]` | output directory, control socket, pending quota, flush policy, retention | `output_dir`, `pending_bytes_quota`, `flush_policy`, `idle_hold_s`, `disk_quota_mb`, `high_reserved_mb`, `incomplete_policy` |
| `[channel.<shm_name>]` | recorded channel list and per-channel parameters | `kind`, `name`, `rate_hz`, `avg_frame_bytes`, `bitrate_kbps`, `crc` |
| `[source.<name>]` | per-source window, priority, channel filter, quota class | `channels`, `before_s`, `after_s`, `priority`, `mute_s`, `flush_policy`, `quota_class` |
| `[quota.<class>]` | per-class sliding hourly trigger quota | `max_triggers_per_hour` |
| `[continuous]` | continuous-recording slices and whitelist | `auto_arm`, `channels`, `slice_mb`, `slice_s`, `wait_idle`, `rate_check` |
| `[chassis]` | chassis mirror offsets used for idle detection | `page_shm`, `struct_id`, `epb_offset`, `speed_offset`, `speed_threshold` |

The `name` of a `[channel.*]` entry (or the shm suffix with its leading `/` removed) must appear in the graph-generated `RecordManifest`; a channel missing from that list is rejected at startup with `channel '<name>' is not recorded in the graph record manifest`. An empty manifest (a standalone deployment without codegen) disables this check.

## Capability map: where each capability is configured

thyme does not have "one file that controls everything": deployment differences live in the INI, topology/threads/depth in `graph.yaml`, limits and the language subset are fixed at compile time, and a few switches are environment variables.

| Capability | Where / which key | Default | Current state |
| --- | --- | --- | --- |
| per-channel CRC policy | `[channel.<shm>] crc = package \| frame \| off` | by kind (bulk=`package`, mirror=`frame`) | configurable per channel; `off` uses the no-CRC wire semantics (`reserved[0]` bit0 + `crc32=0`) |
| replay protection (trigger) | `[security] allow_legacy_unsequenced` / `allow_legacy_v2` | `false` (v1/v2 rejected) | can only be **turned off**; the 64-entry window and the 5 s tolerance are program constants, not INI keys |
| replay protection (recorder control socket) | — | — | **not yet implemented** (uid-only, no auditing) |
| trigger concurrency | `[trigger_center] max_concurrent_captures` | `1` | `>1` is a parse error (the FSM supports only 1) |
| thread count | `graph.yaml` node count and `threads: N` on event sinks | `threads: 1` | one thread and one health slot per physical worker; `N=1..64`, still bounded in-process by `kAbiMaxThreads` (default 64, overridable at build time within 1..1024 via `-DTHYME_ABI_MAX_THREADS`; the limit is **per process**, not per board) |
| queue / ring depth | `graph.yaml`: `ring_cap`, `depth`, `pool_n` | generation-time defaults | validated at generation time (power of two, limits, admission analysis with `--explain-sched`) |
| single writer | — | — | convention (see code comments); a debug-time check is planned |
| SPMC overwrite semantics | — | — | type semantics, not configurable; frame-loss visibility is planned |
| evidence manifest mode | `[security] signing_key` controls signing only | manifest forced to `full` | `full/metadata/off` tiering and incremental hashing are planned |
| log / perf rotation | `[logging] fsm_* / perf_* / total_budget_mb` | 4 MiB×3 / 64 MiB×3 / 0 | all restart-only |
| resource self-check | environment `THYME_RESOURCE_CHECK` + `--check-resources` | `warn` | most thresholds are environment variables and deployment parameters, not INI |
| mlock | environment `THYME_NO_MLOCK` | unset = lock | coordinated with the `RLIMIT_MEMLOCK` self-check |
| replay producer gate | CLI `thyme-replay --no-gate` | gate held | explicit escape hatch that logs and raises a JSON alert |
| quota / mute / priority | `[source.*]`, `[quota.*]`, `[security] max_priority_hint` | see the tables | hot-reloadable (see the SIGHUP matrix) |
| chassis idle detection | `[chassis]` | none (`demo_idle` must be requested explicitly) | offsets and thresholds vary by vehicle platform |
| language subset / sanitizers | CMake: `THYME_CORE_EMBEDDED`, `THYME_STRICT_WARNINGS`, `THYME_ENABLE_TSAN` | see the build documentation | compile-time, not deployment knobs |

`threads: N` applies only to sinks with `exec: event` and no output edges (N>1), and requires at least N input edges. The generator assigns local input edges to `<id>__s0..` by `ordinal % N`; each shard gets its own objects, thread, health slot and scheduling state, with serial execution within a shard and concurrency across shards. The first version does not support sharded producers, cross-shard output mirrors, cross-shard ordering or joins. The default and `threads: 1` do not change the original generation path. Application static/global state does not become thread-safe just because of sharding; known types with shared constructor resources are rejected.

**Why the split**: the INI is deployment configuration (hot-reloadable or restart-bound), `graph.yaml` is a build input (topology is wired in at generation time), and limits belong to the ABI (changing one requires a version bump). Mixing the three would blur three different kinds of semantics.

**Target state**: `graph.yaml` becomes the single source of truth for topology and generates `record_manifest` and `trigger.base.ini`; the INI keeps only deployment-side differences.

**Verification commands**:

```bash
trigger_center --check-config --record-manifest /etc/thyme/record_manifest.h \
    /etc/thyme/trigger-center.ini   # parse + semantic validation
trigger_center --dump-config --record-manifest /etc/thyme/record_manifest.h \
    /etc/thyme/trigger-center.ini   # effective configuration after materializing defaults
thyme_probe abi --mismatches                                   # ABI pre-check before deployment
```



## Minimal working configuration

The configuration below starts a center that records two channels only on crash-level triggers; it is a good baseline for smoke tests and field triage. The complete template is `tools/apps/trigger_center/trigger_config.minimal.ini`.

```ini
# /etc/thyme/trigger-center.ini
[trigger_center]
socket = /run/thyme/trigger.sock
max_concurrent_captures = 1
default_source = manual
crash_priority = 100

[recorder]
output_dir = /opt/usr/thyme/triggered_data
socket = /run/thyme/recorder.sock
pending_bytes_quota = 67108864
flush_policy = idle
idle_hold_s = 30
io_rate_limit_mbps = 20
disk_quota_mb = 20000
high_reserved_mb = 4000
incomplete_policy = keep

[security]
allowed_uids = 1000
allow_root = false
require_peer_pid = true
max_priority_hint = 1000
audit_log = /var/log/thyme/trigger.audit
audit_max_bytes = 4194304
audit_rotate_keep = 2

[logging]
service_file = /var/log/thyme/trigger-center.log
service_max_bytes = 8388608
service_rotate_keep = 2
fsm_max_bytes = 4194304
fsm_rotate_keep = 2
perf_max_bytes = 67108864
perf_rotate_keep = 2
total_budget_mb = 0

[channel./h264_front]
kind = h264_stream
name = h264_front
bitrate_kbps = 4000

[channel./thyme_recr_can]
kind = mirror_ring
name = can_chassis
rate_hz = 50
avg_frame_bytes = 128

[source.manual]
channels = /h264_front, /thyme_recr_can
before_s = 10
after_s = 5
priority = 10
mute_s = 0
quota_class = manual

[source.crash]
channels = /h264_front, /thyme_recr_can
before_s = 10
after_s = 5
priority = 100
mute_s = 0
flush_policy = realtime
quota_class = safety

[quota.manual]
max_triggers_per_hour = 10

[quota.safety]
max_triggers_per_hour = 0

[continuous]
auto_arm = false
channels = small_struct:*
slice_mb = 512
slice_s = 0
rate_check = warn
```

A few deployment details that are easy to forget:

- **socket paths**: the unit defaults to `PrivateTmp=yes`, so `/tmp` is private to the service. Sockets must live in a shared directory (usually `/run/thyme`); otherwise the process runs, but `triggerctl` and monitoring can never connect.
- **`allowed_uids`**: a non-empty list **replaces** the default "only the center's own euid". When running as root, either put `0` in the list or set `allow_root = true`.
- **`quota_class`** must resolve in `[quota.*]`; a typo fails during `Init` instead of silently becoming "unlimited".
- **`[continuous].channels`** matches against the shm names/channel names of `[channel.*]`; a selector that matches no channel fails outright.

## SIGHUP hot-reload matrix

`trigger_center` treats SIGHUP as a **policy** reload. Any value already bound into the recorder/retention/idle objects at startup is rejected with `reload rejected: … requires restart` — accepting it would only produce the illusion of "config reloaded" while the old value stays in force.

| Section / key | SIGHUP behavior |
| --- | --- |
| `[source.*]` (channels, before/after, priority, mute, quota_class) | takes effect immediately |
| `[quota.*]` | takes effect immediately |
| `[security]` allowlist, root, peer-pid, hint limits, legacy acceptance | takes effect immediately |
| `[trigger_center]` `crash_priority`, `allow_empty_sources`, `default_source` | takes effect immediately |
| `[security].audit_log`, audit rotation/fsync settings, `signing_key` path | startup-only: keeps the old values and warns; rotating the **contents** of the key file takes effect on the next seal |
| `[logging]` rotation limits, generations, `total_budget_mb` | startup-only: writers and budget are bound in `Init` |
| `[trigger_center].socket` | startup-only: the listening socket survives a reload and the path is silently kept |
| all `[recorder]` keys | requires restart |
| `[channel.*]` layout and recording parameters | requires restart |
| `[chassis]` presence/offsets/thresholds | requires restart |
| `[continuous]` geometry (`slice_mb`, `slice_s`, `channels`, `auto_arm`, `wait_idle`, `demo_idle`, `rate_check`) | requires restart |

Operational advice: use `kill -HUP` for `[source.*]` / `[quota.*]` / authentication policy; for `[recorder]`, `[channel.*]`, `[continuous]` and `[logging]`, always stop the stack, edit the files and start it again — do not count on a reload.

## Startup resource self-check

After the recorder's `Init` and before binding any socket, `trigger_center` runs one **read-only** host capacity check (`include/thyme/runtime/resource_check.h`): it only calls `getrlimit()` / `statvfs()`, allocates nothing, throws nothing and opens no files.

```bash
# human-readable summary to stderr; exit 0 pass / 2 configuration error / 3 hard requirement unmet
build/tools/apps/trigger_center/trigger_center --check-resources \
    --record-manifest /etc/thyme/record_manifest.h /etc/thyme/trigger-center.ini

# one JSON line to stdout for deployment-script assertions; the two-file form also accepts a trailing --json
build/tools/apps/trigger_center/trigger_center --check-resources \
    --record-manifest /etc/thyme/record_manifest.h /etc/thyme/trigger-center.ini --json
build/tools/apps/trigger_center/trigger_center --check-resources \
    --record-manifest /etc/thyme/record_manifest.h \
    /etc/thyme/trigger.base.ini /etc/thyme/trigger.policy.ini --json
```

| Check | Requirement computation | fatal | warn |
| --- | --- | --- | --- |
| `RLIMIT_NOFILE` | `channels × files_per_channel + baseline` (default 2 × source count + 64) | soft < requirement | cannot be read (unverifiable) |
| `RLIMIT_MEMLOCK` | only when mlockall is active; planned SHM + 64 MiB headroom | never fatal | soft < requirement, or cannot be read |
| output disk free space | `statvfs(output_dir)` | free < `disk_reserve_bytes` (defaults to `high_reserved_mb`) | free < `disk_quota_mb`, or the path is unreadable |
| `/dev/shm` free space | `statvfs("/dev/shm")` | free < planned segment size | free < planned + reserve (16 MiB by default), or unreadable |

The planned segment size defaults to `8 MiB × recorded source count`: one mirror page is ≤ 2 MiB; PinnedSlot channels are created by the producer and do not count against the center's budget.

All deployment knobs are environment variables:

| Variable | Default | Effect |
| --- | --- | --- |
| `THYME_RESOURCE_CHECK` | `enforce` | `off` skips the check; `warn` only warns and never exits; `enforce` makes fatal conditions exit 3 |
| `THYME_RESOURCE_FILES_PER_CHANNEL` | `2` | files per channel per package (`.raw` + `.index.tbin`) |
| `THYME_RESOURCE_BASELINE_FILES` | `64` | baseline fds for sockets / logs / directories |
| `THYME_RESOURCE_SHM_PLANNED_MB` | `8 × channels` | total SHM planned to be created; deployments heavy on PinnedSlot should set this explicitly |
| `THYME_RESOURCE_SHM_RESERVE_MB` | `16` | `/dev/shm` headroom |
| `THYME_RESOURCE_MEMLOCK_HEADROOM_MB` | `64` | stack/arena headroom for mlockall |
| `THYME_RESOURCE_DISK_RESERVE_MB` | `high_reserved_mb` | hard lower bound on the output disk |
| `THYME_NO_MLOCK` | unset | `1/true/yes/on` disables mlockall; the self-check and `App::Prepare()` share the same parser |

Note that with `THYME_RESOURCE_CHECK=warn` the process still returns 0 even when a `fatal` condition is true (a deliberate degradation switch that prints a warning). `resources.checked: false` in the status means the host never ran the check, **not** that it passed; `fatal: true` folds the top-level `ok` to `false`.

`THYME_NO_MLOCK` responds only to the explicit truthy values `1/true/yes/on` (case-insensitive); `0`, the empty string and anything else do not disable locking. By default `App::Prepare()` calls `mlockall(MCL_CURRENT|MCL_FUTURE)`, locking existing and future mappings, including PinnedSlot SHM and thread stacks; this needs a sufficient `RLIMIT_MEMLOCK` or `CAP_IPC_LOCK`. If `RLIMIT_MEMLOCK` is below 64 MiB, `App::Prepare()` skips `mlockall` and reports `kMemoryMlockFailed`, because a successful `MCL_FUTURE` lock with such a small budget makes later thread-stack and SHM mappings fail with `EAGAIN`. Disabling it explicitly avoids that resident-memory cost, but real-time threads may then jitter on page faults. `mallopt` only tunes allocator arenas/trimming and affects allocator jitter; it is no substitute for memory locking.

A locking or tuning failure does not block startup by default. Deployments must inspect `health.memory` for each role in `/status` (role aggregate plus per-worker `flags/state`) or run `thyme_probe echo <health-page> --json`; `disabled` is an explicit policy, not the same as `mlock_failed`, and does not count into `health.errors`. For bit definitions and state semantics see [runbook](../guides/runbook.md) §7.

## systemd units, target and readiness

The startup order of the produce/consume chain is a hard constraint: the producer must create the SHM pages before the recorder can attach.

```text
local-fs.target, /dev/shm
        │
        ▼
thyme-app.service (producer supervisor, creates the named SHM pages)
        │
        ▼
thyme-trigger-center.service (attaches all sources, binds sockets, arms as configured)
        │
        ├── triggerctl
        └── thyme_probe / thyme-replay (read-only, can start at any time)
```

| Unit | Role | Key directives |
| --- | --- | --- |
| `thyme-app.service` | producer supervisor | `After=local-fs.target`, `Restart=on-failure`, `TimeoutStopSec=20s` |
| `thyme-trigger-center.service` | recorder | `After=thyme-app.service`, `Wants=`, `PartOf=thyme-app.service` |
| `thyme-acquisition.target` | group switch | `Wants=` both, `After=` both |

Installing:

```bash
sudo install -Dm644 deploy/systemd/thyme-trigger-center.service \
  /etc/systemd/system/thyme-trigger-center.service
sudo install -Dm644 deploy/systemd/thyme-acquisition.target \
  /etc/systemd/system/thyme-acquisition.target
sudo install -Dm644 deploy/thyme-trigger-center.env.example \
  /etc/thyme/thyme-trigger-center.env
sudo systemctl daemon-reload
sudo systemctl enable --now thyme-acquisition.target
```

`PartOf=thyme-app.service` means stopping or restarting the producer supervisor also stops the recorder. That is deliberate: a supervisor restart creates a new session, and letting the old recorder keep mapping the previous generation's pages would just "silently record nothing".

### Readiness

`Type=notify` + `NotifyAccess=main`. `READY=1` is sent only after all of the following hold:

1. `ExecStartPre`'s `--check-config` passed;
2. `ExecStartPre`'s `--check-resources` passed (it runs again after the output directory is created);
3. every configured source attached successfully;
4. both sockets are bound, and with `auto_arm=true` continuous recording is armed.

Until then the unit stays `activating` and systemd holds back units that depend on it. Sending READY early would let clients send requests to a center that still rejects triggers. `TimeoutStartSec=60s` covers the case where the producer never publishes a page.

### Readiness is not health

Once the unit is active, still check the data path:

```bash
systemctl is-active thyme-trigger-center.service
systemctl show thyme-trigger-center.service -p ActiveState -p SubState -p WatchdogUSec
build/tools/apps/trigger_center/triggerctl status --json --socket /run/thyme/recorder.sock
build/tools/inspect/thyme_probe/thyme_probe list --json
build/tools/inspect/thyme_probe/thyme_probe hz /h264_front
```

`triggerctl status` proves the control socket answers; `thyme_probe hz/watch` prove the producer is actually publishing. `trigger_center` exposes no `/status` JSON endpoint; the control socket plus probe are the current health surface.

### Restarts and the watchdog

| Event | Handled by | Result |
| --- | --- | --- |
| a producer role exits/hangs | the role supervisor inside `thyme-app` | only that role is restarted; session and recorder are left alone |
| producer supervisor exits non-zero | systemd `Restart=on-failure` | new session; `PartOf=` also restarts the recorder |
| producer supervisor exits normally | nobody | planned stop; the recorder stops via `PartOf=` |
| `trigger_center` exits non-zero | systemd `Restart=on-failure`, `RestartSec=5s` | re-attaches to the current producer pages |
| `trigger_center` exits normally | nobody | planned stop; in-flight captures are sealed first |
| main loop stops feeding `WATCHDOG=1` | systemd `WatchdogSec=30s` | SIGABRT (core kept), then restart per `on-failure` |
| startup storm | `StartLimitBurst=5` / `StartLimitIntervalSec=300` | the unit stays failed until an operator intervenes |

Do not add `Restart=always`: a maintenance `systemctl stop` must stay stopped. Nor should you restart the recorder just because a producer role failed — the role supervisor already handles role-level recovery, and restarting the recorder only throws away in-flight windows during a recoverable fault.

`KillMode=mixed` + `KillSignal=SIGTERM`: SIGTERM goes to the main process only, giving it a chance to seal the current continuous slice; once `TimeoutStopSec=30s` elapses, systemd escalates the whole cgroup to SIGKILL.

## Upgrade and rollback

thyme's cross-process objects embed their version in the page header, and on a version mismatch a new peer **refuses explicitly** instead of reading a downgraded layout. Apart from `LatestSlot` (whose page has no version field), mixed-version runs fail at attach/open; `LatestSlot` is only safe with "stop producer → upgrade everything → recreate SHM". Upgrading is therefore a **whole-session** operation.

### ABI pre-check before upgrading

```bash
# constant matrix of this build (deterministic JSON; deployment scripts can diff a golden copy)
build/tools/inspect/thyme_probe/thyme_probe abi --matrix

# host page pre-check: only the current session prefixes
build/tools/inspect/thyme_probe/thyme_probe abi \
    --prefix thyme_ --prefix c9_ \
    --manifest /etc/thyme/probe.manifest

# machine-readable
build/tools/inspect/thyme_probe/thyme_probe abi --json
build/tools/inspect/thyme_probe/thyme_probe abi --mismatches
```

Exit codes: `0` everything matches (or already the target version), `2` no thyme page found (host not running, a normal upgrade), `3` at least one page has a mismatched version/geometry (the whole stack must be stopped and replaced).

Report fields are `name/kind/object/expected/actual/checked/compatible/note`. Probe scans all of `/dev/shm` and therefore includes stale segments from the previous session; mismatches on old segments are expected. `LatestSlot` always shows up as `unverifiable` — the page has no version word, so the manifest can only state sizes, not the writer version.

### Upgrade order

1. **Pre-check**: save the `abi --mismatches` list; confirm that external drivers (cloud/image/h264) are rebuilt from the same tree.
2. **Stop the producer** (a single-unit deployment stops all roles here).
3. **Stop the consumer**.
4. **Replace the binaries/package tree**: every peer must come from the same build.
5. **Handle SHM**: do nothing by default — after restart the producer creates new segments under its new session pid. Only segments listed by `abi --mismatches` and proven by `/proc` to have no mapping process may be cleaned up explicitly. **Never** blindly delete `/dev/shm/thyme_*`.
6. **Start the producer first, then the consumer**.
7. **Verify**: `abi`, `thyme_probe list/info`, `thyme-replay integrity`, `triggerctl status`, key error logs.

The driver script `tools/ops/upgrade/thyme_upgrade.sh` defaults to a dry run and only acts with `--apply`:

```bash
# dry run: prints only; no stop, copy, unlink or package deletion
tools/ops/upgrade/thyme_upgrade.sh \
    --package /opt/packages/thyme-1.2.3 \
    --prefix /opt/thyme \
    --backup-dir /var/backups/thyme/1.2.2 \
    --producer-unit thyme-producers --consumer-unit thyme-consumers \
    --probe build/tools/inspect/thyme_probe/thyme_probe \
    --manifest /etc/thyme/probe.manifest \
    --purge-shm

# actually execute
tools/ops/upgrade/thyme_upgrade.sh ...same flags... --apply
```

A single-unit deployment omits `--producer-unit/--consumer-unit` and uses `--unit thyme-app` (the default) to stop and start all roles at once.

| Option | Effect |
| --- | --- |
| `--apply` | actually execute; without it only the plan is printed |
| `--check-only` | run only the pre-check, change nothing |
| `--matrix` | print this build's ABI constant matrix as JSON |
| `--package DIR` / `--rollback DIR` | upgrade source / rollback source (mutually exclusive) |
| `--backup-dir DIR` | `cp -a` the current prefix before installing |
| `--purge-shm` | delete only pages the pre-check judged incompatible and `/proc` shows unmapped (with `--apply` only) |
| `--allow-downgrade` | continue when a page is newer than the build (rejected by default; rollback mode allows it automatically) |
| `--unit` / `--producer-unit` / `--consumer-unit` | stop/start order |
| `--smoke-cmd CMD` | smoke command to run after startup; failure makes the script exit 3 |

Script exit codes: `0` success/dry run complete, `1` usage error, `2` pre-check refused, `3` a command failed, `4` incompatible pages are still mapped by **live processes** after startup.

### Rollback

Rollback is just as holistic as the upgrade:

1. stop the producer, then the consumer;
2. `--rollback <backup-dir>` overwrites the prefix with the backup tree;
3. for a cross-ABI rollback, re-run `abi --mismatches` with the **restored** `thyme_probe` and use `--purge-shm` as needed;
4. start the producer first, then the consumer;
5. verify with the restored probe.

Rollback mode does not reject "page newer than the restored build" as a downgrade, so `--allow-downgrade` is unnecessary. Packages recorded during the rolling window remain readable across an ABI rollback: the `.tbin` reader accepts v1..v4, and `integrity.manifest` is a v1 read-only format.

Upgrade notes:

- `[logging]` keys are startup-only; changes take effect through this restart, never through SIGHUP.
- The v4 pinned-slot channel's producer gate sits on the core write path, and older drivers cannot write v4 channels; external drivers must be rebuilt from the same tree.
- Do not use `force_replace` / `thyme-replay --force` as an upgrade mechanism: they recreate pages without migrating data and without bypassing a live gate.

## Common failures → runbook

| Symptom | Look here first | First command |
| --- | --- | --- |
| write failures, non-crash triggers rejected with `quota_exceeded` | runbook §1, disk full (ENOSPC) | `df -h <output_dir>`; `ls <output_dir> \| grep '\.incomplete$'` |
| `/dev/shm` keeps growing, probe reports stale pages | runbook §2, SHM leaks | `ls -l /dev/shm \| grep -E 'thyme_\|c9_'` |
| many triggers `muted` / high relay inflight | runbook §3, trigger storms | `grep decision= /var/log/thyme/trigger.audit \| tail` |
| roles `waiting_backoff` / `gave_up`, recorder restarts repeatedly | runbook §4, crash recovery | `systemctl status thyme-trigger-center`; `journalctl -u ... -n 100` |
| log flooding or dropped lines | runbook §5, service log level, rate limiting and rotation | `journalctl -u ... \| grep suppressed=` |
| incompatible pages remain after an upgrade | runbook §6, upgrade/rollback | `thyme_probe abi --mismatches` |
| startup fails immediately, complaining about resources | runbook §7, startup resource self-check | `trigger_center --check-resources <ini> --json` |
| `verify` / `integrity` fails | runbook §8, package verification | `thyme-replay verify <pkg>`; `thyme-replay integrity <pkg>` |
| need to reproduce a crash/ENOSPC/clock jump | runbook §9, fault matrix | `python3 tools/ops/fault_matrix/fault_matrix.py --only disk_full` |

Three hard rules for troubleshooting:

1. Never delete packages that are being written, `.incomplete` packages, or crash packages under `high/` (unless you have confirmed they were already exported).
2. Never blindly delete `/dev/shm`; a stale segment is reclaimed only after the owner has proven it orphaned.
3. Do not "try restarting just one role"; a mixed-version, cross-ABI run only turns a visible failure into data corruption.

## Related documents

- [`acquisition.md`](acquisition.md) — how `[continuous]` actually behaves, slices and retention
- [`triggered-acquisition.md`](triggered-acquisition.md) — how `[source.*]` / `[quota.*]` take effect inside the FSM
- [`perf.md`](perf.md) — `[logging]` budgets, perf dumps and metrics export
- [`probe.md`](probe.md) — page-level diagnostics beyond `thyme_probe abi`
