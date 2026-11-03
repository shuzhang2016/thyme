# Merge-request review checklist

> Scope: `thyme_runtime` / `thyme_recorder` / `thyme_app` / `thyme_replay` and
> changes that touch them. Check the sections relevant to the change; every item
> needs a one-line verdict and a pointer to the enforcing mechanism (test,
> document, or compile-time assertion), so a review takes minutes. Rules are in
> [coding-rules.md](coding-rules.md).

## 1. Concurrency

| # | Check | Enforcing mechanism / evidence |
|---|---|---|
| C1 | Concurrent seqlock payload reads/writes go only through `AtomicPayloadCopyOut/CopyIn` (relaxed atomic word copies); never use bare `memcpy` on a payload the writer may rewrite concurrently; correctness comes from paired release/acquire on `seq` | [adr/0001](../design/adr/0001-seqlock-payload-memory-model.md); `include/thyme/runtime/detail/atomic_payload.h`; `test_seqlock_payload_model` (plus the nightly TSan variant) |
| C2 | No relaxed publication: publish sequence numbers/flags visible across threads must be release-store published and acquire-load consumed; `relaxed` is allowed only for pure counters and payload bytes justified by ADR 0001 | ADR 0001 decision 3; nightly TSan |
| C3 | No new TSan exemptions: do not add `AnnotateIgnoreReadsBegin` call sites (currently zero in the tree); any lease-region exemption must document its lifetime and boundary | ADR 0001 §4; `grep -rn AnnotateIgnoreReadsBegin include src` |
| C4 | New publish paths use the in-place three-step API; first-party slot filling uses `WriteSection` (destructor auto-aborts). `Push`/`Write` are documentation-level deprecated thin wrappers, exempt only for copy paths that already materialize bytes (replay/injection) | [coding-rules.md](coding-rules.md) CR-02; `test_write_section` / `test_shm_spsc_begin_commit` / `test_shm_spmc_begin_commit` / `test_latest_slot_begin_commit` |
| C5 | `BeginWrite` and `CommitWrite` are paired: failure branches use `AbortWrite()` or `WriteSection` destruction; a slot never committed is invisible to readers; two write pairs must not overlap on one container (single-producer discipline) | CR-02; `test_write_section`; `test_copy_in_equivalence` |
| C6 | `BeginWrite` returning `nullptr` (full) is a failed publish: account under the drop policy and **do not spin-retry**; `ShmSpmcRing`/`LatestSlot` overwrite and do not fail | CR-02 |
| C7 | Futex/wakeup accounting: at most one wake syscall per cross-process message; no syscall when there are no waiters; SPMC wakes registered readers without thundering herd; the shared `WakeHub` word always wakes all | `test_wake_coalesce` (ptrace syscall counting, mr_gate; excluded under ASan) |

## 2. ABI

| # | Check | Enforcing mechanism / evidence |
|---|---|---|
| A1 | Any SHM layout or wire-struct **layout or semantics** change bumps magic/version and rejects old peers; a semantic change bumps the version even when bytes do not move (see lease v3→v4) | [abi-compatibility.md](abi-compatibility.md) rules 1/2; `test_pinned_slot_lease_abi`, `test_lease_abi_v4` |
| A2 | New or changed wire/persistent structs need `static_assert(sizeof(...))` and key `offsetof` assertions | Existing patterns: `trigger_protocol.h` (all v1/v2/v3 sizes and offsets), `frame_header.h`, `WalRow == 24`, ring `Layout` offsets |
| A3 | `FrameHeader` stays 40 B; `reserved[0]` bit 0 = `kFrameFlagCrcDisabled` is the only defined flag and all other reserved bits must be 0; **do not infer no-CRC from `crc32==0`** (a real CRC may be 0) | ABI-12; `frame_header.h`; `test_frame_header` / `test_frame` (including negatives) |
| A4 | Trigger protocol changes preserve the v3 contract: request 104 B / response 32 B, CRC32 over all bytes before the `crc32` field; the server validates CRC **before** sequence/priority handling; a bad CRC is audited as `kMalformedRequest`, gets no response, and does not advance the replay window; v1/v2 default to `kUnsupportedVersion` | SEC-04 / TRG-07; [abi-compatibility.md](abi-compatibility.md) v2/v3 layout tables; `test_trigger_protocol` / `test_trigger_security` / `test_trigger_malformed` |
| A5 | New flags in `integrity.manifest` are ABI: v1 already uses bit 1 = partial, and old verifiers reject unknown flags; new bits must follow the same fail-closed design | SEC-03 / REC-18; [abi-compatibility.md](abi-compatibility.md) version table; `test_package_integrity` |
| A6 | Never mix versions: upgrade order is stop producers → upgrade all readers/writers → recreate SHM objects; an ABI-changing MR must state the deployment order | [abi-compatibility.md](abi-compatibility.md) rule 5 |

## 3. Error paths

| # | Check | Enforcing mechanism / evidence |
|---|---|---|
| E1 | ENOSPC is not disguised as a data hole: a drain write failure sets `disk_full_`, actively invalidates the package (abort row plus rename `.incomplete`), and blocks new capture; the host runs retention and calls `ClearDiskFull()` before retrying; a second non-crash failure returns `kQuotaExceeded`, while crash level uses the `high/` reserve | [runbook.md](../guides/runbook.md) §1; `src/record/triggered_recorder.cpp`; `kQuotaExceeded` |
| E2 | `.incomplete` policy is predictable: `[recorder] incomplete_policy = keep\|delete_after_age\|delete_immediately` (default keep); quota/retention scans never touch `.incomplete`, never delete active packages, never follow symlinks, and cap recursion at depth 8 | REC-10; `include/thyme/record/retention.h`; `test_retention_symlink` (mr_gate) |
| E3 | A failed seal means `.incomplete`: failure of seal, data fsync, directory fsync, or `integrity.manifest` must never produce a self-sealed complete package; aborted/crashed packages have no manifest and validators report `absent` | SEC-03; `test_package_integrity`; [runbook.md](../guides/runbook.md) §1 |
| E4 | Trigger FSM preempt/start failures roll back and count; no late error code after `Accepted`: a failed `preempt_capture` rolls back to the pre-enqueue snapshot and increments `preempt_failures`; a failed `start_capture` increments `start_failures`; delivery is determined by package directory/status query | `src/record/trigger_center.cpp` `StartPendingTrigger`; `test_trigger_preempt_fail` / `test_trigger_busy` |
| E5 | Nothing is silently swallowed: every rejection/drop/tear path has a counter or GAP accounting (`drop`, `pinned_skip`, `malformed_datagrams`, `notify_outcome_drops`, `torn_reads`, …); when adding a `return` or ignore branch, ask "where is the counter?" | ARCH-05 (unified error handling, no silent drops); `test_trigger_malformed` / `test_trigger_overflow` assert exactly +1 per failure |
| E6 | A lease-path write failure is fatal; never downgrade to GAP or retry silently. The lease pin covers the whole write sequence, so the copy/post-check fallback is gone | `test_pinned_slot_drain_tear` / `test_continuous_lease_tear` |

## 4. Tests

| # | Check | Enforcing mechanism / evidence |
|---|---|---|
| T1 | `mr_gate` membership: only fast, deterministic tests enter `mr_gate`; new tests do not join by default until review confirms runtime and determinism | `CMakeLists.txt` M7.2 comment; [test-matrix.md](../guides/test-matrix.md) |
| T2 | Fix flakes before quarantining: make scheduling-sensitive tests condition-driven (poll a condition with a bounded deadline), not sleep/fixed-window based; quarantine is a last resort with all five fields, `expires` ≤ 2 weeks, and expiry hard-fails to force review | CI-07; [test-matrix.md](../guides/test-matrix.md) §Quarantine; `tools/ci/quarantine.json` + `tools/ci/apply_quarantine.py` |
| T3 | Handshake instead of timing: wait for thread/process readiness with atomic flags, not sleeps (pattern: `reader_ready` release-store, writer acquire-spins, then waits for `reads>0` before racing) | `test_latest_slot_begin_commit` |
| T4 | Fuzz fast/full split: mr_gate runs the deterministic fast tier (2k–5k iterations, <1 s), nightly `fuzz` label runs full (100k–200k); a new parser needs a fuzz driver, seed corpus, and negative assertions | `CMakeLists.txt` fuzz registration; `tests/fuzz_harness.h`, `tests/fuzz_corpus/` |
| T5 | `RUN_SERIAL` rules: nested-build tests (gen_graph fixtures) and machine-resource contenders (`bench_core`, `test_record_mirror_ring_mp`, `test_event_thread`, `test_empty_app`) must set `RUN_SERIAL TRUE`; copy the pattern for new equivalents | `CMakeLists.txt` |
| T6 | Every bug fix has a regression case; parser changes have negative corpora | `test_trigger_config` / `test_trigger_config_cli` |

## 5. Embedded language subset

| # | Check | Enforcing mechanism / evidence |
|---|---|---|
| L1 | Core libraries have no exceptions/RTTI: no `throw`/`try`/`catch`/`dynamic_cast`/`typeid`; `THYME_CORE_EMBEDDED` (default ON) adds `-fno-exceptions`/`-fno-rtti` to the four core libraries and propagates `-fno-rtti` as `PUBLIC`; targets subclassing core polymorphic types must use the same setting | [coding-rules.md](coding-rules.md) CR-01; [adr/0002](../design/adr/0002-core-language-and-allocation.md); CI `test-werror` |
| L2 | No heap allocation after `Prepare()`/`Start()`: no `new`/`malloc`/`std::vector` growth in cyclic/event/tap/record callbacks; use preallocated arenas, `ObjectPool`, and fixed-capacity rings | CR-01; `test_core_no_alloc` (allocation sentinel overriding `operator new/delete` and asserting zero) |
| L3 | Recording/trigger threads use fixed rings: `queue_`/`cont_queue_` are preallocated `DescriptorRing`s that evict oldest and count `pending_evicted`/`queue_evicted`; no per-message allocation | `test_triggered_recorder` (capacity contract, eviction counters, zero allocation) |
| L4 | Error paths do not throw: return codes / `ReadResult` / counters / logs; out-of-memory behaviour is explicit (drop, GAP, stop capture, exit code); no "throw to the caller" or silent `abort` | CR-01; ADR 0002 §3 |

## 6. Documentation / API

| # | Check | Enforcing mechanism / evidence |
|---|---|---|
| D1 | New or changed public API (`include/thyme/**`) has `/** \brief */`; handle-returning types (pool slot, ring view, `PinnedSlot` lease, SHM region, fd) state ownership and validity (until `release()`/`Reset()`, or "borrowed, do not delete") | [api-docs.md](../design/api-docs.md); review |
| D2 | Generated artifacts (`generated/`) are never hand-edited; version/format drift is caught by `gen_graph --check` (`generated/.gen_meta.json`); migration = full regeneration | [tools/gen/gen_graph/README.md](../../tools/gen/gen_graph/README.md) §Versioning; `test_gen_graph_version` |
