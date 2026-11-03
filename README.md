# thyme

A fixed-topology, single-node C++17 real-time data-plane runtime. The graph is
resolved at generation time, channel semantics are encoded in types, small
messages travel through shared memory by value or index, large frames travel
through leases, and recording/triggering run on a side path.

- **Architecture:** [ARCHITECTURE.md](ARCHITECTURE.md)
- **User manual:** [docs/usage/index.md](docs/usage/index.md)
- **Glossary:** [docs/reference/glossary.md](docs/reference/glossary.md)
- **Observability:** [docs/guides/observability-dashboards.md](docs/guides/observability-dashboards.md)
- **Channel observability audit:** [docs/guides/channel-audit.md](docs/guides/channel-audit.md)

## What it is — and what it is not

| It is | It is not |
|---|---|
| `graph.yaml` → `gen_graph` → static `generated/`; no runtime discovery or registration | A dynamic-discovery middleware such as DDS or ROS 2 |
| node = thread; cyclic nodes run as tick transactions, event nodes wake on messages | An executor or callback-group framework ([ADR 0003](docs/design/adr/0003-no-callback-group-executor.md)) |
| Channel semantics fixed at compile time: latest / queue / broadcast / lease | Runtime QoS knobs |
| Single node (ARM64 or x86 Linux), shared memory + futex | Cross-ECU services (SOME/IP, DDS over the network) |
| Recording, triggered capture, replay, and probing as first-class capabilities | A bare pub/sub transport |

## Channel semantics

The behavior of an edge is determined by the three compile-time parameters of
`Channel<T, Semantics, Backend>` and fixed during generation:

| retention | readers | overflow | transport | behavior on overwrite/full | who can observe it |
|---|---|---|---|---|---|
| latest | one | overwrite | `LatestSlot` | keeps only the newest value | consumers **cannot** count dropped frames; `TickIn.skipped_*` applies only to drain-style inputs |
| queue | one | reject | `ShmSpscRing` (cross-process) | producer receives a failure status | producer: `Publish` return value / `push_fail_count` |
| queue | one | drop | `SpscRing` (in-process) | drops the oldest index | consumer: index gap / `drop_count` |
| queue | many | overwrite | `ShmSpmcRing` | overwrites the oldest slot | each reader: `reader_drop_slots` / `drop_count` |
| queue | many | drop | `PinnedSlotWriter`/`PinnedSlotReader` (large frames) | writer skips while all slots are pinned | writer: `pinned_skip`; reader sees only holes plus `FrameGap` in the packet |

`Publish` carries `[[nodiscard]]`. `ChannelFixedSample<T>` (copy) and blob
samples (lease) are **two distinct types**, so their lifetimes cannot be
confused. See [ARCHITECTURE §5](ARCHITECTURE.md#5-channels-and-transport).

## Quick start

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

# MR gate only
ctest --test-dir build -L mr_gate --output-on-failure
```

The `gen_graph` schema, `--update` mode, and `--check`:
[tools/gen/gen_graph/README.md](tools/gen/gen_graph/README.md); integration
workflow: [tools/gen/gen_graph/INTEGRATION.md](tools/gen/gen_graph/INTEGRATION.md).

## Runtime components

| Library | Contents |
|---|---|
| `thyme::runtime` | `App`/threads, rings/slots/leases, shared-memory pages, health, perf/ProcSampler/host sampler |
| `thyme::record` | record mirror, `.thyme.bin`, trigger center, quotas/audit, MCAP export |
| `thyme::app` | `MpControl`, supervisor, session lease, status server, replay mode |
| `thyme::replay` | replay injection, pause/step/rate, replay gate |
| `thyme::probe` | read-only page probing, ABI matrix, schema view |
| Tools | `gen_graph`, `thyme_log_merge`, `thyme_probe`, `thyme_metrics_exporter`, `thyme-replay`, `trigger_center`, `mcap_export` |

## Recording, triggering, and replay

- **Continuous recording:** writer-side taps feed a record mirror (arena plus
  offset table); the recorder drains it into `.thyme.bin` (v4 keyed registry)
  and can export MCAP from a package. `[continuous]` controls slicing and
  quotas.
- **Triggered capture:** producers pre-record according to `[source.*]`. After
  `trigger_center` receives a request, it harvests the `before_s`/`after_s`
  window from shared memory, pins large frames, and records gaps as
  `FrameGap`. The FSM, quotas, preemption, cancellation, audit, and the
  incremental hash after sealing all live in this layer.
- **Replay:** `thyme-replay` injects into the same channels. The replay gate in
  the channel header pauses producers during replay; `pause/step/rate` follow
  the recorded timeline.

See [docs/guides/trigger-config.md](docs/guides/trigger-config.md) for the
configuration keys and [docs/guides/replay.md](docs/guides/replay.md) for the
package format and replay workflow.

## Observability

- **Perf log:** ten block families (sample, percentile, work, epoch, sys,
  histogram, manifest, series name, budget, clock anchor). A dedicated 1 Hz
  thread collects data; the hot path performs no `/proc` access.
- **Process resources:** `ProcSampler` exposes 16 fixed series (CPU, page
  faults, context switches, RSS, threads, fds, I/O, CPU delay).
- **Optional sources** (off by default,
  `THYME_PROC_EXT=cgroup,psi,disk,net,threads|all`): cgroup v1/v2, host
  PSI/block device/network, and registered-thread CPU. Capability bits in
  `ext_caps` explain "unsupported" instead of pretending it is zero.
- **Host-wide:** the supervisor writes `<prefix>_host_<pid>.bin` with overall
  and per-core CPU, load, memory, and the system fd table.
- **Aggregation:** `thyme_log_merge` (timeline / `--hist` / `--sys`),
  `thyme_metrics_exporter` (Prometheus textfile), and `thyme_probe` (read-only
  page/ring/ABI probing).
- **Gates:** `channel_audit.py` enforces allowlists for `latest`/`reject`/
  single-slot pinned edges and emits a "can the consumer observe it?" matrix.

## Capacity and limits (part of the ABI)

| Item | Value | Notes |
|---|---|---|
| `Frame<T>` transport limit | 65535 B (`kAbiMaxTransportBytes`) | Determined by the uint16 `hdr.size`; larger payloads use `PinnedSlot` |
| Record-layer per-channel limit | 512 KB (`kAbiMaxPayloadBytes`) | Payload limit for `.thyme.bin` and the mirror |
| record-mirror arena | 2 MiB | Sum of all channel capacities |
| Managed threads per process | default 64, build-time 1..1024 | `THYME_ABI_MAX_THREADS` |
| Contract kinds / mirror slots | default 64, build-time 1..1024 | `THYME_ABI_MAX_STRUCT_COUNT` |
| Broadcast readers | fixed 8 | Determined by the SPMC page layout |
| event / cyclic wait inputs | 32 / 8 | Shared by graph validation |
| host sampler cores | 64 | Additional cores are folded into the total row |

## Build options

| Option | Default | Effect |
|---|---|---|
| `THYME_BUILD_TESTS` | ON | Build the test suite and tool tests |
| `THYME_CORE_EMBEDDED` | ON | Build core with `-fno-exceptions`; propagates `-fno-rtti` to linkers |
| `THYME_STRICT_WARNINGS` / `THYME_WERROR` | ON / OFF | Strict warnings; enable `-Werror` only on a fixed CI image |
| `THYME_ABI_MAX_STRUCT_COUNT` / `_THREADS` | 64 / 64 | Build-time capacity profile, included in the page identity hash |
| `THYME_ENABLE_TSAN` | OFF | ThreadSanitizer build |
| `THYME_ENABLE_COVERAGE` | OFF | Coverage build |
| `THYME_REPRODUCIBLE` | ON | Reproducible path mapping |
| `THYME_USE_CCACHE` | ON | Use ccache as the compiler launcher when available |

## Documentation map

| Topic | Entry point |
|---|---|
| Architecture, execution model, transport, MP, observability | [ARCHITECTURE.md](ARCHITECTURE.md) |
| User manual (API, tools, topics) | [docs/usage/index.md](docs/usage/index.md) |
| Scheduling model | [docs/reference/scheduling-model.md](docs/reference/scheduling-model.md) |
| ABI / mixed-version rejection | [docs/reference/abi-compatibility.md](docs/reference/abi-compatibility.md) |
| Coding rules | [docs/reference/coding-rules.md](docs/reference/coding-rules.md) |
| Test matrix and gates | [docs/guides/test-matrix.md](docs/guides/test-matrix.md) |
| Operations runbook | [docs/guides/runbook.md](docs/guides/runbook.md) |

## Related projects

| Project | Role |
|---|---|
| Reference application | Generated with `gen_graph` from a graph definition; not included in this repository |
| Contracts package | Shared contract types and generator inputs; maintained separately |
| Algorithm/driver SDKs | Connected through adapters; not part of the core runtime |
| `thyme-viz` (optional) | Foxglove WS and recorder frontend; related tool targets are skipped when absent |

## License

MIT. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

Copyright (c) 2026 SoftwareMotion Co., Ltd &lt;shu.zhang@sw-motion.cn&gt;.
