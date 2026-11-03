# New-hire onboarding: productive with thyme in one day

Goal: within a day, go through **build → run the demo → run the tests → add a
channel**, and know where the documentation and the rules live. Every command
below was verified on a Linux development machine; all binaries live under the
`build/` tree.

> Only want to run something first? [`tutorial.md`](tutorial.md) is a 15-minute
> end-to-end path (generate a graph app → triggered recording → inspect/export/
> replay). This page is the full one-day onboarding.

## 0. First thing in the morning: what to read

Read in order; you do not need to memorize it all:

1. [`../README.md`](../../README.md) — repository entry point;
2. [`../ARCHITECTURE.md`](../../ARCHITECTURE.md) — execution model (cyclic /
   event / transport) and library split; **start at §5**;
3. [`scheduling-model.md`](../reference/scheduling-model.md) — scheduling
   contract: node = thread, callbacks are serial, there is no executor or
   callback group;
4. [`glossary.md`](../reference/glossary.md) — terminology: `SpscRing` /
   `ShmSpscRing` / `ShmSpmcRing`, `LatestSlot`,
   `PinnedSlotWriter`/`PinnedSlotReader`, lease / seqlock / epoch / watermark,
   package / slice, trigger FSM, perf log block types, and the exact scope in
   which "lock-free" applies;
5. [`coding-rules.md`](../reference/coding-rules.md) — required reading
   **before** you change code.

## 1. Build (~30 min)

```bash
git clone git@github.com:shuzhang2016/thyme.git
cd thyme
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

- Optional sibling repository **thyme-viz** (Foxglove visualization +
  `recorder_cli`): the build works without it; the corresponding tool targets
  are skipped with a CMake status message, which is not an error.
- Compile option `THYME_CORE_EMBEDDED` (default `ON`): adds `-fno-exceptions`
  to the core libraries `thyme_runtime` / `thyme_recorder` / `thyme_app` and
  propagates `-fno-rtti` to targets that link them; tools and tests keep
  exceptions. This is the compile-time backing for `coding-rules.md` (no
  exceptions, no RTTI, no dynamic allocation on hot paths); do not turn it off
  for day-to-day development.
- Reproducible builds: `THYME_REPRODUCIBLE=ON` by default, which adds
  `-ffile-prefix-map` / `-fdebug-prefix-map` / `-fmacro-prefix-map` to
  first-party compilations so the absolute checkout path is removed from
  debug and macro information. The same commit built with the same toolchain
  from different directories should produce identical objects; CI still has to
  pin the image digest and the compiler version.
- Artifact quick reference:

| Artifact | Path |
| --- | --- |
| Core libraries | `build/libthyme_runtime.a`, `libthyme_recorder.a`, `libthyme_app.a` |
| Graph code generator | `build/tools/gen/gen_graph/gen_graph` |
| Trigger-center trio | `build/tools/apps/trigger_center/{trigger_center, triggerctl, demo_producer}` |
| SHM read-only probe | `build/tools/inspect/thyme_probe/thyme_probe` |
| Multi-process log merge | `build/tools/inspect/thyme_log_merge/thyme_log_merge` |
| Metrics bridge | `tools/inspect/thyme_metrics_exporter/thyme_metrics_exporter.py` (Python, no build step; exports status/probe/perf/audit as a Prometheus textfile) |

> The commands below assume a standalone `build/` directory. If thyme is built
> as part of a larger tree, use that tree's build directory instead.

## 2. Run the demo: the triggered-recording chain (~1 h)

The smallest end-to-end demo in the repository is the triggered-recording
chain: `demo_producer` (a producer with a record tap) + `trigger_center` +
`triggerctl`. A full reference application lives in a downstream application
(generated code + hand-written nodes + trigger/recording configuration); this
repository provides the standalone chain.

First prepare a configuration that points at a local writable directory (the
only thing to change is `output_dir`):

```bash
mkdir -p /tmp/thyme_demo_out
sed 's|output_dir = /opt/usr/triggered_data|output_dir = /tmp/thyme_demo_out|' \
    tools/apps/trigger_center/trigger_config.minimal.ini > /tmp/thyme_demo.ini
# Config self-check (any unrecognized key is a parse error; nothing is
# "accepted but ignored")
./build/tools/apps/trigger_center/trigger_center --check-config \
    --record-manifest /etc/thyme/record_manifest.h /tmp/thyme_demo.ini
# To see the effective configuration with defaults materialised:
./build/tools/apps/trigger_center/trigger_center --dump-config \
    --record-manifest /etc/thyme/record_manifest.h /tmp/thyme_demo.ini
```

Start the chain (three terminals, or background them in order):

```bash
# 1) Producer: mirror page /thyme_recr_can + SPMC ring + large-frame channel /h264_front, 100 Hz
./build/tools/apps/trigger_center/demo_producer /thyme_recr_can /thyme_demo_ring /h264_front 100
# 2) Trigger center (blocks the foreground; stderr is the main log surface)
./build/tools/apps/trigger_center/trigger_center \
    --record-manifest /etc/thyme/record_manifest.h /tmp/thyme_demo.ini
# 3) Take a look at the SHM channels (read-only, does not affect the data plane)
./build/tools/inspect/thyme_probe/thyme_probe list
./build/tools/inspect/thyme_probe/thyme_probe info /thyme_recr_can
# 4) Fire one manual trigger (10 s before + 5 s after; wait ~15 s for the package to seal)
./build/tools/apps/trigger_center/triggerctl manual
#   → code=accepted trigger_id=1 priority=10
```

Verify the result:

```bash
ls /tmp/thyme_demo_out                 # a <trigger_id>_<t0>/ directory appears
ls /tmp/thyme_demo_out/1_*/            # wal.log / meta.tbin / small.tbin / frames/ / integrity.manifest
tail /tmp/thyme_trigger.sock.fsm       # FSM event JSONL (why=0 enqueued, 1 started, 5 completed)
tail /tmp/thyme_trigger.sock.audit     # audit line: decision=accepted ... code=accepted
./build/tools/inspect/thyme_log_merge/thyme_log_merge /tmp/thyme_trigger.sock.fsm
```

Cleanup: Ctrl-C both processes; after `demo_producer` exits, run
`rm -f /dev/shm/thyme_recr_can /dev/shm/thyme_demo_ring /dev/shm/h264_front`.
(For crash leftovers see [`runbook.md`](runbook.md) scenario 2 — on a normal
restart, same-named stale segments are reclaimed automatically.)

Configuration semantics are defined by [`trigger-config.md`](trigger-config.md);
continuous capture (`triggerctl arm` / `disarm` / `status`, over
`[recorder].socket`) is documented there as well.

## 3. Run the tests (~30 min)

```bash
ctest --test-dir build -N                          # list all cases
ctest --test-dir build -L mr_gate --output-on-failure   # fast PR gate (~203 cases)
ctest --test-dir build --output-on-failure         # everything
```

- The `mr_gate` label covers the gen_graph contract tests plus focused runtime
  unit tests; it is the tier that must pass locally before you open a pull
  request. Which suite each CI job runs is listed in
  [`test-matrix.md`](test-matrix.md).
- SHM tests use the real POSIX `/dev/shm`; Linux is the primary CI platform.
- Optional: `cmake -S . -B build-tsan -DTHYME_ENABLE_TSAN=ON` (TSan). Coverage
  and bench-gate commands are in the Test section of [`../README.md`](../../README.md).
- The metrics bridge tests are part of ctest as well (`ctest -R
  test_thyme_metrics_exporter`): pure Python, no running service required. For
  the mapping between metrics, dashboards and alerts see
  [`observability-dashboards.md`](observability-dashboards.md).

## 4. Afternoon task: add a channel (~2–3 h)

thyme's wiring is declared in `graph.yaml` and generated by `gen_graph`: **no
graph change, no new channel**; never edit `generated/` by hand.

```bash
# Start with the scaffold to feel the flow (greenfield bootstrap)
./build/tools/gen/gen_graph/gen_graph --out /tmp/my_app --update all
# Edit /tmp/my_app/graph.yaml: add nodes/edges/channels (schema in tools/gen/gen_graph/README.md)
./build/tools/gen/gen_graph/gen_graph --in /tmp/my_app/graph.yaml --explain-rates   # rate derivation, writes nothing
./build/tools/gen/gen_graph/gen_graph --in /tmp/my_app/graph.yaml --update generated # everyday regeneration
./build/tools/gen/gen_graph/gen_graph --in /tmp/my_app/graph.yaml --check            # for CI: hand edits/stale output → exit 1
```

Key points:

1. Adding a channel = adding an edge in `graph.yaml` (plus a node when needed),
   then `gen_graph --update generated`. Only a new node type needs
   `--update nodes` to generate the `src/nodes/<id>.{h,cpp}` stub;
2. Business code only implements `OnLoop()` / `On<Channel>(const T&)` handlers;
   the generated `Loop()`/`Dispatch()` are `final`, and replay pause plus the
   record mirror tap are built in;
3. After every regeneration, read `generated/integration.md` (record manifest,
   replay phases, MP SHM table);
4. Details: [`../tools/gen/gen_graph/README.md`](../../tools/gen/gen_graph/README.md)
   (CLI and `--update` modes),
   [`../tools/gen/gen_graph/INTEGRATION.md`](../../tools/gen/gen_graph/INTEGRATION.md)
   (integration workflow).

## 5. Documentation map and rules

| Document | Content |
| --- | --- |
| [`../ARCHITECTURE.md`](../../ARCHITECTURE.md) | Architecture and execution model (read first) |
| [`tutorial.md`](tutorial.md) | 15-minute run-through: graph app + triggered recording + replay |
| [`scheduling-model.md`](../reference/scheduling-model.md) | Scheduling contract (node = thread, no executor) |
| [`trigger-config.md`](trigger-config.md) | Full trigger/recording configuration key table + SIGHUP matrix |
| [`abi-compatibility.md`](../reference/abi-compatibility.md) | SHM/protocol ABI compatibility rules |
| [`as-built-deviations.md`](../reference/as-built-deviations.md) | As-built deviation log: design claim vs implementation, status (aligned/deviated/deferred) and pointers |
| [`cacheline-audit.md`](../reference/cacheline-audit.md) | Cacheline/false-sharing audit table + layout gate |
| [`mechanism-policy-audit.md`](../reference/mechanism-policy-audit.md) | Mechanism/policy boundary audit: where thresholds, defaults, retries, quotas, rotation, overrun and gating belong, plus follow-ups |
| [`error-handling-audit.md`](../reference/error-handling-audit.md) | Silent-drop audit: fixed and deferred `(void)` / unchecked syscall / fail-open paths |
| [`runbook.md`](runbook.md) | Operational fault handling (disk full / SHM leak / trigger storm / crash recovery) |
| [`upgrade.md`](upgrade.md) | Upgrade/rollback procedure, `thyme_probe abi` preflight, mixed-version matrix |
| [`observability-dashboards.md`](observability-dashboards.md) | Metric sources, Prometheus bridge, Grafana/Foxglove import, alert rules |
| [`test-matrix.md`](test-matrix.md) | Test suite ↔ CI job mapping |
| [`coding-rules.md`](../reference/coding-rules.md) | Coding rules (core libraries without exceptions/RTTI/hot-path allocation) |
| [`api-docs.md`](../design/api-docs.md) | API comment and handle ownership/lifetime rules (no HTML is generated in this repository) |
| [`adr/`](../design/adr/README.md) | Architecture decision records (framing/record format/trigger FSM/MP/swap ring/request-response/transport seam; index in README) |
| [`../deploy/README.md`](../../deploy/README.md) | systemd/busybox deployment, supervisor, service lifecycle |

Minimum self-check before opening a pull request: all of `ctest -L mr_gate`
green, plus `python3 tools/ci/check_doc_links.py` (when you changed
documentation). Test counts, `build/...` commands and the ABI matrix quoted in
the docs are validated by the `check-doc-links` and `test-mr-gate` jobs (see
[`test-matrix.md`](test-matrix.md)).
