# As-built deviations

> Pattern: [`triggered-acquisition-design.md` §12](../design/triggered-acquisition-design.md#12-implementation-status-as-built)
> — the design text stays as written; what actually shipped is recorded here.
> The per-subsystem list in §12.2 remains authoritative for the triggered
> acquisition design; this page is the repository-wide log.

A row exists when a document claims behaviour that the code does not (or does
not yet) provide. **Status** is one of:

| Status | Meaning |
|---|---|
| `aligned` | The claim was wrong or incomplete and the document has been corrected to the as-built behaviour. |
| `deviated` | Code and document still differ deliberately; the deviation is the accepted design. |
| `deferred` | The feature is not implemented yet and is tracked as follow-up work. |

| # | Area | Design claim (source) | As built (evidence) | Status | Reference |
|---|---|---|---|---|---|
| 1 | Trigger FSM log durability | ADR 0006 §6 (before the durability change): one-second append to `.fsm` followed by `fflush`; a crash loses at most one dump period | One unbuffered `write()` per line with `O_APPEND`, rotated by `fsm_max_bytes`/`fsm_rotate_keep`; a crash loses at most the in-flight line (`src/record/trigger_center.cpp`) | aligned | ADR 0006 §6 |
| 2 | "Lock-free transports" | README / ARCHITECTURE once described the transport end to end as lock-free | Only the data plane is lock-free: same-process waits use mutex/condvar (EventThread: futex), cross-process waits are futex syscalls; torn latest-value reads may retry and return no value (`ARCHITECTURE.md` §7.1) | aligned | `docs/reference/glossary.md` |
| 3 | Primitive naming | `PinnedSlot*` was to be renamed to `PinnedSlotStore*`, with old names kept as deprecated aliases for one release | Code still ships only `PinnedSlot*`; there is no `PinnedSlotStore` symbol in `include/` or `src/` | deferred | Documentation-only |
| 4 | MP supervisor restart | Supervisory restart: a stalled/exited role is re-forked with backoff and a per-window budget | Restart is opt-in: `restart_enabled = false` and `restart_stall_timeout_ns = 0` by default, so a failed role fails the run gate instead of being restarted (`include/thyme/app/mp_supervisor.h`) | deviated (by design) | ADR 0007 §3 |
| 5 | Replay producer gate scope | Real producers are gated during replay so replay cannot double-write a live channel | PinnedSlot gates live in `PinnedSlotPageHeader` (v4); small mirror gates live per struct in `RecordMirrorRingDesc` (ring page v6), so only replay targets are blocked and other structs on the same page remain writable | aligned | Per-struct ring gate; `docs/guides/replay.md` §2 |
| 6 | SHM page self-identification | ABI rules: pages carry magic/version so a peer can reject a mismatched layout at attach | `LatestSlot<T>` v3 is a described page: `THYLSLOT` magic + `ShmSuperHeader` + 64 B geometry block; generator/supervisor/role/probe all validate it, and legacy v2 body pages are rejected (no dual-read). The in-process body still has no header (layout v2) | aligned | ABI rule 6; `include/thyme/runtime/latest_slot.h` |
| 7 | Cross-slice continuous replay | `thyme-replay play <dir\|pkg...>` was to open `continuous_*` slices in `slice_seq` order, with `--timeline=absolute\|stitch` and `slice_joins/join_duplicates/join_holes` counters | `play` accepts exactly one `<pkg>`; there is no playlist discovery, timeline mode, or join counters (`tools/replay/thyme-replay/replay_cli.cpp`) | deferred | — |
| 8 | Alert coverage on observability metrics | Alert rules ship for the exported metric set | All 28 rules are live. Recorder status JSON exports `CenterStats` drops and recorder `Stats`; perf type-8/9 export series names and node period/deadline; status health includes actual error codes | aligned | `docs/guides/observability-dashboards.md` §4, §7 |
| 9 | Log budget scope | `[logging].total_budget_mb` bounds log disk usage | It is a **per-process** cap over audit + FSM + perf; an N-process run can consume N× the budget because the counter is not shared through `MpControl` | deviated (documented) | `docs/guides/trigger-config.md` §`logging` |
| 10 | Recorder control-plane credentials | Only a same-euid peer can command the recorder (Linux `SCM_CREDENTIALS`) | Previously a datagram without credentials (`peer_uid == -1`) was treated as "nothing to check" and **accepted**. Now Linux requires credentials and rejects with `auth_rejects_`; `SO_PASSCRED` failure fails `Open()` (fail closed). Apple keeps the historical accept-any-peer behaviour when `LOCAL_PEERCRED` is unavailable | aligned | `src/record/record_control.cpp`; `docs/reference/error-handling-audit.md` E10/E11 |
| 11 | Shared-memory page coverage | Every cross-process SHM object carries a `ShmSuperHeader` so attach validates magic/version/layout hash/page size/features | All payload-bearing pages are described: health, control, LatestSlot, SPSC/SPMC rings, both record-mirror pages, PinnedSlot channel and lease. The only exception is the bare 4-byte futex wake word used by EventThread hubs: it has no payload or layout, is mapped independently, and is validated by the accompanying ring/channel page; wrapping it would add a page whose only field is the word it already is | aligned (documented exception) | `docs/reference/abi-compatibility.md` §SHM page layout_hash; `include/thyme/runtime/futex.h` |

## Maintaining this log

- A change that alters behaviour described in a design or architecture document
  adds a row **in the same change** — as `deviated` (deliberate) or `deferred`
  (not implemented yet), never by silently editing the design text.
- When the code catches up, flip the row to `aligned`, record the closing
  change in **Reference**, and keep the history instead of deleting the row.
- Rows that describe an ABI limitation must agree with
  [`abi-compatibility.md`](abi-compatibility.md) and the matrix in
  [`ARCHITECTURE.md` §8](../../ARCHITECTURE.md#8-abi-compatibility).

## Related

- [`triggered-acquisition-design.md` §12](../design/triggered-acquisition-design.md#12-implementation-status-as-built) — per-area implementation status and the original deviation list
- [`abi-compatibility.md`](abi-compatibility.md) — versioning rules referenced above
- [`upgrade.md`](../guides/upgrade.md) — the upgrade order used as the guard for unversioned pages
- [`adr/`](../design/adr/README.md) — accepted decisions (ADR 0006, 0007, 0009, 0010)
- [`error-handling-audit.md`](error-handling-audit.md) — silent-drop audit; new fail-closed paths are recorded here when they change documented behaviour
- [`onboarding.md`](../guides/onboarding.md) · [`runbook.md`](../guides/runbook.md) — entry points that link here
