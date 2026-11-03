# ADR 0006: trigger FSM and "Accepted = queued" semantics

- Status: Accepted (as-built)
- Date: 2026-10-24
- Scope: `include/thyme/record/trigger_center.h`, `triggered_recorder.h`, `trigger_protocol.h`; `src/record/trigger_center.cpp`

## Context

The trigger path has two roles: the trigger center (policy: who, priority,
quota, quiet period) and the triggered recorder (execution: harvest → drain →
seal). In an early form, `Submit` synchronously performed mkdir/open/harvest, so
socket handling could be starved by one capture's long drain. At the same time,
preemption/start failures needed to be reported, but the client had already
received `kAccepted`, and a single datagram cannot carry a second response code.

## Decision

1. **Two layered FSMs:**
   - Center policy FSM (`trigger_center::State`): `kIdle → kCapturing →
     (kMuted) → kIdle`. `max_concurrent_captures = 1` (queue depth 1 at this
     stage).
   - Recorder pipeline FSM (`triggered::Phase`): `kArmed --Trigger()-->
     kPost --(post window + continuous harvest complete)--> kSettle --(queue
     drained, package sealed)--> kArmed`. With `defer_settle`, it stays in
     kSettle until the host idle detector calls `AllowFlush()`; `Preempt()`
     bypasses that gate.
2. **Asynchronous accept:** `Submit` performs only policy decisions (source
   rules, timestamp bounds, quota-class sliding 1 h window, memory quota,
   disk_full), enqueues the request, and immediately returns
   `kAccepted(trigger_id)`. The actual start/preempt happens in
   `StartPendingTrigger()` from `Tick()`. While pending, a second submission
   returns `kBusy` (retryable); a disconnected recorder returns
   `kRecorderOffline`.
3. **`kAccepted` = queued, not started or produced:**
   - `start_capture` failure rolls the FSM back to the pre-enqueue snapshot (an
     already-preempted old capture is treated as completed, mute applies) and
     increments `start_failures`;
   - `preempt_capture` failure rolls back to the pre-Submit snapshot
     (state/trigger_id/priority/mute), drops the new trigger, and increments
     `preempt_failures`;
   - **no late error codes:** the final result is determined from the package
     directory or a status query; counters go to status JSON/logs. A late error
     code cannot change decisions the caller already made; rollback + counters +
     package directory is idempotent, observable, and does not block the
     business thread.
4. **Preemption and muting (mute/quota):**
   - While CAPTURING, priority > current preempts (truncate and seal the old
     capture, start the new one immediately, force a realtime flush); with
     `force` (manual confirmation), priority >= current is enough; otherwise
     return `kMuted`;
   - While MUTED (inside the source's `mute_ns` window after capture), only
     crash-level requests (priority >= crash_priority) pass; mutation expires
     back to IDLE;
   - Quotas: per quota-class sliding 1 h counters (0 = unlimited; crash level
     may override and counts `quota_overridden`). When the recorder's
     `pending_bytes_quota` is exceeded, low-priority requests are rejected; the
     center links through the `pending_bytes` hook, and crash-level requests
     first `evict_oldest`, then proceed.
5. **Cancellation and outcome notification:**
   - `Cancel(trigger_id)`: a pending trigger is dropped and the FSM rolled back;
     a running capture goes through the same `preempt_capture` hook as
     preemption (truncate and seal written data, apply the source's mute
     window). An unknown or already-completed id returns `kUnknownTrigger` —
     an explicit rejection, never a silent success.
   - Every capture outcome (sealed/preempted/start_failed/preempt_failed/
     cancelled) is pushed into a fixed 64-entry outcome ring.
     `kFlagWaitOutcome` requests are answered from the ring when present;
     otherwise they are attached to a fixed 8-slot deferred waiter table
     (deadline = `response_timeout_ms`, resolved by Tick). Ring overflow, table
     overflow, and expiry are counted.
   - v3 reuses the existing flags/timestamp_ns layout for cancel/wait; it must
     also set `kFlagHasTimestamp`, so an old server rejects the request as
     timestamp-out-of-range instead of misrecording a trigger.
6. **Observability:** each FSM transition records
   `{mono_ns, trigger_id, from, to, reason}` into a fixed 256-entry ring
   (overflow overwrites the oldest and counts `fsm_event_drops`) and appends
   periodically to `<socket_path>.fsm` as JSONL. The writer is unbuffered (one
   `write()` per line, `O_APPEND`) and rotates according to
   `[logging] fsm_max_bytes`/`fsm_rotate_keep`; a crash loses at most the
   in-flight line, and there is no allocation after Init.

## Alternatives

- **A. Synchronous Submit (execute on accept):** socket handling is blocked by
  harvest/mkdir, and trigger storms make the server-side backlog invisible (the
  kernel silently drops datagrams when the AF_UNIX queue is full). Rejected;
  `socket_backlog_events` provides observability.
- **B. Late error codes:** conflicts with asynchronous accept; adding a second
  notification channel complicates the client's synchronous API. Rejected in
  favor of FSM rollback plus counters.
- **C. Reuse `TriggerResponse` for completion notification:** a response can be
  sent only once. A separate outcome ring plus deferred waiter is used without
  changing the v3 layout.
- **D. Multiple concurrent captures (queue depth > 1):** this stage keeps
  `max_concurrent_captures = 1`. Depth 1 keeps "kBusy is retryable" simple and
  drain bandwidth budgetable; higher concurrency requires reworking drain and
  quota accounting and is left for later.

## Consequences

- **Benefits:** sockets are never starved by capture execution; the client
  contract is simple and idempotent (Accepted is safely retryable, carried by
  trigger_id and kBusy); cancellation, preemption, and failures are all
  observable and rollbackable.
- **Costs (explicitly accepted):**
  - the client **must not** treat `kAccepted` as "package produced"; delivery is
    determined by the package directory (`PackageComplete()`), outcome
    notification, or status query;
  - `kBusy` exposes queue pressure explicitly (retry policy is the client's
    responsibility);
  - queue depth 1: under a burst of multi-source triggers, only the newest
    enqueued request is served; the rest follow kBusy/kMuted paths;
  - the outcome ring (64) and waiter table (8) are fixed capacity: under storms
    old outcomes are overwritten (counted as `notify_outcome_drops`), and
    waiters may see a timeout near `kUnknownTrigger` semantics. Alert on the
    counters rather than asserting on a single result.
