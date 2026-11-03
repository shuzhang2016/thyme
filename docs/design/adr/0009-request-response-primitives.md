# ADR 0009: request/response service primitives — evaluate and defer

- Status: Deferred (not implemented now; triggers and gates recorded)
- Date: 2026-10-24
- Scope: service-mode (client/server) primitives; paired with the transport
  seam and the topology-level session lease
- Non-goals (explicitly out of scope): UDP, dynamic discovery, a general RPC
  framework, a core service thread pool

## Context

thyme currently has two kinds of cross-process communication:

1. **Data edges:** `latest` / `notify` / `queue` / `broadcast`, all
   **one-way and uncorrelated** publications. Small structs use rings or
   `LatestSlot`; large frames use pinned-slot stores plus per-slot leases.
2. **Control plane:** the trigger center's Unix datagram socket (`triggerctl`'s
   trigger/arm/disarm/status) and the recorder control socket. These are
   hand-rolled protocols without a unified method/contract/correlation model.

eCAL's `client/server` (service) provides synchronous request/response: a client
calls by service/method name, the server registers a callback and runs it on its
own thread pool, and the transport can be SHM or UDP. It solves the
"query/command" shape, not periodic data flow.

This ADR evaluates whether request/response should become a thyme core
primitive. The conclusion is no, not now, with triggers, alternatives, and
non-goals recorded so that data edges are not later forced into an ad-hoc RPC.

## Forces

1. **Only single-node deployment is confirmed.** Every channel is POSIX SHM; the
   deployment unit is one domain controller. There is no confirmed cross-machine
   request requirement.
2. **Multi-node may come later**, but through the transport seam (fix the
   interface, implement single-node, fill in multi-node later), not by adding a
   network stack now. UDP is an explicit non-goal: connectionless, unreliable,
   unordered, and contrary to thyme's explicit-drop and determinism story.
3. **Determinism first.** Core libraries forbid hot-path allocation and schedule
   per thread. A "server thread pool plus unbounded request queue" would bring
   runtime scheduling and WCET unanalyzability back, conflicting with
   [ADR 0003](0003-no-callback-group-executor.md).
4. **Failures must be explicit.** Existing primitives have no silent
   best-effort: drops are counted, leases return `kBusy`/`kGated`, triggers have
   accepted semantics. Request/response must likewise answer: timeout,
   cancellation, duplicate request, server crash, replay gate.
5. **Lifecycle is complex.** Requests are **stateful**: correlation ids,
   in-flight tables, timeouts, cancellation, orphaned requests after a server
   restart. Data edges have none of this; the control socket avoids it with
   one-shot datagrams, at the cost of no multiplexing/backpressure/contract.
6. **ABI cost.** A new SHM page type means magic/version/layout/upgrade matrix,
   mixed-run rejection, and `thyme_probe` support. Paying that for an unproven
   shape is not worthwhile.
7. **Security boundary.** The control plane validates uid/peer pid. A service
   primitive accepting arbitrary clients must define authentication and
   authorization, or it expands the control plane into an RPC entry point any
   process can call.

## Alternatives

### A. `NotifyPipe` plus an index pool (reuse existing event primitives)

Each endpoint has a pair of directions: the client takes a slot from a request
`ObjectPool`, writes the payload, and pushes the slot index into a request
`NotifyPipe`; the server is woken by `EventThread`, processes it, writes a slot
in the response pool, and pushes a response using the correlation id.

- Pros: no new SHM page type or ABI; reuses `EventThread` static threads and
  `NotifyPipe` wake semantics; pools are naturally bounded and fail explicitly
  when full (countable).
- Cons: correlation, timeout, cancellation, and server-crash cleanup still have
  to be implemented in the protocol layer; it is an RPC state machine rebuilt on
  data edges. As clients grow, each needs its own response channel or a shared
  response ring plus filtering, increasing complexity.
- Fit: internal queries on a single node with a few endpoints and bounded
  in-flight requests.

### B. Per-call SHM slots (one slot/region per call)

Each request creates a slot (or uses one global large ring, one slot per call);
the response goes back into the same slot's response area; the client polls or
waits for notification.

- Pros: simple isolation, natural backpressure (fail when slots run out);
  reclaim by slot timeout after a crash.
- Cons: creating/mapping/naming/cleanup is expensive; at high call rates the
  region count and `shm_open` cost are unacceptable; slot leaks and naming
  collisions need extra lifecycle management; variable-sized large payloads
  return to the lease problem. **Not suitable as a general hot-path primitive**;
  only for low-frequency control/diagnostics.
- Fit: low-frequency, large-payload operational interfaces that can tolerate
  millisecond overhead.

### C. A service abstraction on the transport seam

Define a transport-independent service interface (method, contract, deadline,
cancellation, error codes); the single-node backend uses SHM (implementation A
or B), and a multi-node backend is filled in later.

- Pros: does not put SHM layout into the service API; changing transports later
  does not change callers; implementation A can serve as the single-node backend
  first.
- Cons: before a second backend exists, the abstraction is easy to overdesign;
  once public, the interface enters the compatibility surface. It must wait for
  the transport-seam decision.
- Fit: only after a multi-node requirement is confirmed.

## Decision

**Do not implement request/response primitives now.** Single-node queries and
commands continue to use:

- periodic/state data → existing data edges (`latest` for the newest value,
  `queue` for everything);
- low-frequency commands/queries → the trigger center/recorder Unix socket,
  with versioned JSON requests where needed (like `triggerctl`), outside the
  core hot path;
- internal tools needing "ask once, wait once" → in-process calls or a
  short-lived socket, not a new SHM primitive.

### Re-evaluation triggers (all must hold before implementation)

1. **At least two independent consumers** (not two styles from the same team)
   request a request/response capability that `latest`/`queue` plus a control
   socket cannot satisfy, with call rate, payload size, timeout, and failure
   semantics.
2. **Quantified latency/throughput targets:** p50/p99 end-to-end latency,
   maximum in-flight, allowed request-drop policy; and evidence that
   implementation A (`NotifyPipe` plus pool) can meet them.
3. **A multi-node requirement is confirmed**, and the transport seam is fixed;
   the service API must be transport-independent, and UDP remains excluded.
4. **A failure-semantics table is written and reviewed first:** timeout, cancel,
   duplicate correlation id, server crash, client crash, replay gate, queue
   full, version mismatch — each must specify a result (never "silently drop").
5. **Determinism/WCET argument:** the server side must not introduce unbounded
   queues or dynamic thread pools; request-handling threads, priorities,
   admission, and overrun policy must be statically analyzable; the hot path
   must not allocate.
6. **Security model:** who may call which method (uid/peer pid or signature),
   and how unauthorized requests are rejected and audited.
7. **ABI/lifecycle design:** magic/version/layout for any new page type, the
   upgrade matrix, read-only `thyme_probe` support, and crash reclamation.
   Prefer reusing the session lease rather than inventing another ownership
   model.
8. **Replay/recording interaction:** while replay owns the topology, service
   requests must be explicitly rejected or queued, never double-written with
   the real driver.

### Non-goals

- **No UDP.** Even multi-node must use a connection-oriented or
  acknowledgement-capable transport; UDP's disorder and loss conflict with
  thyme's explicit-drop model.
- **No dynamic discovery or registry.** Endpoints come from the static graph or
  configuration and are fixed at compile or deployment time; discovery is
  limited to read-only lab probing.
- **No general RPC/IDL/cross-language bindings.** Contracts use the existing
  `Frame<T>`/schema ABI; no protobuf/gRPC dependency.
- **No synchronous blocking on the data plane.** Waiting for a response inside
  `OnLoop`/`On<Channel>` breaks the scheduling contract; a future implementation
  may expose only an explicit asynchronous API driven by user threads, without
  changing "node = thread, callbacks serial".
- **No leases on rings/`LatestSlot`/`NotifyPipe`.** Leases belong only to the
  pinned-slot store.
- **No eCAL API compatibility.** Borrow the idea that services are
  first-class, not its transport or discovery model.

## Layering with existing primitives

### `ProducerGate` (per-channel writer ownership)

`ProducerGate` already answers "who writes this channel": `owner_token` plus
pid/starttime plus a heartbeat lease. A second writer gets `kBusy`; the real
driver gets `kGated` while replay holds the gate. A future service endpoint
should be **two gated channels**:

- request channel: client writes, server reads;
- response channel: server writes, client reads.

The gate protects only the write side, so no read-side lease is needed;
correlation and timeouts live in the protocol layer. This reuses the gate's
single-writer ownership semantics without adding leases to existing rings or
`LatestSlot`. A small request payload can use a dedicated service page with the
same gate header rather than changing existing primitive ABIs. While replay
holds the gate, both client requests and server responses return `kGated`, never
a silent double write.

### Session lease (topology-level ownership)

`ProducerGate` is a per-channel view; the session lease elevates "who is writing
this topology" to the session level (recorder/replay/real driver). Service
endpoints should join and leave as session **members**:

- endpoint registration declares `(service, method, contract_version, shm
  endpoint)` in the session lease rather than creating another registry;
- on session crash/restart the lease expires and is reclaimed; in-flight
  requests fail by timeout; a reattached server starts clean (old requests are
  not replayed);
- replay/recording switches follow the same ownership model: stop the writer,
  then transfer the lease; two sessions must never both believe they are the
  server.

Implementing service primitives before the session lease would create a second
ownership model; this is the main architectural reason for deferral.

## Consequences

- **Benefits:** no new ABI/page type/thread model for an unproven shape; the
  single-node SHM path keeps determinism and explicit failure semantics; ad-hoc
  RPC over data edges is not frozen into the core.
- **Costs (explicitly accepted):** there is no typed, multiplexable
  request/response primitive today; queries use a control socket or in-process
  calls, and cross-language/cross-machine support comes later.
- **Risks and mitigations:**
  - teams may each implement request/response over event edges, producing
    inconsistent private protocols. Mitigation: this ADR defines the A/B
    boundaries; new needs register against the triggers above instead of changing
    the core directly;
  - the control socket may grow into a de facto RPC. Mitigation: keep it
    low-frequency, operations-oriented, and versioned JSON; high-frequency or
    data-plane needs go through the trigger review.
- **Constraint on the transport seam:** it must express deadline, cancellation,
  error codes, and correlation id; an abstraction that only "sends bytes" cannot
  carry services later, but service implementation must not be pre-embedded in
  the seam.
- **Constraint on the session lease:** its member model must accommodate
  endpoints, not only channels, or the ownership model will need another change
  when service primitives land.
- **Documentation:** this ADR is the deliverable of the evaluation. Before
  implementation, a new ADR must record the selected option (A/B/C) and
  quantitative evidence; the historical conclusion here must not be rewritten.
