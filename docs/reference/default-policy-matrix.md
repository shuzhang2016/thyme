# Default policy matrix

This page collects thyme's default failure actions, the switches available, and
their costs. Configuration fields are authoritative in
[`trigger-config.md`](../guides/trigger-config.md); incident handling is in
[`runbook.md`](../guides/runbook.md); intentional implementation boundaries are
recorded in [`as-built-deviations.md`](as-built-deviations.md). This table does
not change defaults.

| Scenario | Default action | Switch and cost |
|---|---|---|
| MP child exits or stalls | `restart_enabled=false`: the exited role is not restarted and the run success gate fails. `restart_stall_timeout_ns=0`: stalls are not actively probed | Enabling restart defaults to at most 3 restarts per 60 s with 100 ms–5 s backoff; stall detection still needs a timeout. After SIGTERM the default grace is 500 ms before SIGKILL. Automatic recovery risks state loss, duplicate publication, and restart storms; combine it with health gates and capacity validation |
| Cyclic/event overrun | `OverrunPolicy::kSkip`: count and continue. Cyclic threads do not catch up into a death spiral; event threads keep draining; cause and policy counters are kept | `degrade` temporarily lowers the rate, trading sample rate for stability; `restart` calls a hook on the RT thread and may reset node state. The hook must be non-blocking and zero-allocation; the framework does not clear input rings or application state for it |
| Trigger time-window quota/priority | Default `max_triggers_per_hour=0` is unlimited; with a limit, ordinary requests return `kQuotaExceeded`. Same/lower-priority requests during CAPTURING or mute return `kMuted`. Crash level may override quota and mute, but is counted and audited; a client hint can only lower priority | Quotas/mute/priority are per source; loosening limits increases disk, memory, and I/O peaks. Crash override is not a free channel and must keep auditing and post-incident forensics |
| Pending/disk quota | Pending defaults to 64 MiB: evict the oldest continuous descriptors first, then the oldest descriptors of the current trigger; a single over-budget item is dropped and counted. `disk_quota_mb=0` means unlimited package storage; `.incomplete` is kept by default | Increasing raises RSS and write backlog during failures; decreasing produces GAPs/frame loss. With a disk quota, retention applies; the high-priority reserve defaults to 0. Deleting partial packages sacrifices forensics |
| Replay gate conflict | By default hold the PinnedSlot channels and the small-mirror per-struct `ProducerGate`: a second injector targeting the same destination gets `kBusy`; a real producer is rejected while gated and does not advance if the gate is taken before publish. A live gate is never overridden even by `--force` | `--no-gate` is only for legacy targets with known risk; it disables double-write detection and prints a warning. Gate timeout can reclaim a dead holder, but a long pause must heartbeat; too short a timeout harms slow disk writes |
| Real data during replay | PinnedSlot channels and the small mirror's target structs are each blocked from double writes by the gate; other structs on the same page are unaffected. By default mono/session are rewritten and sequences are reordered; `--preserve-sequences` keeps original sequence numbers | `--no-gate` reintroduces double-write risk and is only for stopped real producers or explicitly isolated legacy targets. Preserving sequences is mainly for forensic comparison and does not change time/session/CRC rewriting |
| Service/FSM/perf/audit log rotation | Service defaults to stderr; an explicit file defaults to 8 MiB × 3 and refuses to start if it cannot open. FSM 4 MiB × 3, perf 64 MiB × 3, audit 4 MiB × 3. `total_budget_mb=0` is bounded only by per-file limits; when the total budget is reached, service/FSM/perf records may be dropped, but the security audit is not | The total budget is **per process**, not global. Service write failures are counted and warned; raising the budget increases disk use and forcing observation records increases failure latency. Audit fsync defaults to Shutdown only |
| Package and evidence files | Default `manifest=full`; recorded files and directories are fsynced in seal order, incomplete packages are marked `.incomplete`, and a partial manifest covers only surviving files. Startup recovery can scan fully; online seal uses incremental hashing | Bulk may explicitly choose `metadata` (raw records size only) or `off` (file list + size only); non-full output is `UNVERIFIED(payload)` and audited; small/control are always full. Signing authenticates only the degraded manifest and does not restore proof of payload content. Keeping partial packages aids forensics; deleting saves space but reduces diagnosability |

**Log-budget scope decision:** keep the per-process `total_budget_mb` and do not
add a global `MpControl` counter. The value is one process's fault-isolation and
disk-backpressure boundary, not a host-wide session quota; deployment must
compute the worst case as N× the budget for N processes. A global counter would
need cross-process atomic decrement, holder crash/restart reclamation, and
session-generation semantics, adding SHM ABI and write contention without
replacing per-process isolation. There is no measured requirement for a global
hard limit, so no separate ABI change is made; if one appears later, design it
as a session-level quota rather than copying the per-process counter into a
shared page.

## Why "crash = experiment failure, timeout = degrade one frame"

An MP child exit means the deployment graph is no longer complete; continuing
could disguise missing inputs, stale SHM, or partial outputs as valid results.
Failing the run by default suits benches and CI, where the goal is "failure
visible, scene preserved". A cyclic overrun only means this tick's timing budget
was exceeded; the next period may still have complete inputs. Counting and
continuing by default prevents one scheduling jitter from amplifying into a
whole-graph restart, and overruns, drops, and sched mismatches are all visible.

These defaults are not permanent. Before 24×7 production, evaluate through HIL
and a 72 h soak whether role restart is safer than whole-graph failure, and
whether overrun degrade/restart is more controllable than continuing. Changing
them requires validating state cleanup, double-write protection, restart budget,
resource peaks, and alert drills together; do not just flip the two booleans.

Related deviations: keep this table consistent with the MP restart, replay-gate
scope, and per-process log-budget entries in
[`as-built-deviations.md`](as-built-deviations.md).
