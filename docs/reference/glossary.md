# thyme glossary

Terms a newcomer meets first, one to three sentences each, with their code
location. Architecture context: [ARCHITECTURE.md](../../ARCHITECTURE.md) §7.
The names below are the **actual names in the code today**.

## 1. Transport primitives

The primary classification is **ring / slot store / latest slot**, not
SPSC/SPMC. In the table, the first column is the conceptual name, the second is
the C++ type/header; delivery semantics differ, so the name and the type must be
stated together.

| Name | Code | Delivery semantics | Full ring and lifetime |
|---|---|---|---|
| **In-process index ring** | `SpscRing` (`include/thyme/runtime/spsc_ring.h`) | In-process index FIFO, paired with pool generations | drop-oldest; not a cross-process lossless queue |
| **Cross-process queue ring** | `ShmSpscRing` (`include/thyme/runtime/shm_spsc_ring.h`) | Cross-process value FIFO, single writer/single reader, ordered and lossless | returns false plus `push_fail` when full; the caller applies backpressure or drops explicitly |
| **Cross-process broadcast ring** | `ShmSpmcRing` (`include/thyme/runtime/shm_spmc_ring.h`) | Single writer, multiple readers; each reader has an independent cursor | overwrites the oldest; a slow reader may drop and count |
| **Latest slot** | `LatestSlot<T>` (`include/thyme/runtime/latest_slot.h`) | latest-wins single-writer snapshot with a two-slot seqlock | overwrites directly; readers retry a bounded number of times and return false when exhausted |
| **Pinned slot store** | `PinnedSlotWriter` / `PinnedSlotReader` (`include/thyme/runtime/pinned_slot_store.h`) | Runtime-sized large slots, random frame index, zero-copy reads | separate lease page, pin generation, copy-active, pinned-skip, and owner-death reclamation |
| **Record mirror ring** | `RecordMirrorRingPage` (`include/thyme/runtime/record_mirror_ring.h`) | Per-struct indexed snapshot ring on the recording side path, not a general transport | may overwrite and count mirror drops; lifetime is tied to the recording tap |
| **Slot store** | No such code type | A classification concept for index-addressed slot storage | PinnedSlot is its lease specialization; do not invent a general protocol template from it |
| **Frame** | `include/thyme/contracts/frame.h`, `include/thyme/contracts/frame_header.h` | Transport wrapper for payloads ≤ 64 KB: bare POD payload plus a 40 B `FrameHeader` (magic/version/size/mono_ns/seq/valid/CRC); larger payloads use the pinned-slot path | Small-structure transport and leased large slots are two explicit paths |
| **data-valid (`FrameHeader.valid`)** | `include/thyme/contracts/frame_header.h`, [abi-compatibility.md](abi-compatibility.md) rule 9 | `valid == 1` means the payload under the header has been fully written and is stable. The writer sets it after payload completion (seal / `FrameMarkValid`); a frame with `valid == 0` must not be read as payload. It is not header structural validity (that is described by magic/version/reserved/size), and it is not a copy of magic. `FrameHeaderOk` enforces `valid == 1` at the data boundary; use `FrameHeaderStructurallyOk` for header-metadata-only checks. | Data validity is independent of header metadata validation |

## 2. Concurrency and lifetime

| Term | Meaning | Code |
|---|---|---|
| **Lease** | A timed slot reservation: it must be released explicitly. If a reader dies, the writer reclaims the slot after the steal timeout (default 5 s). A lease cell packs (generation, copy_active, pin_count) into one word plus a stamped epoch; with v6 (v5 semantics), copy-active may only be reclaimed after pid/start evidence proves the copier is dead. | `PinnedSlotReader::AcquireLease` / `ReleaseLease`, `kPinnedSlotLeaseDefaultStealTimeoutNs` (`include/thyme/runtime/pinned_slot_store.h`) |
| **Loan** | A borrowed value without an expiry (iceoryx / DDS / ROS 2 terminology). thyme's reader-side slot reservation is **not** a loan: it has a timeout, an epoch, and steal semantics. | The closest analogue is `ObjectPool::acquire` plus release (`include/thyme/runtime/object_pool.h`) |
| **seqlock** | Sequence lock: the writer moves the sequence to "write in progress" and then back to a stable value; the reader samples the sequence before and after copying and retries if it changed. Two variants exist in the tree: `LatestSlot::seq_` uses odd/even; `PinnedSlotHeader::seq` uses 0 for "write in progress" and `frame_index + 1` otherwise. | `include/thyme/runtime/latest_slot.h`, `include/thyme/runtime/pinned_slot_store.h`; memory-model rationale in [adr/0001-seqlock-payload-memory-model.md](../design/adr/0001-seqlock-payload-memory-model.md) |
| **seq / epoch** | `seq` is a publication sequence / seqlock counter. `epoch` has two meanings: the **futex wake word** in SHM ring and PinnedSlot channel headers (incremented on every publish; waiters `FUTEX_WAIT` on it), and the **stamp time** in a lease cell (monotonic ns of the last successful acquire, used to decide stealing). | Header comments in `include/thyme/runtime/shm_spsc_ring.h` and `shm_spmc_ring.h`; `PinnedSlotPageHeader::epoch` / `PinnedSlotLeaseCell::epoch` (`pinned_slot_store.h`) |
| **watermark** | A persisted position. `PerfSampler::flushed_` is the absolute sample-sequence watermark for `FlushNew`. Continuous capture records the mono_ns watermark of the last written row in each `small.tbin` slice; samples arriving after the watermark are downgraded to GAP. | `include/thyme/runtime/perf_log.h` (`FlushNew`), `include/thyme/record/triggered_recorder.h` (per-slice watermark) |
| **wake hub** | The wait abstraction behind `EventThread::WaitAny`: every input `Waitable` points at the same hub via `SetNotify`, and any ready input wakes the thread. On Linux it waits on one futex word (which `SetWakeWord` can redirect into SHM so a cross-process producer can wake it); macOS falls back to a condvar. | `include/thyme/runtime/event_thread.h`, `src/runtime/event_thread.cpp` |

## 3. Recording and observability

| Term | Meaning | Code |
|---|---|---|
| **package** | The capture package directory produced by one trigger: `<out_dir>/<trigger_id>_<t0>/` containing `meta.tbin` (trigger metadata plus the manifest block), `small.tbin` (small-structure samples, including GAP holes), `frames/` (raw large frames plus index), `wal.log` (on-disk journal; complete only with a seal line), `schema.json` (record schema sidecar), and `integrity.manifest`. | `include/thyme/record/triggered_recorder.h` (package layout comments, `PackageComplete()`); design in [triggered-acquisition-design.md](../design/triggered-acquisition-design.md) §6.1 |
| **slice** | A rolling continuous-capture package `continuous_<seq>_<ts>/`, closed according to `slice_mb`; parsing validates `0 < slice_mb <= disk_quota_mb`. | `include/thyme/record/triggered_recorder.h`; configuration in [trigger-config.md](../guides/trigger-config.md) `[continuous]` |
| **trigger FSM** | Two layers. The trigger center has `State {kIdle, kCapturing, kMuted}` (preemption/mute/quota decisions); the capture engine has `Phase {kArmed, kPost, kSettle}` (Armed→Post→Settle→Armed). | `include/thyme/record/trigger_center.h`, `include/thyme/record/triggered_recorder.h`; transition diagram in [triggered-acquisition-design.md](../design/triggered-acquisition-design.md) §5.3, configuration in [trigger-config.md](../guides/trigger-config.md) |
| **record manifest** | The generated identity of the ordered recorded-channel table (`kRecordManifestHash` plus version). `RecordStructId` is a row index; adding or removing a recorded channel renumbers later ids (an ABI change). It is written into the `.tbin` v3 block, `schema.json`, and perf-log type 7; readers and analysers compare the hash and reject a mismatch. | `include/thyme/record/record_schema.h`, `include/thyme/record/thyme_bin.h`, `generated/record_manifest.h` |
| **schema sidecar** | Package-level `<pkg>/schema.json`: `thyme-record-schema` v1, containing the manifest identity, the 40 B `FrameHeader` field table, and each record's field layout (`payload_offset` plus field offset/size/count/stride). Written at seal and covered by the integrity manifest; absent means legacy, tampered means rejected. Tools can decode without compiling thyme. | `include/thyme/record/record_schema.h`, `thyme_probe schema`, `thyme-replay schema` |
| **perf log block types** | Block types in a perf log file: 1 = samples (10 B latency samples); 2 = percentile (p50/p95/p99/max); 3 = work (dispatch duration); 4 = epoch (base for the u32 timeline, which wraps roughly every 19.5 h); 5 = sys (ProcSampler process counter deltas plus instantaneous queue depths); 6 = histogram (fixed-bucket histogram with p50/p90/p99/p999 and exact max; bucket counts can be merged across processes); 7 = record manifest (manifest hash/version, one block per file generation; `thyme_log_merge` refuses to merge mismatched files). Older parsers stop at an unknown block type. | `include/thyme/runtime/perf_log.h`, `include/thyme/runtime/histogram.h`, `include/thyme/runtime/proc_sampler.h`, `include/thyme/record/record_schema.h` |

## 4. Scope of "lock-free"

The consistent statement is: **the data plane is lock-free; same-process waits
use mutex/condvar, cross-process waits use the futex syscall.** "Lock-free"
describes only the data plane, not end-to-end delivery:

- **Data plane (lock-free):** slot access and publication use only atomic
  load/store with release/acquire fences, and no lock is held while copying a
  payload. A slow consumer cannot hold a lock the producer needs (the push /
  Begin / Commit paths of the rings, `LatestSlot`, and the pinned-slot store).
- **Same-process waits:** `SpscRing` (which `NotifPipe` forwards through)
  implements `WaitUntil` with `pthread_mutex_t` + `pthread_cond_t`; `Notify`
  skips the mutex when there are no waiters (`include/thyme/runtime/spsc_ring.h`).
  Exception: the `EventThread` wake hub on Linux waits directly on a futex word;
  only macOS falls back to a condvar (`src/runtime/event_thread.cpp`).
- **Cross-process wakeups:** SHM rings, pinned-slot waiters, and a wake hub
  redirected to SHM all block in `SYS_futex` (`include/thyme/runtime/futex.h`);
  non-Linux platforms fall back to polling (`include/thyme/runtime/shm_spsc_ring.h`).
- **Not end-to-end wait-free:** a publisher may enter a futex syscall to wake a
  consumer, and consumer progress depends on the scheduler. Latest-style readers
  (`LatestSlot`, the pinned-slot store, `RecordMirrorRing`) retry a bounded
  number of times on a torn copy and return false when exhausted.

Detailed rationale: [ARCHITECTURE.md §7.1](../../ARCHITECTURE.md#71-synchronization-contract).
