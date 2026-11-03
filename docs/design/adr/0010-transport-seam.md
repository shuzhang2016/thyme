# ADR 0010: transport seam — fix the interface boundary, do not implement a second transport

- Status: Proposed (records the interface boundary and decision; this ADR
  produces **no** code change)
- Date: 2026-10-24
- Scope: transport seam; paired with the SHM super-header, `ChannelBase`,
  session lease, mono↔wall clock anchor, and the request/response evaluation
- Non-goals: UDP, dynamic discovery, serialization/IDL, general RPC, a second
  transport backend

## Context

thyme has one transport today: POSIX shared memory. Every channel is a local SHM
page written by a single writer and read by N readers according to their
semantics:

| Primitive | Delivery | When full | Wakeup | Ownership |
|---|---|---|---|---|
| `SpscRing` / `ShmSpscRing` | Lossless, ordered | Returns false (backpressure) | futex/condvar | Single writer |
| `ShmSpmcRing` | Broadcast, lossy | Overwrites oldest | Reader polling/notification | Single writer |
| `LatestSlot` | Newest only | Overwrites | None | Single writer |
| Pinned-slot store | Large frames, slots | `kOverrun`/`kPinnedSkip`/`kGated` | Reader polling | Single writer + slot lease |
| `NotifPipe` | Lossless, ordered | Returns nullptr (backpressure) | futex/condvar | Single writer |
| `RecordMirror` / ring tap | Side-path snapshot, lossy | Overwrite/skip | None | Single writer |
| `MpControl` / `SharedHealthPage` | State broadcast | Overwrites | None | Fixed slots |
| `StatusServer` / control socket | Command/query | Explicit error | `poll` | Same uid |

These primitives **share a set of cross-cutting semantics** but implement them
separately in each header: page magic/version, wake word, epoch/generation,
writer ownership (`ProducerGate` / slot lease), drop counters, and attach-time
layout validation. Callers (generated code, probe, replay, recorder) face
concrete types, so "change the transport" means changing every call site.

This ADR answers: **should a `Transport`/`Channel` abstraction be introduced
now?** The conclusion is to define only the boundary, not implement it.

## Forces

1. **Single-node is the only confirmed deployment form.** Every channel is local
   SHM; there is no quantified cross-machine data-flow requirement. Writing an
   abstraction for a nonexistent backend has zero benefit and a permanent
   compatibility cost.
2. **Hot-path zero-allocation and no virtual-call budget.** Core publish/read
   runs on RT threads; a runtime-polymorphic `Transport*` would bring indirect
   calls, lifetime, and error handling into `On<Channel>`. See
   [ADR 0002](0002-core-language-and-allocation.md) and
   [ADR 0003](0003-no-callback-group-executor.md).
3. **Failure semantics are fixed and cannot regress.** Drops are counted, gates
   return `kBusy`, leases return `kPinnedSkip`, and attach must reject mixed
   versions. Any transport abstraction must express the same result codes and
   must not introduce silent best-effort.
4. **The ownership model is converging.** There is a per-channel
   `ProducerGate` today, which is being elevated to a session-level lease. In
   parallel, introducing a third transport lifecycle would create contradictory
   ownership semantics.
5. **Cross-cutting concerns are not centralized yet.** Version headers, wakeups,
   gates, counters, and lifecycle are being consolidated into `ChannelBase`.
   Before that, "what a transport must provide" can only be intent, not a stable
   API.
6. **Multi-node is a direction, not a commitment.** If it arrives, the transport
   must be connection-oriented or acknowledgement-capable (TCP/QUIC/automotive
   bus). **UDP is excluded**: connectionless, unreliable, unordered, and
   contrary to thyme's explicit-drop and determinism story.
7. **ABI cost.** A new page type means magic/version/layout/upgrade matrix,
   mixed-run rejection, and `thyme_probe` support. Paying that for a possible
   future is not worthwhile.

## Alternatives

### A. Do nothing (the SHM types are the interface)

- Pros: zero abstraction cost; shortest hot path; every tool works directly
  today.
- Cons: **nowhere to state the transport contract**. Every new tool must relearn
  "latest vs queue vs broadcast", "who owns gate/lease", and "what attach
  failure returns". A future multi-node change touches every call site and every
  primitive header.
- Verdict: keep as the "no new abstraction" baseline, but not writing even the
  **boundary documentation** is unacceptable, because ownership/epoch semantics
  already exist implicitly in many places.

### B. Thin seam: define only the boundary and map the single-node backend (selected)

Define a **backend-independent interface concept** without runtime
polymorphism on the hot path:

- the interface describes only what one publish / one subscribe / one attach
  must provide (full list below);
- the single-node backend is a **zero-cost mapping** to existing SHM primitives
  (templates/inline adapters, or type erasure at the host/assembly layer);
  no new page type, no ABI change, no hot-path change;
- until a second backend exists, the seam is only **documentation plus
  compile-time checkpoints**;
- when a multi-node backend lands, only the assembly layer changes; callers do
  not change `On<Channel>`.

- Pros: writes down contracts that already exist implicitly; gives the
  super-header, `ChannelBase`, and session lease a common target; preserves
  today's hot path; produces no second implementation to maintain.
- Cons: the interface cannot be validated by use before the first real backend
  and may become an imagined API; it must wait for `ChannelBase` before being
  fixed into types.
- Verdict: **adopt**, but strictly defer implementation until the triggers are
  met.

### C. Full transport abstraction including a network backend

Abstract publish/subscribe/open/close, serialization, reliability, discovery,
reconnect, and backpressure behind a runtime interface, with a network backend.

- Pros: one step; callers need no change for multi-node.
- Cons: serialization and endianness/layout versions enter the core; reliability
  semantics (retransmission, ordering, duplication) must align with the existing
  explicit-drop story; network latency/jitter is not statically analyzable and
  breaks the WCET argument; discovery/registry are explicit non-goals; the
  abstraction cannot be validated without a second backend and will almost
  certainly be overdesigned.
- Verdict: **reject now**. Not "never", but only after a quantified multi-node
  requirement exists.

## Decision

**Adopt option B: define a minimal seam, do not implement UDP, do not refactor
existing primitives.**

### What a transport must provide (interface boundary)

Each item is a **semantic requirement**, not a C++ signature. Signatures are
written after `ChannelBase` is fixed by the base-layer work; until then, no
implementation may assume this wording is already a stable API.

1. **Endpoint identity and contract**
   - `endpoint`: a stable channel name (SHM name/graph edge name) plus
     `contract` (struct id, `contract_version`, `layout_hash`).
   - `open` must reject a mismatched contract and return a **diagnosable**
     `kNotFound` / `kBadVersion` / `kBadLayout` / `kBusy`; it must not "read 0"
     or silently misread. This depends on the super-header.
2. **Lifetime**
   - `open(mode)` / `close()`; `mode` distinguishes at least writer / reader /
     replay (replay is a "synthetic writer", not a third data semantic).
   - `close()` must be idempotent; after a crash, reclamation is by
     lease/heartbeat, not by a normal close.
   - `epoch` (writer generation) is returned by open/attach; a reader that sees
     an epoch change must reattach instead of continuing to read a stale page.
3. **Publish**
   - `publish(payload, timestamp, flags)` returns an explicit result:
     `kOk` / `kOversize` / `kGated` / `kBackpressure` / `kEpochChanged`.
   - Each endpoint must declare its **delivery discipline** in the contract:
     `latest` (overwrite) / `queue` (ordered, backpressure when full) /
     `broadcast` (lossy per reader) / `notify` (ordered events). A transport
     must not mix disciplines on one endpoint.
   - Every rejection path is counted; the hot path must not log per message
     (CR-03).
4. **Subscribe/read**
   - `subscribe` returns a view or a copy, and the two must be explicit (a
     zero-copy view's lifetime is bounded by the slot lease; a copy path has no
     lease).
   - A read result must distinguish "no new data", "data overwritten (gap)",
     "torn/unavailable", and "epoch changed"; it must not collapse them into
     false.
5. **Wakeup**
   - Same-process futex/condvar waits are allowed; cross-process waits must be
     bounded with explicit timeout. The transport must declare whether wakeups
     are hints or guarantees: a lost wakeup must not lose data (the data is
     still in the ring), but may add latency.
6. **Ownership and leases**
   - A transport instance does not own the final "who is writing" decision; it
     exposes gate/lease result codes (`kBusy` / `kGated` / `kStolen`) and hands
     ownership to the session lease.
   - Double writes fail explicitly with `kBusy`; silent overwrite is forbidden.
     The per-channel `ProducerGate` is a member view of the session lease.
   - A slot lease (pin/skip) protects the **read side**, not writer ownership;
     the abstraction must not conflate the two.
7. **Diagnostics**
   - Every channel exposes drop/oversize/pinned-skip/gate-reject/epoch-change
     counters; `thyme_probe` can see at least these counters and layout
     validation results read-only.

### Mapping existing primitives to the seam

| Seam operation | Today's single-node implementation | Notes |
|---|---|---|
| `open(writer)` | `PinnedSlotWriter` / `ShmSpscRing` producer / `LatestSlot` writer / `NotifPipe` producer | Create or reuse page; super-header validation applies after it lands |
| `open(reader)` | `PinnedSlotReader` / ring consumer / slot reader | Attach failure returns `kBadLayout` and similar |
| `open(replay)` | `PinnedSlotWriter` + `ProducerGateAcquire` | Acquire the gate, then write; `kBusy` is failure |
| `publish` | `Begin/Commit`, `Push`, `PushLatest`, `NotifPipe::CommitWrite` | Result codes already exist (`BeginStatus`/bool/`GateResult`) |
| `subscribe` | `TryRead` / `TryPop` / `ReadIfNew` | Gap/torn/epoch results already exist; only naming needs unification |
| `wake` | `EventThread` wake word, `SpscRing::Notify` | Hint semantics |
| `close` | Destructor / `Shutdown` / `munmap` | Lease release is handled by the session lease |
| Diagnostics | `drop_count`, `pending_skipped`, `gate.rejects/steals`, probe | Counters exist; a unified export is missing |

**Key point:** no row in the mapping table requires new code. The first version
of the seam is valuable because it gives the super-header, `ChannelBase`, and
session lease a common target shape, not because it wraps today's types.

### Explicitly out of scope

- No new SHM page type, no wire/ABI change, no change to the public types of
  existing headers.
- No runtime virtual calls in `OnLoop`/`Dispatch`/tap paths.
- No UDP, discovery, registry, serialization/IDL.
- No `Transport` class in this ADR; after `ChannelBase` is fixed, a new ADR
  records the actual types and migration.

## Consequences

### For the SHM super-header

The seam's `open` contract depends on the super-header's
`magic / abi_version / layout_hash / page_bytes / feature_bits`. The
super-header must land first; otherwise the seam cannot promise "mixed-version
attach is rejected". The seam is a **consumer** of the super-header and defines
no new layout.

### For `ChannelBase`

Version headers, wakeups, gates, counters, and lifecycle belong to
`ChannelBase`'s cross-cutting layer. It must land first; otherwise the seam
reimplements the same logic. **Order: super-header → `ChannelBase` → typed
seam; the session lease may proceed in parallel with `ChannelBase`.**

### For the session lease

The seam defines no second ownership model. `open(writer|replay)` carries
session id + pid/starttime + heartbeat; gate result codes come from the lease;
crash reclamation is completed by lease heartbeat/expiry. Until the session
lease lands, the seam stays at the documentation level; otherwise it would
freeze the incorrect assumption that the per-channel gate is the final
ownership model.

### For the clock anchor

Cross-machine transport makes `mono_ns` non-comparable. The seam's `publish`
timestamp must allow either "sender mono plus anchor" or "absolute time"; the
single-node backend keeps `mono_ns`. Anchor semantics are defined by the clock
work; the seam only carries them.

### For request/response

If service primitives are built later, they should be **two gated channels** on
the seam (request: client writes/server reads; response: server writes/client
reads) plus protocol-layer correlation id/timeout/cancellation. The seam must
carry deadline, cancel, and correlation id as **metadata**, but must not
implement the request state machine. The deferral conclusion is unchanged.

### For future multi-node

If a multi-node requirement is confirmed, the second backend must:

- provide connection-oriented or acknowledgement-capable semantics
  (TCP/QUIC/automotive bus), **not UDP**;
- state what `latest`/`queue`/`broadcast` mean on that backend (for example,
  `latest` over a network can only be "best-effort newest", and readers must
  detect gaps);
- make epochs globally comparable (otherwise a reader cannot tell "new writer"
  from "new machine");
- keep explicit result codes, never degrading to "drop when the network is bad".

Once those hold, add a new ADR and backend following this boundary without
changing callers.

## Status and follow-ups

- This is a **design record, not a refactor**. The absence of code changes is
  deliberate.
- Follow-ups: super-header and attach-rejection matrix; `ChannelBase`
  (centralized version/wakeup/gate/counter/lifecycle); session lease (single
  ownership source); clock anchor (cross-machine timestamp semantics); service
  primitive evaluation (still deferred).
- If a second real backend requirement appears, first add an ADR with triggers
  and quantified targets, then change this decision's status; do not rewrite
  this ADR's historical conclusion.
