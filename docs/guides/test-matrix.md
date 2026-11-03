# Test matrix

Which test suite runs in which CI job, what it is allowed to fail, and where it
is intentionally excluded. The pipeline definition is
[`.github/workflows/ci.yml`](../../.github/workflows/ci.yml); the per-test labels
and timeouts live in [`CMakeLists.txt`](../../CMakeLists.txt).

A default configure builds **270 ctest cases**, 203 of them labelled
`mr_gate`. Sanitizer and TSan cases are added by their CMake options
(`THYME_ENABLE_TSAN`, the ASan/UBSan flag sets) and are not part of the base
count.

## Matrix

| Job | Trigger | Suite / selector | Sanitizer | Blocking |
|-----|---------|------------------|-----------|----------|
| `test-mr-gate` | pull request | `ctest -L mr_gate` (203 cases minus quarantined: gen_graph contracts/golden/negative goldens/layout asserts/quick fuzz, record/trigger core, runtime units, ABI matrix/checker/upgrade script, cacheline audit, coverage gate) | none | yes |
| `test-werror` | pull request | build only (`THYME_STRICT_WARNINGS=ON`, `THYME_WERROR=ON`) | none | yes |
| `test-asan-ubsan-mr` | pull request | `-R 'frame\|spsc\|latest_slot\|pinned_slot\|record_mirror\|queue\|object_pool\|seqlock'` | ASan + UBSan, `halt_on_error` | yes |
| `check-doc-links` | PR + default branch | Markdown links, fenced commands vs CMake targets, ABI matrix vs headers | n/a | yes |
| `check-license-inventory` | PR + default branch | `NOTICE`, third-party inventory, vendored-dir check | n/a | yes |
| `check-sbom` | PR + default branch | dependency-depth + SBOM JSON | n/a | yes |
| `check-clang-tidy` | pull request | full-tree clang-tidy: every `src/` main TU plus first-party `include/` diagnostics (headers checked through their TUs) | n/a | yes |
| `test-linux` | default branch | full `ctest` (270 cases minus quarantined; includes the `fuzz`-labelled full passes) | none | yes |
| `nightly-ctest` | schedule | full `ctest` minus quarantined, then a separate `THYME_ENABLE_TSAN=ON` tree with `-R tsan` | TSan (second tree) | yes |
| `nightly-aarch64-cross` | schedule | full AArch64 Release build on an `ubuntu-22.04` runner, then QEMU user-mode execution of exactly 6 cases: `test_abi_layout`, `test_abi_profile`, `test_abi_cross_version`, `test_cacheline_audit`, `test_frame_header`, `test_crc32_hw`; missing tests/tools fail the job | none | yes |
| `nightly-repeat` | schedule, `workflow_dispatch` | `ctest -L mr_gate` minus heavy serial gen_graph/empty-app builds, fuzz targets and benches, `--repeat until-fail:20` | none | yes |
| `nightly-quarantine` | schedule | only the quarantined tests, `--repeat until-fail:3` | none | no (`continue-on-error: true`; signal only) |
| `nightly-asan-ubsan` | schedule | full suite **minus** `test_gen_graph*`, `test_empty_app`, `test_wake_coalesce`, and quarantined tests | ASan + UBSan | yes |
| `nightly-fuzz` | schedule | YAML corpus smoke + 300 s timed libFuzzer; the structured gen_graph full pass (`test_fuzz_gen_graph_full`) runs with the full suite in `nightly-ctest` | libFuzzer | yes |
| `nightly-fault` | schedule, `workflow_dispatch` | `ctest -L fault` (fault matrix: kill -9 per role, SHM full, disk full, simulated clock jump); JSON summary at `build/fault_matrix.json` | none | yes |
| `nightly-coverage` | schedule | full `ctest` on a `THYME_ENABLE_COVERAGE=ON` tree, then the gated per-library report | none (instrumentation only) | yes (coverage gate) |
| `nightly-bench` | schedule, `workflow_dispatch` | `bench_core`, `bench_object_pool` and `bench_trigger_seal` (p99/p999), compared per runner and host with the recorded baseline; a runner without a baseline skips | none | yes (bench gate, never PR-blocking) |
| `perf-eval-chains` | schedule, `workflow_dispatch` | generated `head → mid → tail` app under `cpu`/`io`/`irq` loads, normalized throughput/E2E metrics compared per runner | none | yes (baseline gate; no baseline passes as a skip) |
| `report-acceptance` | schedule (after the nightlies) | renders `acceptance.md`/`.json` from the ctest/coverage/bench artifacts | n/a | no (report only) |
| HIL | manual, target board | e2e + bench + fault injection on target hardware | n/a | not in GitHub Actions |

## Quarantine

A test that flakes under CI load (and cannot be fixed immediately) is
quarantined: removed from the gating jobs so it stops blocking PRs, but kept
running nightly so it keeps generating signal. The registry is
[`tools/ci/quarantine.json`](../../tools/ci/quarantine.json); the gate logic is
[`tools/ci/apply_quarantine.py`](../../tools/ci/apply_quarantine.py).

**Who adds an entry.** The owner of the failing PR or the on-call maintainer,
in the same PR that would otherwise go red — or a dedicated PR if the flake is
discovered on a nightly. Quarantining is a last resort: a scheduling-sensitive
test should first be rewritten to drive the state machine directly (poll a
condition with a bounded deadline) instead of relying on sleeps and fixed time
windows.

**Entry shape.** One object per test in `entries`:

```json
{
  "test": "test_some_flake",
  "reason": "why it flakes and what the fix plan is",
  "issue": "#412",
  "quarantined": "2026-10-23",
  "expires": "2026-11-06"
}
```

All five fields are required. Entries with `"example": true` are templates and
ignored by the tooling.

**SLA.** Expiry is at most **2 weeks** after quarantining. The expiry date is a
hard deadline, not a reminder: once `expires` is in the past,
`apply_quarantine.py` exits 1 and every gating job (PR gate, branch, and the
hard nightlies) **fails until the entry is re-reviewed** — an expired
quarantine never persists silently. Re-review means one of:

- the flake is fixed → the entry is deleted and the test is back in the gate
  (graduation), or
- the fix needs more time → the entry is renewed with an updated `reason` and
  a fresh `expires` (renewal should be exceptional, not routine).

An entry naming a test that no longer exists also fails validation.

**Where quarantined tests run.** `test-mr-gate`, `test-linux`,
`nightly-ctest`, and `nightly-asan-ubsan` exclude them via
`apply_quarantine.py --exclude-regex`. `nightly-quarantine` runs exactly the
quarantined set (`--only-regex`, `--repeat until-fail:3`) with
`continue-on-error: true` — the failures stay visible in the workflow run
without failing it. `nightly-coverage` keeps running the full suite so coverage
thresholds are unaffected.

Local dry-run:

```bash
python3 tools/ci/apply_quarantine.py --list          # active entries
python3 tools/ci/apply_quarantine.py --self-test     # validation unit tests
ctest --test-dir build -N | \
  python3 tools/ci/apply_quarantine.py --exclude-regex --tests -
```


### SHM isolation and leak gate

`test_environment_preflight` / `python3 tools/ci/check_test_environment.py`
reports the load average and the SHM segment count/free space read-only, and
treats a `yes`, `demo_producer` or `test_*` workload with ppid=1 as a hard
failure; it never cleans up a production segment. `test_shm_leak_audit` calls
the same self-check before scanning, so old workloads cannot cause false
failures.

`test_shm_leak_audit` (label `unit`, `RUN_SERIAL`) runs every non-rebuild,
non-fuzz case on its own and asserts zero delta in `/dev/shm`; any leaking case
fails it. CMake injects `THYME_SHM_TRACK=1` into the test process:
`ShmRegion::Create` records names, and on process exit (including a `CHECK`
failure — `check.h` now calls `exit` rather than `abort`) it reclaims the
objects created by this process and removes test segments left by dead child
processes, keyed by pid. Production builds do not set the variable, so segment
lifetime semantics are unchanged.

Every CTest case also goes through `tools/ci/test_launcher.py`: each case gets
its own `THYME_SHM_TEST_PREFIX`, the launcher appends its own pid and cleans
only that prefix before starting; on exit or on SIGTERM/SIGINT it terminates
the corresponding process group and removes the case's prefix plus segments
left by dead owner PIDs. The Linux child subreaper adopts orphans of
short-lived parents, and unrelated SHM files are never scanned and deleted. The
unique names in `tests/test_shm.h` and probe enumeration use the same prefix;
`test_check_test_environment` covers unrelated-file protection, exit 7, orphan
process groups and SIGTERM.

If the launcher itself is `SIGKILL`ed it cannot perform user-space cleanup;
that is an explicit recovery boundary: the existing PDEATHSIG on the production
path, the next start with the same prefix, and the post-hoc checks in
`test_shm_leak_audit` reclaim or report the leftovers. There is no global blind
deletion of `/dev/shm`.

## Repeat-until-fail

`nightly-repeat` is the flake detector for the fast, deterministic suite. It
runs the `mr_gate` label with `ctest --repeat until-fail:20 -j2`: every test is
retried until it fails or reaches 20 clean repetitions. Any failure fails the
job (`continue-on-error` is off); the full log is published as the
`repeat-report/` artifact even on failure.

Excluded from the loop:

- `test_gen_graph*`, `test_empty_app`: nested CMake configure/build fixtures
  (`RUN_SERIAL`) whose cost is in the build, not the assertion;
- `bench_*`, `test_trigger_seal*`: wall-clock measurements, which belong to
  `nightly-bench` and are never retried as a correctness signal;
- `test_fuzz_*`: the quick fuzz passes are deterministic but iteration-heavy,
  and `nightly-fuzz` already owns long fuzz runs;
- quarantined tests: they run in `nightly-quarantine` instead.

The job also runs on demand via `workflow_dispatch`. Pull-request jobs are
unchanged: repeated runs are too expensive for the PR budget. The regression
test behind each fixed bug is indexed in
[`regression-corpus.md`](../reference/regression-corpus.md), so a repeated
failure can be traced back to the corpus entry and its fix commit.

## Test timing: waits vs. deliberate delays

A fixed `sleep` used to sequence threads ("let the worker block", "give the
roles time to start") is a scheduler guess: too short and the test is racy, too
long and every run pays the worst case. New tests synchronise through
[`tests/test_wait.h`](../../tests/test_wait.h):

- `WaitFor(pred, deadline)` re-checks an observable condition (an atomic
  counter, a registered-waiter count, a file appearing) until it is true or a
  monotonic deadline expires. The 100 µs poll interval is the wait mechanism,
  not the assertion; a stalled peer fails the `CHECK` at the deadline.
- `EventFd` is an eventfd handshake for test-owned threads: `Signal()` from one
  side, a bounded `poll()` wait on the other.

Both fail closed, so a broken peer produces a test failure instead of a hang.

Converted (sleep removed, observable handshake added):

| Test | Replaced |
|---|---|
| `test_spsc_ring`, `test_notif_pipe` | 20 ms "waiter registered" sleep → `waiter_count()`/`slot_waiter_count()` (new accessors) |
| `test_cyclic_thread` | 200/100/50/20 ms settles → `ticks`/`loop_count()`/`first_ns` waits; the 80 ms wake assertion is now bounded by `WaitFor` |
| `test_event_thread` | 50 ms idle waits → `wake_waiter_count()`; replay-pause window → `HealthSlot::alive_counter` advance |
| `test_app` | 200/50 ms → `handled_count()` and `loop_count()` |
| `test_mp_supervisor` | 300 ms after `ForkRoles` → the child publishes `proc_loops` every iteration; `WaitChildrenLooping()` waits for it |
| `test_shm_spsc_ring`, `test_shm_spmc_ring` | 100/20 ms before the measured push → `epoch_waiters()`/`waiter_count()` |
| `test_thyme_bin_tcp` | 50 ms after starting the server thread → retry `ThymeBinTcpConnect` under a deadline |

**Deliberately kept** (a fixed sleep here is the subject, not sequencing):

- *Workload windows* — `test_latest_slot_stress`, `test_latest_slot_concurrent`,
  `test_object_pool_concurrent`, `test_shared_health_stress`,
  `test_pinned_slot_lease_steal`, `test_proc_sampler`: the assertion is progress
  or absence of tears over a window; a handshake cannot express "run for
  400 ms".
- *Negative/quiet-period assertions* — `test_mp_supervisor_restart` (no
  restart within 150 ms), `test_replay_backpressure` (paused drain stays
  frozen for 40 ms), `test_thyme_bin_fdsink` (a disabled sink emits nothing
  for 20 ms). Proving "nothing happened" needs a window.
- *Tick-driven engine windows* — the trigger/replay/record suites call
  `SleepNs(200 µs … 1 ms)` between `Tick()` calls; pre/post windows, mute
  budgets, settle and seal latency are wall-clock state machines, and their
  assertions are on timestamps or elapsed windows.
- *Sample ageing* — `test_thyme_probe` sleeps so the observed frame age is
  above the sampling period; `test_log_rotation` waits 10 ms for several
  rotations; every producer loop paces itself (`SleepNs(kPaceNs)`,
  `eval_chains`, `bench_*`).
- *Cross-process sequencing* — `test_shm_spsc_ring`/`test_shm_spmc_ring`
  children delay their push so the parent blocks first; the child has no
  readiness channel to signal on.
- *Timeout guards* — the `while (deadline) usleep(1 ms)` helpers
  (`WaitCount`, `PollUntil`, `PollForCmd`, attach retries) are already
  bounded; they are kept as the fail-closed deadline and are not delays.
- *Tool-side polling* — `tools/replay/thyme-replay`'s signal monitor (20 ms poll),
  `tools/ops/upgrade/thyme_upgrade.sh`'s post-restart wait,
  `tools/ops/fault_matrix`'s child poll and
  `tools/inspect/thyme_metrics_exporter --watch`'s sampling interval are bounded
  pollers or sampling periods, not test sequencing.

The converted set is verified with
`ctest --repeat until-fail:20 -j1` before landing (same policy as
`nightly-repeat`). A new fixed sleep needs one of the reasons above and a
comment at the call site.

## Sanitizer coverage

- **ASan/UBSan**: PR runs the concurrency/record core subset only, to keep the
  gate fast; the nightly job runs everything except three suites that are not
  compatible with the sanitizer run:
  - `test_gen_graph*` and `test_empty_app` spawn a nested CMake configure +
    build, which is slow and unrelated to sanitizer signal;
  - `test_wake_coalesce` uses ptrace and is excluded from ASan.
- **TSan**: `nightly-ctest` configures a dedicated `-DTHYME_ENABLE_TSAN=ON`
  tree. The TSan cases are `test_notif_pipe_tsan`, `test_shm_spsc_ring_tsan`,
  `test_shm_spmc_ring_tsan`, `test_event_thread_tsan`,
  `test_latest_slot_concurrent_tsan`, `test_seqlock_payload_model_tsan`,
  `test_latest_slot_stress_tsan`, `test_object_pool_concurrent_tsan`,
  `test_spsc_notify_order_tsan`, `test_spmc_notify_race_tsan`.
- Both nightly sanitizer jobs are hard failures (no `continue-on-error`).

## Coverage

`nightly-coverage` builds with `-DTHYME_ENABLE_COVERAGE=ON` using Clang (the
Ubuntu runner installs `clang` + `llvm`), runs the full ctest suite with
`LLVM_PROFILE_FILE` set, exports `llvm-cov` data, and then runs

```bash
python3 tools/ci/check_coverage.py --build-dir build-cov
```

which prints a per-library table, compares line coverage against
[`tools/ci/coverage_thresholds.json`](../../tools/ci/coverage_thresholds.json),
and exits non-zero when a library drops below its threshold. The report
(`summary.txt`, `html/`, export) is published as the `coverage-report/`
artifact even when the gate fails.

Library coverage counts the lines of that target's own `src/*.cpp` files;
headers shared between targets are not attributed. A local GCC build uses the
repo's established `gcov`/`lcov` path instead of `llvm-cov` (one format,
selected by the configured compiler), and the same gate script understands
both exports.

Refresh the baseline deliberately, after reviewing the numbers, with:

```bash
ctest --test-dir build-cov -j4
python3 tools/ci/check_coverage.py --build-dir build-cov \
  --write-baseline tools/ci/coverage_thresholds.json
```

## Bench baseline

`nightly-bench` builds and runs three benches in Release:

- `bench_core` — five fixed core paths (`LatestSlot` write+read, SPSC
  push/pop, PinnedSlot 19 MiB `CopyPayloadTo`, PinnedSlot 19 MiB
  `ReadFrameStable`, trigger-centre config load), with sampled `p99`/`p999`.
- `bench_object_pool` — scan vs free-list acquire cost (fragmented/dense),
  with sampled `p99`/`p999`.
- `bench_trigger_seal` — trigger→seal latency distribution
  (`p50/p90/p99/p999/max`) through the real trigger pipeline. The `p50` is
  the gate's steady-state row; latency SLO budgets remain reference values in
  [`trigger-config.md`](trigger-config.md).

`tools/ci/check_bench.py` compares the documents with
[`tools/ci/bench_baseline.json`](../../tools/ci/bench_baseline.json) for the
run's **runner and recording host**, and fails on a regression beyond the
documented tolerance (`ns/op` +25%, `p99` +50%, `p999` +100%). A metric (or
percentile) present in the baseline but missing from the run fails; a new one
is reported as `NEW` and is not gated until the baseline is refreshed. A runner
without a baseline, or a host mismatch, is reported and skipped unless
`--require-baseline`/`--require-host-match` is passed.

The baseline is per runner, never a target-board budget:

- `x86_64-local` — the development box, recorded here as evidence of the
  current cost of each path. Reference-only.
- `x86_64-ci` — the GitHub-hosted `ubuntu-latest` runner. It is deliberately
  **unset** until the runner records its own baseline on its first scheduled
  run; until then the gate prints `no baseline` and passes, so no fabricated
  CI numbers are committed.

The gate never runs on pull requests: wall-clock numbers do not block a merge.
The comparison logic and the job's YAML shape are unit-tested by
`test_check_bench` (`mr_gate`). Full policy, schemas and the recording
procedure are in [`bench-baselines.md`](bench-baselines.md).

Record a runner's baseline from its own artifacts:

```bash
python3 tools/ci/check_bench.py \
  --run bench-report/bench_core.json \
  --run bench-report/bench_object_pool.json \
  --run bench-report/bench_trigger_seal.json \
  --write-baseline
```

## eval_chains scenarios

`perf-eval-chains` (schedule only, or `workflow_dispatch`) builds the committed
generated-app fixture `head → mid → tail` (head 200 Hz, mid/tail 400 Hz) and
runs `eval_chains` three times: under pinned CPU spinners (`cpu`), `/tmp`
write+fsync load (`io`), and a timerfd + `sched_yield` storm (`irq`). The loads
are built into `eval_chains`; the job installs no external stress tool, and a
load process that exits early is logged as DEGRADED and recorded in the JSON.

`tools/ci/run_eval_chains.py` writes the raw reports plus a normalized
`thyme-eval-chains-run` JSON; `tools/ci/check_eval_chains.py` compares it with
[`tools/ci/eval_chains_baseline.json`](../../tools/ci/eval_chains_baseline.json)
per runner (same policy as `nightly-bench`: a runner without an entry passes as
`no baseline`, a host/arch mismatch skips unless `--require-host-match`). The
comparison logic is covered on every PR by `test_check_eval_chains`
(`unit;mr_gate`). Scenario definitions, the local reproduction commands and the
full baseline policy are in [`eval-chains.md`](eval-chains.md).

## Acceptance table

`report-acceptance` runs after the nightly stage and reads the artifacts of
`nightly-coverage` (JUnit results, ctest metadata, coverage export) and
`nightly-bench` (`bench_core.json`). It publishes `acceptance.md` and
`acceptance.json` with test totals and per-label counts, per-library coverage
against its threshold, bench deltas against the runner baseline, and gate
statuses. Missing artifacts are reported as `unavailable` rather than failing
the report. The same generator works on a local build:

```bash
cmake -S . -B build && cmake --build build -j"$(nproc)"
ctest --test-dir build -j4
python3 tools/ci/gen_acceptance.py --build-dir build --out acceptance.md
```

## Cacheline / padding review

The machine-checked half is `test_cacheline_audit` (**mr_gate**): it pins
every `sizeof`/`alignof`/hot-field offset in
[`cacheline-audit.md`](../reference/cacheline-audit.md) with `static_assert`, and
`./build/test_cacheline_audit --table` reprints the table. A field that pushes
a later field off its cache line, or that grows a page, fails that test.

The compiler/tooling half is a **manual review step**, not a CI gate: these
layouts are built out of intentional 64-byte holes (one line per hot word), so
`-Wpadded` reports every one of them and a gate would be all false positives.
Before touching a hot/shared struct, run:

```bash
# Padding scan. Hits on audited structs must appear in the "Padding" column of
# docs/reference/cacheline-audit.md; a new hit there is an unreviewed layout change.
g++ -std=c++17 -O2 -I include -Wpadded -c /tmp/cacheline_scan.cpp -o /dev/null

# When pahole is installed (CI runner / workstation): confirm holes explicitly.
g++ -std=c++17 -g -O2 -I include -c /tmp/cacheline_scan.cpp -o /tmp/scan.o
pahole -C PinnedSlotPageHeader /tmp/scan.o
```

`/tmp/cacheline_scan.cpp` is the ten-line TU listed in
[`cacheline-audit.md`](../reference/cacheline-audit.md#method).

## Retry counters

The bounded seqlock retry loops (budget: `kLatestSlotReadRetries`,
`kSpmcReadRetries`, `kPinnedSlotReadRetries`, `kMirrorReadRetries`,
`kMirrorSampleReadRetries`, `kHarvestSweepRetries`, all 16) expose their
counters through the reader object, never through a page: consumers map SHM
read-only, so a page-side counter would either fault or need an ABI bump. (The
read-only probe uses its own `kProbeReadRetries` and reports per-sample
`torn` instead — it has no per-reader state.)

- `LatestSlot::Read/ReadIfNew(out, LatestSlotReadStats*)` — caller-owned
  `retries`/`torn`.
- `SpmcReader::retry_count()/torn_count()`, `PinnedSlotReader::retry_count()`
  (next to the existing `torn_read_count()`/`drop_count()`), and the
  caller-owned `torn_drops` of `RecordMirrorPage::ReadSlot` /
  `RecordMirrorRingPage::{PeekRingSample,ReadRingSample}`.
- `TriggeredRecorder` exposes `Stats::harvest_retries` for the pre-window
  re-sweep.

`test_retry_accounting` (**mr_gate**, in-process containers) and
`test_retry_accounting_shm` (`unit`, named regions like the other `/dev/shm`
tests) force the mid-write state and assert the exact budget was counted; a
successful read must not move the counters.

## Local commands

```bash
# PR gate
cmake -S . -B build && cmake --build build -j"$(nproc)"
ctest --test-dir build -L mr_gate --output-on-failure

# Full suite
ctest --test-dir build --output-on-failure -j"$(nproc)"

# ASan/UBSan subset (same selector as CI)
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j"$(nproc)"
ctest --test-dir build-asan -R 'frame|spsc|latest_slot|pinned_slot|record_mirror|queue|object_pool|seqlock'

# TSan
cmake -S . -B build-tsan -DTHYME_ENABLE_TSAN=ON
cmake --build build-tsan -j"$(nproc)"
ctest --test-dir build-tsan -R tsan --output-on-failure

# Coverage + gate
cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Debug -DTHYME_ENABLE_COVERAGE=ON
cmake --build build-cov -j"$(nproc)"
ctest --test-dir build-cov -j4
python3 tools/ci/check_coverage.py --build-dir build-cov

# Bench + gate (writes <build>/bench_core.json when run through ctest)
./build/bench_core --json build/bench_core.json
python3 tools/ci/check_bench.py --run build/bench_core.json

# Acceptance table from the local build (coverage report is optional)
python3 tools/ci/gen_acceptance.py --build-dir build --out acceptance.md
```
