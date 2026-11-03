# H1 performance budgets and placement constraints

This page turns the H1 risks into executable budgets, placement constraints, and
an evidence index. **The deliverable is budgets and constraints, not proof that
target hardware, HIL, or a 72 h soak has passed.** `pending` below is not a
pass, and short x86 microbenchmarks do not replace whole-target measurements.

Status terms:

- **x86 value available:** local data with a documented measurement boundary;
  runner/host must still be checked. Usable only as same-machine reference or
  same-runner regression evidence.
- **Test exists:** proves the listed correctness/invariant, not CPU, bandwidth,
  RSS, latency, or a 72 h budget.
- **pending/HIL:** requires a specified target, real deployment, or long soak;
  never close the corresponding gate or claim target readiness without it.

## 1. Evidence identity and archive

Create `perf-evidence/<git_sha>/<runner_tag>/` for every performance change and
soak run (the directory is an archive convention; build artifacts are not
committed):

```text
manifest.json             git SHA, dirty state, runner_tag, hostname, arch, CPU,
                          toolchain, build_type, config hash, start/end, operator
bench-core.json           raw bench_core JSON
bench-object-pool.json    raw bench_object_pool JSON
bench-trigger-seal.json   raw bench_trigger_seal JSON
bench-gate.json           check_bench gate result
eval-chains.json          end-to-end scenario results (link/placement changes)
eval-chains-gate.json     end-to-end gate result
sched.txt                 actual ps/chrt/taskset/systemctl scheduling state
drain.pidstat             drain CPU and disk time series
probe.pidstat             probe/logger CPU time series (per thread or process)
memory.pidstat            72 h RSS/VSZ/page-fault time series
shm.tsv                   /dev/shm usage and PinnedSlot geometry over time
probe-info.before/after   pinned_skip, lease_steal, and page counters
page-hash.before/after    read-only hash of quiescent pages
notes.md                  workload, peak-rate math, thresholds, anomalies, verdict
```

Never label a runner only `x86_64`; use `<arch>-local` / `<arch>-ci` or a
dedicated target tag and also record the real hostname in `manifest.json`. Never
copy `x86_64-local` numbers into a CI or target row. Every performance change
must attach runner/host, raw JSON, and gate output; "feels faster locally" or a
screenshot is not evidence. Baselines and tolerances are in
[bench-baselines.md](../guides/bench-baselines.md).

## 2. Placement and bandwidth constraints

### 2.1 Required deployment invariants

Number online logical CPUs and partition non-overlapping `RT_CPUS`,
`DRAIN_CPUS`, and `AUX_CPUS`. CPU sets must name concrete CPU ids; "let the
kernel choose" is not accepted.

| Role | CPU set and scheduling | CPU/bandwidth budget | Current status |
|---|---|---|---|
| recorder drain | Fixed `DRAIN_CPUS`; isolated from RT; non-RT or lower priority than all business RT; I/O in a separate cgroup; peak accounting, not idle-bandwidth assumptions | Sustainable drain write bandwidth `>= 1.2 × peak recording rate`; during bursts `pinned_skip` no higher than baseline and no unexpected `lease_steal` growth | Drain runs on the host path, not a node RT callback. `io_rate_limit_mbps` is an upper/sustained-rate input, not a minimum guarantee. Fixed drain affinity, a dedicated drain thread, and `io.max` are **not yet in deployment files; pending/HIL** |
| logger | Fixed `AUX_CPUS`; `SCHED_OTHER`/non-RT; file sink, rotation, and export must not use the RT set | Logger total `<= 0.5%` of one core; independent 1 Hz `/proc` sampler `<= 0.1%` | Fixed stack buffers, rate limiting, and delayed RT rotation exist; the sink still has mutex + `write(2)`, and the async log ring/logger placement are not complete. Target CPU unmeasured |
| `thyme_probe` / metrics sidecar | Fixed `AUX_CPUS`; non-RT; continuous observation defaults to 1 Hz; never on `RT_CPUS` | 60 edges total `<= 0.5%` of one core; writer skip matches the no-probe baseline | Read-only, no pin, no cursor advance is tested; `watch` defaults to `--period 1000`. There is no `--follow`. 60-edge CPU and target skip A/B **pending/HIL** |
| RT cyclic/event | Fixed `RT_CPUS`; explicit `sched_policy: fifo` + `priority` + `placement_fail: require`; run admission first | Reserved RT cores `<= online cores − 1`, leaving at least one for drain/logger/probe/OS; per-core `ΣWCET/period <= 1.0`, `> 0.9` warns | Graph CPU sets, RM, actual scheduling mismatch, and admission are implemented/tested; the default best-effort path is not an RT guarantee. `SCHED_DEADLINE` is not enabled |

Additional constraints:

- `DRAIN_CPUS` and `AUX_CPUS` may share a non-RT headroom pool, but the total
  CPU and I/O contention must be validated together; per-role thresholds do not
  prove the combination.
- `NRT = online_cpus − RT_reserved >= 1` is a lower bound, not proof that drain
  has enough. If drain needs a dedicated core, carve it out and redo admission.
- cgroup `io.max` throttles but does not guarantee minimum bandwidth. Measure
  minimum bandwidth on the target disk with the real filesystem, mount options,
  concurrent I/O, and bursts; use a separate device/IO weight if needed.
- `chrt`/`ps` proves one instant's policy/priority/affinity; recheck after
  restart. `sched_mismatches == 0` does not replace external `ps` evidence for
  recorder/probe.
- Current systemd units have only `IOWeight=100`; they do not set
  `CPUAffinity=`, fixed recorder/probe CPU sets, or per-device `io.max`. BusyBox
  paths have none of these either. Do not describe this as "deployment is
  isolated".

Scheduling fields, RM, admission, and `SCHED_DEADLINE` boundaries are in
[scheduling-model.md](scheduling-model.md); blocking points are in
[priority-inversion-audit.md](priority-inversion-audit.md).

### 2.2 Target collection and decision

Run these on the target; they need `sysstat` (`pidstat`), `util-linux`, and
systemd (systemctl lines only). Substitute real values and archive stdout/stderr
verbatim. `pidstat` `%CPU=100` means one logical core is saturated; do not
divide by the machine's core count.

```bash
GIT_SHA=<commit> RUNNER=<target-runner> OUT=perf-evidence/<commit>/<target-runner>
mkdir -p "$OUT"
RECORDER_PID=<pid> PROBE_PID=<pid> DRAIN_TID=<tid> LOGGER_TID=<tid>

ps -Lo pid,tid,cls,rtprio,psr,comm -p "$RECORDER_PID,$PROBE_PID" > "$OUT/sched.txt"
chrt -p "$RECORDER_PID" >> "$OUT/sched.txt"
taskset -pc "$RECORDER_PID" >> "$OUT/sched.txt"
systemctl show thyme-trigger-center -p CPUAffinity -p IOWeight \
  -p IOSchedulingClass -p IOSchedulingPriority >> "$OUT/sched.txt"

pidstat -t -d -p "$RECORDER_PID" 1 > "$OUT/drain.pidstat"
pidstat -t -p "$PROBE_PID" 1 > "$OUT/probe.pidstat"
build/tools/inspect/thyme_probe/thyme_probe info /thyme_cloud_1 --json \
  > "$OUT/probe-info.after"
```

`DRAIN_TID`/`LOGGER_TID` attribute `-t` output; if recorder/logger have no
dedicated threads, do not label process totals as those threads. Compute:

```text
peak_record_Bps = max_over_configured_burst(sum(frame_bytes × frame_rate_hz))
required_drain_Bps = 1.2 × peak_record_Bps
measured_drain_Bps = successful_package_payload_bytes / active_drain_seconds
PASS only when measured_drain_Bps >= required_drain_Bps
                  and pinned_skip(after)-pinned_skip(before) <= approved_baseline_delta
                  and lease_steal has no unexpected increase
```

`io_rate_limit_mbps >= required_drain_Bps / 1,000,000` is a configuration
prerequisite, not a substitute for `measured_drain_Bps`; sustained-rate checks
do not prove burst capacity. For concurrent drains, attach per-channel and
aggregate curves; do not multiply single-channel capacity by the number of
channels.

## 3. Hot-path exclusions and current proof

| Constraint | Executable configuration / existing test | Open gap |
|---|---|---|
| Whole-package SHA-256/HMAC is not on the RT thread | Recorder host finalizes at seal/invalidation; successful writes update the hash incrementally; `test_package_integrity` uses `--wrap=read` to prove online seal does not re-read package content | Startup crash sweep may re-read a small WAL for a trusted checkpoint, or all surviving files when untrusted; recovery stays on the host path, and target seal CPU/I/O curves are pending |
| Redaction is not on the RT thread | Offline processing is the constraint | No acceptable redaction pipeline exists in this repo; do not claim "redacted" or a redaction performance report |
| Trigger packages are compressed externally after sealing | No in-thyme compression; the core does not change `.tbin`/`.raw`; continuous recording is uncompressed; H.264 is not recompressed | External tar+zstd CPU/I/O is not thyme deployment evidence; if deployed, measure it separately and never call recorder/RT callbacks |
| loop/frame does not read `/proc` | `ProcSampleThread` samples independently at low frequency; `test_proc_sampler` proves 100k hot-loop `Note()` calls add no `/proc` reads | Covers only the tested path, not every downstream node; 60-edge and whole-target CPU pending |
| loop/frame does not `fprintf`/format per message | Forbidden by CR-03; `test_log` covers rate limiting, fixed buffers, rotation, and zero allocation; `test_realtime_guard` covers delayed RT rotation | New downstream call sites still need review/static scanning; the sink mutex+write risk remains |
| loop/frame does not heap-allocate | `test_core_no_alloc` covers cyclic, record tap, and event dispatch after Start+warm-up; `test_log`/`test_histogram` have their own sentinels; `test_triggered_recorder` covers fixed descriptor rings | `test_core_no_alloc` excludes recorder drain; test scope cannot be extrapolated to uncovered downstream code |

**Static-check status:** CR-01/03 and the priority-inversion audit are rules and
manual/static review; allocation sentinels are runtime tests. There is no
independent CI source scan for `/proc`, `fprintf`, or `new` on loop/frame paths.
Therefore "sentinel + CI static check both pass" is currently **partial
evidence, static CI pending**; do not report G-PERF-3 fully green.

## 4. H1 budgets and evidence index

"Current evidence" describes available material only; "verdict" says what is
missing. All numbers are x86 reference, not target budgets.

| H1 item | Executable budget/threshold | Current x86 values and tests | Verdict / missing proof |
|---|---|---|---|
| Drain, scheduling, probe | drain `>=1.2× peak recording rate`; probe/logger `<=0.5%` of one core; RT cores `<= cores−1`; `pinned_skip <= baseline`; fixed CPU sets/priorities; admission first | Graph placement/RM/admission and scheduling observability tests exist; eval chains have x86 link-load numbers, but they do not prove drain/probe | **Target placement, drain bandwidth, CPU pending/HIL**. Collect `sched.txt`, pidstat, pin/steal curves; `IOWeight` alone is not proof |
| Wakeup cost | `<=1` wake syscall per message; 0 without waiters; kHz event edges `<=1%` of one core | `test_wake_coalesce` pins syscall counts with ptrace. Historical x86_64/Release: no-waiter Notify **4.559 ns**, single-waiter cross-thread round trip **1109.414 ns**; at 1 kHz about **0.000456% / 0.111%** of one core. `bench_core` can reproduce | x86 microbenchmark and correctness exist; real multi-reader event edges and target syscall/CPU **pending/HIL**. Keep same-runner JSON; round-trip numbers are not full-node WCET |
| Package integrity cost | Incremental hash on write; online seal does not re-read; seal meets TRG-14; once per package, not per frame/record; not on RT | `test_package_integrity` read wrapper proves online seal without signatures performs `read()==0`; covers signing/big-frame/continuous/partial. Historical x86 64-sample trigger→seal p99 **82.4 ms**, about **12 ms** above the window floor, meeting the 50 ms reference SLO for that package type | x86 seal numbers and no-re-read evidence exist; default-pending-quota large-package I/O bytes and target seal histogram **pending/HIL**. Crash-recovery re-reads must not be misreported as online-seal regression |
| Observability overhead | Independent 1 Hz `/proc` `<=0.1%` of one core; probe/logger constraints as above; zero formatting/allocation on the hot path; before/after bench difference `<1%` | `test_proc_sampler`, `test_log`, `test_core_no_alloc` exist. Historical short-window `SampleNow≈6.3 µs`, about `0.0006%` at 1 Hz, but the full runner/host was not retained | Static CI and sampling A/B `<1%` **pending**; logger and 60-edge target CPU **pending/HIL**; 72 h counters/RSS under G-PERF-5 |
| Trigger wake latency | `<=50 µs` per wakeup; no per-message socket/copy; SHM mirror + eventfd batch wakeups; large frames only leases/descriptors; trigger→first-frame write no worse than baseline, CPU not higher | Default fork. `test_trigger_center_split` measured on one x86 host, three rounds × 8 runs: max wake **10.8 µs**; first-frame median difference within **1.4 ms**, with one split round lower (including 20 ms post grace); idle 1 s tick 0/1 split, 2/3 same process. Command pages carry only channel names; frames stay in mirror/lease | Local samples meet the budget. Target re-test **pending/HIL**; do not present these as board results |
| Swap-ring threshold | Re-evaluate only when armed steady-state copying `>=5%` of one core or `>=50 MB/s`; otherwise keep copies; adoption needs equivalence + bandwidth/instruction report | Current reference workload about **1.2 MB/s**; x86 240 KiB tap write **7,588.5 ns/op** (short window) plus the rate-budget formula | Threshold not triggered; keep copies. Full-channel summation and target recheck pending. Do not compare a 32 GB/s microbenchmark capability with a 50 MB/s workload |
| Read-only probing cost | 1 Hz, 60 edges `<=0.5%` of one core; continuous observation is not default high frequency; read newest only, no pin/write/cursor advance; page hash unchanged; writer skip matches no-probe baseline | `test_thyme_probe` hashes quiescent pages whole, covers tail stability, no pin/skip/steal, and consumers still reading; CLI in [probe.md](../usage/probe.md) | Read-only correctness exists; 60-edge CPU, live writer-skip A/B, target CPU **pending/HIL**. Quiescent-page hashing is not proof that a running page's bytes are unchanged |
| Cacheline layout | `alignas(64)` only for control headers/hot shared structs; never payload slot arrays; RSS growth `<=5%` | [cacheline-audit.md](cacheline-audit.md) records layouts and rationale; `test_cacheline_audit` pins sizes/offsets with static_assert; no `pahole` at the time, compiler layout + `-Wpadded` used | Layout correctness exists; pre/post RSS and target `pahole` **pending/HIL**. Do not substitute summed `sizeof` for process RSS |
| Pinned-slot capacity | `N >= ceil((before+after+per_frame_write)×rate)+pin_budget`; effective capacity N−R; one pin per frame; 19.2 MB per slot/path counted in total memory; `pinned_skip` thresholded | `test_pinned_slot_lease_capacity` covers N−R; SCH-12 uses the same formula with `pin_budget` | x86 capacity semantics exist; real multi-reader memory accounting and target pin/skip curves **pending/HIL**. Power-of-two-rounded memory must also be counted |
| Perf ring memory | Per-process default perf ring total `<=32 MiB`; per-edge capacity covers `rate×window×1.5`, never above the window cap; reset after periodic dump; 72 h RSS bounded | `test_perf_ring` covers `[4096,262144]`, fixed capacity, dump-reset, crash readability; a sample is 12 B, a max ring about 3 MiB. Format/formula in [perf.md](../usage/perf.md) | Window/capacity correctness exists; **default total memory and 72 h RSS/SHM curves pending/HIL**. Sum sampler, work, proc, histograms, and all node/edge instances, not one ring |
| Schema sidecar cost | One `schema.json` per package; record header carries only id/hash; package overhead `<=1%`; not on the per-record hot path | `test_record_schema`/`test_record_manifest` cover sidecar, manifest identity, strict parsing, tampering | Sidecar mechanism exists; small-package `<=1%` overhead and write-bandwidth A/B **pending**. If small fixed packages cannot meet 1%, record a size-floor/budget review; do not delete the threshold or claim pass |

Memory accounting must include at least:

```text
pinned_slot_shm_bytes = sum(channels: N × slot_stride + N × 64 + channel/lease headers)
perf_bytes(process) = sum(all sampler/work/proc rings + fixed histograms + writer buffers)
total_memory = sum(process RSS) + unmapped-but-reserved SHM accounted once
required: total_memory <= target_memory_budget
          and perf_bytes(process) <= 32 MiB
          and layout-change RSS delta <= 5%
```

Do not count shared SHM once per attacher, and do not omit reservations that
have not faulted in yet. Report both "configured reservation" and "actually
touched". PinnedSlot's 19.2 MB per slot/path and power-of-two growth must enter
the reservation.

## 5. Gate commands and stage exit

### 5.1 Common build and test commands

These produce current x86 evidence; archive test output and JSON together. They
do not perform target bursts, 60-edge, or 72 h measurements.

```bash
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DTHYME_WERROR=ON
cmake --build build-perf -j4 \
  --target bench_core bench_object_pool bench_trigger_seal \
           bench_copy_budget bench_pinned_slot_copy
ctest --test-dir build-perf --output-on-failure -R \
'^(test_core_no_alloc|test_log|test_proc_sampler|test_perf_ring|test_package_integrity|test_record_schema|test_record_manifest|test_thyme_probe|test_pinned_slot_lease_capacity|test_cacheline_audit|test_wake_coalesce)$'

mkdir -p bench-report
./build-perf/bench_core --json bench-report/bench_core.json --tag x86_64-local
./build-perf/bench_object_pool --json bench-report/bench_object_pool.json --tag x86_64-local
./build-perf/bench_trigger_seal --iters 200 \
  --json bench-report/bench_trigger_seal.json --tag x86_64-local
./build-perf/bench_copy_budget \
  --json bench-report/bench_copy_budget.json --tag x86_64-local
./build-perf/bench_pinned_slot_copy \
  --json bench-report/bench_pinned_slot_copy.json --tag x86_64-local
python3 tools/ci/check_bench.py \
  --run bench-report/bench_core.json \
  --run bench-report/bench_object_pool.json \
  --run bench-report/bench_trigger_seal.json \
  --require-baseline --require-host-match \
  --json-out bench-report/bench-gate.json
```

Use `--require-baseline --require-host-match` for formal stage exit so
`no-baseline` or `host-mismatch` cannot be skipped into a false pass. The
independent copy/pinned-slot JSON does not enter `check_bench.py`; keep the raw
JSON and judge per their documents.

### 5.2 Per-gate criteria

| Gate | Executable acceptance/evidence | S2/S3/S4 exit |
|---|---|---|
| G-PERF-1 | Review all rows in §4; fill actual values, thresholds, verdict, and evidence paths into `notes.md`; run applicable tests/benches. **Missing values must say pending, never N/A or pass** | S2: budgets/constraints/commands complete, target rows may be explicitly pending; S3: applicable x86 measurements and regressions for this stage; S4: all rows measured on target and within budget; any over-budget/missing budget blocks |
| G-PERF-2 | Collect `drain.pidstat`, `probe-info.before/after`, and package bytes during a real burst; record `peak_record_Bps`, `required_drain_Bps`, `measured_drain_Bps` in `notes.md`; no generic burst tool exists, so drive it with the deployment workload, not `bench_copy_budget` | S2/S3: target curves may be pending; S4: measured target bandwidth `>=1.2× peak`, skip no higher than the approved baseline, no unexpected steal growth. No burst curve means fail |
| G-PERF-3 | Run `test_core_no_alloc test_log test_proc_sampler test_histogram test_triggered_recorder`; archive CR-01/03 review and static-scan output. Only manual audit exists today; independent CI source scanning is still missing | S2/S3: sentinels pass on covered paths, but the gate is marked "static CI pending"; S4: CI static checks and sentinels cover all loop/frame paths. Do not claim full pass now |
| G-PERF-4 | Run `test_package_integrity test_record_schema`; archive seal JSON/read-wrapper results; review that hash/HMAC/redaction/compression are not in `Loop/Dispatch/On*`. External compression starts only after `PackageComplete()` and keeps its own log | S2/S3: host-boundary/no-re-read tests pass; mark redaction/external compression not deployed if absent; S4: target scheduling/trace proves none are on RT, plus post-seal evidence if external compression is deployed |
| G-PERF-5 | Run `test_perf_ring test_cacheline_audit test_pinned_slot_lease_capacity`; write `memory-plan.tsv` per §4. On target, sample 72 h with `pidstat -r -p "$RECORDER_PID" 60 4320 > "$OUT/memory.pidstat"` and record `shm.tsv` on the same cadence | S2/S3: configured memory accounting and x86 correctness, RSS may be pending; S4: RSS+SHM reservation and measured values within total budget, perf<=32 MiB, layout RSS<=5%, no sustained leak or counter drift over 72 h. Short runs are not a substitute |
| G-PERF-6 | Run `test_thyme_probe` and keep quiescent-page hash/read-only results; on target, sample the real 60 edges at 1 Hz with `watch --count 1 --period 1000 --json`, archive aggregate `probe.pidstat`, and run writer-skip A/B with/without probe. Page hash applies only to quiescent pages | S2/S3: read-only correctness passes, 60-edge CPU/skip may be pending; S4: 60 edges total `<=0.5%` of one core, quiescent hashes unchanged, writer skip matches baseline. `test_thyme_probe` alone is not sufficient |
| G-PERF-7 | For any H1 change run the three baseline benches + `check_bench.py`; for copy/large-frame changes also run their benches; for link changes run the three eval-chain groups and generate gate JSON. Archive runner/host and raw JSON | All stages: H1 regression blocks merge; formal exit requires baseline+host match; skip is not pass. S3/S4 also need stage target evidence; MR correctness tests cannot replace nightly/HIL benches |

For the 72 h SHM curve, collect alongside `memory.pidstat`, one point per 60 s,
4320 points. `<shm-names...>` must be the deployment's actual segment names; do
not glob and double-count hard links or parent pages.

```bash
OUT=perf-evidence/<commit>/<target-runner>
for i in $(seq 1 4320); do
  date -u +%Y-%m-%dT%H:%M:%SZ
  du -sb /dev/shm/<shm-names...>
  sleep 60
done > "$OUT/shm.tsv"
```

### 5.3 Current blockers

All remain **pending**, not completed acceptance:

1. Target CPU sets/priorities, cgroup I/O, drain `1.2×` peak bandwidth, and
   pin/steal curves; dedicated drain/logger placement is not in deployment
   files.
2. Probe/logger target CPU, 60-edge 1 Hz aggregate metering, and writer-skip A/B.
3. Trigger wake latency on target; local x86 A/B does not replace board results.
4. 72 h RSS+SHM, total perf memory, and target multi-reader PinnedSlot capacity /
   pin-duration curves.
5. ARM/target wakeup, seal, schema overhead, and real-workload copy rechecks.
6. Independent CI static source scanning for G-PERF-3 and OBS-04 sampling
   before/after `<1%` bench A/B.
7. Quantified measurements for small-package `<=1%` overhead and layout RSS
   `<=5%`.
8. A four-round formal local bench gate with
   `--require-baseline --require-host-match` is still not fully green; failures
   include `spsc_ring_push_pop` tail latency and
   `latest_slot_inplace_write_read.ns_per_op`, with round-to-round drift. The
   baseline and tolerances were not changed, so G-PERF-7 cannot be marked pass.

This page supports S2 "budgets and commands executable" review and same-runner
S3 regression, **but is not by itself proof of full S3 performance exit or S4
target/HIL exit.**
