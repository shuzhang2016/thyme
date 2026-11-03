# ADR 0007: multi-process supervisor (MP) scope and SHM topology ownership

- Status: Accepted (as-built)
- Date: 2026-10-24
- Scope: `include/thyme/app/mp_supervisor.h`, `mp_control.h`; [ARCHITECTURE.md §11](../../../ARCHITECTURE.md)

## Context

Once a graph is split across processes (`processes[]` in `graph.yaml`), someone
must answer: who creates cross-process shared memory, who forks/execs each role,
who decides that "this run succeeded", and what happens when a role dies or
stalls. thyme keeps the supervisor as a **table-driven library skeleton** (the
"how"); run duration, success-gate composition, and exit-code policy (the
"what") stay in the application's playbook (the generated
`mp_supervisor_adapter.cpp`). thyme ships no demo-specific exit policy.

## Decision

1. **SHM topology: the supervisor owns creation and reclamation.** `Prepare()`
   creates the whole SHM set (prefix defaults to `/c9_%d`): `_h`
   (`SharedHealthPage`) and `_c` (`MpControl`) are mandatory; each cross-process
   channel gets a `LatestSlot` region; record mirrors are created when recording
   is enabled. `Cleanup()` unmaps and unlinks. Role processes only **attach**
   (fork + exec the same executable with
   `--role/--shm-prefix/--proc-idx [--record-shm]`; generated
   `AssembleRoleLocals` does the wiring). Each channel's writer is owned by its
   producer role; the supervisor never touches the data plane. If `ForkRoles`
   fails, every already-forked child is stopped and reaped
   (`WaitChildTimeout`: SIGKILL on timeout and keep waiting, so a wedged child
   can never deadlock the supervisor). No orphan leaks.
2. **Success gates are generated configuration, not product policy:**
   - per-process loop minimum: `MpControl::proc_loops[]` must reach the minimum
     hz derived from the slowest worker in each process;
   - cross-process frame gate: an edge marked `mp_gate: true` maps to a
     `cross_frames[]` slot and must reach ≥ 1 by the end of the run; the gate's
     shm kind must actually appear on a cross-process edge (misconfigured gates
     must not pass silently);
   - health scan: after the run, aggregate drops/deadline_miss/error from health
     pages; `health_scan_strict` can turn this into a failure.
3. **Regulation scope: detect exit and stall; restart is opt-in.**
   `PollRestarts()` performs `waitpid(WNOHANG)` to reap exits. A role whose
   heartbeat **and** `proc_loops` have not advanced for
   `restart_stall_timeout_ns` is stalled → SIGTERM, then SIGKILL after
   `restart_kill_grace_ns`. After reaping, the same role index/name is restarted
   with the same arguments; before restart, its health slot is cleared (a stale
   heartbeat must not make a new role look alive) and `proc_loops` is rebased.
   The budget is `max_restarts`/`restart_window_ns`, with exponential backoff
   clamped to `[kMinRestartIntervalNs, restart_backoff_max_ns]` (minimum 1 ms, so
   a child that dies instantly cannot spin the supervisor). Exhausting the
   budget enters `kGaveUp`: **no more restarts; the supervisor stays alive and
   reports it**.
   **Defaults preserve pre-restart behaviour:** `restart_enabled = false` and
   `restart_stall_timeout_ns = 0`, meaning "detect only, never restart"; the run
   success gate then fails. This is deliberate: during validation and
   calibration, failures must be **visible**, not silently masked. Restarting
   hides design defects, so it must be enabled explicitly.
4. **Supervisor-level supervision lives outside.** Deployment supervises the
   supervisor: under `THYME_SD_NOTIFY`, `ForkRoles` success sends `READY=1`, and
   `PollRestarts` sends at most one `WATCHDOG=1` per second. Stopping the poll
   stops the watchdog feed, so systemd restarts a wedged supervisor. Role-level
   stalls are handled inside thyme; supervisor-level stalls are handled by
   systemd. A read-only JSON status endpoint is provided over a UNIX stream
   socket and fails closed if bind fails.
5. **Configuration consistency:** `config_hash` (FNV-1a) is registered in-process
   for comparison at attach; it does not enter the SHM wire format.

## Alternatives

- **A. The supervisor restarts everything, always:** makes validation runs "look
  stable" while hiding role crashes from the success gate. A restarted role
  reattaches to supervisor-owned SHM, but in-flight frames and leases cannot be
  recovered; clearing the health slot and starting over is the only option.
  Rejected as a default; retained as an option (`restart_enabled`).
- **B. An external orchestrator manages everything** (systemd manages each role
  directly): SHM lifetime across roles (who creates/deletes, prefix consistency)
  and the graph's "one run" semantics would have to move out of thyme, and
  cross-process gates could not be expressed. Keeping role-level handling in
  thyme and supervisor-level handling in systemd preserves the layering.
- **C. Single process, multiple threads:** loses fault isolation (one role crash
  takes down the whole graph) and independent deployment/permission boundaries.
  MP exists for isolation. Rejected.

## Consequences

- **Benefits:** single SHM ownership (the supervisor creates and deletes), so a
  role crash does not leak SHM. Stall/exit detection and restart policy are
  table-driven and fixed-capacity (no heap allocation on the restart path).
  `kGaveUp` makes "restart cannot save this" an explicit terminal state instead
  of an infinite loop.
- **Costs (explicitly accepted):**
  - no restart by default: production deployments must explicitly configure
    `restart_enabled` and its budget, or a role death means run failure;
  - restart is **reattach**, not state recovery: in-role state not persisted is
    lost, and in-flight frames and peer leases are not restored (clearing the
    health slot prevents a false-alive signal);
  - the supervisor is a skeleton: exit codes, report formats, and trigger-relay
    polling live in the application playbook, so a new application must rewrite
    the "what" layer;
  - `SupShmSet::kMaxChannels = 16` (cross-process channels) and
    `MpControl::kMaxProcSlots` are fixed limits; exceeding them is a
    configuration error.
