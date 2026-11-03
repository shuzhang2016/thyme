# Error handling audit: return codes / counters / logs, no silent drops

Scope: core paths in `include/thyme/` and `src/` (runtime primitives, record,
trigger center, replay, MP supervisor, status/control endpoints,
logging/rotation). The goal is to find paths where "a call fails and nothing
happens" and handle them under one rule:

> **A fallible operation must produce one of three outcomes: return an
> explicit status, increment a counter, or fail closed at a startup/security
> boundary. Logging is a supplement, not a substitute.**

Audit date: 2026-10-24. Line numbers refer to the files at the time of the
audit.

## 1. Method and scope

- Mechanical scan: **204** `(void)` forced discards, **67** statement-level
  `write/fsync/rename/unlink/close/chmod/fcntl/setsockopt` calls, **0** `catch`
  blocks (core builds with `-fno-exceptions`, see ADR 0002), **0** `throw`s.
- Manual triage: only **fallible** calls count as findings. `close()`,
  `(void)unused_param`, `RotateIfNeeded()` after `rotate()`, and the like are
  classified by semantics.
- Key paths covered: hot-path (`Loop`/`Dispatch`/tap) drops already have a
  counter regime; this batch focuses on the control plane, endpoints, startup,
  and sealing — the places where "nobody knows when it fails".
- Compatibility: every fix keeps observable behaviour unchanged (defaults,
  error codes, success paths); only pathological paths that previously
  continued silently now fail explicitly or count.

## 2. Fixed (17)

Totals: **17 fixed**, **1 deferred**, **10 categories accepted as
non-findings**. The common property of the fixes is "default behaviour
unchanged": success paths, error codes, and output formats are all the same;
only previously invisible failures become observable counts or explicit
failures.

| # | Location (after fix) | Original problem | Category | Fix |
|---|---|---|---|---|
| E1 | `src/app/status_server.cpp:484` | When `max_clients` was full the code called `close(fd)` directly: the connection was shed with no counter or log (the comment only said "shed load") | Missing counter | Added `rejected_clients()` (`status_server.h:163`), counting every shed; regression test `TestConnectionCapIsCounted` |
| E2 | `src/app/status_server.cpp:441-452` | An `accept()` failure other than `EINTR` (e.g. `EMFILE`) returned directly, so the status thread stopped silently | Counter + log | `accept_errors()` (`status_server.h:167`) + `THYME_LOG_WARN_RATED(kOnceThenEvery1000)` |
| E3 | `src/app/status_server.cpp:281` | A failed `chmod(0600)` was discarded, so the endpoint could be world-readable | Security boundary | fail-closed: `Start()` returns false and cleans up fd/path |
| E4 | `src/app/status_server.cpp:300-312` | Listener `O_NONBLOCK`/`FD_CLOEXEC` failures were discarded; a failed non-blocking setup would make the accept loop block | Mechanism contract | `O_NONBLOCK` failure → `Start()` fails; `CLOEXEC` failure → explicit WARN |
| E5 | `src/app/status_server.cpp:456-468` | Client fd `O_NONBLOCK`/`CLOEXEC` failures were discarded; a slow client could block the whole status thread | Explicit reject + counter | `O_NONBLOCK` failure → close that connection and WARN; `CLOEXEC` failure → WARN |
| E6 | `src/record/trigger_center.cpp:150-160` | A failed `SO_PASSCRED` was discarded; every later datagram would then be rejected for missing `SCM_CREDENTIALS`, yet startup still "succeeded" | Security boundary | fail-closed: `Init()` returns false (and records errno) |
| E7 | `src/record/trigger_center.cpp:180-200` | Control-socket `chmod(0600)` / `O_NONBLOCK` failures were discarded | Security boundary | fail-closed: close fd, unlink, return false |
| E8 | `src/record/trigger_center.cpp:1162` | A failed direct-reply `sendto` was ignored completely (on EAGAIN/ENOBUFS the client can only wait for its timeout) | Counter + log | `CenterStats::reply_send_failures` (`trigger_center.h:177`) + `THYME_LOG_WARN_RL` |
| E9 | `src/record/trigger_center.cpp:572` | A failed async outcome-notification `sendto` was not counted at all (only successes incremented `notify_delivered`) | Counter + log | `CenterStats::notify_send_failures` (`trigger_center.h:172`) + `THYME_LOG_WARN_RL` |
| E10 | `src/record/record_control.cpp:154-162` | A failed `SO_PASSCRED` on the recorder control socket was discarded | Security boundary | fail-closed: `Open()` returns false |
| E11 | `src/record/record_control.cpp:245-262` | On Linux `peer_uid == -1` (no credentials) was treated as "no check needed" → **fail-open** when the credential chain is broken | Security semantics | Reject when there are no credentials (count in `auth_rejects_` + `THYME_LOG_WARN_RL`); Apple keeps its original semantics, with a comment on the platform difference |
| E12 | `src/record/record_control.cpp:175-192` | `chmod` / `O_NONBLOCK` failures were discarded | Security boundary | fail-closed: close fd, unlink, return false |
| E13 | `src/app/mp_supervisor.cpp:256` | The `RegisterMpSessionIdentity()` return value was discarded with `(void)`; when registration failed, attach degraded to header-only validation with no trace at all | Explicit log | WARN, stating the fallback to header validation |
| E14 | `src/app/mp_supervisor.cpp:788` | The `status_.Publish(snap)` return value was discarded (publication is skipped while a reader holds it) | Counter | `status_publish_skips()` (`mp_supervisor.h:317`); resent on the next tick, semantics unchanged |
| E15 | `src/app/mp_supervisor.cpp:432,454` | On the abort path, `WaitChildTimeout()`'s `waitpid` error was discarded via `(void)st`, and the wait duration was hard-coded to 2 s | Counter + policy moved up | `waitpid == -1` → WARN; wait duration becomes `MpSupervisorConfig::shutdown_timeout_ns` (`mp_supervisor.h:105`, same batch as the mechanism/policy audit) |
| E16 | `src/runtime/rotating_file.cpp:179-192` | `unlink(oldest)` / `rename()` failures were discarded with `(void)`, yet **the budget was still decremented by the evicted bytes** — actual disk usage was underestimated | Counter + accounting correctness | Deduct the budget only on successful unlink/`ENOENT`; added `rotate_errors()` (`rotating_file.h:121`); on failure the accounting errs high (drops earlier, never exceeds the budget) |
| E17 | `src/record/triggered_recorder.cpp:49-60,955-968,1284,1599` | Failed `fwrite`/`fsync` on the abort line and failed `fsync` of the sealing directory were all discarded (the old comment explicitly said "logged nowhere by design") | Counter | `SyncDir`/`SyncParentDir` return bool; added `Stats::sync_errors` (`triggered_recorder.h:200`); the `kStatus` line prints `sync_errors=` (`tools/apps/trigger_center/trigger_center_main.cpp`) |

### Representative regression tests

`tests/test_status_server.cpp::TestConnectionCapIsCounted`:

- `max_clients = 1` while 4 connections are established concurrently;
- asserts `rejected_clients() >= 1` (before the fix this was always 0 and the
  connections were closed silently);
- also covers the existing contract that "the server stays usable after a
  connection is shed" (subsequent connections and stop).

`test_fault_matrix` continues to cover the `disk_errors` path;
`test_log_rotation` covers rotation generations and budget (the E16 accounting
change keeps the original assertions passing).

## 3. Previously deferred item closed

| # | Location | Original problem | Closure evidence |
|---|---|---|---|
| D1 | `src/record/thyme_bin.cpp` `LoadIndex()` | A failed `fseek` used to be left as `kAbsent`, so "no footer" and an I/O failure could not be distinguished | `IndexStatus::kIoError` was added; failures in locating the footer, querying the size, reading the footer, and locating the entries are now all reported explicitly; `HasIndex()` and seek still reject. `test_thyme_bin_index::TestIndexIoErrors` injects three `fseek` failure paths via linker wrap; old-file and bad-footer semantics are unchanged |

## 4. Re-checked as non-findings (recorded to avoid re-scanning next round)

| Location | Why it is not a silent drop |
|---|---|
| `src/record/record_schema.cpp:706,852,972,1087,1118,1136` `(void)j.Expect(...)` | Every call is preceded by a matching `Peek()` and the loop structure guarantees the character exists; failure branches are reported by the outer `Fail()`. This is consumption of an already-proven success |
| `src/runtime/log.cpp:78` `(void)clock_gettime(CLOCK_REALTIME)` | A real-time clock failure has no better recovery path; formatting degrades to a 1970 timestamp (a visible anomaly), and the logger must not recursively log errors |
| `src/runtime/shm_lifecycle.cpp:233,254` `(void)complete` | `Classify()` already folds `scan.complete` into its return value (`kStale` is only returned for a complete scan); `complete` is only a diagnostic parameter and does not affect the decision |
| `src/record/thyme_bin.cpp:152` `fcntl(O_NONBLOCK)` | A non-blocking flag on a regular file has no semantic effect; open failures already returned on the previous line |
| `src/record/package_integrity.cpp:102-107` `SyncDirFd` | The directory `fsync` is still best-effort (the manifest rename already fsyncs the data): there is no stats object to attach to. Upgrading it to fail-closed should happen together with the package state machine |
| `src/record/trigger_center.cpp:743`, `src/probe/page_reader.cpp:105`, `src/app/replay_mode.cpp:70` | Platform branch / unused parameter, not an error path |
| `src/runtime/rotating_file.cpp:134,231` `(void)RotateIfNeeded()` | Rotation is a soft cap; continuing to append after a failure is intentional, and accounting failures are already exposed by E16's `rotate_errors()` |
| `src/app/mp_supervisor.cpp:624,634` `(void)kill(...)` | `ESRCH` is the normal "already exited" race; the timeout path is already handled inside `WaitChildTimeout` |
| `close/unlink` in `src/app/status_server.cpp` Stop/Cleanup; `unlink` in `src/record/record_control.cpp`/`trigger_center.cpp` Close | Best-effort cleanup during process teardown; failure does not affect the runtime contract, and the next startup's stale-path check rejects or replaces the path |
| `src/record/trigger_center.cpp:142` stale socket `unlink` | A failure makes the following `bind()` return an error, which `Init()` reports as an explicit failure — not a silent path |

## 5. Rules

The [coding rules](coding-rules.md) forbid per-message logging on hot paths and
prefer counters. This audit extends that into a uniform checklist for error
paths:

1. **Return values must not be dropped**: a fallible API's return value is
   either checked, or explicitly `(void)` with the reason in a comment on the
   same or previous line ("cannot fail because …"). An uncommented `(void)` on
   a fallible call is forbidden.
2. **Counters before logs**: error counters go into `CenterStats` / `Stats` /
   `LogBudget` / endpoint getters, where status/probe/metrics can read them;
   logs use `_RL` rate limiting so an error storm does not flood the sink.
3. **Startup and security boundaries fail closed**: control-socket
   permissions, credential capability (`SO_PASSCRED`), and the listener
   socket's non-blocking flag must refuse startup on failure instead of
   "degrading and continuing".
4. **Soft failures must be documented**: best-effort paths (directory fsync,
   cleanup unlink) must state in a comment "what the source of truth is" (for
   example the wal abort/seal row), and must count when they can.
5. **No empty `catch`**: core has no exceptions (ADR 0002); if tools/tests use
   exceptions, empty catch blocks or `catch (...) {}` are not allowed.

Review entry: the error-path section of
[`review-checklist.md`](review-checklist.md) (E1–E6) maps one-to-one to this
table.

## 6. Follow-ups

| Follow-up | Target |
|---|---|
| Make the package manifest's directory fsync part of the state machine | Package state machine |
| Export `sync_errors` / `reply_send_failures` / `notify_send_failures` / `rejected_clients` to metrics/alerts | Metrics and alerting work (same family as the existing counter-export items) |
| Apple control-plane credential semantics (whether to fail closed when `LOCAL_PEERCRED` is missing) | Platform security work |
| Fleet-level log budget (currently counted only within a process) | Multi-process ownership work |

## Related

- [`coding-rules.md`](coding-rules.md) — counters before logs on hot paths,
  `_RL` rate limiting
- [`as-built-deviations.md`](as-built-deviations.md) — registry of places where
  behaviour differs from design; this audit's "soft failure" entries are
  recorded under its rules
- [`priority-inversion-audit.md`](priority-inversion-audit.md) — error paths
  must not perform slow operations (fsync/logging/rotation) on RT threads
- [`mechanism-policy-audit.md`](mechanism-policy-audit.md) —
  mechanism/policy ownership of defaults such as timeouts and retries
- [`adr/0010-transport-seam.md`](../design/adr/0010-transport-seam.md) — the
  transport interface requires "failure must be an explicit result code", the
  same principle as this audit
