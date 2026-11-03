# ADR 0014: ring unification covers cross-cutting mechanism only, not delivery protocols

- Status: Accepted
- Date: 2026-10-26
- Scope: the `SwapRing<T>` and `Ring<Storage, Notify, OverflowPolicy>` unification proposals; does not change existing types, SHM ABI, or generated code

## Context

Early proposals treated PinnedSlot as "a SwapRing with larger `slot_bytes`" and
suggested unifying every ring behind storage, notification, and overflow-policy
parameters. The implementation has shown that the differences are not simple
template parameters:

| Type | Data and delivery protocol | Full/slow behavior | Key state and lifetime |
|---|---|---|---|
| In-process `SpscRing` | Index ring | drop-oldest | Pool generation and dropped index |
| `ShmSpscRing` | Cross-process value queue, ordered/lossless | Returns failure, explicit backpressure | head/tail, push_fail, epoch/waiter |
| `ShmSpmcRing` | Single-writer multi-reader broadcast | Overwrites oldest, lossy per reader | Per-reader cursor, seqlock, drop counters |
| `LatestSlot` | Latest snapshot | Overwrites | Two-slot seqlock, published index |
| Pinned-slot store | Large frames written directly, zero-copy reads | pinned-skip / gated / oversize | Separate lease page, pin generation, copy-active, owner-death recovery, `ProducerGate` |

PinnedSlot in particular became a lease protocol: the payload page is read-only
to readers, the lease page is independently writable, and plain `memcpy` is
allowed only inside the copy-active write-exclusion window. It is no longer "a
large-size instance of the same swap ring".

## Why no unified template

1. **Result semantics differ.** `false`, drop-oldest, overwrite, latest,
   pinned-skip, `kGated`, and `kOverrun` cannot be compressed into one
   `OverflowPolicy` without losing the distinctions callers need most.
2. **Hot paths differ.** Value copy, index publication, seqlock retry, lease
   zero-copy, and copy-active have different memory models. A unified template
   would put branches and checks back on every hot path or force some channels
   to accept weaker correctness constraints.
3. **ABI and tools differ.** Page magic/version/fields, probe access, replay
   gates, lease companion pages, and attach validation all differ. Sharing a
   layout/footer template would create a false compatibility promise.
4. **No benefit evidence.** No same-load A/B shows that a unified template
   reduces CPU, copies, or tail latency; fewer types is not itself a runtime
   benefit. The SPSC lossless-backpressure role cannot be deleted for
   unification.

## Decision

1. Do not implement `SwapRing<T>` or a general
   `Ring<Storage, Notify, OverflowPolicy>` delivery-protocol template, and do
   not reduce existing types to thin aliases.
2. Type = semantics: queue, broadcast, latest, and the pinned-slot lease evolve
   separately. Template parameters must not let one endpoint silently switch
   delivery discipline.
3. Share only **cross-cutting mechanism that carries no delivery policy**, such
   as byte-layout calculation, CRC, atomic copy, cacheline auditing, error and
   counter definitions, and test utilities. Sharing requires equivalence tests
   first.
4. If `ChannelBase` continues, it may centralize version headers, wakeups,
   gates, counters, and lifecycle only; it must not inherit a "unify all ring
   protocols" mandate. Slot semantics and lease/copy hot paths stay in the
   concrete types.
5. The naming migration may express these contract differences, but aliases
   must not be read as protocol equivalence or interchangeability.

## Consequences and re-evaluation conditions

- Accept a small amount of duplicate but semantically local implementation in
  exchange for auditability and hot-path determinism. This is not grounds for a
  fourth general abstraction.
- Current regression evidence: `test_spsc_ring`, `test_shm_spsc_ring`,
  `test_shm_spmc_ring`, `test_latest_slot`, `test_pinned_slot_channel`,
  `test_pinned_slot_lease_pin`, `test_pinned_slot_lease_copy`,
  `test_primitive_properties`, `test_primitive_properties_shm`, and
  `test_copy_in_equivalence` all pass.
- A **specific subset** may be re-evaluated only after equivalence tests cover
  every delivery/backpressure/replay/probe/lease semantic and same-machine A/B
  shows a material performance benefit. PinnedSlot lease and
  LatestSlot/SPMC are never in the same candidate subset.
