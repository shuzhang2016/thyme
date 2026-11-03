# ADR 0011: SCHED_DEADLINE evaluation and enablement gates

- Status: Accepted (not enabled)
- Date: 2026-10-26
- Scope: OS placement of `CyclicThread` / `EventThread`; does not involve a callback executor

## Context

`SCHED_DEADLINE` gives each thread `runtime/deadline/period` parameters and
kernel CBS (Constant Bandwidth Server) admission. It can cap CPU bandwidth for
hard cyclic tasks, but it also changes the system's scheduling hierarchy:
eligible DEADLINE threads run before ordinary `SCHED_FIFO/RR` threads. It is not
a general switch for making one node higher priority.

thyme already has static threads, CPU affinity, FIFO/RR/OTHER, rate-monotonic
priorities, declarative `wcet_ns`, per-core `Σ C_i/T_i` reporting, and capacity
admission. `ApplyThreadPlacement` uses `pthread_setschedparam` and has no
`sched_attr` runtime/deadline/period interface. Cyclic nodes use their period
implicitly as the deadline; an event node's `deadline_ns` is a Dispatch
overrun-detection budget, not a kernel CBS period.

## Decision

1. **Continue with FIFO + RM; do not enable `SCHED_DEADLINE`.** No new
   configuration keys, syscall wrappers, or runtime policy branches.
2. `SCHED_DEADLINE` can only be a future optional placement backend. It does not
   change the model "one node = one thread, callbacks serial, no runtime
   executor".
3. Only hard cyclic nodes can be candidates. Event-node message rates, wake
   intervals, and batch lengths are not fixed periods, so `hz` or the Dispatch
   deadline cannot be mapped directly to CBS period/runtime.
4. Until every enablement gate is met, declared `wcet_ns` is used only by the
   existing analysis and must not be interpreted as a validated kernel CPU
   reservation.

## Why it is not enabled now

- **Incomplete three parameters.** Graphs have periods and WCET, but no
  separate reservation runtime with a measurement margin, and cyclic nodes have
  no separate relative deadline. Setting `runtime = wcet` would turn an
  unvalidated analysis value into a kernel-enforced bandwidth.
- **Different admission scope.** The current `Σ C_i/T_i` is a static check
  within the graph on the first `cpu_set` core. Kernel DEADLINE admission also
  depends on other tasks, CPU online topology, and reserved system bandwidth.
  Current reporting cannot replace a system-level admission proof.
- **Non-candidate threads lack protection.** Recorder drain, logging, probe,
  supervisor, and ordinary FIFO threads have no unified CPU/bandwidth
  reservation. Introducing a higher scheduling class could move frame drops
  from candidate nodes onto these dependency threads.
- **No target-board evidence.** There are only short x86 tests; there is no CBS
  tail-latency, throttling, wakeup, or recovery data on the target board.
  Best-effort local failures without privileges do not prove target behaviour.
- **No bottleneck proves the benefit.** Known problems are handled first by
  static placement, RM, capacity validation, overrun observation, and I/O
  isolation. No evidence shows CBS is the necessary fix.

## Mandatory gates for future re-evaluation

All must hold; missing one blocks implementation:

1. Select only hard cyclic nodes; `runtime_ns`, `deadline_ns`, and `period_ns`
   have independent, explicit semantics satisfying
   `0 < runtime <= deadline <= period`.
2. Runtime comes from target-board WCET measurement plus an agreed margin.
   Generation and `--check` reject missing, overflowing, and out-of-range
   parameters; unknown must never be treated as 0.
3. `cpu_set` is explicit, `placement_fail: require` is set, and a scheduling
   backend failure must fail `Start()` and tear down the thread. Silent
   fallback to FIFO/OTHER is forbidden.
4. `--explain-sched` reports CBS utilization and system-level admission
   assumptions for each candidate; deployment must prove admission for all
   DEADLINE tasks plus reserved bandwidth, not only for the thyme graph.
5. Recorder, logging, probe, supervisor, and other FIFO/ordinary threads have
   explicit CPU/cgroup/I/O isolation and headroom, so candidate overruns do not
   become global starvation.
6. Target-board acceptance covers steady state, bursts, CPU/I/O/IRQ
   interference, throttling, process kill/restart, and long runs. Results must
   be no worse than the FIFO+RM baseline, with comparative data for
   `pinned_skip`/deadline misses/tail latency.
7. Keep FIFO+RM as the default path and retain a single placement interface. If
   the requirement can only be expressed with a runtime callback executor, write
   a separate ADR rather than smuggling it in through CBS.

## Consequences

- Current configuration and generation are unchanged; no new ABI,
  configuration, or runtime branch.
- Adding DEADLINE merely for "more real-time" without the system-level evidence
  above should be rejected in review.
- If the gates are met later, implement it as a separate placement backend and
  then define configuration schema, a `sched_attr` compatibility layer, actual
  scheduling-state reporting, and a fault matrix.

## Validation

This is an applicability evaluation and produces no runtime code. Existing
implementation check: `include/thyme/runtime/placement.h` has only affinity and
`pthread_setschedparam`; WCET, capacity, and overrun semantics in
`docs/reference/scheduling-model.md` are unchanged. Documentation link and
command checks act as this ADR's regression gate.
