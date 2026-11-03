# ADR 0008: swap ring / copy-on-trigger — the steady-state copy trade-off for medium structs

- Status: Accepted (as-built: keep copies in steady state; start the swap ring only if measurements cross the threshold; not implemented now)
- Date: 2026-10-24
- Scope: `RecordMirrorRingPage` tap copy, pinned-slot leases, copy-budget accounting; cost analysis from the record/trigger review

## Context

While armed (not triggered), producer taps continuously copy small-structure
samples into the record mirror ring. This is a steady-state copy, measured at
about **1.2 MB/s** (60 channels × 100 Hz × 200 B). Large frames already avoid
the extra copy: the pinned-slot store is itself a swap ring — the producer
writes in place and the recorder reads through a lease pin.

Question: should medium structs also become a swap ring / copy-on-trigger —
write only descriptors while armed, and harvest the `[t0-before, t0+after]`
window from the transport ring at trigger time? The benefit is eliminating the
steady-state copy. The costs are:

- transport ring depth must cover the pre-window, moving memory from the
  recorder's mirror into the producer's transport ring, and it **stays resident
  even when not recording**;
- trigger time has a window-copy **burst** competing with fault handling
  (crash/estop) for CPU and I/O;
- without a lease there can still be overwrite-to-hole; completeness requires a
  lease or a deeper window.

## Decision

1. **Keep copies in steady state.** Small structs use `RecordMirrorRingPage`
   tap copies (mirror arena budget `kRecordMirrorMaxArenaBytes = 2 MB`, part of
   the SHM layout ABI); large frames use pinned-slot leases (zero copy; the pin
   covers the whole fwrite/index window).
2. **Defer the swap ring, and do not start it early.** The threshold is explicit:
   **implement only when steady-state copying reaches ≥ 5% of one core or
   ≥ 50 MB/s; otherwise do not**. Current 1.2 MB/s is two orders of magnitude
   below the threshold. Acceptance requires equivalent behaviour plus a
   bandwidth/instruction-count report; if there is no benefit, close it.
3. **If it is implemented later, the shape is already decided** (avoid
   re-litigating):
   - `RecordMirrorRingPage` gains `BeginWrite(struct_id, len)` /
     `CommitWrite(...)` (set seq odd, write in place, on commit bump seq/head),
     matching the in-place three-step API used elsewhere;
   - the tap writes only descriptors; at trigger time the window is scanned, and
     drain re-checks `(index, seq)`; overwrite becomes a GAP (consistent with
     current hole semantics);
   - size routing: `< ~1 KB` keeps copies (negligible cost, zero trigger
     latency); `~1 KB–512 KB` uses a swap ring plus copy-on-trigger (zero
     steady-state copy, one burst at trigger); `> 512 KB` uses the pinned-slot
     store (already a swap ring).

## Alternatives

- **A. Adopt the swap ring now:** pays resident memory (pre-window depth per
  ring) plus trigger bursts competing with fault handling plus lease/window
  completeness complexity for a 1.2 MB/s steady-state saving. The benefit and
  cost are disproportionate, and it saves only one copy (the payload still has
  to be written into a shared ring). Rejected by the threshold rule.
- **B. Multi-ring physical buffer swap (whole-ring ping-pong):** turns a time
  filtering problem into an ownership-reclamation problem — doubles memory,
  complicates crash recovery, and still saves only one `memcpy`. Explicitly not
  recommended; time filtering via `mono_ns` is retained for multi-ring
  consistency.
- **C. Route small structs through the leased large-frame channel:** a 200 B
  sample would pay the fixed cost of a 64 B header, 64 B footer, and lease cell;
  ring memory and management cost far exceed the copy itself. Rejected.

## Consequences

- **Benefits:** steady-state behaviour is simple and budgetable (tap copy about
  1.2 MB/s, negligible); pre-window memory is capped by the mirror arena's fixed
  ABI budget; the trigger path has no extra burst, so crash-level capture drain
  bandwidth is not squeezed by harvest copies.
- **Costs (explicitly accepted):**
  - steady state is not zero-copy: small/medium structs always pay one `memcpy`
    while armed. This cost is tracked in the copy-budget ledger; once
    measurements cross the threshold (≥ 5% of one core or ≥ 50 MB/s), start the
    swap-ring work;
  - the swap ring's upside is inherently small (one copy saved), so future
    workloads (more channels, higher rates) may overturn this decision. The
    overturning evidence must be measured numbers, not architectural taste.
