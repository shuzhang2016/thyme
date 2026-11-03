# Mechanism/policy boundary audit

Scope: every "tunable number" and "default behaviour" in core (the runtime,
record, and replay primitives in `include/thyme/` and `src/`), deciding whether
it is a **mechanism** (protocol, memory model, or security boundary that must
not change across deployments) or a **policy** (thresholds, defaults, retry
counts, retention, quotas, rotation, overrun, gating — decided by the
deployment or host).

Audit date: 2026-10-24. Line numbers refer to the files at the time of the
audit.

## 1. Criteria

| Category | Criterion | Examples |
|---|---|---|
| **Mechanism** | Changing it changes the protocol, ABI, memory model, or security semantics; or it is a hard boundary that stops resources or attacks from running away | 40 B `FrameHeader`, 64 KB transport limit, seqlock retry bound, retention recursion depth cap, SHM `layout_hash` validation |
| **Policy** | Changing it only changes "how fast / how strict / how long", not the observable protocol shape | quota window, slice size, GOP fallback timeout, log rotation size, gate timeout, restart backoff |
| **Host configuration** | The policy is already set explicitly by the host through `Config`/INI; core only provides the structure | `retention::Config`, `triggered::Config`, `MpSupervisorConfig`, `StatusServer::Config` |

Two additional rules:

1. **A mechanism may have a default, but the host must be able to override
   it** (parameter or `Config` field). The default is a "safe fallback", not
   "the only option".
2. **Configuration keys come from one schema only.** This batch only moves
   constants into existing host structs; new INI keys are left to the
   single-schema work, so that no "key the parser does not know" appears.

## 2. Audit table

Disposition: `mv now` (changed in this batch), `later` (has a concrete target
item), `keep` (already in the right place).

| # | Concern | Current location | Should live in | Action | Rationale / target |
|---|---|---|---|---|---|
| 1 | Trigger-count quota window (sliding 1 h) | `src/record/trigger_center.cpp:24` `kQuotaWindowNs` | Mechanism (contract) | keep | The configuration key is literally `max_triggers_per_hour` (`trigger_config.h:133`), so the window is part of the key name; making the window configurable is a schema change, not part of this item |
| 2 | Hourly cap per quota class | `include/thyme/record/trigger_config.h:133` + `[quota.*]` | Host | keep | Already parsed from INI with typo validation (`test_quota_class_typo`) |
| 3 | Maximum concurrent captures | `include/thyme/record/trigger_config.h:126` | Host | keep | Hard validation at parse time (only 1 is supported for now) |
| 4 | Crash priority threshold | `include/thyme/record/trigger_config.h:129` | Host | keep | What counts as a crash is a deployment policy |
| 5 | Per-source before/after/mute/quota_class/priority | `include/thyme/record/trigger_config.h:110-121` | Host | keep | Parsed field by field from `[source.*]`, hot-reloaded on SIGHUP |
| 6 | Disk quota `disk_quota_bytes` | `include/thyme/record/retention.h:60`; the host calls `Enforce` at `tools/apps/trigger_center/trigger_center_main.cpp:459,470` | Host | keep | Both the quota value and the sweep timing live in the host; `Manager` only implements the "delete oldest" mechanism |
| 7 | `high/` reserved-area name and reserved bytes | `include/thyme/record/retention.h:61-62` | Host | keep | The directory name is deployment layout; the reserved amount is policy |
| 8 | `.incomplete` disposition policy / maximum age | `include/thyme/record/retention.h:63-64` | Host | keep | `keep` is the safe default; whether to delete or keep is a deployment decision |
| 9 | Retention sweep period (5 min) | `tools/apps/trigger_center/trigger_center_main.cpp:465` | Host | keep | Already in the host main loop, not in core |
| 10 | Retention directory recursion depth cap | `include/thyme/record/retention.h:45` `kMaxTreeDepth` | Mechanism | keep | Hard boundary against symlink/deep-directory attacks, not an operational policy |
| 11 | Continuous slice size/time | `include/thyme/record/trigger_config.h:206-207` | Host | keep | `[continuous].slice_mb/slice_s`, with validation that `slice_mb <= disk_quota_mb` |
| 12 | Continuous-mode GOP fallback roll timeout | **Changed**: `include/thyme/record/triggered_recorder.h:151` `Config::gop_roll_timeout_ns`; previously the function-local `kGopRollTimeoutNs` in `src/record/triggered_recorder.cpp` | Host | **mv now** | "How long to wait for an IDR before forcing a cut" is a slicing-cadence policy; it is now a `Config` field (default 5 s), and the INI key is not exposed yet |
| 13 | Continuous-mode bitrate accounting mode (warn/enforce) | `include/thyme/record/trigger_config.h:211` | Host | keep | Fail-fast at startup vs warn-only is a deployment choice |
| 14 | Recorder pending-byte quota / descriptor ring capacity | `include/thyme/record/triggered_recorder.h:121` + `src/record/triggered_recorder.cpp` (`DescriptorRingCapacity`) | Host value + mechanism derivation | keep | The quota is host policy; the ring capacity is derived from the quota, which is a mechanism implementation detail |
| 15 | drain batch / per-Tick time budget / post grace | `include/thyme/record/triggered_recorder.h:137-142` | Host | keep | Affects host main-loop responsiveness, not the protocol |
| 16 | Rate-limit token bucket rate/burst | `include/thyme/record/triggered_recorder.h:145-146`; semantics in `include/thyme/record/token_bucket.h` | Host value + mechanism semantics | keep | `rate=0` = unlimited, and oversized items being allowed to borrow debt, are mechanism semantics; rate/burst are host policy |
| 17 | Overrun action (skip/degrade/restart) | `include/thyme/runtime/cyclic_thread.h:99`, `include/thyme/runtime/event_thread.h:134` (setters) | Host (per node) | keep | Already set explicitly by generated code/nodes; the `kSkip` default preserves existing behaviour |
| 18 | Consecutive good cycles required to recover after degrade (8) | `include/thyme/runtime/cyclic_thread.h:121`, `include/thyme/runtime/event_thread.h:151` | Mechanism | keep | Hard upper bound implementing "a single miss must not spiral into degrade"; if this ever needs tuning it should be exposed through the channel policy structure |
| 19 | Per-input dispatch budget per batch | `include/thyme/runtime/event_thread.h:159` | Host (per node) | keep | 0 = unlimited; generated code sets it per node |
| 20 | Idle poll cap (1 s / half period) | `include/thyme/runtime/event_thread.h:298-304` | Mechanism | keep | Derived from the node's declared frequency (`IdlePollNs`); the cap is a safety value preventing a high-frequency node from waking only after 1 s |
| 21 | seqlock read retry bound (16) | `latest_slot.h:30`, `shm_spmc_ring.h:21`, `pinned_slot_store.h:90`, `record_mirror.h:54` | Mechanism | keep | The bounded retry budget for RT readers; unifying the naming and making it a single point belongs to a later channel refactor |
| 22 | Producer gate default timeout 2 s / heartbeat 250 ms | `include/thyme/runtime/producer_gate.h:41-42` | Mechanism default + host policy | later | Call sites can pass an explicit timeout; the defaults should become session-lease parameters (heartbeat period/expiry are session policy) |
| 23 | PinnedSlot slot lease preemption timeout | `include/thyme/runtime/pinned_slot_store.h:76` | Mechanism default + host policy | later | An explicit parameter already exists (`Open`/`Acquire` accept it); should converge with the gate into a channel-level policy |
| 24 | MP restart switch/budget/backoff/stall | `include/thyme/app/mp_supervisor.h:73-98` | Host | keep | The default "do not restart" preserves gate-failure semantics; the whole operational policy already lives in the struct |
| 25 | MP abort-path child wait duration | **Changed**: `include/thyme/app/mp_supervisor.h:105` `shutdown_timeout_ns`; previously `src/app/mp_supervisor.cpp` used `kMpWaitTimeoutNs` directly | Host | **mv now** | "How long to wait for the child before SIGKILL" is a deployment shutdown policy; the default is still the original 2 s |
| 26 | MP attach open/wait timeout | `include/thyme/app/mp_control.h:82,85` | Mechanism | keep | Mechanism deadlines for the startup and shutdown handshakes, not runtime policy |
| 27 | MP restart minimum interval (1 ms) | `include/thyme/app/mp_supervisor.h:119` | Mechanism | keep | Safety floor that prevents a role which dies immediately from spinning |
| 28 | Status endpoint concurrency/timeout/request size | `include/thyme/app/status_server.h:115-127` | Mechanism bound + host configuration | keep | `kMaxClients`/`kMaxRequestBytes` are memory bounds; `max_clients`/`client_timeout_ns` are configurable |
| 29 | perf ring capacity/time window | `include/thyme/runtime/perf_log.h:58-60` | Mechanism derivation + host window (pending) | later | Capacity is derived from node frequency × 10 s; making the window deployment-tunable is part of the observability wiring, together with the scheduling observability work |
| 30 | Trigger protocol/client default response timeout 100 ms | `include/thyme/record/trigger_protocol.h:73`, `include/thyme/record/trigger_client.h:57` | Mechanism default | keep | Each request can carry `response_timeout_ms`; the default is only a client convenience |
| 31 | Log rate-limit policy (3 lines, then every 1000) | `include/thyme/runtime/log.h:101` | Mechanism (contract) | keep | A contract for hot-path logging, not a tunable operational parameter |
| 32 | Log rotation size/retention generations | `include/thyme/record/trigger_config.h:164-179` + `RotatingFile` | Host | keep | `[logging].*`, `audit_max_bytes`, etc.; already wired |
| 33 | Total process log budget | `include/thyme/record/trigger_config.h:180`; `include/thyme/runtime/rotating_file.h` | Host (process level) | keep | Per-process budget; a fleet-level budget needs a shared cross-process counter (`MpControl`) and belongs to the multi-process ownership domain |
| 34 | SHM liveness determination/lease semantics | `include/thyme/runtime/shm_lifecycle.h`, `src/runtime/shm_lifecycle.cpp` | Mechanism | keep | "Whether it is stale" is decided by lease/pid, not by a tunable threshold |
| 35 | `/proc` sampling period | `include/thyme/runtime/proc_sampler.h:341` (`Start(sampler, interval_ns)`) | Host | keep | The host starts the sampling thread and passes the period; core only guarantees the RT path never reads it |
| 36 | Startup resource self-check thresholds | `include/thyme/runtime/resource_check.h` + `THYME_RESOURCE_CHECK` | Host (env) | keep | Deployment capacity, injected via environment variable or plan |

**36** items in total: `mv now` **2**, `later` **3**, `keep` **31**. All three
`later` items (gate timeout/heartbeat, slot-lease preemption timeout, perf
window) depend on the session-lease and channel-policy designs being settled
first; moving them now would only create a second policy structure.

## 3. Changes landed in this batch

### 3.1 Continuous-mode GOP fallback timeout → `triggered::Config`

- Before: `constexpr uint64_t kGopRollTimeoutNs = 5 s` inside
  `StreamPinnedSlots()` in `src/record/triggered_recorder.cpp`.
- Now: `include/thyme/record/triggered_recorder.h:151`
  `uint64_t gop_roll_timeout_ns = 5000000000ull;`, and `StreamPinnedSlots`
  reads `cfg_.gop_roll_timeout_ns`.
- Compatibility: the default is unchanged; no new INI key was added, so the
  "implemented fields" table in
  [`trigger-config.md`](../guides/trigger-config.md) is not violated. Exposing
  a key under `[continuous]` is left to the config-schema work.

### 3.2 MP abort-path child wait → `MpSupervisorConfig`

- Before: the `ForkRoles()` failure-cleanup path in
  `src/app/mp_supervisor.cpp` used `kMpWaitTimeoutNs` (2 s) directly.
- Now: `include/thyme/app/mp_supervisor.h:105`
  `uint64_t shutdown_timeout_ns = kMpWaitTimeoutNs;`; both abort paths read
  `cfg_.shutdown_timeout_ns`, and the timeout log prints the configured value.
- Compatibility: the default is unchanged;
  `restart_kill_grace_ns + kMpWaitTimeoutNs` (the restart stall path) is
  untouched, because it expresses "grace + fixed teardown" rather than this
  policy.

Both are small changes that keep defaults and semantics unchanged while
letting the host override them; no module structure changed and no ABI was
added.

## 4. Follow-ups (with targets)

| Follow-up | Target | Notes |
|---|---|---|
| Producer gate / slot lease timeout and heartbeat become session policy | Session lease, channel-level policy | Both defaults currently live in two headers; once the lease lands, session parameters should inject them uniformly |
| seqlock retry budget, perf ring window, and single-point gate/counter/wake | Channel-level policy | Cross-cutting policy implemented once; the perf window becomes a host-tunable item |
| New keys such as `[continuous].gop_roll_timeout_ms` and the quota window | Single config schema | One schema generates parser, docs, and validation; no hand-added keys in this batch |
| Quota window key name and semantics (`per_hour` vs `per_window`) | Single config schema | If a non-1 h window is ever supported, the key name and docs must change first, not just the constant |
| Fleet-level log budget | Multi-process ownership | Needs a shared counter across processes (`MpControl`) |
| Resource self-check thresholds | Environment/plan | Continue via env/plan, not core constants |

## 5. Maintenance rules

- Before adding a constant, ask: **would changing it change the wire format,
  ABI, or security boundary?** If yes → mechanism; if no → put it in an
  existing host `Config` with a default.
- Mechanism defaults must be overridable by the host; hard-code only when
  there is no reasonable override point, and state the reason in a header
  comment (as with the attack-surface rationale for `kMaxTreeDepth`).
- New tunables must go through the single configuration schema; neither
  "INI keys the parser does not know" nor "keys documented but never read by
  code" are allowed.
- When reviewing, cross-check the ABI and error-path sections of
  [`review-checklist.md`](review-checklist.md); when the mechanism/policy
  boundary is disputed, decide by "does this affect the observable protocol?".

## Related

- [`adr/0010-transport-seam.md`](../design/adr/0010-transport-seam.md) — the
  transport boundary; it assigns version header/wakeup/gate/counter/lifecycle
  to the transport and session layers
- [`error-handling-audit.md`](error-handling-audit.md) — mechanism-layer
  failures must be returned or counted explicitly; silent drops are not
  allowed
- [`trigger-config.md`](../guides/trigger-config.md) — implemented/future
  table of host configuration keys
- [`priority-inversion-audit.md`](priority-inversion-audit.md) — host/RT
  thread boundary; consistent with "policy lives on host threads" here
