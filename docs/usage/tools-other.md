# Other tools: gen_graph, log merging and operations scripts

This page collects the command-line tools in the thyme repository other than
`thyme_probe`, `thyme-replay` and `triggerctl`. Each tool gets a short purpose
statement, a minimal command, key options and exit codes; details live on the
corresponding topic page and are not repeated here.

| Tool | Binary/entry point | Purpose |
| --- | --- | --- |
| `gen_graph` | `build/tools/gen/gen_graph/gen_graph` | Generate wiring/scaffold from `graph.yaml` |
| `thyme_log_merge` | `build/tools/inspect/thyme_log_merge/thyme_log_merge` | Merge multi-process observability logs |
| `thyme_metrics_exporter` | `tools/inspect/thyme_metrics_exporter/thyme_metrics_exporter.py` | Prometheus textfile sidecar |
| `thyme_py` | `python3 -m thyme_py` | Pure-standard-library read-only package parser |
| `thyme_upgrade.sh` | `tools/ops/upgrade/thyme_upgrade.sh` | Upgrade/rollback driver |
| `demo_producer` | `build/tools/apps/trigger_center/demo_producer` | trigger_center smoke producer |
| `trigger_center` | `build/tools/apps/trigger_center/trigger_center` | Recording/triggering center daemon |
| `thyme_bin_to_mcap` | `build/tools/replay/mcap_export/thyme_bin_to_mcap` | Single-file `.thyme.bin` → MCAP |

## 1. `gen_graph`

`gen_graph` is a host-side graph code generator: it reads `graph.yaml`, generates
the `generated/` wiring, and produces skeletons such as `src/nodes/` and
`src/main.cpp` from templates/scaffolds. It does not link `thyme::runtime` and
takes no part at runtime.

```bash
# Daily: regenerate generated/ after a yaml change
build/tools/gen/gen_graph/gen_graph --in graph.yaml --update generated

# CI/pre-commit: only check that generated/ matches the yaml
build/tools/gen/gen_graph/gen_graph --in graph.yaml --check

# Show the version (reads no graph, writes no files)
build/tools/gen/gen_graph/gen_graph --version
```

`--version` prints:

```text
gen_graph 3.11.1 (output format 3)
```

| Option | Description |
| --- | --- |
| `--in PATH` | Input yaml; required except for the `--update all` bootstrap case |
| `--from-json PATH` | Read a canonical JSON graph; mutually exclusive with `--in` |
| `--emit-json` | Parse `--in` and print canonical JSON to stdout without generating files |
| `--emit-yaml` | With `--from-json`, print the canonical `graph.yaml` to stdout |
| `--out DIR` | App root directory; defaults to the directory of `--in` |
| `--update MODE` | `generated` (default), `mp`, `nodes`, `app`, `all` |
| `--check` | Regenerate in a staging directory and byte-diff `OUT/generated/`; exit 1 on drift |
| `--explain-rates` | Print the derived rate JSON and exit without writing files |
| `--explain-sched` | Scheduling/admission report: JSON to stdout, human table to stderr |
| `--sched-priority=MODE` | `best-effort` (default) or `strict` |
| `--sched-admission=MODE` | `error` (default), `warn`, `off` |
| `--derive-template` | Derive the Group 1 template overlay from the yaml |
| `--template-out DIR` | Output directory for `--derive-template`; defaults to `<out>/template` |
| `--project-name NAME` | Banner name for `--derive-template` |
| `--contracts-include PATH` | Fallback contracts header path for `--derive-template` |
| `--template-dir DIR` | Project overlay template directory (base ∪ overlay) |
| `--force` | Overwrite existing node stubs/app files, or derived templates |
| `--no-health-reporter` | Do not inject the default `health_reporter` aggregator node |
| `--health-hz HZ` | Frequency of the injected `health_reporter`; default 10, must be > 0 |

`--update` semantics: `generated` is the everyday edit; `mp` refreshes only the mp
tables; `nodes` generates node stubs; `app` generates the app skeleton; `all` is
for bootstrapping a new project (not a daily command). There is no `init`
subcommand; `--process` is not implemented and exits 2 when passed.

Exit codes:

| Exit code | Meaning |
| --- | --- |
| `0` | Generation/check/query succeeded |
| `1` | `--check` found that `generated/` is stale or was hand-edited |
| `2` | Usage, yaml parsing, semantic validation or emit failure |

For the graph structure and scheduling model, see [Runtime API](api-runtime.md) and
[Configuration and deployment](config-deploy.md).

## 2. `thyme_log_merge`

`thyme_log_merge` merges observability logs from multiple processes onto an
absolute `CLOCK_MONOTONIC` timeline: inputs may be `.fsm`/JSONL (with `ts` in
mono_ns) or a perf dump binary produced on the target board. It supports `--hist`
mode, which skips the timeline and merges type-6 histogram blocks across processes
by `(node, series)`.

```bash
# Multi-process timeline, JSONL output
build/tools/inspect/thyme_log_merge/thyme_log_merge node_a.fsm node_b.fsm perf.perf \
    --from 1000000000 --to 2000000000 --json -o merged.jsonl

# Merge only histograms and emit percentiles
build/tools/inspect/thyme_log_merge/thyme_log_merge *.perf --hist --json -o hist.jsonl

# Require all inputs to come from the same record manifest
build/tools/inspect/thyme_log_merge/thyme_log_merge a.fsm b.fsm \
    --expect-hash 0x9914c81f74037d59
```

| Option | Description |
| --- | --- |
| `--from ns` / `--to ns` | Filter by `ts_ns`, inclusive at both ends |
| `--json` | Emit JSONL, one `{"ts":...,"src":"...","kind":"...",...}` per line |
| `--hist` | Skip the timeline and merge only type-6 histogram blocks |
| `-o out` | Write to a file; defaults to stdout |
| `--expect-hash 0x...` | Require every input's record manifest to match |

`--hist` output reports `p50/p90/p99/p999/max` per `(node, series)`; `series`
comes from `PerfSampler::kHistSeries*` (`latency`, `edge_latency`,
`callback_work`).

Exit codes:

| Exit code | Meaning |
| --- | --- |
| `0` | Success |
| `2` | Usage error or unparsable argument |
| `3` | An input could not be opened (an output file that cannot be opened also returns 3) |
| `4` | Record manifest mismatch/missing; never emits a "silently wrong" timeline |

See [Performance and baselines](perf.md) for the meaning of histograms and perf
metrics.

## 3. `thyme_metrics_exporter`

`thyme_metrics_exporter` is a sidecar with **no HTTP server**: it translates the
existing observability surfaces (the `/status` endpoint, `thyme_probe list --json`,
`thyme_log_merge --hist --json`, the audit log, `statvfs`) into the Prometheus text
exposition format and hands the result to node_exporter's textfile collector or any
scraper that can read textfiles.

```bash
python3 tools/inspect/thyme_metrics_exporter/thyme_metrics_exporter.py \
    --status file:/tmp/thyme-status.json \
    --probe-bin build/tools/inspect/thyme_probe/thyme_probe \
    --disk /data --shm-dir /dev/shm \
    --out /var/lib/node_exporter/textfile/thyme.prom --once

# Periodic collection, once every 5 seconds
python3 tools/inspect/thyme_metrics_exporter/thyme_metrics_exporter.py \
    --status /run/thyme/status.sock --watch 5 --iterations 12
```

| Option | Description |
| --- | --- |
| `--status PATH` | `/status` UNIX socket, or `file:PATH` to read a saved `/status` snapshot (default `/run/thyme/status.sock`) |
| `--status-timeout SEC` | Per-source timeout; default 2.0 |
| `--probe PATH` | Read saved `thyme_probe list/info --json` output |
| `--probe-bin PATH` | Run `thyme_probe` directly on each cycle |
| `--hist PATH` | JSONL from `thyme_log_merge <dumps> --hist --json` |
| `--audit PATH` | Audit log (key=value lines) |
| `--disk PATH` | Filesystem to monitor (for example the recording output disk) |
| `--shm-dir PATH` | SHM directory, for example `/dev/shm` |
| `--out PATH` | Write atomically to this file; defaults to stdout |
| `--once` | Collect once and exit (default) |
| `--watch SEC` | Collect every SEC seconds until interrupted |
| `--iterations N` | With `--watch`, stop after N collections |

`--out` writes a sibling temp file to completion and then renames it, so
node_exporter never reads half a file. A single failing data source does not fail
the process; it sets the corresponding `*_up` to 0.

Exit codes:

| Exit code | Meaning |
| --- | --- |
| `0` | Snapshot written (even if some sources failed) |
| `2` | Usage error |
| `3` | Output file cannot be written |

See `docs/guides/observability-dashboards.md` for the metric-to-source mapping and
dashboards; do not assume metrics that do not exist here.

## 4. `thyme_py`

`thyme_py` is a pure-standard-library, read-only package parser supporting v1–v4
`.thyme.bin` files and package directories; it does not link thyme and does not
depend on third-party Python packages. It is useful for quick forensics or field
inspection in environments without a C++ toolchain.

```bash
cd tools/inspect/thyme_py
python3 -m thyme_py info /data/captures/trigger_000012
python3 -m thyme_py info /data/captures/trigger_000012 --json --no-crc

# Stream records (base64 payload; use --hex for hexadecimal)
python3 -m thyme_py dump /data/captures/trigger_000012 --limit 10
PYTHONPATH=/path/to/thyme/tools/inspect/thyme_py python3 -m thyme_py dump \
    /data/captures/trigger_000012/small.tbin --hex
```

| Subcommand | Options | Description |
| --- | --- | --- |
| `info` | `--json`, `--no-crc` | Package/file summary: version, session, struct, manifest, counts, index |
| `dump` | `--limit N`, `--hex`, `--no-crc` | Emit record-by-record JSON Lines |

Exit codes: `0` success; `1` file/format/CRC error (stderr starts with
`thyme_py: error:`). See `docs/guides/python-api.md` for the API and package
layout.

## 5. `thyme_upgrade.sh`

`thyme_upgrade.sh` implements the upgrade/rollback sequence: pre-flight ABI check →
stop producers → stop consumers → install the package (optional backup) → optionally
clean up incompatible SHM → start producer/consumer → re-check ABI/smoke test.

**The default is a dry run**: each step only prints the command it would execute; it
does not stop units, copy files, unlink SHM or delete packages. Only `--apply`
performs the work.

```bash
# Rehearsal: safe, changes nothing by default
tools/ops/upgrade/thyme_upgrade.sh \
    --package /opt/thyme-staging --prefix /opt/thyme \
    --probe build/tools/inspect/thyme_probe/thyme_probe

# Perform the upgrade
tools/ops/upgrade/thyme_upgrade.sh --apply \
    --package /opt/thyme-staging --prefix /opt/thyme \
    --backup-dir /var/backups/thyme --probe build/tools/inspect/thyme_probe/thyme_probe

# Only run the pre-flight ABI check, or print this build's matrix
tools/ops/upgrade/thyme_upgrade.sh --check-only
tools/ops/upgrade/thyme_upgrade.sh --matrix

# Restore from a backup
tools/ops/upgrade/thyme_upgrade.sh --apply --rollback /var/backups/thyme
```

| Option | Description |
| --- | --- |
| `--apply` | Execute the plan; the default only prints it |
| `--check-only` | Run only the pre-flight ABI check |
| `--matrix` | Print this build's ABI constant matrix (JSON) |
| `--package DIR` | Staged package tree to install (required for an upgrade) |
| `--prefix DIR` | Install prefix; default `/opt/thyme` |
| `--backup-dir DIR` | Back up the current prefix before installing |
| `--rollback DIR` | Restore binaries from this backup instead of installing a new package |
| `--delete-package DIR` | Delete the old package directory after success; only effective under `--apply`, never implicit |
| `--probe PATH` | Path to `thyme_probe` (defaults to searching PATH) |
| `--manifest FILE` | Probe manifest (required for LatestSlot pages) |
| `--shm-prefix P` | Prefixes scanned by the pre-flight check; repeatable, default `thyme_`, `c9_` |
| `--purge-shm` | After stopping, unlink incompatible SHM pages with no live process mapping; only effective under `--apply` |
| `--allow-downgrade` | Allow page versions newer than the current build |
| `--skip-abi-check` | Do not abort even if the pre-flight check fails |
| `--unit NAME` | Single unit that takes all roles; default `thyme-app` |
| `--producer-unit NAME` | Producer-only unit; repeatable; stopped first |
| `--consumer-unit NAME` | Consumer-only unit; repeatable; stopped last |
| `--unit-ctl CMD` | Unit control command; default `systemctl` |
| `--smoke-cmd CMD` | Smoke command to run after startup |
| `--verify-wait-ms N` | Wait after startup before re-checking the ABI; default 2000 |

Dangerous options:

- Use `--purge-shm` only when "all relevant units are stopped, the pages are
  confirmed incompatible, and no live process maps them"; the script also performs
  its own liveness check, but **do not** replace it with
  `rm -rf /dev/shm/thyme_*`. Blind deletion misses segments with other prefixes and
  may unlink a segment that a live peer is mapping.
- `--delete-package` deletes a directory, and only runs when given explicitly with
  `--apply`.
- `--rollback` and `--package` are two separate paths; confirm the backup directory
  is complete before rolling back.
- `--allow-downgrade` / `--skip-abi-check` both weaken the upgrade gate; use them
  only in explicit rollback/rescue scenarios.

Exit codes:

| Exit code | Meaning |
| --- | --- |
| `0` | Success (a dry run counts as success) |
| `1` | Usage error |
| `2` | Pre-flight ABI failure, or downgrade refused |
| `3` | A command failed |
| `4` | Post-startup verification failed |

See [Configuration and deployment](config-deploy.md) for the upgrade order and SHM
lifecycle, and [thyme_probe ABI preflight](probe.md) for the ABI compatibility
matrix.

## 6. `demo_producer` and `trigger_center`

### `demo_producer`

`demo_producer` is the bring-up/smoke producer for trigger_center: it creates a
`RecordMirrorRingPage` (one `Pod16` struct), a `ShmSpmcRing` with the record tap
armed, and an optional pinned-slot channel (`PinnedSlotWriter`/`PinnedSlotReader`);
it publishes at a fixed rate until SIGTERM/SIGINT. Business logic is not the point —
it verifies that "recording is a side effect of the tap".

```bash
build/tools/apps/trigger_center/demo_producer \
    /thyme_demo_page /thyme_demo_ring /thyme_demo_frames 100
```

```text
usage: demo_producer <page_shm> <ring_shm> [channel_shm] [rate_hz]
```

Passing an empty string or `-` for `channel_shm` means no pinned-slot channel is
created; `rate_hz` defaults to 100. A creation failure exits 1, insufficient
arguments exit 2, and SIGTERM/SIGINT cause a normal exit. If a replay gate owns the
pinned-slot channel, a gated warning is printed once instead of spinning silently.

### `trigger_center`

`trigger_center` is the recording/triggering center daemon, started from an INI
configuration; it also provides `--check-config`, `--dump-config` and the read-only
`--check-resources` entry point. The resource self-check only calls
`getrlimit`/`statvfs`, allocates no memory and starts no threads, so it is suitable
as a deployment gate.

```bash
build/tools/apps/trigger_center/trigger_center \
    --record-manifest /etc/thyme/record_manifest.h /etc/thyme/trigger.ini
build/tools/apps/trigger_center/trigger_center --check-config \
    --record-manifest /etc/thyme/record_manifest.h /etc/thyme/trigger.ini
build/tools/apps/trigger_center/trigger_center --check-resources \
    --record-manifest /etc/thyme/record_manifest.h /etc/thyme/trigger.ini --json
```

```text
usage: trigger_center [mode] --record-manifest /etc/thyme/record_manifest.h \
       <config.ini> | <base.ini> <policy.ini> [--json]
```

`--check-resources` exit codes: `0` pass, `2` configuration error, `3` a hard
requirement is not met; `--json` prints one line on stdout containing the
`checked/worst/fatal/warnings/nofile/memlock/disk/shm` object. Startup failure in
normal daemon mode also exits non-zero; see stderr for details.

Recorder control (arm/disarm/status/verify/manual trigger) all goes through
[`triggerctl`](replay-cli.md). Configuration options, resource-check environment
variables and deployment units are described in
[Configuration and deployment](config-deploy.md); the trigger chain design is in
[Triggered acquisition](triggered-acquisition.md).

## 7. `thyme_bin_to_mcap`

`thyme_bin_to_mcap` exports a single `.thyme.bin` file directly to MCAP, suitable
for single-file troubleshooting or handing an old recording to Foxglove.
Package-level export with schema sidecars should prefer
[`thyme-replay export`](replay-cli.md).

```bash
build/tools/replay/mcap_export/thyme_bin_to_mcap small.tbin /tmp/small.mcap --crc
```

```text
usage: thyme_bin_to_mcap <in.thyme.bin> <out.mcap>
       [--no-events] [--no-summary] [--no-header-session] [--crc]
```

| Option | Description |
| --- | --- |
| `--no-events` | Do not write per-event messages (implies no header session) |
| `--no-summary` | Do not write the Statistics summary |
| `--no-header-session` | Do not write header/session messages |
| `--crc` | Include a data section CRC |

A failed export deletes the incomplete output file. Exit codes: `0` success; `2`
argument error; `3` input cannot be opened; `4` input is corrupt (prints the record
offset and expected/actual CRC); `5` data references an unregistered struct id; `6`
MCAP write failure.

## Related documentation

- [thyme_probe read-only observability](probe.md)
- [Replay CLI and triggerctl](replay-cli.md)
- [Triggered acquisition](triggered-acquisition.md)
- [Configuration and deployment](config-deploy.md)
