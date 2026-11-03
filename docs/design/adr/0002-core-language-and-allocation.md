# ADR 0002: core language subset and allocation policy

- Status: Accepted (partially implemented; paired with the allocation and test work)
- Date: 2026-10-23
- Scope: `thyme_runtime`, `thyme_recorder`, `thyme_app`, `thyme_replay`

## Context

The core libraries run on real-time threads and recording threads. Exceptions,
RTTI, and heap allocation on the hot path cause:

- unpredictable latency and page faults;
- larger code size from exception tables and type information;
- unclear behaviour on allocation failure (throw, abort, or drop a frame);
- inconsistency with embedded builds using `-fno-exceptions -fno-rtti`.

## Decision

1. **Core language subset**
   - `thyme_runtime` / `thyme_recorder` / `thyme_app` / `thyme_replay` are built
     with `-fno-exceptions -fno-rtti`.
   - Do not write `throw` / `try` / `catch` / `dynamic_cast` / `typeid`.
   - Tools, tests, and optional UI code may keep exceptions, but must not leak
     exception types or RTTI dependencies into core public headers.
   - CMake switch: `THYME_CORE_EMBEDDED` (default ON). `-fno-exceptions` is
     applied only to the core; `-fno-rtti` propagates to linking targets through
     `PUBLIC`, because subclassing a core polymorphic type must be consistent.

2. **Allocation policy**
   - After `App::Prepare()` / `Start()`, cyclic/event/tap/record hot paths
     perform no C++ heap allocation.
   - Use preallocated arenas, `ObjectPool`, and fixed-capacity rings. Do not
     call `new` / `malloc` in callbacks or let `std::vector` grow.
   - Per-message allocation on recording/trigger threads is eliminated;
     `SwapDescriptor` queues use fixed-capacity rings.
   - Tools may allocate, but must not leak those allocations into core paths.

3. **Allocation-failure behaviour**
   - Do not throw; degrade explicitly per scenario:
     - sampling/transport: drop the frame and count it;
     - recording: write a GAP or stop the capture;
     - unrecoverable at startup: return failure/exit code.
   - Do not "throw it to the caller" or silently `abort` (except assertion
     failures for programming errors).

4. **Enforcement and evidence**
   - Rule: `docs/reference/coding-rules.md` CR-01.
   - Build: `THYME_CORE_EMBEDDED=ON`.
   - Tests: `test_core_no_alloc` covers cyclic/tap/event paths after `Start`;
     recording/trigger threads are covered by the recording allocation tests.
   - CI: sanitizers + `-Werror` (fixed image) + allocation sentinels.

## Consequences

- Core code cannot use exception-dependent library facilities; error paths use
  return codes, `ReadResult`, and counters.
- Downstream code that subclasses core polymorphic classes must also use
  `-fno-rtti`; otherwise the link fails on the missing base typeinfo.
- `test_core_no_alloc` counts only C++ `operator new`, not `malloc` inside
  libc; this is a known boundary.
