# eval_chains scenarios (OBS-10)

`eval_chains` is the performance reporter shipped by every generated app
scaffold ([scaffold/tools/eval_chains.cpp](../../tools/gen/gen_graph/scaffold/tools/eval_chains.cpp)).
It runs the generated pipeline in-process (or forks `app --mp`), samples the
per-node latency/work rings and per-edge latency sampler, joins head-to-tail
frames into an end-to-end (E2E) latency, and prints a JSON report. The report
contains percentile summaries plus per-sample series for the offline renderer
([render_eval.py](../../tools/gen/gen_graph/scaffold/tools/render_eval.py)).

OBS-10 puts that report on a reproducible schedule: three load scenarios, a
compact normalized JSON, and a per-runner baseline gate. The scenarios run in
the scheduled (or manually triggered) `perf-eval-chains` job (see
[`.github/workflows/ci.yml`](../../.github/workflows/ci.yml)); nothing wall-clock is gated on merge
requests.

## Scenario definitions

Each scenario is a list of `test-linux`-safe actions implemented **inside**
`eval_chains`; no external stress tool (`stress-ng`, `stress`, `fio`) is
installed by the job.

| Scenario | Load | Why it is reproducible |
|----------|------|------------------------|
| `cpu` | Three spinner processes pinned to CPUs 1, 2, 3 (a 64-bit LCG loop, no I/O) | Pure userspace busy-work; fails only if `sched_setaffinity` is unavailable, which does not change the exit code |
| `io` | Two writer processes, each looping: write 16 × 64 KiB to `/tmp/thyme_eval_io_<pid>`, `fsync`, close, unlink | Uses only `/tmp`; if the write or `open` fails the child exits and the run is marked DEGRADED |
| `irq` | One `timerfd` storm (1 µs first expiry, 50 µs interval) plus two `sched_yield` storms | Linux timer/softirq + scheduler churn without privileged tools; if `timerfd_create` is blocked the child exits and the run is marked DEGRADED |

The app under test is the committed fixture
[`tools/gen/gen_graph/testdata/eval_chains`](../../tools/gen/gen_graph/testdata/eval_chains):
`head -> mid -> tail`, head at 200 Hz and mid/tail at 400 Hz, one SPSC ring per
hop, distinct pools so the frame join can attribute every hop. The faster
downstream nodes drain any transient backlog instead of letting it accumulate
for the rest of the run; `gen_graph` prints a report-only `RATE_WARN` for that
deliberate declared-rate/derived-rate mismatch. The nodes forward one frame per
loop and call `NoteInFrame` / `NoteOutFrame` / `NoteLatencyEdge`, which is the
same instrumentation generated nodes use.

### Degradation

`eval_chains` starts the load processes, waits 100 ms, and checks that each
child is still alive. It prints `load=<name> ready (N processes)` or
`load=<name> DEGRADED (alive/requested)` on stderr and emits
`load_requested` / `load_alive` in JSON. The run still produces metrics; the
driver logs a WARNING and records `degraded: true` instead of failing the job.
A scenario with zero completed E2E frames **does** fail: that means the chain
or the reporter is broken, not that a load tool is missing.

## Local reproduction

```bash
# 1. Build gen_graph, then generate + build the fixture app's eval_chains.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4 --target gen_graph
tools/ci/eval_chains_build.sh \
  --gen-graph build/tools/gen/gen_graph/gen_graph \
  --thyme "$PWD" --out build-eval-chains

# 2. Run the three scenarios and write the normalized run JSON.
python3 tools/ci/run_eval_chains.py \
  --eval-chains build-eval-chains/build/eval_chains \
  --seconds 10 --out eval-chains-report/eval-chains.json \
  --raw-dir eval-chains-report/raw

# 3. Compare against the recorded per-runner baseline.
python3 tools/ci/check_eval_chains.py \
  --run eval-chains-report/eval-chains.json \
  --json-out eval-chains-report/eval-chains-gate.json
```

To inspect one scenario directly (the raw report has the full time series):

```bash
build-eval-chains/build/eval_chains --mode inproc --seconds 10 \
  --chain head,mid,tail --load irq > /tmp/eval-irq.json
python3 tools/gen/gen_graph/scaffold/tools/render_eval.py \
  --in /tmp/eval-irq.json --out /tmp/eval-irq.html
```

## Normalized metrics

`run_eval_chains.py` keeps the raw reports as artifacts and writes
`eval-chains.json` (`kind: thyme-eval-chains-run`) with one entry per scenario:

| Metric | Direction | Meaning |
|--------|-----------|---------|
| `frames_per_sec` | higher is better | completed head-to-tail frame joins per second |
| `e2e_p95_ns` | lower is better | frame-joined E2E latency p95 |

`frames_per_sec` is used rather than the raw join count so that the baseline
stays valid if `--seconds` / `THYME_EVAL_CHAINS_SECONDS` changes. The scenario
entry still records the raw `frames`, `broken` and `broken_pct` for context.

The raw reports keep the per-node work log, per-edge latency, p50/p99 and the
full time series for investigation; only throughput + E2E p95 are
baseline-gated. On a shared host the per-hop distribution is bimodal (a hop
catches either the next period or the one after), so tighter per-edge or p99
gates would flake without saying anything actionable about a regression.

`broken` / `broken_pct` are integrity signals. The driver fails a run when
more than `--max-broken-pct` (default 5%) of joins are broken; the percentage
itself is not a tolerance-gated performance metric.

## Recorded reference numbers

The committed `x86_64-local` entry was recorded on 2026-10-24 on
**shu-desktop**, x86_64, 12th Gen Intel Core i5-12490F (12 threads), Release,
10 s per scenario. It is labelled with that host/arch in the JSON and is
*reference-only*: there is no `x86_64-ci` entry yet, so the scheduled job
reports `no-baseline` and passes until the runner records its own.

| Scenario | `frames_per_sec` | `e2e_p95_ns` | raw `frames` (10 s) | `broken_pct` |
|----------|-----------------:|-------------:|--------------------:|-------------:|
| `cpu` | 202.0 | 2,539,520 (~2.54 ms) | 2020 | 0.05% |
| `io` | 202.1 | 2,523,136 (~2.52 ms) | 2021 | 0.00% |
| `irq` | 202.0 | 2,589,696 (~2.59 ms) | 2020 | 0.05% |

Throughput is pinned at the head's 200 Hz in all three scenarios; the loads
move the E2E tail (p95) rather than the rate, which is exactly what the
bimodal-tolerance design assumes.

## Baseline policy

Baselines live in
[`tools/ci/eval_chains_baseline.json`](../../tools/ci/eval_chains_baseline.json)
and follow the same conventions as
[`tools/ci/bench_baseline.json`](../../tools/ci/bench_baseline.json) /
[`check_bench.py`](../../tools/ci/check_bench.py):

- **Keyed by runner tag.** `x86_64-local` is the development machine and is
  recorded as *reference-only evidence*: it shows the current cost of the
  scenarios, not a target-board budget. `x86_64-ci` is the GitLab runner; it
  records its own baseline on its first scheduled run. Never copy numbers
  from one runner to another.
- **Tolerance is relative.** A latency metric regresses when it exceeds its
  baseline by more than `tolerance_pct`; `frames_per_sec` regresses when it
  drops by more than the same percentage. The default is **100%**: under
  synthetic CPU and I/O load the chain latency is bimodal on a shared host, so
  the gate is calibrated to catch order-of-magnitude regressions and
  missing/zero metrics, not to police a 25% budget. Once a runner has stable
  history, lower `tolerance_pct` in the baseline file deliberately. A metric
  present in the baseline but missing from the run fails; a newly added metric
  is reported as `NEW` and is not gated until the baseline is refreshed.
- **No baseline means no gate.** A runner tag without an entry reports
  `no-baseline` and exits 0, exactly like `nightly-bench`. Use
  `--require-baseline` to make a missing baseline fatal.
- **Host mismatch skips by default.** If the recorded hostname or architecture
  no longer matches the run, the gate prints `host-mismatch` and exits 0;
  `--require-host-match` turns that into a failure. Re-record the baseline on
  the new host rather than silencing it.
- **Refresh with `--write-baseline`.** The run must have all scenarios `ok`.
  Review the numbers first; this is evidence of a deliberate change, not a
  way to make a red job green.

```bash
python3 tools/ci/check_eval_chains.py \
  --run eval-chains-report/eval-chains.json --write-baseline
```

## Comparison tests

`tools/ci/test_check_eval_chains.py` exercises the gate with fixture JSON:
tolerance pass/fail, `frames_per_sec` direction, missing metric, missing
scenario, new metric, failed run scenario, zero baseline, hostname/arch
mismatch, no baseline, baseline writing, and CLI exit codes. It also covers the
driver's normalization (`summarize`): per-second throughput that is independent
of `--seconds`, degraded-load reporting, and zero-frame failure. It is
registered as
`test_check_eval_chains` with labels `unit;mr_gate` in the root
[`CMakeLists.txt`](../../CMakeLists.txt), so the comparison logic is gated on
every merge request while the wall-clock scenarios stay in the schedule.
