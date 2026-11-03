# Observability views and alerts

This page explains where thyme's metrics come from, how they become data that
Prometheus/Grafana can display, how to import the Grafana dashboard and the
Foxglove layout, how alerts are wired, and **which fields are not exported at
all today** (that part must be stated honestly; it must not be papered over
with metrics that only "look like" the real thing).

Supporting artifacts:

| Artifact | Path | Form |
| --- | --- | --- |
| Prometheus textfile bridge | [`../tools/inspect/thyme_metrics_exporter/thyme_metrics_exporter.py`](../../tools/inspect/thyme_metrics_exporter/thyme_metrics_exporter.py) | Python 3 sidecar, `--once` / `--watch SEC` |
| Exporter tests | [`../tools/inspect/thyme_metrics_exporter/test_thyme_metrics_exporter.py`](../../tools/inspect/thyme_metrics_exporter/test_thyme_metrics_exporter.py) | `unittest`, fixtures + goldens + rule contracts |
| Grafana dashboard | [`../dashboards/grafana/thyme-overview.json`](../../dashboards/grafana/thyme-overview.json) | 12 panels, provisionable |
| Foxglove layout | [`../dashboards/foxglove/thyme-layout.json`](../../dashboards/foxglove/thyme-layout.json) | 5 panels, aimed at MCAP replay |
| Alert rules | [`../dashboards/alerts/thyme-alerts.yml`](../../dashboards/alerts/thyme-alerts.yml) | Prometheus rule group, 29 live rules |

**Key fact: thyme has no Prometheus exporter and no HTTP metrics endpoint.**
`StatusServer` is read-only JSON over AF_UNIX, perf dumps are binary, and
`.fsm`/`.audit` are text files. This design therefore takes the honest
approach: a real sidecar translates the existing interfaces into the
Prometheus text format, rather than inventing a data source for the dashboard
that does not exist.

## 1. Where the metrics come from

### 1.1 perf dump

Binary perf-dump format; block types are defined in
[`../include/thyme/runtime/perf_log.h`](../../include/thyme/runtime/perf_log.h):

| type | Content | Record size | Timestamp unit |
| --- | --- | --- | --- |
| 1 | samples: one record per sample (`ts/lat/id/io`) | 10 B | `ts` = mono>>14 (16.384 µs) |
| 2 | percentile: p50/p95/p99/max per id | 20 B | `window_end_ts`, same as above |
| 3 | work: batch duration + in/out frame nibble + flags | 10 B | same as above |
| 4 | epoch: u32 timeline wrap base | — | one block per dump window |
| 5 | sys: `ProcSampler` process counter delta + instantaneous queue depth | 10 B | same as above |
| 6 | histogram: fixed-bucket histogram | 1,340 B | `end_ts`, same as above |
| 7 | record manifest: `version u16 / reserved u16 / hash u64`, one block per file generation | 12 B | none (build-wide) |
| 8 | series name: mapping from type-5 `series` to a stable name | 32 B | none |
| 9 | node budget: declared period / dispatch deadline (ns) | 16 B | none |

A few things you must know (all documented in the header comments):

- `lat` is in 2^13 ns units, clamped at 0xFFFFFF (≈137 s); `work.dur` is in
  2^10 ns units; `sys.value` is a u32 clamp.
- **Only when a type-4 epoch block is present** can the u32 timeline be
  expanded unambiguously (it wraps every 19.5 h). `PerfLogDumper`
  ([`../include/thyme/runtime/perf_dump.h`](../../include/thyme/runtime/perf_dump.h))
  writes an epoch before each window; generated MP applications that flush once
  at **exit** via `T9_PERF_LOG=<prefix>` produce two kinds of files
  (`<prefix>_node_<role>_<pid>.bin`, `<prefix>_edge_<role>_<pid>.bin`) with no
  epoch block. Those are "one-shot legacy dumps": `thyme_log_merge` only
  carries the low-word wrap for them (see the note in `log_merge.h`), so it
  misjudges data that spans a 19.5 h wrap.
- type-5 `series` 0..5 are the fixed process counters (utime/stime/minflt/
  majflt/vol_ctx/nonvol_ctx); 6..15 are runtime resource series; `series >= 16`
  are probes whose meaning depends on registration order. The type-8 metadata
  in each file generation carries the series→name mapping; `thyme_log_merge
  --json` adds `series_name` to sys rows. type-9 period/deadline are added to
  the JSON and `--hist` rows as well.

  Resource series (all written into the same type-5 block, sampled at 1 Hz):

  | series | Name | Unit | Meaning |
  |---|---|---|---|
  | 6 | `rss_kb` | KiB | instantaneous VmRSS |
  | 7 | `vsize_kb` | KiB | instantaneous VmSize |
  | 8 | `threads` | count | instantaneous NumThreads |
  | 9 | `fd_count` | count | instantaneous number of `/proc/self/fd` entries |
  | 10 | `fd_limit` | count | `RLIMIT_NOFILE` soft limit (4294967295 = unlimited) |
  | 11 | `io_read_kb` | KiB | `read_bytes` delta over the interval |
  | 12 | `io_write_kb` | KiB | `write_bytes` delta over the interval |
  | 13 | `io_read_syscalls` | count | `syscr` delta over the interval |
  | 14 | `io_write_syscalls` | count | `syscw` delta over the interval |
  | 15 | `cpu_delay_ms` | ms | delayacct blocked-wait delta over the interval |

  6..10 are instantaneous values, 11..15 are per-interval deltas (`sys.value`
  is a u32, so a row is not a cumulative quantity). Reads of `/proc/self/io`
  and `/proc/self/fd` are best-effort: when procfs is restricted the
  corresponding series degrades to 0 while the CPU/page-fault series keep
  working.

  **Optional sources are off by default**; names are only registered and
  records only written when `THYME_PROC_EXT` requests them. series 63 is the
  capability bitmask `ext_caps` (1 cgroup, 2 psi, 4 disk, 8 net, 16 threads),
  64–80 are cgroup/PSI/host block-device/network fields, and 96–103 are the CPU
  time of registered threads. For units and v1/v2 differences see the "Optional
  sources" section of [`../usage/perf.md`](../usage/perf.md); `ext_caps` exists
  so offline analysis can distinguish "not requested", "not supported by the
  kernel" and "zero this interval".

The offline consumer is
[`../tools/inspect/thyme_log_merge/log_merge.h`](../../tools/inspect/thyme_log_merge/log_merge.h):

```bash
# Timeline (multi-process .fsm + perf dump merged by absolute mono_ns)
build/tools/inspect/thyme_log_merge/thyme_log_merge \
    /var/log/thyme/perf_node_*.bin /tmp/thyme_trigger.sock.fsm --json
# Histogram merge (p50/p90/p99/p999/max per (node, series))
build/tools/inspect/thyme_log_merge/thyme_log_merge \
    /var/log/thyme/perf_node_*.bin --hist --json > /run/thyme/hist.jsonl
# Process/resource series (only kind:"sys" rows, fed to exporter --sys-json)
build/tools/inspect/thyme_log_merge/thyme_log_merge \
    /var/log/thyme/perf_node_*.bin --sys --json > /run/thyme/sys.jsonl
```

Row format of `--hist --json` (`HistSink::Print` in `log_merge.cpp`):

```json
{"node":3,"series":0,"n":18400,"p50_ns":8200,"p90_ns":16400,"p99_ns":32700,"p999_ns":65500,"max_ns":91234,"period_ns":10000000,"deadline_ns":8000000}
```

`node` is the node id passed to the flush, and `series` is a
`PerfSampler::kHistSeries*` value: 0 latency, 1 edge latency, 2 callback work.
Percentiles are **bucket midpoints**, with +6.25%/−5.56% error; `max_ns` is
exact.

Note where `node` comes from
(`tools/gen/gen_graph/scaffold/src/mp/mp_roles.cpp`): `_node_` files use the
graph node index, but **`_edge_` files always pass node=0** — per-edge
attribution lives in the sampling record's `id` byte, and type-6 histograms
deliberately do not keep `id` (see the `kHistSeriesEdgeLatency` comment in
`perf_log.h`). A merged edge-latency histogram therefore has a single row
`node=0, series=1`; read it together with `series_kind` on the dashboard and do
not drill down by node.

### 1.2 Status JSON

`StatusServer` answers a single HTTP request over AF_UNIX `SOCK_STREAM`
([`../include/thyme/app/status_server.h`](../../include/thyme/app/status_server.h)):

```bash
curl --unix-socket /run/thyme/status.sock http://localhost/status
```

Returns (field names follow the serialisation in `src/app/status_server.cpp`):

```json
{"ok":true,"session_id":"c9_4242","build":"thyme 0.1.0","uptime_ms":184000,
 "role_count":2,
 "roles":[{"id":"perception_0","role":"perception","state":"running","pid":4301,
           "restarts_attempted":1,"restarts_succeeded":1,
           "last_exit_code":0,"last_term_signal":15,
           "stall_age_ms":12,"loops":1840,
           "health":{"alive":18400,"deadline_misses":3,"drops":7,
                     "errors":0,"sched_mismatches":1,
                     "error_codes":{"camera":0,"lidar":0},
                     "drop_counts":{"camera":5,"lidar":2},
                     "memory":{"flags":1,"state":"disabled","workers":{
                       "camera":{"flags":1,"state":"disabled"},
                       "lidar":{"flags":1,"state":"disabled"}}}}}],
 "recorder":{"present":true,"trigger_relay_sent":120,
             "trigger_relay_debounced":4,"trigger_req_seq":120,
             "trigger_ack_seq":118},
 "logging":{"budget_limit_bytes":268435456,"budget_used_bytes":12582912,
            "budget_drops":3,"rotations":17},
 "resources":{"checked":true,"fatal":false,"warnings":1,
              "nofile":{"soft":524288,"required":130},
              "shm":{"free_bytes":3817816064,"required_bytes":33554432},
              "disk":{"free_bytes":82400000000,"required_bytes":20971520}}}
```

`state` is a string (`not_started/running/terminating/waiting_backoff/gave_up/
stopped/unknown`) mapping to `MpRoleState` 0..6. `health.*` aggregates
[`../include/thyme/runtime/health_slot.h`](../../include/thyme/runtime/health_slot.h):
`alive_counter`, `deadline_miss_cnt`, `drop_cnt`, `error_code`,
`sched_mismatch_cnt`. `errors` is the **instantaneous** number of slots with a
non-zero `error_code` (recounted on every publish in
`src/app/mp_supervisor.cpp:703`) and is exported as the
`thyme_role_error_slots` gauge; the other four are cumulative counters.
`error_codes` additionally gives each graph node's actual error code, exported
as `thyme_health_slot_error_code`; `drop_counts` gives each consumer edge's
`HealthSlot::drop_cnt`, exported as `thyme_edge_drops_total` with an `edge`
label. `memory.flags` is the bitwise OR of the worker `memory_flags`, while
`memory.workers` keeps per-worker values; `state` distinguishes
`ok/disabled/degraded/unsupported/mixed`. bit0 is an explicit disable, bit1/2
are mlock/mallopt failures, bit3 is unsupported. `disabled` and these failure
flags are not counted in `errors`, which still counts only non-zero
`error_code`; whether memory hardening meets deployment requirements must be
checked via `memory` separately.

**Resource self-check:** `resources` is a one-time read-only host capacity check
at startup (fd / memlock / output disk / `/dev/shm`) that is re-sent with every
tick's snapshot; `checked:false` means the host did not run the check (not that
it passed), and `fatal:true` turns the top-level `ok` into `false`. `/health`
remains the role liveness probe (its `{"ok":...}` shape is unchanged).
Thresholds, environment-variable knobs and remediation are in
[`runbook.md`](runbook.md) §7.

**`deadline_miss_cnt` is the overrun counter**: a cyclic thread calls
`MarkDeadlineMiss()` when `work > period`, and an event thread when `work >
dispatch_deadline` (`src/runtime/cyclic_thread.cpp:230`,
`src/runtime/event_thread.cpp:309`). The reason for the miss (long callback /
queue backlog / os preemption) is exported through the perf type-5 + type-8
`series_name`.

The `/status` response limit is `kMaxResponseBytes = 16384`, roles are capped at
16, and clients are capped at 4 concurrent connections. The exporter sends a
single `GET /status`; it does not poll repeatedly.

### 1.3 SHM pages (`thyme_probe`)

```bash
build/tools/inspect/thyme_probe/thyme_probe list --json
build/tools/inspect/thyme_probe/thyme_probe info /h264_front --json
```

JSON fields (the `printf` template in `tools/inspect/thyme_probe/main.cpp`):
`name/kind/abi_version/compatible/slots/slot_bytes/stride/region_bytes/
head/tail/epoch/drop_count/skip_count/steal_count/session_id/frame_shaped/
geometry_source/note`. Fields that do not apply to the page are `null`
(`kNone` on the C++ side).

- `drop_count`: the container's own failure count (SPSC `push_fail`, PinnedSlot
  `dropped_oversize`, record-mirror drops).
- `skip_count` / `steal_count`: **from the `<name>.lease` lease table**, namely
  `pinned_skip` / `lease_steal`; `null` for pages without a lease table.
- `compatible=false`: magic/version/geometry mismatch; `note` gives the reason
  (see [`abi-compatibility.md`](../reference/abi-compatibility.md)).

`thyme_probe` is **read-only**: it subscribes to nothing, advances no cursor and
takes no lease.

### 1.4 audit and FSM logs

`.audit` (see
[`../include/thyme/record/trigger_audit.h`](../../include/thyme/record/trigger_audit.h))
writes one `key=value` line per request, with a fixed field order:

```
ts=<RFC3339> ts_ns=<u64> mono_ns=<u64> pid= uid= req=trigger|cancel|wait|preempt
proto= seq= src= hint_raw= hint_eff= clamp= decision=accepted|rejected
reason= code= trigger_id= quota_override= by=
```

`decision` is `preempted` on a preemption-execution record. The file rotates by
`audit_max_bytes` (`<path>.1..N`), so **the line count is not a monotonic
counter**.

`.fsm` is JSONL, one line per event:
`{"ts":<mono_ns>,"id":<trigger_id>,"from":<state>,"to":<state>,"why":<reason>}`.
It is a 256-entry ring appended once per second; since the logging rework every
record is a single unbuffered `write()` (no more `fflush`), and the file
rotates to `<socket>.fsm.1..N` by `[logging].fsm_max_bytes`. The meaning of
`why` is tabulated in [`runbook.md`](runbook.md) §3. The `ts` field lets
`thyme_log_merge` place these lines on the same timeline as perf dumps; each
rotated `.1..N` file is a complete JSONL file and can be passed to
`thyme_log_merge` as an independent input.

### 1.5 Disk and SHM usage

thyme itself **does not export** filesystem usage: ENOSPC shows up only as
`trigger_center` stderr lines (`disk full during drain ...`), as
`code=quota_exceeded` in the audit, and as `disk_errors` in the drain-statistics
record at the end of the package's `meta.tbin`. The bridge therefore measures
`[recorder].output_dir` and `/dev/shm` directly with `statvfs()`, which is the
same number as `df -h` / `ls -l /dev/shm` in runbook §1/§2.

Status additionally exposes the **per-process** log budget and rotation
counters (`logging.{budget_limit_bytes,budget_used_bytes,budget_drops,rotations}`,
see `[logging]` in [`trigger-config.md`](trigger-config.md)). This is a
process-level budget (audit + `.fsm` + perf), **not** a filesystem quota; the
package directory is still bounded by `[recorder].disk_quota_mb`. For the disk
upper bound of log files see the "log-surface disk upper bound" table in
runbook §1.

## 2. How the bridge works

`tools/inspect/thyme_metrics_exporter/thyme_metrics_exporter.py` only does
"read an existing interface → render text format"; it makes no guesses beyond
knowledge of thyme's internals:

| Argument | Reads | Output |
| --- | --- | --- |
| `--status PATH` (default `/run/thyme/status.sock`) | AF_UNIX `GET /status`; `file:PATH` reads a saved snapshot | `thyme_status_*`, `thyme_role_*`, `thyme_health_slot_error_code`, `thyme_trigger_relay_*` |
| `--recorder-status PATH` | `status-json` from the recorder control socket; `file:PATH` reads a saved snapshot | recorder Stats, CenterStats drops, `thyme_recorder_status_up` |
| `--probe FILE` / `--probe-bin PATH` | output file of `thyme_probe list --json`, or runs the binary directly each cycle | `thyme_shm_page_*`, `thyme_probe_up` |
| `--hist FILE` | JSONL from `thyme_log_merge ... --hist --json` | `thyme_perf_latency_*`, `thyme_perf_samples`, `thyme_hist_up` |
| `--sys-json FILE` | JSONL from `thyme_log_merge ... --sys --json` | `thyme_process_*` (RSS/VSize/threads/fd/IO/CPU delay), `thyme_process_probe{series=...}`, `thyme_process_sys_up` |
| `--audit FILE` | audit file | `thyme_trigger_audit_records` / `_unparsed_lines` / `_decisions`, `thyme_audit_up` |
| `--disk PATH` | `statvfs(PATH)` | `thyme_filesystem_{size,avail}_bytes`, `_used_percent` |
| `--shm-dir PATH` | `statvfs` + `scandir` | `thyme_shm_{size,avail}_bytes`, `_used_percent`, `thyme_shm_objects` |

Behaviour contract:

- **A single failing source does not break the snapshot**: when the status
  socket cannot be read, the exporter still writes `thyme_status_up 0`, prints
  one reason line to stderr, and exits 0. That way the textfile collector
  always holds a complete snapshot that can be alerted on, instead of the file
  disappearing.
- `*_per_interval` values from `--sys-json` are **the delta of the last
  sampling interval**, not cumulative counters: do not wrap them in `rate()`;
  use the gauge itself or `avg_over_time`. Offline entries take the latest row
  (by `ts`), split by `node` for multiple processes.
- `--out PATH` writes `PATH.tmp` and replaces it atomically with
  `os.replace()`, so a scrape never sees a half-written file; without `--out`
  the output goes to stdout.
- Exit codes: 0 snapshot produced (a failing source only shows in `*_up`), 2
  argument error, 3 failed to write the output file.
- Every snapshot is a full rewrite with no incremental state; the exporter is
  therefore stateless and can be restarted at any time.

Run it once by hand (no service required):

```bash
python3 tools/inspect/thyme_metrics_exporter/thyme_metrics_exporter.py \
    --status file:/tmp/status.json \
    --recorder-status file:/tmp/recorder-status.json \
    --probe /tmp/probe.json --hist /tmp/hist.jsonl \
    --audit /tmp/thyme_trigger.sock.audit \
    --disk /opt/usr/triggered_data --shm-dir /dev/shm --once
```

### 2.1 Wiring the node_exporter textfile collector

```ini
# node_exporter startup flag (files must end in .prom, and the directory must
# contain only exporter output)
--collector.textfile.directory=/var/lib/node_exporter/textfile
```

```bash
# One-shot / driven by a timer
python3 tools/inspect/thyme_metrics_exporter/thyme_metrics_exporter.py \
    --recorder-status /tmp/thyme_recorder.sock \
    --probe-bin build/tools/inspect/thyme_probe/thyme_probe \
    --hist /run/thyme/hist.jsonl \
    --audit /tmp/thyme_trigger.sock.audit \
    --disk /opt/usr/triggered_data --shm-dir /dev/shm \
    --out /var/lib/node_exporter/textfile/thyme.prom --once
```

```ini
# /etc/systemd/system/thyme-metrics.service — watch mode, paced inside the process
[Unit]
Description=thyme Prometheus textfile bridge
After=thyme-app.service

[Service]
User=thyme                      # must match the status socket euid (0600)
ExecStart=/usr/bin/python3 /opt/thyme/tools/inspect/thyme_metrics_exporter/thyme_metrics_exporter.py \
  --recorder-status /tmp/thyme_recorder.sock \
  --probe-bin /opt/thyme/build/tools/inspect/thyme_probe/thyme_probe \
  --hist /run/thyme/hist.jsonl \
  --audit /tmp/thyme_trigger.sock.audit \
  --disk /opt/usr/triggered_data --shm-dir /dev/shm \
  --out /var/lib/node_exporter/textfile/thyme.prom --watch 5
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

`hist.jsonl` is not a live stream: a timer periodically runs `thyme_log_merge
... --hist --json` to overwrite it (for example, scan the last 15 min of dumps
every minute). The exporter only translates. **Do not** configure the exporter
to read perf dumps directly through `--hist` — it does not parse the binary
perf format, and `thyme_log_merge` owns the only parser (which avoids two
implementations of the format drifting apart).

### 2.2 Wiring Prometheus

```yaml
scrape_configs:
  - job_name: node
    static_configs:
      - targets: ["localhost:9100"]   # node_exporter + textfile collector
```

Textfile-collector metrics share the target with node_exporter's own metrics,
so no extra configuration is needed; the `--watch 5` cadence determines metric
freshness (`thyme_status_up` goes stale if the exporter stalls, and
`ThymeExporterStale` catches that with `absent()`).

## 3. Grafana dashboard

### 3.1 Import

UI: **Dashboards → New → Import → Upload JSON file** →
[`../dashboards/grafana/thyme-overview.json`](../../dashboards/grafana/thyme-overview.json)
→ choose the Prometheus data source (template variable `ds`, type datasource,
query `prometheus`). After import the `uid` is fixed to `thyme-overview`.

Provisioning (recommended; ships with the deployment):

```yaml
# /etc/grafana/provisioning/dashboards/thyme.yaml
apiVersion: 1
providers:
  - name: thyme
    folder: thyme
    type: file
    options:
      path: /opt/thyme/dashboards/grafana
```

Panel overview (aligned with alerts and the runbook):

| panel | Query | Runbook |
| --- | --- | --- |
| Status endpoint | `thyme_status_up`, `thyme_status_healthy` | §4 |
| Role state | `thyme_role_state{role=~"$role"}` | §4 |
| HealthSlot drops / s | `rate(thyme_role_drops_total{...}[$__rate_interval])` | §1/§3 |
| Deadline miss / overrun / s | `rate(thyme_role_deadline_misses_total{...}[...])` | `scheduling-model.md` §4 |
| PinnedSlot pinned_skip / lease_steal | `rate(thyme_shm_page_{pinned_skip,lease_steal}_total[...])` | §1/§2 |
| SHM container drops | `rate(thyme_shm_page_drops_total[...])` | §3 |
| Latency p99 / max | `thyme_perf_latency_quantile_ns{quantile="0.99"}`, `..._max_ns` | — (snapshot) |
| Recorder disk used % | `thyme_filesystem_used_percent` (85/95 threshold lines) | §1 |
| /dev/shm pressure | `thyme_shm_used_percent`, `thyme_shm_objects` | §2 |
| Trigger audit decisions | `sum by (decision, code) (thyme_trigger_audit_decisions)` | §3 |
| Trigger relay inflight | `thyme_trigger_relay_inflight` | §4 |
| Role restarts | `increase(thyme_role_restarts_*_total[...])` | §4 |

Verification scope: JSON syntax and "queries reference only metrics the
exporter really produces" are guaranteed by
`RuleContractTests.test_dashboard_queries_stay_off_unexported_metrics` in
`test_thyme_metrics_exporter`; there is **no** import regression test against a
real Grafana instance (see §7).

## 4. Wiring alerts

```yaml
# prometheus.yml
rule_files:
  - /opt/thyme/dashboards/alerts/thyme-alerts.yml
```

`thyme.runtime` contains 29 rules that use only metrics the exporter really
produces; the three former pending-export rules are now enabled, plus an SLO
rule for a node p99 above its dispatch deadline.

Every rule has `expr` + `for` + `labels.severity` (critical/warning) +
`labels.component` + `annotations.runbook` (pointing at the section number of a
document in this repository). Thresholds are **starting points derived from
design invariants, not field baselines**: `0.01/s` (about once per 100 s) is
the floor for "any sustained drop is a defect", and `1/s`, `0.5/s` are
operations-level red lines; adjust them against the recorded performance
baseline after rollout.

Alert-to-runbook mapping:

| Alert | Runbook |
| --- | --- |
| `ThymeDiskFull` / `ThymeDiskPressure` / `ThymeHealthDropsCritical` | §1 Disk full (ENOSPC) |
| `ThymeShmPressure` / `ThymeShmObjectsGrowth` / `ThymeLeaseSteal` / `ThymeShmPageIncompatible` | §2 SHM leak |
| `ThymeTriggerStorm` / `ThymeHealthDrops` / `ThymePinnedSkip` / `ThymeShmPageDrops` | §3 Trigger storm |
| `ThymeStatusEndpointDown` / `ThymeRoleGaveUp` / `ThymeRoleStalled` / `ThymeRoleRestartStorm` / `ThymeOverrunCritical` | §4 Crash recovery |
| `ThymeOverrun` / `ThymeSchedMismatch` | [`scheduling-model.md`](../reference/scheduling-model.md) §4/§5 |
| `ThymeExporterStale` | This page §2 |

`ThymeAuditTailTorn` uses `thyme_trigger_audit_unparsed_lines` (lines in the
audit file without `req=`, i.e. a tail truncated by a crash). It is **not**
`audit_drops` — the latter is `CenterStats::audit_drops`, see §7.

## 5. Foxglove layout

### 5.1 What can be exported today

| Data | Tool | Format | Opens directly in Foxglove |
| --- | --- | --- | --- |
| Recording package (`.tbin` → MCAP) | `build/tools/replay/mcap_export/thyme_bin_to_mcap` | MCAP (`thyme/<struct>`, `thyme/gap`, `thyme/trigger`, `thyme/session`) | **Yes** (Open local file) |
| perf dump | `thyme_log_merge ... --json` | JSONL / text timeline | no |
| `.fsm` / `.audit` | `thyme_log_merge`, `grep` | JSONL / key=value | no |
| Service log | `journalctl` / `grep` / file sink | single-line key=value | no |
| Histograms | `thyme_log_merge ... --hist --json` | JSONL (one node/series per line) | no |
| Process/resources | `thyme_log_merge ... --sys --json` | JSONL (kind=`sys`, with `series_name`) | no |

Foxglove's native data sources are MCAP (and ROS bags) only; **there is no
JSONL data source**. The repository also has no bridge that turns perf/FSM
JSONL into MCAP or a Foxglove WebSocket (that belongs to the sibling
`thyme-viz`, see [`onboarding.md`](onboarding.md) §1). `thyme-layout.json` is
therefore positioned as a "**recording-package replay view**":

- `StateTransitions`: `thyme/trigger.event`, `thyme/session.event`
- `Plot`: `thyme/trigger.pre_roll_ns`, `thyme/trigger.post_roll_ns`
  (JSON schema in `kTriggerSchemaJson` in `src/record/mcap_export.cpp`)
- `Plot` + `RawMessages`: `thyme/gap.missed_count`, `thyme/gap`
  (a GAP is a hole caused by overwrite/timeout, the closest evidence of a drop
  on the replay side)

Data channels `thyme/struct_<id>` (or `thyme/<name>`) use encoding `thyme` and
an opaque payload; the schema is advisory only — **Foxglove can show channels
and timestamps but cannot decode fields** (a known limitation). Plotting
latency/drop curves here would first require converting perf dumps into
channels with a JSON schema; this repository has no such converter.

### 5.2 Import

1. `thyme_bin_to_mcap <pkg>/small.tbin out.mcap` (to export a whole package,
   merge it first following the evidence-collection procedure in `runbook.md`
   §4);
2. Foxglove Studio → **Open local file** → `out.mcap`;
3. **Layouts → Import from file** →
   [`../dashboards/foxglove/thyme-layout.json`](../../dashboards/foxglove/thyme-layout.json).

Topics in the layout are written exactly as they appear in MCAP
(`thyme/trigger`, without a leading `/`). Some Foxglove versions display
non-ROS channels as `/thyme/trigger`; in that case add a `/` in the panel's
message path. Verification scope: the JSON structure (keys in `configById`
match the references in `layout`) is checked by hand; there is **no** import
regression test against a specific Foxglove version (see §7).

## 6. Mapping table: metric → source → view

| metric | Source (file / field) | panel | Alerts |
| --- | --- | --- | --- |
| `thyme_status_up` | whether `GET /status` is readable (decided by the exporter) | Status endpoint | `ThymeStatusEndpointDown`, `ThymeExporterStale` (`absent`) |
| `thyme_status_healthy` | status `ok` | Status endpoint | `ThymeStatusUnhealthy` |
| `thyme_app_uptime_seconds` | status `uptime_ms` | — | — |
| `thyme_roles` | status `role_count` | — | — |
| `thyme_role_state` | status `roles[].state` → 0..6 | Role state | `ThymeRoleGaveUp`, `ThymeRoleNotRunning` |
| `thyme_role_up` | `state == "running"` | Role state | `ThymeRoleNotRunning` |
| `thyme_role_restarts_{attempted,succeeded}_total` | status `restarts_*` | Role restarts | `ThymeRoleRestartStorm` |
| `thyme_role_last_exit_code` / `_last_term_signal` | status fields of the same name | — (troubleshooting) | — |
| `thyme_role_stall_age_seconds` | status `stall_age_ms` | — | `ThymeRoleStalled` |
| `thyme_role_loops_total` | status `loops` | — | — |
| `thyme_role_alive_total` | status `health.alive` | — | — |
| `thyme_role_deadline_misses_total` | status `health.deadline_misses` | Deadline miss / overrun | `ThymeOverrun`, `ThymeOverrunCritical` |
| `thyme_role_drops_total` | status `health.drops` | HealthSlot drops | `ThymeHealthDrops`, `...Critical` |
| `thyme_edge_drops_total` | status `health.drop_counts` | HealthSlot drops per consumer edge | — |
| `thyme_role_error_slots` | status `health.errors` (instantaneous count of slots with a non-zero error_code) | — | — |
| `thyme_role_sched_mismatches_total` | status `health.sched_mismatches` | — | `ThymeSchedMismatch` |
| `thyme_recorder_present` | status `recorder.present` | — | — |
| `thyme_trigger_relay_sent_total` | status `recorder.trigger_relay_sent` | — | — |
| `thyme_trigger_relay_debounced_total` | status `recorder.trigger_relay_debounced` | — | `ThymeTriggerStorm` |
| `thyme_trigger_relay_seq{kind}` | status `trigger_{req,ack}_seq` | — | — |
| `thyme_trigger_relay_inflight` | `(req − ack) mod 2^32` | Trigger relay inflight | `ThymeRelayInflightHigh` |
| `thyme_log_budget_limit_bytes` | status `logging.budget_limit_bytes` (single process) | Log budget | — |
| `thyme_log_budget_used_bytes` | status `logging.budget_used_bytes` | Log bytes used | — |
| `thyme_log_budget_drops_total` | status `logging.budget_drops` | Drops when the budget is full (audit never drops) | — |
| `thyme_log_rotations_total` | status `logging.rotations` | audit/.fsm/perf rotation count | — |
| `thyme_service_log_sink_open` | recorder status-json `logging.service_sink_open` | Whether the service file sink is enabled | — |
| `thyme_service_log_records_{written,suppressed,dropped,truncated}_total` | recorder status-json `logging.service_records_*` | Service writes / rate-limited / dropped / truncated | `dropped` backs `ThymeServiceLogDrops` |
| `thyme_service_log_rotations_total` | recorder status-json `logging.service_rotations` | Service file rotation count | — |
| `thyme_shm_page_compatible` | `thyme_probe` `compatible` | — | `ThymeShmPageIncompatible` |
| `thyme_shm_page_{head,tail,epoch}` | probe `head/tail/epoch` | — | — |
| `thyme_shm_page_drops_total` | probe `drop_count` | SHM container drops; for SPMC this is the page-level total of reader misses | `ThymeShmPageDrops` |
| `thyme_shm_spmc_reader_drops_total{reader_slot}` | probe `reader_drop_slots` | Misses for a single SPMC reader; slots are not reused during the page lifetime and are not summed with overwritten | — |
| `thyme_shm_page_pinned_skip_total` | probe `skip_count` (`.lease` table) | PinnedSlot pinned_skip | `ThymePinnedSkip`, `ThymePinnedSkipCritical` |
| `thyme_shm_page_lease_steal_total` | probe `steal_count` (`.lease` table) | PinnedSlot lease_steal | `ThymeLeaseSteal` |
| `thyme_perf_latency_{quantile,max}_ns` | `p*_ns`/`max_ns` from `log_merge --hist --json` | Latency p99 / max | `ThymePerfNodeDeadlineSLO` |
| `thyme_perf_node_{period,deadline}_ns` | perf type-9 node budget | — | Per-node thresholds for the SLO |
| `thyme_perf_samples` | `n` from the same source | Latency p99 / max | — |
| `thyme_process_{rss,vsize}_kb`, `thyme_process_open_fds`, `thyme_process_fd_limit`, `thyme_process_threads` | `log_merge --sys --json` (instantaneous) | RSS / address space / fd / threads | — |
| `thyme_process_{cpu_user,cpu_kernel}_ns_per_interval`, `thyme_process_*_faults_per_interval`, `thyme_process_*_ctx_per_interval`, `thyme_process_io_*_per_interval`, `thyme_process_cpu_delay_ms_per_interval` | Same source (per-interval deltas, **not counters**) | CPU / page faults / context switches / IO / blocked wait per interval | — |
| `thyme_process_probe{node,series}` | Same source, registered probe series | Registered probes such as queue depth and overrun | — |
| `thyme_process_ext_caps` / `thyme_process_ext_supported{source}` | `ext_caps` bitmask from `log_merge --sys --json` | Whether optional sources were requested and are supported by the kernel | — |
| `thyme_process_cgroup_{cpu_usage_us,cpu_throttled_us,cpu_nr_throttled,oom_events}_per_interval`, `thyme_process_cgroup_{mem_current,mem_max}_kb`, `thyme_process_cgroup_io_{read,write}_kb_per_interval` | Same source, `cgroup_*` | CPU throttling / memory / OOM / block IO from the cgroup's point of view | — |
| `thyme_process_psi_{cpu_some,mem_some,mem_full,io_some,io_full}_us_per_interval` | Same source, `host_psi_*` | Whole-machine PSI stall time (not this process) | — |
| `thyme_process_host_disk_{read,write}_kb_per_interval`, `thyme_process_host_net_{rx,tx}_kb_per_interval` | Same source, `host_disk_*` / `host_net_*` | Whole-disk/host network throughput (a container view may include host traffic) | — |
| `thyme_process_thread_cpu_ns_per_interval{node,thread}` | Same source, `thr_<name>_cpu_ns` | CPU time of a registered thread | — |
| `thyme_host_cpu_{user,system,idle,iowait,irq,steal}_ns_per_interval`, `thyme_host_core_busy_ns_per_interval{core}` | `host_cpu_*` / `core<i>_busy_ns` from `_host_` files | Whole-machine / per-core CPU (host scope) | — |
| `thyme_host_load{1,5,15}`, `thyme_host_mem_{total,available}_kb`, `thyme_host_file_nr_{allocated,max}` | Same source, `host_load*_milli` / `host_mem_*` / `host_file_nr_*` | Host load, memory, system fd table | — |
| `thyme_health_slot_error_code` | status `health.error_codes` | — | — |
| `thyme_recorder_*_total` | recorder status-json `stats` | — | `ThymeRecorderDiskErrors` |
| `thyme_trigger_{audit,fsm_event}_drops_total` | recorder status-json `trigger` | — | `ThymeAuditDrops`, `ThymeFsmEventDrops` |
| `thyme_trigger_audit_records` | audit lines (with `req=`) | Trigger audit decisions | — |
| `thyme_trigger_audit_unparsed_lines` | audit lines (without `req=`) | — | `ThymeAuditTailTorn` |
| `thyme_trigger_audit_decisions{decision,code}` | audit `decision=`/`code=` | Trigger audit decisions | — |
| `thyme_filesystem_*` | `statvfs(output_dir)` | Recorder disk | `ThymeDiskFull`, `ThymeDiskPressure` |
| `thyme_shm_{used_percent,size,avail}_bytes`, `thyme_shm_objects` | `statvfs`/`scandir(/dev/shm)` | /dev/shm pressure | `ThymeShmPressure`, `ThymeShmObjectsGrowth` |
| `thyme_{status,probe,hist,audit}_up` | The exporter's own read result | — | (for troubleshooting; `ThymeExporterStale` covers a total stall) |

## 7. Things that are explicitly not exported (do not pretend otherwise)

| Field | Status | Impact | Follow-up |
| --- | --- | --- | --- |
| Number of `.incomplete` trigger packages, bytes released by retention | not exported | The runbook §1 procedure still needs manual `ls`/`df` | Optional: add `--output-dir` scanning to the exporter (not implemented) |

Two more points, stated honestly:

- **The Grafana dashboard has no real import regression test**: JSON structure,
  panel-id uniqueness and the existence of every metric referenced by a query
  are covered by tests; Grafana version differences (schemaVersion 39, the
  `${ds}` variable, `$__rate_interval`) are only written against the Grafana
  ≥9 model.
- **The alert rules are not validated with `promtool check rules`**: this
  environment has no `promtool`; tests cover YAML syntax, the
  severity/for/runbook completeness of every rule, and that live rules
  reference only exported metrics. PromQL semantics still depend on Prometheus
  loading them.

## 8. Tests and verification

```bash
# Exporter unit tests (14 cases: golden, unix socket, watch, atomic write,
# label escaping, --probe-bin, probe info objects, rule/dashboard↔metric contracts)
python3 tools/inspect/thyme_metrics_exporter/test_thyme_metrics_exporter.py -v
# The same test is wired into ctest (label unit;mr_gate)
ctest --test-dir build -R test_thyme_metrics_exporter --output-on-failure
# Doc links, command targets and the ABI matrix
python3 tools/ci/check_doc_links.py
python3 tools/ci/check_doc_commands.py            # add --build-dir build for numeric checks
python3 tools/ci/check_abi_matrix.py
```

The golden file is
[`../tools/inspect/thyme_metrics_exporter/testdata/expected_metrics.txt`](../../tools/inspect/thyme_metrics_exporter/testdata/expected_metrics.txt):
changing an exported field requires updating it, and it is the first gate
against "a panel/alert references a metric that does not exist".

## References

- [`runbook.md`](runbook.md) — the four fault scenarios and their handling
- [`onboarding.md`](onboarding.md) — build / run the chain / tool locations
- [`scheduling-model.md`](../reference/scheduling-model.md) — overrun and scheduling-mismatch observability
- [`trigger-config.md`](trigger-config.md) — `audit_*`, `disk_quota_mb` and other configuration keys
- [`abi-compatibility.md`](../reference/abi-compatibility.md) — what `compatible=false` means
- [`../include/thyme/runtime/perf_log.h`](../../include/thyme/runtime/perf_log.h) — perf-dump block format
- [`../include/thyme/app/status_server.h`](../../include/thyme/app/status_server.h) — status endpoint contract
- [`../include/thyme/probe/probe.h`](../../include/thyme/probe/probe.h) — probe read-only contract and fields
