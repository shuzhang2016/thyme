# thyme architecture decision records (ADR) index

Each ADR records a landed (as-built) architecture decision: context (forces),
decision (verified against the current code), alternatives, and consequences
(including known limitations). New decisions take the next number; changing an
existing decision means adding a new ADR rather than rewriting history.

| Number | Title | Status | Related |
| --- | --- | --- | --- |
| [0001](0001-seqlock-payload-memory-model.md) | seqlock payload memory model | Proposed | Layered undefined-behavior strategy for seqlock/lease payload reads and writes |
| [0002](0002-core-language-and-allocation.md) | core language subset and allocation policy | Accepted | `-fno-exceptions -fno-rtti`, zero allocation on the hot path |
| [0003](0003-no-callback-group-executor.md) | no callback group / executor | Accepted | Static thread model; a thread is its own mutual-exclusion group |
| [0004](0004-frame-wire-format-and-two-paths.md) | Frame wire format and the Frame / PinnedSlot dual path | Accepted | 40 B header, 64 KB transport limit, version/flags discipline |
| [0005](0005-thyme-bin-record-format-and-package-layout.md) | thyme_bin record format and recording package layout | Accepted | wal/meta/frames package structure, footer index, integrity.manifest, schema sidecar, record manifest, continuous slicing |
| [0006](0006-trigger-fsm-and-accepted-semantics.md) | Trigger FSM and the "Accepted = queued" semantics | Accepted | accept/preempt/cancel/notify, no late error codes, mute/quota |
| [0007](0007-multi-process-supervisor-scope.md) | Multi-process supervisor (MP) scope and SHM topology ownership | Accepted | SHM creation/reclamation, stall detection, opt-in restart |
| [0008](0008-swap-ring-copy-on-trigger-deferral.md) | swap ring / copy-on-trigger: the steady-state copy trade-off for medium structs | Accepted | Keep copies in steady state; defer swap ring |
| [0009](0009-request-response-primitives.md) | request/response service primitive evaluation | Deferred | Continue with data edges plus a control socket; implementation triggers, non-goals (no UDP), and session lease layering |
| [0010](0010-transport-seam.md) | transport seam: fix the interface boundary before implementing a second transport | Proposed | The seven semantics a transport must provide (open/publish/subscribe/wake/lease/epoch/diagnostics), mapping to existing primitives, no UDP; order: headers, then types, then seam; ownership at the session layer |
| [0011](0011-sched-deadline-evaluation.md) | SCHED_DEADLINE evaluation and enablement gate | Accepted (not enabled) | Continue with FIFO+RM; only hard cyclic nodes are candidates; three parameters, system-level admission, non-candidate thread protection, and target-board comparison are mandatory gates |
| [0012](0012-pinned-slot-lease-copy.md) | PinnedSlot lease-copy write exclusion and crash recovery | Accepted | Single-word arbitration between copy-active and steal; plain `memcpy` only inside the actual write-exclusion window; owner-death recovery, no active-timeout stealing |
| [0013](0013-shm-spsc-ring-role.md) | Keep ShmSpscRing as the cross-process lossless queue role | Accepted | Backpressure on a full queue ring is irreplaceable; keep the v3 ABI; unification and deprecation require equivalent semantics and A/B evidence |
| [0014](0014-ring-unification-boundary.md) | Ring unification limited to cross-cutting mechanism, not delivery protocol | Accepted | No SwapRing/policy templates; queue/broadcast/latest/lease stay distinct; ChannelBase shares only cross-cutting mechanism |
| [0015](0015-per-backend-channel-types.md) | Split Channel types per backend | Accepted (in progress) | Policy type `Channel<T, Semantics, Policy>`; the generator emits includes for the backend the app actually uses; the umbrella header serves hand-written code only; no runtime erasure |
| [0016](0016-tick-transaction-model.md) | One-tick transaction model for cyclic nodes | Accepted (implemented) | Covers local queue, IndexPipe/LatestSlot, broadcast/event, and PinnedSlot; follow uses bounded last-wins; leases release at tick end; PinnedSlot outputs predeclare size via PrepareOutput; `OnLoop` remains only for nodes without edges |
