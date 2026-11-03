# ADR 0012: PinnedSlot lease-copy write exclusion and crash recovery

- Status: Accepted
- Date: 2026-10-26
- Scope: lease-backed `View::CopyPayloadTo`; does not change the default
  zero-copy view, non-lease seqlock copies, or recorder lease lifetime

## Context

Segmented atomic reads and writes eliminate the C++ data race of "ordinary
payload access during the seqlock re-check". A large-frame lease already pins
the slot, but the writer can still steal it after the lease expires, so two
apparently convenient optimizations do not hold:

- plain `lease + memcpy + release`: a steal can happen during `memcpy`;
- refresh the epoch and then `memcpy`: a timeout only postpones the steal; it
  does not exclude overwrite after the thread is preempted.

`SourceIntact()` re-checking can reject a torn result but cannot undo a data
race that already happened. Therefore lease-backed copy cannot simply be
replaced with plain `memcpy` for throughput reasons.

## Decision

1. **Default path unchanged.** A PinnedSlot lease view stays zero-copy; the copy
   protocol is entered only when the caller explicitly asks to materialize the
   payload.
2. **copy-active and steal arbitrate on the same `pin_gen` atomic word.** A copy
   registers active only when the generation matches, the pin is valid, and
   active is unset; the writer may steal only while active is unset. Whichever
   CAS succeeds decides the order; the other side skips or falls back.
3. **After a successful active registration, use plain `memcpy`.** On
   completion, clear active and re-check logical validity. When there is no
   lease, the owner protocol is unavailable, or another copy is active, keep the
   atomic copy. An invalid lease must not fall back to copying.
4. **Other pin state transitions must preserve active.** Acquire/release on
   other leases must not accidentally clear the copy exclusion via a pin ± 1
   operation.
5. **Never steal active by timeout.** Even if an active copier is preempted for
   a long time, the writer may only pinned-skip. Reclaim is allowed only after
   the owner's process identity is proven dead, avoiding both a permanent pin
   after a crash and treating a live copier as a corpse.
6. **Owner identity uses pid plus `/proc/<pid>/stat` starttime.** The owner
   publishes before active; the reader caches its identity on the slow Open
   path. The writer checks the owner only when active is set and the epoch has
   expired, so steady-state copy adds no `/proc` overhead.
7. **Protocol upgrade.** The cell stays 64 B and existing field offsets do not
   move; reserved space carries the copy owner. Because an old writer does not
   understand active, the lease ABI must be bumped and mixed versions rejected.

## Invariants

- When active is observed, the owner pid/starttime has been fully published.
- While active is set, acquire/release on other leases does not change active,
  and the writer must not return a writable payload for that slot.
- Active of a live owner is never force-stolen by timeout; a dead owner's active
  may be reclaimed, and the generation must increment so that an old lease's
  release cannot decrement the new pin.
- After copy-end clears active, an ordinary expired lease is still reclaimed by
  the normal rules; transitions do not underflow or leave an owner behind.
- The atomic fallback covers data-plane correctness only; it does not promise
  plain-`memcpy` throughput.

## Consequences

- Lease-copy can achieve near-local `memcpy` copy performance while preserving
  default zero-copy and non-lease correctness.
- A slot has at most one plain-`memcpy` copier at a time; concurrent copies use
  the atomic fallback. This is a deliberate complexity boundary: no active
  counter and no more complex owner set.
- An active but wedged copier keeps causing pinned-skip; the system chooses
  visible frame loss over a data race. Process death is handled by owner-death
  recovery.
- The ABI bump requires rebuilding all PinnedSlot producers/consumers together;
  upgrading one side only is not supported.

## Acceptance

- Deterministic tests for copy-first, writer-first, stale generation, concurrent
  copy, other lease acquire/release during active, expired active with a live
  owner, and crashed owner with active; no tearing, data race, or pin/owner leak
  under default and sanitizer builds.
- Local `memcpy`, atomic copy, and lease-copy A/B on 240 KiB/1 MiB/19 MiB;
  copy-body target is at least 90% of `memcpy`.
- Joint reporting of acquire+copy+release pin duration with `pinned_skip`, lease
  steal, and copy failure/frame loss; throughput alone must not decide pass/fail.
- Short x86 tests prove only the local path; target hardware is re-tested
  separately.
