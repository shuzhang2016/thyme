# ObjectPool generation wrap, release ABA, and free-list evaluation (RT-04)

Scope: `include/thyme/runtime/object_pool.h`.

## Placement and ABI status

`ObjectPool<T, N>` is process-local. Its slots are heap-allocated
(`slots_ = new Cell[N]`) and the object is only referenced by pointer from
generated graph code (`using FooPool = thyme::ObjectPool<Frame<Foo>, kPoolN>`);
it is not written into, or sized by, any SHM region and it has no row in
`docs/reference/abi-compatibility.md`.

Consequences:

- Adding the opt-in free-list storage (a template parameter, see below) does
  not change the SHM ABI.
- The default instantiation (`kFreeList == false`) keeps its previous layout:
  the storage is an empty private base, so the object only gains a base that
  the compiler elides (EBO).
- If a pool is ever embedded in an SHM struct, it must stay on the default
  instantiation and any layout change then needs an ABI version bump following
  the normal rules.

## State machine and generation width

Each slot moves FREE → WRITING (`acquire`) → READY (`publish`) → FREE
(`release`); `abort_write` returns WRITING → FREE without publishing.
`generation_[i]` is a `uint32_t` incremented exactly once per successful
`acquire`; an abort does **not** roll it back.

Width decision: 32 bits, no widening or value skipping. `release` compares the
full 32-bit generation (`g != gen`), so the unsigned wrap does not corrupt the
comparison and no wrap-unsafe ordering arithmetic exists.

Wrap math for one slot, continuously acquired and released:

| rate | 2^32 reuses | wall time (24/7) |
|---|---|---|
| 10 Hz | 4,294,967,296 | ≈ 13.6 years |
| 50 Hz | 4,294,967,296 | 85,899,345.92 s ≈ 994.2 days ≈ **2.72 years** |

At the design rate of 10–50 Hz and with the load spread over N slots, a
single slot reaches 2^32 reuses only after years of uninterrupted operation —
far beyond process lifetime, upgrade cadence, and SHM rebuild intervals. At
the boundary a stale release aliases exactly one generation again; this is the
only wrap consequence and is asserted/documented in
`tests/test_object_pool_gen_wrap.cpp`. A u64 generation would add 4 bytes per
slot and change layout for no practical gain; skipping 0 would only move the
alias, not remove ABA.

## ABA analysis for `release`

`release(idx, gen)` is safe under the documented single-owner contract for
three cases:

1. **Release after abandon.** If the owner never published
   (`state == WRITING`), the `READY → FREE` CAS fails. If it called
   `abort_write` (`state == FREE`, generation unchanged), the generation still
   matches but the same CAS fails because the observed state is not READY.
   Both paths are rejected (`RunAbaRejections` in the wrap test).

2. **Stale generation release.** A new owner increments the generation as
   part of `acquire`, before it can publish. A release carrying the previous
   owner's generation therefore fails the load-and-compare at the top of the
   loop. The only way it can match again is the full 2^32 wrap above. Tests:
   `test_object_pool_gen_wrap`, `test_object_pool_freelist`, and TST-04's
   `ObjectPoolSchedule` stale-generation steps.

3. **Concurrent acquire/release.** The state transitions are single atomic
   RMWs, so exactly one acquirer wins a FREE slot and a successful
   `READY → FREE` CAS establishes exclusive ownership for the free-list push.
   There is one theoretical window: two threads releasing the *same* slot
   concurrently (double release, outside the contract) can both pass the
   generation load. A's CAS frees the slot; a new owner acquires and
   publishes it (generation +1); B's CAS then succeeds using the earlier
   generation and frees a slot owned by someone else. Under the contract this
   cannot occur: for B's load to match, B must have run while the slot was
   still READY, i.e. B is a concurrent second releaser. A genuinely stale
   caller (its slot was already recycled) loads the incremented generation and
   is rejected.

   Coverage: `test_object_pool_concurrent` (both acquire variants, per-slot
   `in_use` marker counts double hand-outs, `bad_release` counts rejected
   releases), its TSan variant, and TST-04 `ObjectPoolConcurrent`. All exercise
   legal single-owner usage.

   Hardening option, deliberately not taken: pack state and generation into
   one 64-bit atomic and CAS both, which closes even the double-release
   interleaving. It refactors every transition and changes the per-slot
   layout, while double release remains a caller-contract violation. Revisit
   only if an API is added that legitimately tolerates concurrent release.

## Free-list option and evaluation

`ObjectPool<T, N, kFreeList = false>` adds an O(1) acquire path:

- A tagged Treiber stack of free indices: 64-bit head (`index` in the low 32
  bits, per-push tag in the high 32 bits) plus one `next_[N]` word per slot.
  The tag is bumped on every push, so a stale pop/push CAS observes a
  different head value; the tag itself wraps after 2^32 pushes, the same
  timescale as the generation wrap.
- `release` pushes after a successful `READY → FREE` CAS;
  `abort_write` pushes after a successful `WRITING → FREE` CAS; `acquire`
  pops, stores WRITING, and increments the generation. Empty means
  `kInvalidIndex`, exactly as before.
- Contract preserved: same states, same generation semantics, same
  no-double-hand-out invariant. Only the hand-out order differs (LIFO instead
  of cursor order), which was never part of the contract.
- Enabled cost: one extra CAS per release/abort and `4N + 8` bytes per pool.
  Disabled cost: zero (empty base, stock scan path unchanged).

Benchmark (`bench_object_pool`, N = 10,000 slots, one thread, Release build,
measured on the RT-04 host; timing is reported, never gated):

| scenario | scan | free-list |
|---|---|---|
| fragmented: N−1 slots held, one recyclable | 46,414–49,581 ns/acquire | 27.6–29.3 ns/acquire |
| dense: all slots free | 17.0–18.9 ns/acquire | 28.4–29.7 ns/acquire |

The fragmented case is the scan path's worst case: roughly N probes per
acquire, ~1,680–1,710× slower than the free-list. The dense case is the scan
path's best case (the next cursor position is free); there the free-list pays
an extra pop+push CAS pair, about +10 ns per acquire/release round trip.

**Decision: implement as opt-in, keep the default OFF.** The 10–50 Hz acquire
rate does not need O(1) worst-case latency, and the default path is faster in
the uncontended/dense case that dominates. Deployments that hold most of a
large pool busy and recycle a small subset can instantiate
`ObjectPool<T, N, true>`. This is an evaluation, not a production blocker.

## Tests and follow-ups

| test | gate | what |
|---|---|---|
| `test_object_pool_gen_wrap` | `unit;mr_gate` | 0xFFFFFFFF→0 boundary, stale-release rejection, abort across wrap, no double hand-out, both variants |
| `test_object_pool_freelist` | `unit;mr_gate` | hand-out set is exactly the index space, 4,000-step model schedule for both variants, double-free |
| `test_object_pool_concurrent` (+ `_tsan`) | - | both variants under concurrent acquire/release, double hand-out / bad release counters |
| `bench_object_pool` | not gated | numbers above |

Follow-ups: if double release ever becomes supported, use the single 64-bit
state+generation CAS described above (and treat it as a layout change); if a
pool is embedded in SHM, forbid `kFreeList == true` and version the layout.
