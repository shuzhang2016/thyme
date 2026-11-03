# thyme coding rules

> Applies to the four core libraries: `thyme_runtime`, `thyme_recorder`,
> `thyme_app`, and `thyme_replay`. Tools, tests, and downstream repositories may
> relax these rules as needed.

## CR-01: core libraries use the embedded language subset

Core libraries **must** satisfy:

- **No exceptions:** do not write `throw`/`try`/`catch`, and do not depend on
  library paths that throw; compile with `-fno-exceptions`.
- **No RTTI:** do not write `dynamic_cast`/`typeid`; compile with `-fno-rtti`.
- **No dynamic allocation after `Prepare()`/`Start()`:** hot paths use
  preallocated arenas, `ObjectPool`, and fixed-capacity rings. Do not call
  `new`/`malloc` or grow a `std::vector` in cyclic/event/tap/record callbacks.
- **Out-of-memory behaviour is explicit:** drop the frame, write a GAP, stop
  the capture, or return an exit code. "Throw it to the caller" is forbidden.
- **Error paths do not throw:** use return codes, `ReadResult`, counters, and
  logs; error messages are handled uniformly at the boundary.

CMake switches:

```bash
# Default ON: core libraries get -fno-exceptions -fno-rtti
cmake -S . -B build

# Disable only for local debugging or an explicit toolchain exception
cmake -S . -B build -DTHYME_CORE_EMBEDDED=OFF
```

Notes:

- CMake can enforce only `-fno-exceptions`/`-fno-rtti`. **"No dynamic
  allocation" is enforced by review plus an allocation-sentinel test**
  (`test_core_no_alloc`), not by a compiler switch.
- `-fno-exceptions` is applied only to the four core libraries; tools and tests
  keep exceptions.
- `-fno-rtti` propagates through the core target's `PUBLIC` option to linking
  targets. Applications that subclass core polymorphic types such as
  `CyclicThread`/`EventThread` must use the same RTTI setting; otherwise the
  link fails on the missing base typeinfo.
- Tools and tests may keep exceptions. A genuine RTTI requirement must be
  evaluated separately and must not break the core ABI.

## CR-02: publish paths use the in-place API

Core publish primitives provide a three-step in-place sequence:
`BeginWrite()` acquires a slot, the payload is filled in place, and
`CommitWrite()` publishes it. `AbortWrite()` explicitly discards an uncommitted
write. This covers `ShmSpscRing`, `ShmSpmcRing`, `LatestSlot`, `NotifPipe`, and
the pinned-slot writer (plus `EventPipeRef` forwarding in generated code).
First-party slot filling uses `WriteSection<Container>` (construction = Begin,
`commit()` publishes, destruction aborts automatically); do not hand-write
Abort. `BeginWrite`/`CommitWrite`/`AbortWrite` remain hook and test APIs. See
`write_section.h` for the four abort semantics.

Rules:

- **New code must use the in-place path.** `Push(const T&)` / `Write(const T&)`
  remain only as thin compatibility wrappers (they run the same publication
  sequence internally) and are **documentation-level deprecated**. Do not add
  `[[deprecated]]`, to avoid flooding existing callers and tests with warnings.
- **Allowed wrapper exceptions:** copy paths that already materialize data,
  such as replay/injection (the source is itself a byte buffer, so the copy is
  unavoidable).
- A `BeginWrite` returning `nullptr` (full) is a failed publication: account for
  it under the drop policy and **do not spin-retry** (backpressure is the
  contract). `ShmSpmcRing`/`LatestSlot` are overwrite/latest-wins semantics, so
  `BeginWrite` does not fail.
- **Abort/half-write semantics:** after `BeginWrite` without `CommitWrite`, the
  slot stays invalid/unpublished to consumers; a reader never sees a
  half-written payload. `AbortWrite()` performs explicit cleanup (container
  specifics are in each header: SPSC has no state to roll back, SPMC sacrifices
  the overwritten old frame, `LatestSlot` only closes the seqlock range without
  flipping `published_idx`, and `NotifPipe` returns the slot to the free list).
- **Single producer:** two `BeginWrite/CommitWrite` pairs must not overlap on
  the same container.
- Generated code (gen_graph templates and scaffold) has migrated to the
  in-place path. After changing templates, regenerate the golden trees; never
  hand-edit `testdata/golden_*`.

## CR-03: no per-message logging on the hot path

Inside `Loop()`, event `Dispatch()`, tap/record code, and per-frame/per-message
callbacks, **do not** unconditionally call `fprintf`/`printf`/`std::cerr`/
`std::cout`, and do not format a log line per message (even through a unified
logger).

Rules:

- **Counters first:** observability goes through counters (perf type 5/6,
  `CenterStats`, `Stats`, status endpoints). Keep only the small set of log
  lines that show a human the first occurrence.
- When the hot path genuinely needs a log, use `THYME_LOG_*_RL`
  (`include/thyme/runtime/log.h`): the first three calls at each site are
  always emitted, then one per 1000 with `suppressed=N`. Use the explicit
  `thyme::log::kOnceThenEvery1000` at sites that should report only once.
- Logger constraints on the hot path: fixed stack buffers (`kMessageMax` =
  384 B, `kFieldsMax` = 256 B, `kRecordMax` = 832 B), no exceptions, no RTTI,
  and **no allocation**. One record is one `write()`; the file sink uses the
  rotating file implementation. Logger zero-allocation is covered by
  `test_log`'s operator-new sentinel; core hot paths remain covered by
  `test_core_no_alloc`.
- Startup and error paths use the macros without `_RL`; keep messages in
  `key=value` style so `grep 'trigger_id=7'` works.
- **Never call the logger from a signal handler** (mutexes, `vsnprintf`, and
  rotating `rename` are not async-signal-safe). Use `PerfLogDumper`'s periodic
  append for crash forensics.
- **Never call the logger in a child process after fork in a multi-threaded
  process** (the sink lock may be held by a thread that no longer exists). Such
  paths keep raw `write(2)`/`fprintf` and explain why in a comment.

Review evidence: a new per-message log site must prove its rate limiting in a
test; changing the core logger requires `test_log` (mr_gate) and
`test_core_no_alloc`.

The unified error-path checklist (return codes, counters, fail-closed, no silent
drops) is in [`error-handling-audit.md`](error-handling-audit.md): the return
value of a call that can fail must not be cast to `(void)` without a comment,
and startup and security-boundary failures must fail closed.

## Review entry point

Merge-request review follows [review-checklist.md](review-checklist.md) item by
item: concurrency (seqlock/in-place/futex), ABI (version/layout assertions),
error paths (ENOSPC/`.incomplete`/FSM rollback), tests
(mr_gate/quarantine/fuzz/RUN_SERIAL), and language subset (no
exceptions/RTTI/hot-path allocation).
