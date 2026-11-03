# Bench baselines (PERF-06)

`tools/ci/check_bench.py` compares fresh bench JSON against
`tools/ci/bench_baseline.json`. The gate is **never run on merge requests**:
wall-clock numbers on a shared runner are evidence, not a contract. The
scheduled/manual `nightly-bench` job builds the binaries, runs them and enforces
the baseline for that runner.

## Documents understood

| `kind` | Produced by | Metric rows |
| --- | --- | --- |
| `thyme-bench-core` | `bench_core --json PATH` | `ns_per_op`, sampled `p99_ns_per_op`/`p999_ns_per_op` |
| `thyme-bench-object-pool` | `bench_object_pool --json PATH` | `ns_per_op`, sampled `p99_ns_per_op`/`p999_ns_per_op` |
| `thyme-bench-trigger-seal` | `bench_trigger_seal --json PATH` | `trigger_to_seal` with `p50` as `ns_per_op` plus `p99`/`p999` |

Pass all three to one gate invocation with repeated `--run` options; they must
report the same `runner_tag` and host.

## Per-runner keys

Baselines are keyed by `runner_tag`, and every entry records the host that
produced the numbers:

```json
{
  "schema": 2,
  "runners": {
    "x86_64-local": {
      "recorded_at": "2026-10-24T09:05:23Z",
      "role": "reference-only",
      "git_sha": "5d91ea5",
      "toolchain": "GCC 11.4.0",
      "build_type": "Release",
      "host": {"hostname": "shu-desktop", "machine": "x86_64"},
      "benchmarks": {
        "spsc_ring_push_pop": {
          "bench": "thyme-bench-core",
          "ns_per_op": 15.7035,
          "p99_ns_per_op": 37.0,
          "p999_ns_per_op": 88.0
        }
      }
    }
  }
}
```

| Tag | Status | Use |
| --- | --- | --- |
| `x86_64-local` | recorded | Reference-only evidence from the development machine. It is never a target-board budget. |
| `x86_64-ci` | **unset** | The GitLab runner records its own entry on its first scheduled run. Until then the gate prints `no-baseline` and exits 0 (skip). Never copy `x86_64-local` numbers into this entry. |

`bench_core`, `bench_object_pool` and `bench_trigger_seal` all default the tag
to `<arch>-local` (or `<arch>-ci` when `CI` is set); `THYME_BENCH_RUNNER_TAG`
overrides it. Use `--tag` for a one-off run.

## Tolerance policy

| Statistic | Default tolerance | Why |
| --- | --- | --- |
| `ns_per_op` (median/steady state) | **+25%** | Catches a real regression while absorbing host and cache noise. |
| `p99` | **+50%** | Tail samples are noisier on a shared runner; a 50% move is still a real tail regression. |
| `p999` | **+100%** | Nearest-rank tail. For fewer than 1000 samples p999 is the maximum, so it is a coarse signal and gets the widest band. |

The values live in `bench_baseline.json` (`tolerance_ns_per_op_pct`,
`tolerance_p99_pct`, `tolerance_p999_pct`) and can be overridden per run with
`--tolerance`, `--tolerance-p99`, `--tolerance-p999`. Lower them only per
runner, after a stable history.

`p99`/`p999` are **sampled per-operation latencies** whose sample includes the
clock read. They are comparable to the same runner's baseline, not to an
absolute latency budget. For `bench_core` fast paths the clock read dominates
the smallest operations; that is expected and cancels in the delta.

## Gate semantics

| Situation | Result | Exit |
| --- | --- | --- |
| Runner tag absent from `runners` | `no-baseline`, rows reported as `new` | 0 (1 with `--require-baseline`) |
| Run hostname/arch differs from the entry | `host-mismatch`, skipped | 0 (1 with `--require-host-match`) |
| Baseline metric missing from the run | row `missing` | 1 |
| Baseline `ns_per_op`/`p99`/`p999` exceeded | row `fail` | 1 |
| Metric or percentile newly emitted | row/stat `new`, not gated | 0 |
| Bench document has `"ok": false` | `fail` | 1 |

Only metrics belonging to the document kinds passed on the command line are
compared, so a subset run (for example only `bench_core`) does not report the
object-pool rows as missing.

## Recording a baseline

Run the binaries on the runner, then merge the documents in one write. The
write merges benchmark keys into the runner entry instead of replacing them, so
the three commands can also be run separately.

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench -j4 --target bench_core bench_object_pool bench_trigger_seal
mkdir -p bench-report
./build-bench/bench_core --json bench-report/bench_core.json --tag x86_64-local
./build-bench/bench_object_pool --json bench-report/bench_object_pool.json --tag x86_64-local
./build-bench/bench_trigger_seal --json bench-report/bench_trigger_seal.json --tag x86_64-local
python3 tools/ci/check_bench.py \
  --run bench-report/bench_core.json \
  --run bench-report/bench_object_pool.json \
  --run bench-report/bench_trigger_seal.json \
  --write-baseline
```

Then enforce exactly what CI enforces:

```bash
python3 tools/ci/check_bench.py \
  --run bench-report/bench_core.json \
  --run bench-report/bench_object_pool.json \
  --run bench-report/bench_trigger_seal.json \
  --json-out bench-report/bench-gate.json
```

The `thyme-bench-gate` result document (`schema: 2`) carries the runner, host,
the three tolerances, and one row per metric with `status`, `p99_status` and
`p999_status`.
