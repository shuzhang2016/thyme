# ADR 0001: seqlock payload memory model

- Status: Proposed (implementation guide; the code has not been converted yet)
- Date: 2026-10-23
- Scope: payload reads and writes in `LatestSlot`, the pinned-slot store, and `RecordMirrorRingPage`

## Context

These containers use an atomic `seq` variable as a seqlock: the writer first
sets an odd/0 value, writes the payload, and finally release-stores `seq`; the
reader acquire-loads `seq`, reads the payload, and re-checks `seq`.

The problem: `seq` is atomic, but the payload is ordinary byte access. Under
the C++ memory model, if the writer and reader access the same payload byte
concurrently, it is a data race and undefined behavior even when the reader
would discard the result after seeing `seq` change. It currently works on
x86/ARM, and TSan is silenced with `AnnotateIgnoreReadsBegin`, but that is not a
correctness proof.

The lease in the pinned-slot store excludes "writer writes after a successful
pin", but it does not exclude:

- the writer starting to write before the reader pins;
- immediate views without a lease such as `TryRead`/`ReadFrame`;
- the "copy first, re-check afterwards" path in `ReadFrameStable`;
- tap reads from `RecordMirrorRingPage`.

## Decision

Use a **layered strategy** rather than pursuing "every path zero-copy and
lock-free":

1. **Paths without a lease or that must survive across calls** (`ReadFrameStable`,
   consumers of `TryRead`, probe/replay tools):
   - copy the payload with **byte-wise `std::atomic_ref`** (or an equivalent,
     standards-permitted atomic byte access);
   - re-check `seq`/footer after copying; discard on mismatch;
   - stop using `memcpy` to read a payload that may be written concurrently.
2. **Lease paths** (`Acquire` succeeded and the writer has observed the pin):
   - the writer does not write the slot after a successful pin, so ordinary
     reads within the lease are not a data race;
   - the **validation phase before the pin** still follows rule 1, or reads only
     header/footer and not the payload;
   - if `AcquireLease` CRC validation reads the payload, it must also use atomic
     byte access, or read only after the pin confirms the writer will not write.
3. **Writer paths**:
   - payload writes may be ordinary writes followed by a release-store of
     `seq`; atomic byte reads on the reader side pair with release/acquire and
     avoid undefined behavior;
   - if `atomic_ref` write cost is unacceptable, the writer may keep ordinary
     writes, but the reader must use atomic byte reads; both sides cannot be
     ordinary.
4. **TSan annotations**:
   - allowed only inside a known and proven lease-protected region;
   - the ADR or comment must state the annotation's lifetime and boundary;
     global blanket ignores are forbidden.

## Alternatives

- **A. Keep ordinary reads/writes plus annotations:** best performance, but it
  does not meet the "no undefined behavior" requirement; transitional only.
- **B. Copy out on every path:** easy to make correct, but adds one copy for a
  19 MB large frame, which point-cloud consumers do not accept.
- **C. Double buffering or an atomic payload array:** correct, but doubles
  memory and the reader still has to handle overwrite.
- **D. The layered approach here:** atomic byte copy for non-lease paths,
  copy-free after pin for lease paths; requires benchmarking the atomic-copy
  cost.

## Implementation and acceptance

1. Add small `AtomicByteCopy`/`AtomicByteRead` helpers (header-only,
   `std::atomic_ref<std::uint8_t>`), starting with `ReadFrameStable`.
2. Switch the stable-copy paths of `LatestSlot`/`RecordMirrorRing` to those
   helpers; either document that `TryRead` views are valid only for immediate
   consumption or convert them to stable copies.
3. `PinnedSlotReader::AcquireLease`: header/footer validation does not read the
   payload; CRC validation moves after the pin or uses atomic byte reads.
4. Benchmark atomic copy versus `memcpy` for a 19 MB frame and record
   throughput/instruction counts. If atomic copy exceeds budget, fall back to a
   restricted API: leases only, with copy-out for non-lease paths.
5. Tests: TSan/ASan/UBSan green; ARM litmus/stress tests;
   `test_seqlock_payload_model`.
6. After completion, remove or narrow `AnnotateIgnoreReadsBegin`.

## Consequences

- Non-lease paths may become slower; evaluate against the copy budget.
- Lease paths should not slow down. If `AcquireLease` CRC needs atomic reads,
  pin duration increases and must be included in the budget.
- This is an API/implementation change; it does not change the wire format.
