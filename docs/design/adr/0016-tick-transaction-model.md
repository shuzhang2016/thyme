# ADR 0016: one-tick transaction model for cyclic nodes

- Status: Accepted (implemented)
- Related: channel facade and index-handoff adapter, execution model, unified channel facade

## Context

The original generated `Loop()` had only three steps: return immediately on a
replay pause, call `OnLoop()`, and call `CreditQueuePushFails()`. Everything
else stayed on the hand-written side:

- reading input edges (latest takes the newest, queues drain);
- `BeginWrite` to borrow an output slot, fill the payload in place, and
  `CommitWrite` or `AbortWrite`;
- taking and returning `LocalIndexQueue` slots;
- `RecordTap` (a generated helper, but called by hand-written code).

Event nodes already had a clean model: `Dispatch()` drains each in-edge within
its compile-time budget and calls `On<Channel>(Sample&)` per message. **One
message is one call**, so borrowing/returning slots and recording can be fully
owned by the generated side.

Cyclic nodes cannot copy "one virtual function per edge": they are triggered by
a timer and must read several inputs and compute several outputs in the same
tick. A control node may compute steering and braking together; splitting it
into per-channel callbacks would force the algorithm to store state between
callbacks.

## Decision

`Loop()` has four fixed steps (`produce: one`, the default):

1. **Replay pause as before:** return immediately.
2. **Read input edges:** latest takes the newest frame; queues drain within the
   budget written in the graph, capped by the compile-time `Cap`. Output edges
   first `BeginWrite` (a zero-copy queue receives a pool slot).
3. **Call `OnTick(in, out)`.**
4. **Commit and reclaim:** `CommitWrite` outputs marked `ready` (recording
   happens on commit), `AbortWrite` the rest; `Release` every input slot; queue
   failure counters are also recorded here.

`TickIn` / `TickOut` are generated from the node's edges:

- local queue, raw pool/IndexPipe (including cross-process `LatestSlot`), and
  ordinary broadcast inputs are `const Frame<T>*`; PinnedSlot inputs are
  `const uint8_t*` plus `uint32_t size`, so a blob is not disguised as
  `Frame<T>`;
- latest inputs are read once; follow inputs drain within the effective `Cap`
  and keep the last sample. Cyclic nodes remain last-wins; follow is not turned
  into a per-frame event callback;
- every input carries `skipped_<name>` in `TickIn`: the number of old frames
  released while draining (queue / IndexPipe / follow / PinnedSlot follow). A
  pure latest read sees intermediate frames the backend cannot expose, so the
  value is always 0 and means "unknown", not "nothing lost". This makes "can the
  consumer detect an overwrite?" measurable on queue and follow channels
  without changing callback shape;
- local queue/raw pool outputs are `Frame<T>*`; event pipe/broadcast outputs are
  payload `T*`; PinnedSlot outputs are `uint8_t* + size`. Multiple subscription
  edges on one channel borrow the write slot once;
- every pointer is **valid only for this call** and must not be retained to the
  next tick; outputs are written directly into the borrowed slot, and large
  objects are not returned by value;
- returning `false` suppresses all outputs for this tick; suppressing one route
  clears that route's `ready`;
- `Publish`, `RecordTap`, and slot borrowing/returning no longer appear in
  converted `src/nodes`.

A PinnedSlot latest/follow sample holds its lease until `OnTick`/`OnProduce`,
output commit or abort, and all other input releases finish; only then is
`Reset()` called. PinnedSlot follow releases the previous sample's lease before
retaining the next one, so it never accumulates pins over a whole queue in one
tick.

Nodes with PinnedSlot outputs additionally generate `TickRequest` and an empty
default `PrepareOutput(in, request)`. The default size is `sizeof(T)`;
variable-length SHM outputs override the callback before borrowing a slot,
providing this tick/frame's length, after which generated code calls
`BeginWriteAt(size, 0)`. In-process PinnedSlot remains fixed-size; backend
boundaries are not changed for callback uniformity.

Nodes that emit multiple frames per tick use a second shape:
`while (budget remains && OnProduce(in, out))`, committing one frame per true
return. The graph selects `produce: one` (default) or `produce: many`; the two
shapes are not combined in one `OnTick`. Multi-frame mode borrows slots only
inside a bounded loop, and PinnedSlot re-prepares the size each iteration.
Local queues and Frame channels record via `RecordTap` only after a successful
commit; a failed handle must not be recorded as a published frame.

**Channel types are unchanged:** `OnTick`/`OnProduce`/`PrepareOutput` are node
callbacks, peers of the existing `OnLoop`, not channel virtual functions. Raw
pool/IndexPipe still move only indexes; cross-process `LatestSlot` keeps its
existing SHM↔pool copy, with no second copy. Conversion happens only when all
inputs and outputs of a node can be represented; cyclic nodes with data edges
now satisfy this, and isolated edge-less nodes keep `OnLoop`. The generator does
not overwrite existing `src/nodes`.

## Consequences

- Recording is fixed at commit time, preserving the per-source `mono_ns`
  monotonic contract; the replay pause path is unchanged.
- Input slot and PinnedSlot lease lifetimes move from "hand-written convention"
  to a generator guarantee; callbacks must not retain their pointers across
  ticks.
- Queue failure counters and health drop accumulation move to the generated
  side, avoiding per-node duplication.
- Cost: each node pays one `TickIn`/`TickOut` assembly and one callback (the
  same order as the former `OnLoop`).

## Validation

- `test_gen_graph_ports` covers queue inputs/outputs, `produce: many` bounded
  loops, single-slot borrowing, and record-on-successful-commit.
- `test_gen_graph_broadcast` covers ordinary latest/follow inputs, Frame
  outputs, and loss accounting.
- `test_gen_graph_pinned_slot` covers PinnedSlot latest/follow, fixed-size SP/MP
  outputs, MP variable-length `PrepareOutput`, and lease release at tick end.
- `test_gen_graph_tick_transports` verifies real sequence numbers and payloads
  for cross-process `LatestSlot`, event queue, and broadcast in one MP graph.
- `test_channel_facade` covers `frame_ptr()` for Frame/raw/failed handles and
  move-only lease samples.
- Goldens, `test_shm_leak_audit`, and the full CTest suite prevent generated
  drift, slot/shared-page leaks, and old-path regressions.
