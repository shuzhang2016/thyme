# Performance and observability

This page covers three things: the on-disk format of thyme's performance data (the perf dump), how to read it back and merge it (`thyme_log_merge`), and how to turn a measurement into a comparable, gateable conclusion with the benches and eval_chains. All fields, constants and defaults follow the headers and tool sources in this tree.

## Observability surfaces at a glance

| Surface | Artifact | How to read it | Good for answering |
| --- | --- | --- | --- |
| perf dump | binary, appended periodically | `thyme_log_merge` | node/edge latency distribution, compute time, process counters, queue depth |
| FSM events | `<socket>.fsm` JSONL | `tail` / `thyme_log_merge` | trigger state transitions and which phase a trigger is stuck in |
| audit | `<socket>.audit` `key=value` | `grep` / metrics exporter | the decision and rejection reason of every trigger |
| process status | `/run/thyme/status.sock` | `curl --unix-socket` | role health, overruns, logging budget |
| SHM pages | `/dev/shm` | `thyme_probe list/info/watch` | channel geometry, head/tail, drop/skip/steal |
| metrics | Prometheus textfile | `thyme_metrics_exporter.py` | dashboards and alerts |

The perf dump is wired by the host through `PerfLogDumper`; generated applications enable it with a perf-log prefix environment variable (see the [runbook](../guides/runbook.md)) and, on exit, flush once into `<prefix>_node_<role>_<pid>.bin` and `<prefix>_edge_<role>_<pid>.bin`.

## Perf log block types (1–10)

The file starts with a 4-byte magic followed by a series of blocks. Each block header is fixed at `type u8 | node u8 | count u32`, and records are little-endian. All sample timestamps lie on one absolute time axis: `CLOCK_MONOTONIC >> 14` (16.384 µs units); the u32 wraps after about 19.5 h of runtime. Type 10 additionally stores a full-width mono/realtime nanosecond pair.

| type | Contents | Record size | Key fields |
| --- | --- | --- | --- |
| 1 | samples: per-sample latency | 10 B | `ts u32`, `lat u24` (2^13 ns, clamped at `0xFFFFFF` ≈ 137 s), `id u8`, `io u8`, `rsvd u8` |
| 2 | percentile: p50/p95/p99/max per id | 20 B | `tag 0xA1`, `node u8`, `n u16`, four u24 values (2^13 ns), `window_end_ts u32` |
| 3 | work: compute time per Loop/Dispatch batch | 10 B | `ts u32`, `dur u24` (2^10 ns, about 17.2 s ceiling), `io u8`, `node u8`, `flags u8` |
| 4 | epoch: wrap base of the u32 time axis | 8 B | `u64` = `(now >> 14) & ~0xFFFFFFFF`, one block per dump window |
| 5 | sys: process counter deltas + instantaneous queue depth | 10 B | `ts u32`, `value u32` (clamped), `series u8`, `rsvd u8` |
| 6 | histogram: fixed-bucket histogram | 1,340 B | `tag 0xA2`, `series u8`, `buckets u16`, `count u64`, `max_ns u64`, `end_ts u32`, `u32[329]` |
| 7 | record manifest: channel-list identity | 12 B | `manifest_version u16`, `reserved u16`, `manifest_hash u64` |
| 8 | series name: sys probe name | 32 B | `series u8`, `name char[31]` (zero-padded) |
| 9 | node budget: node period and dispatch deadline | 16 B | `period_ns u64`, `dispatch_deadline_ns u64` |
| 10 | clock anchor: mono↔wall time anchor | 16 B | `mono_ns u64`, `realtime_ns u64`, one block per non-empty dump |

A few easy-to-miss points:

- The `flags` of type 3 are semantic bits, not decoration: bit0 means the batch spans more than one input frame (indivisible), bit1 means it produced output, bit2 means it consumed input. `io == 0` does not mean "no attribution" (nibble 0 is a legal frame number), so read the flags.
- The high nibble of the `io` byte is the output frame nibble and the low nibble is the input frame nibble; cross-node frame joins rely on it.
- Type 5 `series` 0..5 are the fixed process counters, 6..15 are the fixed process resource series (RSS/VSize/threads/fd/IO/CPU delay, see `proc_sampler.h`), and `series >= 16` are probes numbered **in registration order**; type 8 metadata maps a named probe's series to a stable name, and offline tools must not invent semantic labels for unnamed entries.
- The type 7 `node` byte is always 0: the manifest is build-wide and belongs to no single node. It is written once per file generation (on first write and after every rotation), and a parser must refuse to merge inputs whose manifests differ.
- Type 10 `node` is likewise always 0, but the anchor is file-wide. Both fields are full u64 nanoseconds and do not reuse the low u32 word of a sample; older parsers stop at an unknown type under the format's extension rule and never reinterpret old blocks a second way.
- Only files that carry a type 4 epoch block can unwrap the 19.5 h rollover unambiguously. A legacy dump written by the exit-time single-flush path has no epoch or clock-anchor block; its mono samples are still read with legacy semantics but gain no wall-clock field.

## PerfSampler: sampling ring and Flush/Reset windows

`PerfSampler` is a fixed-length ring buffer plus one aggregated histogram, with zero allocation on the hot path.

| Item | Value | Notes |
| --- | --- | --- |
| Default capacity | 4096 | `kCap`, a power of two |
| Maximum capacity | 262144 | `kMaxCap`, 12 B per sample, about 3 MiB per ring |
| Default window | 10 s | `kPerfRingDefaultWindowNs` |
| Capacity formula | `rate_hz × window_ns × 3/2`, rounded up to a power of two | `CapacityForRate()`; rate clamped to 1 MHz, window clamped to 1 h |
| Allocation time | once, at construction | `Note()` never allocates; if allocation fails the sampler degrades to a no-op (`capacity() == 0`) instead of throwing |

API semantics:

- `Note(ts_ns, lat_ns, id = kNoId, io = 0)`: records one sample and bumps one relaxed atomic histogram bucket. `id = 0xFF` (`kNoId`) means "no channel attribution" and is replaced by the node-level id at Flush; `0xFF` is reserved, so every `RecordStructId` (including 0) is still a valid, explicit edge id.
- `Flush(w, node_id)`: writes out every sample retained in the ring, one sample block plus one percentile block per valid id, and appends a type-6 histogram block last.
- `FlushNew(w, node_id)`: writes only the samples newer than the last watermark and advances the watermark; samples already pushed out of the ring are lost silently, which is why the ring must be sized with `CapacityForRate()`.
- `Reset()`: clears samples, the watermark and the histogram. This is a dump-path operation and must not run concurrently with `Note()`; samples in the boundary window may be lost.

`PerfLogDumper` composes the contract above into crash-safe periodic persistence:

1. It first checks that the sampler/work-log capacities fit the writer budget; a mismatch aborts immediately, before the interval, rotation or write path is inspected. The interval gates the rest; `PerfDumpRotation` then decides whether to rotate (after a rotation both the magic and the manifest are rewritten, otherwise the new generation cannot be parsed standalone).
2. It writes an epoch block and a type 10 anchor for that instant, then `FlushNew()` on the sampler, the work log and the proc sampler. Every non-empty dump refreshes the anchor; when there are no new samples the original contract holds and no empty dump containing only epoch/anchor is written.
3. One unbuffered `write()` (`O_APPEND`); a crash loses at most the tail of this dump.
4. Only a successful write triggers `Reset()` on the three rings (dump-then-reset); on failure the ring contents are kept, but the watermark has already advanced.

Default rotation is 64 MiB per file with 2 generations retained; the `sample_cap`/`work_cap` passed to `Open()` must each be at least the `capacity()` of the sampler/work log passed in. Non-default capacities require matching caps; when you change the interval, the sampling rings still have to be sized for the actual rate and that interval, and the writer budget must cover them. A mismatch is a wiring bug: `DumpIfDue` aborts at its entry point and never writes incomplete or truncated analysis data.

## Histogram

`LatencyHistogram` is a log-linear (HDR-style) fixed-bucket histogram meant for p999 and exact max, not a replacement for the per-id percentiles of type 2.

- **Bucket layout**: 8 sub-buckets per octave, `1 + 41 × 8 = 329` buckets in total. `0..15 ns` is exact per value; from `>= 16 ns` each bucket is 1/8 of its octave.
- **Resolution**: a percentile takes the integer midpoint of the hit bucket, with a worst-case relative error of `+6.25% / -5.56%`; `max` is the exact observed ns value (u64), not a bucket boundary.
- **Memory**: the bucket array is `329 × u32 = 1,316 B` plus 8 B for max, roughly 1.3 KB per series, with no allocation after construction. A type-6 record is a fixed 1,340 B.
- **Cost**: one relaxed atomic increment per sample (a few ns), plus a single CAS only when a new max appears.
- **Series labels**: 0 = node consume latency, 1 = edge latency, 2 = callback work. Histograms are aggregated per sampler; per-edge ids never enter type 6, so an edge file's histogram is always `node=0, series=1` and cannot be drilled down by node.
- **Merging**: buckets are summed across processes and max takes the maximum; percentiles still use nearest-rank plus bucket midpoint, the same rule as the online computation.

Because `Reset()` lives on the dump path, histogram statistics cover everything since the previous Reset; that includes samples already evicted from the ring, deliberately — p999 should not lose its tail to ring pressure.

## ProcSampler: process counters and probes

`ProcSampler` concentrates every `/proc` read into one low-frequency sampling point; the hot path never touches `/proc`.

- **Sampling rate**: `ProcSampleThread` defaults to 1 Hz; one sample reads `/proc/self/stat` and `/proc/self/status` once each (plus best-effort `/proc/self/io` and a `/proc/self/fd` directory scan), takes tens of µs, and costs about 0.005% CPU. When procfs is restricted, the io/fd series degrade to 0 without affecting the CPU/page-fault series.
- **Process counter series (per-period delta)**: `0` utime ns, `1` stime ns, `2` minflt, `3` majflt, `4` voluntary ctx switches, `5` nonvoluntary ctx switches. CPU time is already converted from ticks to ns.
- **Process resource series**: `6` rss_kb, `7` vsize_kb, `8` threads, `9` fd_count, `10` fd_limit (all instantaneous, KiB/count); `11` io_read_kb, `12` io_write_kb, `13` io_read_syscalls, `14` io_write_syscalls, `15` cpu_delay_ms (these are period deltas, not cumulative counters). The page size is cached in `Start()` and `sysconf` is not called again during sampling.
- **Probe series (instantaneous values)**: start at `kProbeBase = 16`, at most 8 of them. `RegisterProbe(fn, ctx)` returns a series id, or `-1` when full; the named overloads `RegisterProbe(name, fn, ctx)` / `RegisterCounter(name, counter)` write the name into type-8 metadata.
- **Registration sources**: `EventThread::RegisterObservability(&sampler)` registers the queue depth of every wait input that can report a `Depth()`, then the three overrun-reason counters (long callback / queue backlog / OS preemption), and, when input budgets are configured, a budget-exhaustion counter per input.
- **Regression constraint**: `proc_reads()` is the regression counter for "the hot path reads no /proc"; loop iterations must not increase it.
- **Capacity**: the ring holds 1024 entries, equal to the writer's type-5 budget, so even a full ring flushes in a single `FlushNew`. `Reset()` preserves the `/proc` delta baseline, so deltas across a reset stay correct.

Series ids still depend on registration order, but each generation's type-8 blocks carry the series-to-name mapping; `thyme_log_merge --json` outputs `series_name`. Type-9 blocks carry the node period/deadline, and `--hist --json` preserves both fields so p99 SLO alerts can compare them per node. `thyme_log_merge --sys --json` emits only `kind:"sys"` lines in timeline order and feeds straight into `thyme_metrics_exporter --sys-json`; resource/CPU/IO values are **period deltas**, exported as `*_per_interval` gauges — do not wrap them in `rate()`.

### Optional sources (all off by default)

These sources depend on kernel configuration, a mounted cgroupfs and namespaces, and are **not sampled by default**; a source not listed in `THYME_PROC_EXT` reads no files, registers no names and occupies no records.

```bash
# only PSI and networking; omitting this line disables everything (default)
export THYME_PROC_EXT=psi,net
# or enable everything at once (at most 42 records per period, so the 1024-entry ring holds about 24 periods)
export THYME_PROC_EXT=all
```

| Source | Token | What it reads | Preconditions / behavior on failure |
|---|---|---|---|
| cgroup CPU/memory/IO | `cgroup` | v2: `<self cgroup>/cpu.stat`, `memory.current`, `memory.max`, `memory.events`, `io.stat`; v1: `cpuacct.usage`, `cpu.stat` (throttled_time/nr_throttled), `memory.usage_in_bytes`/`limit_in_bytes`/`oom_control`, `blkio.throttle.io_service_bytes` | Detects v1/v2 automatically; if cgroupfs is invisible or this process's group is unreadable, the **whole source is marked unsupported** and no series are registered |
| host PSI | `psi` | `total` in `/proc/pressure/{cpu,memory,io}` (monotonic, read as a delta) | Requires `CONFIG_PSI`; otherwise unsupported. **Machine-wide**, not this cgroup |
| host block devices | `disk` | `/proc/diskstats`, counting only whole disks that have `/sys/block/<name>` (partitions are not double-counted) | Unsupported only if `diskstats` cannot be read; inside a container these are the host's numbers |
| host networking | `net` | `/proc/net/dev`, every interface except `lo` | Same as above; namespace/host scope is yours to interpret |
| thread CPU | `threads` | utime+stime from `/proc/self/task/<tid>/stat` for up to 8 threads registered with `RegisterThread(tid, name)` | `/proc/self/task` always exists; only explicitly registered threads occupy a series |

Capability visibility: when any source is requested, each sampling period first writes an `ext_caps` record (series 63, an instantaneous bitmask: 1 cgroup, 2 psi, 4 disk, 8 net, 16 threads) and then the fields actually read in that period. So when an offline reader sees "no cgroup data" it can tell "not requested", "kernel does not support it" and "nothing changed this period" apart.

Fields and units (series 64–80, 96–103):

| series | Name | Unit | Semantics |
|---|---|---|---|
| 64 | `cgroup_cpu_usage_us` | µs | period delta; v2 `usage_usec`, v1 `cpuacct.usage` (ns→µs) |
| 65 | `cgroup_cpu_throttle_us` | µs | time lost to throttling within the period |
| 66 | `cgroup_cpu_nr_throttled` | count | number of throttling episodes within the period |
| 67 | `cgroup_mem_current_kb` | KiB | instantaneous usage |
| 68 | `cgroup_mem_max_kb` | KiB | instantaneous limit; `0xFFFFFFFF` = unlimited |
| 69 | `cgroup_oom_events` | count | OOM events within the period |
| 70/71 | `cgroup_io_read_kb` / `_write_kb` | KiB | block-device bytes read/written within the period (summed over devices) |
| 72–76 | `host_psi_{cpu_some,mem_some,mem_full,io_some,io_full}_us` | µs | delta of the PSI total within the period |
| 77/78 | `host_disk_read_kb` / `_write_kb` | KiB | whole-disk bytes read/written within the period |
| 79/80 | `host_net_rx_kb` / `_tx_kb` | KiB | bytes received/sent within the period (excluding `lo`) |
| 96–103 | `thr_<name>_cpu_ns` | ns | per-period CPU time (utime+stime) of a registered thread |

Target-board note: whether the target kernel provides `CONFIG_PSI`, `CONFIG_TASK_IO_ACCOUNTING` and `CONFIG_TASK_DELAY_ACCT`, and whether its cgroups are v1 or v2, must all be confirmed on the board. This repository's x86 development container is **v1 with readable PSI** and an unreadable cgroup directory — both paths are covered by tests, but which ones the target board supports is bring-up evidence, not something this page can state.

Cost warning: these sources are **per-sampler**. Every event node in a generated role has its own `ProcSampler`, so `all` in a process with 8 event nodes means 8 PSI/diskstats/netdev reads per second (cgroup reads each node's own slice). At 1 Hz that is still far below 0.1% of one core, but with many nodes prefer enabling only the sources you need (for example `psi,net`) and prefer the `latest` probe over enabling everything per node.

### Whole-machine sampling

When the perf-log prefix is set, the supervisor writes `<prefix>_host_<pid>.bin` in addition to the role files: `/proc/stat` (overall + per core), `/proc/loadavg`, `/proc/meminfo`, `/proc/sys/fs/file-nr`, at 1 Hz, as type-5 + type-8, on the same time axis as the role perf files. The sampler exists **once per machine** (the supervisor is the only long-lived process), which avoids every role/event node re-reading the same host counters; for the same reason it belongs to no graph node and does not affect topology, record tables or health gates.

| series | Name | Unit | Semantics |
|---|---|---|---|
| 0–5 | `host_cpu_{user,system,idle,iowait,irq,steal}_ns` | ns | overall CPU period delta (user includes nice, irq includes softirq) |
| 6–8 | `host_load{1,5,15}_milli` | milli-load | instantaneous; divide by 1000 when exporting |
| 9/10 | `host_mem_total_kb` / `host_mem_available_kb` | KiB | instantaneous (`MemAvailable`, falling back to `MemFree` on old kernels) |
| 11/12 | `host_file_nr_allocated` / `host_file_nr_max` | count | system-wide fd table (`0xFFFFFFFF` = no limit) |
| 16+i | `core<i>_busy_ns` | ns | per-core busy delta = user+nice+system+irq+softirq+steal; `idle`/`iowait` excluded |

Notes:

- The core limit is **64** (`kMaxCpus`): machines with more cores keep the overall row plus the first 64 cores, and name blocks are registered only for cores that were actually emitted.
- Inside a container, `/proc/stat`, `/proc/loadavg` and `/proc/sys/fs/file-nr` are usually **host-global**; use cpuset/affinity filtering if you want "cores available to this container", and do not treat them as container-exclusive.
- This file merges with the role perf files through the same `thyme_log_merge --sys --json`, and the exporter exports it as `thyme_host_*`; with no perf-log prefix set, not a single byte is written.
- Sampling is 1 Hz over 4 small files per tick, far below 0.1% of one core; it runs on a dedicated supervisor thread, not on any real-time thread.

## Log rotation and budgets (`[logging]`)

Non-package append files (audit, `.fsm`, perf dump) share `RotatingFile`: `O_APPEND`, one `write()` per record, no userspace buffering, rotation by size via rename.

```ini
[logging]
fsm_max_bytes = 4194304
fsm_rotate_keep = 2
perf_max_bytes = 67108864
perf_rotate_keep = 2
total_budget_mb = 0
```

| Key | Default | Effect |
| --- | --- | --- |
| `fsm_max_bytes` | 4 MiB | rotation threshold for `<socket>.fsm`, must be `> 0` |
| `fsm_rotate_keep` | 2 | keeps `<path>.1 … <path>.N`, range 1..32 |
| `perf_max_bytes` | 64 MiB | rotation threshold per perf dump file |
| `perf_rotate_keep` | 2 | maximum number of perf generations, 1..32 |
| `total_budget_mb` | 0 | total budget per process (service + audit + FSM + perf); 0 = per-file limits only |

Default upper bounds (per file `max_bytes × (keep + 1)`, exceeded by at most one record):

| File | Default upper bound |
| --- | --- |
| `<socket>.audit` | 4 MiB × 3 ≈ 12 MiB |
| `<socket>.fsm` | 4 MiB × 3 ≈ 12 MiB |
| each perf dump | 64 MiB × 3 ≈ 192 MiB |

`total_budget_mb` is a **process-level** leash, not a filesystem quota: once the budget is exhausted, non-mandatory writers (FSM, perf) drop and count, while the audit writer never drops. The `logging` block of `/status` reports `budget_limit_bytes / budget_used_bytes / budget_drops / rotations`.

All of these keys are startup-only: the writers are already open and the rotation paths are fixed, so SIGHUP keeps the running values and warns. Ship changes with a restart.

## Reading and merging logs

`thyme_log_merge` is the only parser for the perf dump format; it can also merge `.fsm` files and JSONL records that carry a `ts` (mono ns) field.

```text
thyme_log_merge <inputs...> [--from ns] [--to ns] [--json] [--hist]
                [--expect-hash 0x<16 hex>] [-o out]
```

| Argument | Effect |
| --- | --- |
| `--from` / `--to` | clip the time window by absolute mono ns |
| `--json` | emit JSONL instead of the text timeline |
| `--hist` | skip the timeline, merge type-6 histograms and print p50/p90/p99/p999/max per `(node, series)` |
| `--expect-hash` | require every input's record manifest to match; a mismatch or a missing manifest exits 4 |
| `-o` | write to a file instead of stdout |

Exit codes: `0` success, `2` usage error, `3` an input cannot be opened, `4` record manifest mismatch.

Perf events with a type 10 anchor additionally emit `realtime_ns` (identically in text and JSONL): each input file independently uses the closest preceding anchor and converts with `realtime_ns + (event_mono_ns - anchor_mono_ns)`, overflow-safe, never borrowing another process's anchor. Records before the first anchor, legacy/JSONL inputs, and records whose delta conversion overflows simply lack the field; no wall clock is fabricated. An anchor with an illegal count/node, a truncated record, a mono regression or an unknown block type stops that input at the corrupted tail. Sorting and `--from/--to` still use mono only; `realtime_ns` is an additional axis for cross-process and cross-day alignment, and realtime jumps caused by wall-clock discipline never rewrite mono order.

Timeline (multi-process FSM + perf dump):

```bash
build/tools/inspect/thyme_log_merge/thyme_log_merge \
    /var/log/thyme/perf_node_*.bin \
    /tmp/thyme_trigger.sock.fsm --json
```

Histogram merge (consumed by the exporter and dashboards):

```bash
build/tools/inspect/thyme_log_merge/thyme_log_merge \
    /var/log/thyme/perf_node_*.bin --hist --json > /run/thyme/hist.jsonl
```

Each line looks like:

```json
{"node":3,"series":0,"n":18400,"p50_ns":8200,"p90_ns":16400,"p99_ns":32700,"p999_ns":65500,"max_ns":91234}
```

A rotated generation is a complete file (each generation carries the magic and the manifest, and the first non-empty dump carries the clock anchor), so you can feed the `.1…N` you have as independent inputs:

```bash
build/tools/inspect/thyme_log_merge/thyme_log_merge \
    /var/log/thyme/perf_node_sensor_*.bin.1 \
    /var/log/thyme/perf_node_sensor_*.bin --hist
```

## Benchmarks

All three wall-clock benches are `Release` builds run with `RUN_SERIAL`; ctest only verifies that the measurement pipeline works, it never turns a latency magnitude into an assertion.

| bench | Structure | `kind` output | Key metrics |
| --- | --- | --- | --- |
| `bench_core` | primitive microbenchmarks | `thyme-bench-core` | `latest_slot_write_read`, `latest_slot_inplace_write_read`, `spsc_ring_push_pop`, `spsc_ring_notify_no_waiter`, `spsc_ring_wake_one_waiter`, `ref_publish_release_fence`, `ref_publish_sc_store`, `trigger_config_load` |
| `bench_object_pool` | object pool alloc/free | `thyme-bench-object-pool` | `ns_per_op` plus sampled p99/p999 |
| `bench_trigger_seal` | real Center → TriggeredRecorder trigger→seal | `thyme-bench-trigger-seal` | `trigger_to_seal_ns` (n/min/p50/p90/p99/p999/max/mean) |

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4 --target bench_core bench_object_pool bench_trigger_seal
./build/bench_core --json bench-report/bench_core.json --tag x86_64-local
./build/bench_object_pool --json bench-report/bench_object_pool.json --tag x86_64-local
./build/bench_trigger_seal --iters 200 --json bench-report/bench_trigger_seal.json --tag x86_64-local
```

`bench_trigger_seal` defaults to `--iters` 64 (minimum 4) and a window of 20 ms pre / 50 ms post; `bench_core` and `bench_object_pool` accept only `--json PATH` and `--tag`. All three default the tag to `<arch>-local`, use `<arch>-ci` under `CI`, and allow `THYME_BENCH_RUNNER_TAG` to override it.

`eval_chains` is the end-to-end scenario benchmark; it runs the generated `head -> mid -> tail` fixture (head 200 Hz, mid/tail 400 Hz) and offers three loads:

| Scenario | Load | Failure/degradation semantics |
| --- | --- | --- |
| `cpu` | 3 spinning processes pinned to CPUs 1/2/3 | pure userspace busy-wait |
| `io` | 2 writers looping 16 × 64 KiB to `/tmp` then `fsync`/unlink | cannot open or write fails → `degraded: true` |
| `irq` | 1 `timerfd` storm (1 µs first, 50 µs interval) + 2 `sched_yield` storms | `timerfd_create` denied → `degraded: true` |

```bash
cmake --build build -j4 --target gen_graph
tools/ci/eval_chains_build.sh \
  --gen-graph build/tools/gen/gen_graph/gen_graph \
  --thyme "$PWD" --out build-eval-chains
python3 tools/ci/run_eval_chains.py \
  --eval-chains build-eval-chains/build/eval_chains \
  --seconds 10 --out eval-chains-report/eval-chains.json \
  --raw-dir eval-chains-report/raw
```

To run a single scenario and look at the raw time series:

```bash
build-eval-chains/build/eval_chains --mode inproc --seconds 10 \
  --chain head,mid,tail --load irq > /tmp/eval-irq.json
```

After normalization only two metrics enter the gate: `frames_per_sec` (higher is better) and `e2e_p95_ns` (lower is better); `frames`, `broken` and `broken_pct` are kept as context, and a `broken_pct` above `--max-broken-pct` (default 5%) fails the run outright. Zero completed frames also fails — that means the chain or the reporter is broken, not that a load tool is missing.

## Baselines, tolerances and gates

Bench baselines live in `tools/ci/bench_baseline.json`, keyed by `runner_tag`, and each entry records `git_sha`, toolchain, build type and host.

| Tag | Status | Use |
| --- | --- | --- |
| `x86_64-local` | recorded | reference evidence from the development machine; never a target-board budget |
| `x86_64-ci` | not set | recorded by the first scheduled run; until then the gate prints `no-baseline` and exits 0 |

Tolerances (defaults, overridable per runner):

| Statistic | Tolerance | Why |
| --- | --- | --- |
| `ns_per_op` (median/steady state) | `+25%` | catches real regressions while absorbing host/cache noise |
| `p99` | `+50%` | tail samples are noisier on shared runners |
| `p999` | `+100%` | with fewer than 1000 samples p999 is just max, a coarse signal at best |

`--tolerance`, `--tolerance-p99` and `--tolerance-p999` override them for one run. `p99`/`p999` are **sampled per operation** and include the cost of reading the clock, so they can only be compared against the baseline of the same runner, never used as an absolute latency budget.

Gate semantics:

| Situation | Result | Exit code |
| --- | --- | --- |
| runner tag not in the baseline | `no-baseline`, rows marked `new` | 0 (1 with `--require-baseline`) |
| hostname/arch differs from the entry | `host-mismatch`, skipped | 0 (1 with `--require-host-match`) |
| a baseline metric is missing from this run | row `missing` | 1 |
| `ns_per_op`/`p99`/`p999` tolerance exceeded | row `fail` | 1 |
| new metric/percentile | `new`, not gated | 0 |
| bench document says `"ok": false` | `fail` | 1 |

Only the document kinds passed on the command line are compared, so running just `bench_core` will not report the object-pool rows as missing. Use `--write-baseline` to record or refresh a baseline; the write merges by key rather than replacing the whole section.

The eval_chains baseline lives in `tools/ci/eval_chains_baseline.json` with the same rules, but its default tolerance is **100%**: chain latency on a shared host is bimodal (each hop either catches the next period or the one after), so the gate only catches order-of-magnitude regressions plus missing/zero values. A metric present in the baseline but missing from this run fails; a new metric is `NEW` and not gated. No baseline, no gate; host mismatch is skipped by default; refreshing also uses `--write-baseline` and requires every scenario to be `ok`.

## Metrics export and dashboards

thyme ships no Prometheus exporter and no HTTP metrics endpoint of its own. `thyme_metrics_exporter.py` is a sidecar: it reads existing interfaces and renders the Prometheus text format.

```bash
python3 tools/inspect/thyme_metrics_exporter/thyme_metrics_exporter.py \
    --status file:/tmp/status.json \
    --probe /tmp/probe.json --hist /tmp/hist.jsonl \
    --audit /tmp/thyme_trigger.sock.audit \
    --disk /opt/usr/triggered_data --shm-dir /dev/shm --once
```

| Argument | What it reads | Main outputs |
| --- | --- | --- |
| `--status PATH` (default `/run/thyme/status.sock`) | AF_UNIX `GET /status`, or a `file:PATH` snapshot | `thyme_status_*`, `thyme_role_*`, `thyme_recorder_*`, `thyme_trigger_relay_*` |
| `--probe FILE` / `--probe-bin PATH` | `thyme_probe list --json` | `thyme_shm_page_*`, `thyme_probe_up` |
| `--hist FILE` | JSONL from `thyme_log_merge --hist --json` | `thyme_perf_latency_*`, `thyme_perf_samples`, `thyme_hist_up` |
| `--audit FILE` | audit log | `thyme_trigger_audit_*`, `thyme_audit_up` |
| `--disk PATH` | `statvfs` | `thyme_filesystem_{size,avail}_bytes`, `_used_percent` |
| `--shm-dir PATH` | `statvfs` + `scandir` | `thyme_shm_*`, `thyme_shm_objects` |

Conventions: a failing single source does not spoil the whole snapshot (an unreadable status still yields `thyme_status_up 0` and exit code 0); `--out PATH` replaces atomically via `PATH.tmp` + `os.replace()`; exit code `0` produces a snapshot, `2` is an argument error, `3` means the output could not be written. Do **not** point `--hist` at a perf dump — it does not parse perf dumps, and `thyme_log_merge` is the only parser.

Wiring it into the node_exporter textfile collector:

```ini
# node_exporter startup flag
--collector.textfile.directory=/var/lib/node_exporter/textfile
```

```bash
python3 tools/inspect/thyme_metrics_exporter/thyme_metrics_exporter.py \
    --recorder-status /tmp/thyme_recorder.sock \
    --probe-bin build/tools/inspect/thyme_probe/thyme_probe \
    --hist /run/thyme/hist.jsonl \
    --audit /tmp/thyme_trigger.sock.audit \
    --disk /opt/usr/triggered_data --shm-dir /dev/shm \
    --out /var/lib/node_exporter/textfile/thyme.prom --once
```

The dashboards and alerts are static artifacts in this repository: `dashboards/grafana/thyme-overview.json` (12 panels, provisionable), `dashboards/foxglove/thyme-layout.json` (for replay, 5 panels) and `dashboards/alerts/thyme-alerts.yml` (29 live rules in the `thyme.runtime` group). `ThymeExporterStale` uses `absent()` to catch an exporter that has stopped. Recorder Stats and CenterStats drops come from `--recorder-status`; overrun names and node period/deadline come from perf type-8/9.

## Workflow: measure → compare → gate

1. **measure**: fix the build type, runner tag, window and iteration count, and produce a `--json` document; keep the raw perf dumps and the `--hist` JSONL as evidence.
2. **compare**: compare against a baseline from the same runner, never across machines; look at p50/`ns_per_op` first, then p99/p999, and finally use `--hist` to see whether the whole tail moved right.
3. **gate**: CI enforces the baseline gate only on scheduled/manual jobs; merge requests do not run the wall-clock gate. When it fails, confirm the host and build identity first, then decide whether to fix the code or re-record the baseline.

```bash
# 1) take a measurement
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4 --target bench_core bench_object_pool bench_trigger_seal
mkdir -p bench-report
./build/bench_core --json bench-report/bench_core.json --tag x86_64-local
./build/bench_object_pool --json bench-report/bench_object_pool.json --tag x86_64-local
./build/bench_trigger_seal --json bench-report/bench_trigger_seal.json --tag x86_64-local

# 2) compare against the baseline (the exact entry point CI uses)
python3 tools/ci/check_bench.py \
  --run bench-report/bench_core.json \
  --run bench-report/bench_object_pool.json \
  --run bench-report/bench_trigger_seal.json \
  --json-out bench-report/bench-gate.json

# 3) end-to-end scenario (local equivalent of the scheduled job)
python3 tools/ci/check_eval_chains.py \
  --run eval-chains-report/eval-chains.json \
  --json-out eval-chains-report/eval-chains-gate.json
```

The gate result document (`thyme-bench-gate`, `schema: 2`) carries the runner, host, the three tolerances, and `status` / `p99_status` / `p999_status` per metric. Archive it together with the raw dumps, so the next regression can answer "did the code change, or did the machine change?".

## Related documents

- [`config-deploy.md`](config-deploy.md) — `[logging]` keys, startup self-checks and deployment gates
- [`triggered-acquisition.md`](triggered-acquisition.md) — measuring the trigger→seal SLO and the package-side evidence
- [`probe.md`](probe.md) — SHM page fields and the read-only views of `thyme_probe`
- [`tools-other.md`](tools-other.md) — tool overview and exit-code conventions
