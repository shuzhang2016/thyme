# thyme user manual

thyme is a single-node-first real-time data middleware: shared-memory
primitives, recording and triggered capture, replay, and observability tooling.
This manual is split into **API** and **tools** volumes, with **topic** guides
(performance, continuous capture, triggered capture, configuration and
deployment) for end-to-end usage.

> All commands assume the repository root as the working directory, with build
> artifacts in `build/`.
> Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`

## Reading paths

| Your role | Suggested order |
|---|---|
| First contact | [15-minute tutorial](../guides/tutorial.md) → "Runtime primitives API" → "Triggered capture" |
| Integration developer | [Runtime primitives API](api-runtime.md) → [Recording and trigger API](api-record-trigger.md) → [Replay API](api-replay.md) |
| Tool user | [thyme_probe](probe.md) → [thyme-replay and triggerctl](replay-cli.md) → [Other tools](tools-other.md) |
| Performance/observability | [Performance and observability](perf.md) → [Bench baselines](../guides/bench-baselines.md) |
| Operations/deployment | [Configuration and deployment](config-deploy.md) → [Runbook](../guides/runbook.md) → [Upgrade/rollback](../guides/upgrade.md) |

## Page map

### API

- [Runtime primitives API](api-runtime.md) — `SpscRing`/`ShmSpscRing`/`ShmSpmcRing`, `LatestSlot`, `ObjectPool`, `NotifPipe`, `Frame<T>`, PinnedSlot leases, and producer gating.
- [Recording and trigger API](api-record-trigger.md) — `TriggeredRecorder`, trigger protocol v1–v3, `TriggerCenter` FSM/audit, continuous capture, retention, and package verification.
- [Replay API](api-replay.md) — `ReplayPackage`, `ReplaySession`, injectors, header policy, and topology mapping.

### Tools

- [thyme_probe](probe.md) — read-only SHM probe: `list/info/watch/hz/bw/delay/echo/abi/schema`.
- [thyme-replay and triggerctl](replay-cli.md) — replay/export/verification/status, plus the recording control CLI.
- [Other tools](tools-other.md) — `gen_graph`, `thyme_log_merge`, `thyme_metrics_exporter`, `thyme_py`, upgrade scripts, and more.

### Topics

- [Performance and observability](perf.md) — perf log block types, histograms, benchmarks, baseline gates, and metric export.
- [Continuous capture](acquisition.md) — always-on recording, slicing/rotation, quotas and retention, and package verification.
- [Triggered capture](triggered-acquisition.md) — trigger FSM, pre/post windows, quotas/preemption/cancellation, auditing, and SLOs.
- [Configuration and deployment](config-deploy.md) — configuration sections and hot-reload matrix, resource self-checks, systemd, and upgrade/rollback.

## Conventions

- **Time:** All cross-process and on-disk timestamps are `mono_ns`
  (`CLOCK_MONOTONIC`). Conversion to wall-clock time uses the anchor in the
  package; logs and audits may also carry `ts` (wall clock).
- **Errors:** Core libraries do not throw exceptions and do not use RTTI.
  Errors are surfaced through return codes, status enums, and counters; silent
  drops are forbidden (see [coding rules](../reference/coding-rules.md)).
- **Read-only tools:** `thyme_probe` never unlinks or writes shared memory. Use
  `thyme_upgrade.sh` or an operational procedure for cleanup.
- **ABI:** Mixed versions are rejected explicitly. Run
  `thyme_probe abi --mismatches` before an upgrade (see
  [configuration and deployment](config-deploy.md)).

## Related documents

- [15-minute tutorial](../guides/tutorial.md)
- [Glossary](../reference/glossary.md)
- [Operations runbook](../guides/runbook.md)
- [ABI compatibility rules](../reference/abi-compatibility.md)
