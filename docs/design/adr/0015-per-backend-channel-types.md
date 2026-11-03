# ADR 0015: split Channel types per backend, binding the compile surface to the backends actually used

- Status: Withdrawn (implementation showed insufficient type-constraint benefit; kept as a decision record)
- Related: unified channel facade, slot/endpoint consolidation, this item, generator convergence, ADR 0014 (rings do not unify delivery semantics)

## Context

`Channel<T, Semantics, Backend, Cap, …>` is a single template carrying six
delivery shapes (local latest, local queue, cross-process queue, cross-process
broadcast, local pin, cross-process PinnedSlot). Any translation unit that names
`Channel` must see every backend header.

Measured single-TU preprocessed line counts (2026-10-30):

| Include | Lines |
|---|---|
| `channel.h` | 90,961 |
| All rings + contracts + capability (excluding PinnedSlot) | 87,554 |
| `pinned_slot_store.h` alone | 70,193 |
| `shm_spmc_ring.h` / `shm_spsc_ring.h` / `latest_slot.h` | 46,181 / 44,854 / 43,236 |
| `notif_pipe.h` | 12,363 |

Splitting out the PinnedSlot header saves only 3.7%: the weight is in the ring
headers themselves, and they include each other. The generator already knows
each edge's transport/retention/readers/backend at generation time, but generated
Ports still wrote only `Channel`, so they could not include just the used
backend.

## Decision

1. `Channel` takes a **backend policy type**: `Channel<T, Semantics, Policy>`.
   Each policy lives in its own header (`channel_local_latest.h`,
   `channel_local_queue.h`, `channel_shm_queue.h`,
   `channel_shm_broadcast.h`, `channel_pinned.h`, `channel_pinned_store.h`) and
   implements that backend's bind/begin/commit/abort/read/empty/notify/wake/
   counters.
2. The generator selects the concrete policy from the edge's capabilities and
   emits `#include`s for **the set of backends the whole app uses**. Ports
   members and Ref template parameters use concrete types, not generic
   `Channel`.
3. `channel.h` remains a compatibility umbrella, aliasing the old seven-parameter
   form to "choose a policy by `Semantics` + `Backend`", so hand-written tests
   and tools do not change. New generated code does not use the umbrella.
4. Runtime type erasure and vtables remain forbidden: policies are selected at
   compile time and all hot paths are inlined.

## Consequences

- An app's compile surface scales with the backends it uses instead of always
  covering every backend; a small local-queue-only node was expected to drop
  from 90.9k to ~12k lines. The reduction for mixed apps depends on actual edge
  types (the latest header alone is 43k), so no uniform ratio can be promised.
- Each policy implements only its own semantics; policies do not share delivery
  logic, consistent with ADR 0014.
- Wire/ABI is unchanged; only generated type names and include sets change, so
  the generator version is bumped and the whole tree regenerated.
- `FrameChannelRef`/`PinnedSlotRef` `Local`/`Shm` parameters become two concrete
  channel types; binding overloads for endpoint pointers can depend on forward
  declarations, avoiding full definitions in signatures.

## Rejected alternatives

- **Split `channel.h` per file** (`channel_types.h`/`bigframe_types.h`/`.cpp`):
  measured only 3.7% saving because `Channel` still sees every backend.
- **Unify into one Ring template:** rejected by ADR 0014; queue/broadcast/
  latest/lease failure semantics differ.
- **Runtime type erasure/vtable:** violates the earlier decision and adds
  indirect calls to hot paths.

## Implementation finding (withdrawal reason)

After changing `ChannelSample` to a "policy-owned borrowed handle" as step 1
requires, the `pinned_slot` fixture immediately failed to compile:

- generated `DualChannelRef<Local, Shm>` holds both a local channel and a
  cross-process channel of the same family, but `TryRead(Sample*)` can accept
  only one `Sample` type;
- parameterizing `Sample` by backend made `Local::Sample`
  (`PinnedRing::Borrowed`) and `Shm::Sample`
  (`PinnedSlotReader::BorrowedFrame`) different, so the Ref could not compile.

To work around that, `Sample` would have to be parameterized by **family**, with
a hand-written two-arm borrowed union for the pinned family (local pin /
cross-process lease). That is possible, but the benefit calculation is:

| App shape | Current (approx.) | Split (approx.) | Reduction |
|---|---|---|---|
| Local queue only | 90.9k | 12k | −87% |
| Local latest + queue | 90.9k | ~45k | −50% |
| Plus cross-process queue/broadcast | 90.9k | ~87k | −4% |
| With PinnedSlot | 90.9k | 90.9k | 0 |

The weight is in the ring headers themselves (spmc 46k / spsc 45k / latest
43k), and cross-process apps necessarily include all of them; real systems are
almost always multi-backend. **Only single-family apps see a large benefit;
multi-backend apps gain a few percent**, which does not justify duplicating
`Sample` and channel types per family.

## Conclusion

This ADR is withdrawn; per-backend splitting is not implemented. If compile time
becomes a bottleneck, first trim transitive includes in the ring headers
(measuring what each header actually needs) rather than duplicating channel
types per backend; that step does not change the type model and carries much
less risk.

## Validation (planned, not executed)

- Preprocessed line counts and `-fsyntax-only` timing for representative TUs
  (local queue only, local latest, cross-process queue, broadcast, PinnedSlot).
- Full CTest, generator regression, and golden byte comparison; `--check` with
  zero drift.
- Behavioural equivalence: the same graph produces the same data through the
  umbrella and the new form; `bench_channel_facade` and
  `bench_pinned_slot_copy` do not regress.
