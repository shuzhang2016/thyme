# ADR 0013: keep ShmSpscRing as the cross-process lossless queue role

- Status: Accepted
- Date: 2026-10-26
- Scope: downstream dependencies, semantic necessity, alternatives, and
  deprecation gates for `ShmSpscRing`; does not change existing types, ABI, or
  generation strategy

## Context

`ShmSpscRing` is not just another ring with different parameters from
`ShmSpmcRing`. It provides **cross-process, single-writer/single-reader,
ordered, lossless delivery**: a full ring returns failure and increments
`push_fail`; the caller chooses backpressure or an explicit drop, and unconsumed
data is never overwritten.

`gen_graph`'s `delivery: queue` instantiates it directly in MP event transport;
`EventPipeRef` unifies the same-process `NotifPipe` and cross-process
`ShmSpscRing` behind Begin/Commit/TryPop; the `EventThread` cross-process wake
word also depends on this path. Removing it is therefore a delivery-semantics
and ABI change, not unused-code cleanup.

## Alternatives

### A. Unify on ShmSpmcRing

Rejected. SPMC `Push` overwrites the oldest slot when full and a slow consumer
loses frames, turning queue's "fail explicitly when full" into silent overwrite.
Multi-reader cursors, seqlock read retries, and drop statistics cannot provide
equivalent guarantees for a single-reader queue. `lossless` and the explicit GAP
semantics required by triggering/recording would not survive the replacement.

### B. Unify on LatestSlot or a shared-pool latest index ring

Rejected. Both are latest-wins and do not retain every queued event. They fit
state snapshots, not event streams that must be consumed in order.

**latest-only exception:** if an edge genuinely needs only the newest value, do
not simulate it with `SpscRing::push_latest` — the ring's `Cap` slots and
head/tail merely retain a bounded history. Use `LatestSlot<T>` for small
in-process values and the pinned latest store for large payloads. When only an
index is moved, the index itself may be published through a single latest slot,
but pool slots must be protected by pin/double-buffering/reference counting;
"one atomic index" must not hide a lifetime problem.

### C. Delete ShmSpscRing and keep only the same-process NotifPipe

Rejected. NotifPipe covers only same-process queues. MP events still need POSIX
SHM, cross-process wakeups, and layout validation. Turning MP queues into a
polling socket or another transport would add per-message copies/syscalls and a
second protocol, with no quantified benefit.

### D. Keep it (selected)

Keep `ShmSpscRing` as the cross-process implementation of `delivery: queue`.
Keeping it does not require a general ring abstraction; a future policy
template may extract a common implementation only if semantics stay unchanged
and equivalence tests and benchmarks pass.

## Cost and performance judgement

- Each payload currently has one by-value write and one consumer read, plus
  cacheline isolation, acquire/release publication, and futex wakeup. It is not
  a zero-copy/loan channel.
- Compared with SPMC, SPSC has no multi-reader cursor or seqlock payload retry
  cost. A single-consumer queue should not pay those costs merely for type
  unification.
- With no waiters, a shared `epoch_waiters` word skips the futex syscall; with
  waiters, the wake is a delivery hint and the queue still holds the data.
  Existing tests cover wakeup, backpressure, Begin/Commit, cross-process
  attach, and probing.
- No same-load A/B data proves that deleting SPSC improves steady-state
  performance. Type count or speculation cannot replace performance evidence;
  a future replacement proposal must provide semantic-equivalence tests and
  throughput/latency/CPU data, not just compare one `Push` instruction count.

## Decision and consequences

1. Keep `ShmSpscRing`, `delivery: queue`, and the existing v3 SHM ABI. Do not
   enter deprecation or add a deprecated marker.
2. SPMC, `LatestSlot`, and pinned-slot leases continue to serve broadcast,
   latest state, and large-frame lifetime respectively; none impersonates a
   general substitute for another.
3. A future ring-template evaluation may continue, but queue's full-ring
   failure, ordering, `push_fail`, wakeup, replay/backpressure, and probe ABI
   must first be locked by equivalence tests; do not change it to drop-oldest
   for template consistency.
4. Existing evidence: `test_shm_spsc_ring`, `test_shm_spsc_begin_commit`,
   `test_primitive_properties_shm`, `test_queue_push_fail_credit`,
   `test_event_wake_word`, `test_wake_coalesce`, `test_event_thread`,
   `test_gen_graph_event`, and `test_record_mirror_ring_mp` all pass.

## Necessary conditions for reopening deprecation

Only when all hold:

- no deployed graph uses MP `delivery: queue`, or all have migrated to a
  semantically equivalent transport; absence of an in-repo example is not
  enough;
- the replacement proves compatible ordered/lossless delivery, explicit
  full-ring backpressure, cross-process wakeup, replay, and probe ABI, with
  cross-process regression tests;
- same-machine, same-load A/B shows a material benefit in at least one of
  throughput, tail latency, or CPU, without buying metrics with frame loss or
  weaker backpressure;
- after one release cycle of deprecation warnings and downstream migration,
  delete the type and ABI.
