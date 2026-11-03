# Get thyme running in 15 minutes (tutorial)

Goal: generate a graph application from scratch, build and run it, record one
package through the in-repo trigger chain, then inspect, export and replay it
with `thyme-replay`. **Every command below was actually executed on a Linux
development tree of this repository**; expect 12–15 minutes (the build takes
most of it).

- Prerequisites: you already ran `cmake -S . -B build` and
  `cmake --build build -j4` per [`onboarding.md`](onboarding.md) §1; all
  commands below run from the **repository root**.
- The two chains serve different purposes: `gen_graph` generates a **business
  graph application** (an empty graph has only `tick` and `health_reporter`),
  while triggered recording uses the in-repo **`demo_producer` +
  `trigger_center`**. The tutorial runs each once, then inspects and replays
  the resulting package.

| Minute | Step |
| --- | --- |
| 0–2 | Generate the graph application |
| 2–6 | Build and run the application |
| 6–9 | Start the trigger chain, fire one manual trigger |
| 9–11 | Wait for the seal, `thyme-replay list/info/schema/integrity` |
| 11–13 | `export` (MCAP/NDJSON) |
| 13–14 | `play` into a new target |
| 14–15 | Troubleshooting, cleanup |

## 1. Generate the graph application (~2 min)

```bash
./build/tools/gen/gen_graph/gen_graph --out /tmp/thyme_tutorial_app --update all
```

Expected:

```text
gen_graph: injected default health_reporter (hz=10, health_n=2); disable with --no-health-reporter
gen_graph: SCHED admission not analyzed; run --explain-sched for per-core detail
nodes=2 pool_n=4 ring_cap=8
```

`--update all` is the greenfield bootstrap path: it creates `graph.yaml`,
`generated/`, `src/nodes/`, `src/main.cpp` and a CMake project. After
generation you can inspect the topology and the regeneration contract
(optional):

```bash
./build/tools/gen/gen_graph/gen_graph --in /tmp/thyme_tutorial_app/graph.yaml --explain-rates
./build/tools/gen/gen_graph/gen_graph --in /tmp/thyme_tutorial_app/graph.yaml --explain-sched
```

## 2. Build and run the application (~4 min)

```bash
cmake -S /tmp/thyme_tutorial_app -B /tmp/thyme_tutorial_app/build \
    -DTHYME_SOURCE_DIR="$PWD"
cmake --build /tmp/thyme_tutorial_app/build -j4
/tmp/thyme_tutorial_app/build/app --seconds 2
```

Expected tail (the `mlockall` warning is very common on ordinary development
machines: without `CAP_IPC_LOCK` it is harmless and does not affect this
walkthrough):

```text
ad_stack v1
tick         loops=21
health_reporter loops=21
```

The application exits with code 0. Business code goes only in
`/tmp/thyme_tutorial_app/src/nodes/`; never edit `generated/` by hand. After a
graph change, regenerate with `gen_graph --update generated` for everyday work
and use `--check` before committing (see
[`../tools/gen/gen_graph/README.md`](../../tools/gen/gen_graph/README.md)).

## 3. Record one trigger package (~3 min)

Prepare the configuration: change only the output directory, and lower the idle
flush hold from the default 30 s to 1 s (otherwise you wait half a minute for
the seal after firing the trigger):

```bash
mkdir -p /tmp/thyme_demo_out
sed -e 's|output_dir = /opt/usr/triggered_data|output_dir = /tmp/thyme_demo_out|' \
    -e 's|flush_policy = idle|flush_policy = idle\nidle_hold_s = 1|' \
    tools/apps/trigger_center/trigger_config.minimal.ini > /tmp/thyme_demo.ini
./build/tools/apps/trigger_center/trigger_center --check-config \
    --record-manifest /etc/thyme/record_manifest.h /tmp/thyme_demo.ini
```

Expected `trigger_center: config OK`. Then start the trio in the same terminal:

```bash
./build/tools/apps/trigger_center/demo_producer \
    /thyme_recr_can /thyme_demo_ring /h264_front 100 \
    >/tmp/demo_producer.log 2>&1 &
PROD=$!
./build/tools/apps/trigger_center/trigger_center \
    --record-manifest /etc/thyme/record_manifest.h /tmp/thyme_demo.ini \
    >/tmp/trigger_center.log 2>&1 &
TC=$!
sleep 2
./build/tools/apps/trigger_center/triggerctl manual
```

Expected `code=accepted trigger_id=1 priority=10`. The `manual` source is
configured with `before_s = 10` + `after_s = 5`; wait for the seal to complete:

```bash
sleep 10
PKG=$(ls -d /tmp/thyme_demo_out/*/ | tail -1)
echo "$PKG"
find "$PKG" -maxdepth 2 -type f
```

You should see `wal.log`, `meta.tbin`, `small.tbin`, `schema.json`,
`integrity.manifest` and `frames/h264_front.{raw,index.tbin}`. In the FSM log,
`why=5` means the package is sealed:

```bash
tail -3 /tmp/thyme_trigger.sock.fsm
```

```text
{"ts":...,"id":1,"from":0,"to":1,"why":0}
{"ts":...,"id":1,"from":1,"to":1,"why":1}
{"ts":...,"id":1,"from":1,"to":0,"why":5}   # 5 = complete
```

## 4. Inspect the package (~2 min)

```bash
./build/tools/replay/thyme-replay/thyme-replay list "$PKG"
./build/tools/replay/thyme-replay/thyme-replay info "$PKG"
./build/tools/replay/thyme-replay/thyme-replay schema "$PKG"
./build/tools/replay/thyme-replay/thyme-replay integrity "$PKG"
```

How to read the output:

- The first line of `list` must be `wal : present (sealed)`, otherwise the
  package is not fully sealed yet; `channels: 3` (`meta` / `small` /
  `h264_front`);
- `info` gives `RATE_HZ` / `BYTES/S` per channel; `small` is about 100 Hz;
- `schema` is the schema sidecar (`Pod16`, `FrameIdx`, `TrigMeta`); this build
  has no record manifest, so it prints `manifest : unavailable`, which is
  normal;
- `integrity` must be `integrity: OK` with `files : 6 ok, 0 missing, 0
  mismatched`. If it prints `manifest : absent` + `integrity: FAILED`, the
  package was not sealed (see §6).

A non-zero `GAPS` for `h264_front` is expected: `before_s = 10` exceeds the
large-frame ring's time window, and the gap is accounted for explicitly instead
of silently dropping frames.

## 5. Export and replay (~2 min)

Export (read-only, streaming; does not modify the package):

```bash
./build/tools/replay/thyme-replay/thyme-replay export "$PKG" --out /tmp/tut.mcap
./build/tools/replay/thyme-replay/thyme-replay export "$PKG" --out /tmp/tut.ndjson --format ndjson
```

Expected `export : OK  format=mcap` / `format=ndjson`, plus the event count for
each of the three channels. MCAP opens directly in Foxglove; NDJSON is suitable
for scripting.

Replay needs an explicit target. The tool creates the large-frame channel; the
`small` ring is skipped when it has no target (non-strict mode by default):

```bash
./build/tools/replay/thyme-replay/thyme-replay play "$PKG" \
    --pinned_slot h264_front=/thyme_tut_replay --as-fast
rm -f /dev/shm/thyme_tut_replay
```

Expected key lines:

```text
target  : channel 2 (h264_front, pinned_slot) -> /thyme_tut_replay  1024 B x 64
playback: as-fast  rate=1.000  events=1424 injected=126 skipped=796 ...
result  : OK  state=finished
```

`skipped=796` is the `small` channel without a `--small` target; to inject
small structs, first create a same-named ring from a producer process, then use
`--small small:0=<ring>:<struct-id>` (see [`replay.md`](replay.md) §5).

Cleanup:

```bash
kill "$TC" "$PROD"
rm -f /dev/shm/thyme_recr_can /dev/shm/thyme_demo_ring /dev/shm/h264_front
```

## 6. Where to look when something goes wrong

| Symptom | Look first | Action |
| --- | --- | --- |
| `gen_graph: --in is required` | Did you use `--update all --out DIR`? | There is no `init` for greenfield; use `--update all` |
| The app build cannot find thyme | `-DTHYME_SOURCE_DIR` | Pass the **absolute path** of the repository root |
| `mlockall ... failed` | Runtime environment | Warning when `CAP_IPC_LOCK` is missing; functionality is unaffected |
| No package in the output directory | `/tmp/trigger_center.log`, `ls /tmp/thyme_demo_out` | Check recorder Init / disk; the audit is in `/tmp/thyme_trigger.sock.audit` |
| The package directory exists but has no `integrity.manifest` | `tail -3 /tmp/thyme_trigger.sock.fsm` | Only `why=0/1` means it is not finished; wait for `why=5`, or confirm `idle_hold_s` was lowered |
| `integrity: FAILED` + `manifest: absent` | Same as above | An unsealed package is treated as `.incomplete`; record again |
| `play` shows many `UNMAPPED` | Target arguments | That channel has no `--pinned_slot`/`--small`; `--strict-targets` exits 3 instead |
| `schema` reports `manifest : unavailable` | Build configuration | Normal for a build without a record manifest; the sidecar still works |

For more operational scenarios (disk full, SHM leak, crash recovery) see
[`runbook.md`](runbook.md); for the full trigger configuration key table see
[`trigger-config.md`](trigger-config.md).

## Next steps

- Change the graph / add a channel: [`onboarding.md`](onboarding.md) §4,
  [`../tools/gen/gen_graph/README.md`](../../tools/gen/gen_graph/README.md),
  [`../tools/gen/gen_graph/INTEGRATION.md`](../../tools/gen/gen_graph/INTEGRATION.md);
- Replay semantics, gating and export formats: [`replay.md`](replay.md);
- Canonical JSON for editor round-trips:
  [`../tools/gen/gen_graph/README.md`](../../tools/gen/gen_graph/README.md)
  §Canonical JSON;
- Trade-offs of the service mode (request/response):
  [`adr/0009-request-response-primitives.md`](../design/adr/0009-request-response-primitives.md).
