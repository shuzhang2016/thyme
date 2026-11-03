<!-- Code generated for the generated graph. DO NOT EDIT. -->
<!-- gen_graph 3.11.1 (output format 3) -->
<!-- Hand integration: src/nodes/, include/.../contracts/, config/viz/ -->

# Integration guide (this graph)

Generated automatically from `graph.yaml` and the template catalog. The general workflow is described in `tools/gen/gen_graph/INTEGRATION.md` in the thyme repository; **this file covers only the manual steps and CLI parameters for the current graph.**

Regenerate with:

```bash
gen_graph --in graph.yaml --out . --update generated [--template-dir <dir>]
```

## Graph summary

| Item | Value |
|------|-------|
| nodes | 1 |
| edges | 0 |
| processes | 1 |
| pool_n | 4 |
| ring_cap | 8 |
| health_n | 1 |
| record topics | 0 (`kRecordRegistryCount`) |
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

### `tick` — TickThread

| Property | Value |
|----------|-------|
| type | `tick` |
| Hz | 10 |
| process | `main` |
| Hand-written file | `src/nodes/tick.cpp` |
| Generated header | `generated/tick_generated.h` |

**Input edges (consume):**

- (no data-edge inputs)

**Output edges (produce + record tap):**

- (no output data edge)

**Catalog contracts (preflight / graph_check):**

- (the template declares no contracts; yaml edges are authoritative)

**Manual checklist:**

- [ ] Implement `TickThread::Loop()`
- [ ] Unknown type: define the SDK seam and I/O yourself

## Recording — topics for this graph

See `generated/record_registry.h` for the registry. The recorder CLI is documented in `tools/gen/gen_graph/README.md` (recorder-cli section) in the thyme repository.

This graph has **no recorded channels**, so `kRecordRegistryCount=0`; the recorder still compiles but the mirror has no data topics. (Edges with `record: false` still use their in-process transport; they simply do not enter the manifest.)

## Replay — stages for this graph

No inject stages.

## Visualization (viz_bridge) — channel allowlist for this graph

Enable these in `channels:` in `config/viz/viz.yaml`. Field-level decoding is provided by codegen (`generated/viz_ser.h` plus contract `contracts_json.h`), so the PC side no longer needs a user script:

```yaml
channels:
```

```bash
viz_bridge --shm /thyme_rec_<pid> --config config/viz/viz.yaml
```

## Integration verification (suggested for this graph)

- [ ] `cmake --build` passes after `gen_graph --update generated`
- [ ] No static_assert failures in `generated/graph_check.h`
- [ ] Run the project's CTest cases related to record/mp/viz
