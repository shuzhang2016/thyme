<!-- Code generated for the ports fixture graph. DO NOT EDIT. -->
<!-- gen_graph 3.11.1 (output format 3) -->
<!-- Hand integration: src/nodes/, include/.../contracts/, config/viz/ -->

# Integration guide (this graph)

Generated automatically from `graph.yaml` and the template catalog. The general workflow is described in `tools/gen/gen_graph/INTEGRATION.md` in the thyme repository; **this file covers only the manual steps and CLI parameters for the current graph.**

Regenerate with:

```bash
gen_graph --in graph.yaml --out . --update generated --template-dir ports_tpl
```

## Graph summary

| Item | Value |
|------|-------|
| nodes | 3 |
| edges | 1 |
| processes | 1 |
| pool_n | 4 |
| ring_cap | 8 |
| health_n | 3 |
| record topics | 1 (`kRecordRegistryCount`) |
| shm_prefix | `/c9_%pid` |

`hz`: a **cyclic** node's value is its loop period in Hz (`1/period`); an **event** node's value is a lower bound on expected steady-state messages/s (the same order as `NoteHandled`), **not** a cyclic period and not `channels ×` upstream declared hz. The supervisor's `MinLoopsForProcess` uses the process **min hz** × seconds / 4, so a single node is not required to saturate production.

## Generated vs manual

| Category | Path | Notes |
|----------|------|-------|
| Topology glue | `generated/wiring.*`, `generated/*_generated.h` | Regenerate; do not edit by hand |
| Record manifest | `generated/record_manifest.h` | Ordered recorded-channel table (id/hash); inspect the diff after regeneration |
| Record registry | `generated/record_registry.h`, `record_table.h`, `record_tap_helpers.h` | struct_id / sizeof / RecordTap* |
| Replay | `generated/replay_inject.h`, `replay_stages.h` | `ReplayInjectOne` / stage filtering |
| Multi-process tables | `generated/graph_ir.h` | SHM suffixes and process ownership |
| Contract types | `include/.../contracts/` (template provides stubs) | **Edit payload evolution here**; regeneration is usually unnecessary |
| Algorithm + SDK seam | `src/nodes/<id>.cpp` | **Implement Loop() and SDK integration here** |
| Viz configuration | `config/viz/viz.yaml` | Channel allowlist (see below) |
| Recorder/control binaries | `tools/recorder/`, `tools/recorderctl/` | Thin shells; the logic lives in `thyme::recorder` |

## Node and SDK integration (manual)

Rule (all nodes): inside `Loop()` use **acquire → write slot → publish → release inputs**; never pass raw SDK pointers through a ring; call `RecordTap*` only **after publish** (if there is an output).

### `prod` — ProducerThread

| Property | Value |
|----------|-------|
| type | `producer` |
| Hz | 10 |
| process | `main` |
| Hand-written file | `src/nodes/prod.cpp` |
| Generated header | `generated/prod_generated.h` |

**Input edges (consume):**

- (no data-edge inputs)

**Output edges (produce + record tap):**

- `payload` → `cons` · `Foo` · `RecordTapPayload(page, pod)`

**Catalog contracts (preflight / graph_check):**

- (the template declares no contracts; yaml edges are authoritative)

**Manual checklist:**

- [ ] Implement `ProducerThread::Loop()`
- [ ] For each output, call the matching `RecordTap*` after `publish()` (see `generated/record_tap_helpers.h`)
- [ ] Unknown type: define the SDK seam and I/O yourself

### `cons` — ConsumerThread

| Property | Value |
|----------|-------|
| type | `consumer` |
| Hz | 10 |
| process | `main` |
| Hand-written file | `src/nodes/cons.cpp` |
| Generated header | `generated/cons_generated.h` |

**Input edges (consume):**

- `payload` ← `prod` · `Foo` · pool `FooPool`

**Output edges (produce + record tap):**

- (no output data edge)

**Catalog contracts (preflight / graph_check):**

- (the template declares no contracts; yaml edges are authoritative)

**Manual checklist:**

- [ ] Implement `ConsumerThread::Loop()`
- [ ] Materialize SDK output into a pool slot (`WriteInto` / memcpy); lifetime is limited to this `Loop()`

### `health_reporter` — HealthReporterThread

| Property | Value |
|----------|-------|
| type | `health_reporter` |
| Hz | 10 |
| process | `main` |
| Hand-written file | `src/nodes/health_reporter.cpp` |
| Generated header | `generated/health_reporter_generated.h` |

**Input edges (consume):**

- (no data-edge inputs)

**Output edges (produce + record tap):**

- (no output data edge)

**Catalog contracts (preflight / graph_check):**

- (the template declares no contracts; yaml edges are authoritative)

**Manual checklist:**

- [ ] Implement `HealthReporterThread::Loop()`
- [ ] Aggregate `HealthSlot`; no SDK and no record tap

## Recording — topics for this graph

See `generated/record_registry.h` for the registry. The recorder CLI is documented in `tools/gen/gen_graph/README.md` (recorder-cli section) in the thyme repository.

Topic order defines `RecordStructId`: recorded channels are sorted by channel name (see `generated/record_manifest.h`).

### Topic table

| id | channel | cpp_type | producer | process | priority |
|----|---------|----------|----------|---------|----------|
| 0 | `payload` | `Foo` | `prod` | `main` | P1 |

### Example commands (replace `<pid>` / paths)

**Continuous recording (single process or attach to a shared mirror):**

```bash
# The default mirror name is project-specific; for the MP dummy it is /c9_<supervisor_pid>_rec
recorder --shm /thyme_rec_<pid> --out /data/sess.thyme.bin --seconds 300
```

**Filter by topic** (channel or cpp_type names, comma-separated):

```bash
recorder --shm /thyme_rec_<pid> --out /data/subset.bin \
  --topics payload --seconds 60
```

**Triggered recording plus control plane:**

```bash
recorder --shm /c9_<pid>_rec --trigger --out-dir /data/triggers \
  --pre 30 --post 60 --settle-ms 500 \
  --control-socket /tmp/thyme_recorder.sock --seconds 86400

thyme-recorderctl --socket /tmp/thyme_recorder.sock trigger
```

On the module side, `record_trigger::Request(reason)` goes through the supervisor's `--relay-recorder-socket` (same socket path). **Do not** connect directly to the recorder from `Loop()`.

## Replay — stages for this graph

Allowed `--inject-stage` values (consumer node ids): `cons`. See `generated/replay_stages.h` for the mapping.

```bash
replay_tool --bin /data/sess.thyme.bin --inject-stage fusion --as-fast
ad_stack --replay /data/sess.thyme.bin  # default: inject into paused producers only
replay_tool --bin /data/sess.thyme.bin --inject-all --as-fast  # full bin replay
```

## Visualization (viz_bridge) — channel allowlist for this graph

Enable these in `channels:` in `config/viz/viz.yaml`. Field-level decoding is provided by codegen (`generated/viz_ser.h` plus contract `contracts_json.h`), so the PC side no longer needs a user script:

```yaml
channels:
  - payload
```

```bash
viz_bridge --shm /thyme_rec_<pid> --config config/viz/viz.yaml
```

## Integration verification (suggested for this graph)

- [ ] `cmake --build` passes after `gen_graph --update generated`
- [ ] No static_assert failures in `generated/graph_check.h`
- [ ] Every producer calls `RecordTap*`
- [ ] `recorder --shm ... --out /tmp/t.bin --seconds 5` produces a non-empty bin
- [ ] Run the project's CTest cases related to record/mp/viz
