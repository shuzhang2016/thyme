# Cacheline / false-sharing audit (RT-10)

Scope: the SHM pages that two processes (or a producer and N readers) touch
per frame, plus the in-process ring whose hot words are trimmed by hand. The
goal is to document **why** each hot word sits where it does, record the
padding that is there on purpose, and make new padding a reviewable change.

This is an audit, not a restructuring: **no layout was changed** by RT-10.
Where a field pair shares a line, the reason is written down next to it in the
owning header.

## Method

`pahole` is not installed in the development container used for this audit, so
the table below was produced from the compiler's own layout computation plus a
`-Wpadded` scan (both are reproducible in any checkout):

```bash
# 1. Exact layout table (the same values the audit test pins):
cmake --build build --target test_cacheline_audit
./build/test_cacheline_audit --table

# 2. Padding scan (GCC). Every hit must be in the "padding" column below:
cat > /tmp/cacheline_scan.cpp <<'EOF'
#include "thyme/app/mp_control.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/record_mirror.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shared_health.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"
#include "thyme/runtime/spsc_ring.h"
#include "thyme/runtime/perf_log.h"
struct Pod8 { uint64_t a; };          // payload used in the table below
template struct thyme::LatestSlot<Pod8>;
template class thyme::SpscRing<4>;
template class thyme::ShmSpscRing<Pod8, 4>;
template class thyme::ShmSpmcRing<Pod8, 4>;
EOF
g++ -std=c++17 -O2 -I include -Wpadded -c /tmp/cacheline_scan.cpp -o /dev/null

# 3. When pahole is available (CI image / workstation), use it directly:
g++ -std=c++17 -g -O2 -I include -c /tmp/cacheline_scan.cpp -o /tmp/scan.o
pahole -C PinnedSlotPageHeader /tmp/scan.o
pahole -C RecordMirrorPage /tmp/scan.o
```

`-Wpadded` cannot be a CI gate: these layouts are *built* out of intentional
64-byte holes (one line per hot word), so every one of them is reported. What
the scan is good for is finding padding that nobody claimed. On the tree this
audit was written against it reports 53 warnings: the audited padding in the
table below plus the out-of-scope in-process types (`PinnedSlotReader::View`,
`Lease`, `HeaderSnapshot`, `PinnedSlotWriter::BeginView`, the `thyme::log`
types, `LatencyHistogram`). A hit on an **audited** struct that is not in the
table is an unreviewed layout change.

## Table

`sizeof` / `alignof` in bytes. `Pod8` = `{ uint64_t a; }`; ring capacities are
4 in the instantiations below. Offsets are the field's line: `line(n)` means
`offsetof % 64 == 0` at that line index.

| Struct | sizeof | alignof | Hot fields (line) | Padding | Why |
|---|---|---|---|---|---|
| `PinnedSlotPageHeader` | 384 | 64 | `ShmSuperHeader` (0), `head` (2), `epoch` (3), `gate` (4), `gate_aborted` (5) | 108..128 after `dropped_oversize`; 328..384 tail | ARCH-11 v4 super-header leads the page; one line per cross-process publish/wake word; meta blob starts at 384, so the header size is the channel ABI |
| `PinnedSlotHeader` | 64 | 64 | `seq`/`mono_ns`/`data_len`/`flags`/`crc32` (all line 0) | 32..64 (32 B tail); `reserved` (28..32) is declared ABI padding | The seqlock word and the metadata a reader validates must be one line fetch; the tail is declared ABI padding |
| `PinnedSlotFooter` | 64 | 64 | `magic`/`frame_index` (line 0) | 16..64 (48 B, `reserved[6]`) | Payload-complete marker written last; own line so the completion store does not bounce the slot header |
| `PinnedSlotLeaseHeader` | 128 | 64 | `ShmSuperHeader` (0), `pinned_skip`, `lease_steal`, `steal_timeout_ns` (1) | 104..128 (24 B, `reserved[3]`) | ARCH-11 v6 super-header; all lease-table-level counters read together by the writer and the probe |
| `PinnedSlotLeaseCell` | 64 | 64 | `pin_gen`, `epoch`, `copy_owner_pid`, `copy_owner_start` (line 0) | 32..64 (32 B, `reserved[4]`) | One reader-owned line per slot: Acquire/Release/copy-active CAS and the writer's pin/steal check must not share a line with the neighbouring cell |
| `ProducerGate` | 64 | 64 | owner token/pid/start-time, heartbeat | tail `reserved` | Own line inside the channel header; written by the replay holder, read by every producer |
| `HealthSlot` | 64 | 64 | `alive_counter`, `state`, `last_finish_mono_ns`, drop/miss/error counters, `sched_mismatch_cnt`, `memory_flags` | 44..64 (20 B) | One worker thread writes one slot per frame; the probe reads the array. Separate lines stop worker→worker false sharing. RT-23 `drop_cnt` is a 64-bit word at 24 |
| `SharedHealthPage::Header` | 64 | 64 | `session_id`, `worker_count`, `control` | 16..64 (48 B declared `reserved`) | Health identity cell after the ARCH-11 super-header; `slots[]` starts at 128 |
| `ShmSuperHeader` | 64 | 64 | `magic`, `abi_version`, `layout_hash`, `page_bytes`, `feature_bits` | 12..16 (`reserved0`), 40..64 (`reserved[3]`) | Shared ARCH-11 attach header: offset 0 of every migrated page; magic@0/version@8, reserved must be zero |
| `LatestSlotPageGeometry` | 64 | 64 | `payload_bytes`, `cell_stride`, `slot_count` | 24..64 (`reserved[4]`) | ARCH-11 v3 LatestSlot geometry at offset 64: lets the probe/any reader derive the cell offsets from the page instead of guessing from a manifest; published before the super-header magic |
| `SharedHealthPage` | 4224 | 64 | 64 × `HealthSlot` | none | 64 B super-header + 64 B health identity + one line per worker (`kMaxWorkers == 64`) |
| `MpSessionIdentityPage` | 128 | 64 | `ShmSuperHeader` (0), `config_hash`/`graph_hash`/`supervisor_start_ns`/pid/`health_n`/`role_count` (1) | 100..128 (24 B declared `reserved`) | ARCH-17 identity page: one line of identity words after the super-header; read once per role attach |
| `MpControl` | 320 | 64 | `stop`, trigger relay words, `proc_loops[]`, `cross_frames[]` | 288..320 (32 B declared `reserved`) | Deliberately **not** one field per line: it is a low-frequency supervisor/role counter page, not a per-frame line. The 64 B super-header owns the first line; the scalars fit in the second |
| `RecordMirrorSlotHeader` | 64 | 64 | `seq`, `mono_ns`, `payload_size`, `payload_offset` (line 0) | 4..8 (after the u32 `seq`); 32..64 (32 B tail) | Probe reads `payload_size`/`payload_offset` out of process; one line per struct keeps the seqlock + offset table together |
| `RecordMirrorPage::Header` | 64 | 64 | magic/version/session/count, `write_generation`, drop counters | 20..24 (4 B); 56..64 (8 B) | Counters and magic read together by the probe; offsets are page ABI |
| `RecordMirrorPage` | 2109568 | 64 | – | – | `super` line 0 (ARCH-11), `struct_table` line 2 (128), `slot_headers` line 130 (8320), `payload_arena` line 194 (12416): every sub-table starts on a line (ABI-11 128 B descriptor) |
| `RecordMirrorRingSlotHeader` | 64 | 64 | `seq`, `payload_size`, `mono_ns`, `publish_index` (line 0) | 32..64 (`reserved`, 32 B) | One sample per line: producer CAS + reader sample in a single line |
| `RecordMirrorRingDesc` | 128 | 64 | `ring_head`, geometry (line 0); per-struct `ProducerGate` (line 1) | 40..64 (24 B, after `reserved1`) | Ring head is written per publish; geometry is setup-only. REP-10 v6 gives each struct an independent gate cache line, so gating one struct does not block or share a hot gate line with another |
| `RecordMirrorRingPage::Header` | 128 | 64 | magic/version/session/count, `config_hash`, `arena_bytes`, `write_generation`, skip/drop counters | 72..128 (56 B) | Fields end at 72; the second line is headroom (see Findings) |
| `RecordMirrorRingPage` | 16576 | 64 | – | – | v6 fixed tables are `192 + 256*N` (default N=64): `super` line 0 (ARCH-11), `struct_table` line 3 (192), `ring_descs` line 131 (8384), arena line 259 (16576); each ring descriptor has geometry + gate lines |
| `LatestSlot<T>::Cell` | 64 | 64 | `T value` | rounds up to the line | The two payload cells must be on separate lines: the writer fills one while readers copy the other |
| `LatestSlot<Pod8>` | 192 | 64 | `published_idx_` + `seq_` (line 2), `slot_[0]` (line 0), `slot_[1]` (line 1) | 154..192 (38 B, process-local tail) | Reader loads `seq_` and `published_idx_` from the same line (one fetch per attempt). The tap pointer shares that line but is written only by `SetRecordTap` at setup |
| `ShmSpscRing<Pod8,4>::Layout` | 576 | 64 | `ShmSuperHeader` (0), `cap` (1), `head` (2), `tail` (3), `epoch`/`epoch_waiters`/`push_fail` (4), `slots` (5) | 268..320 in the epoch line | ARCH-11 v4: super-header leads the page; `cap` gets its own line; producer (`head`), consumer (`tail`) and the wake decision (`epoch`+waiters) each own a line; payload cells start at 320 |
| `ShmSpmcRing<Pod8,4>::Layout` | 576 | 64 | `ShmSuperHeader` (0), `cap` (1), `head` (2), `epoch`+`epoch_waiters`+`reader_drops`+`reader_slot_mask` (3), `reader_drop_slots` (4), `slots` (5) | 212..256 after `reader_slot_mask` | ARCH-11 v4: super-header leads the page; single writer owns `head`; the futex word and sleeper count share a line for the wake decision (PERF-14); per-reader drop accounting uses one diagnostic cache line at 256; payload cells start at 320 |
| `SpscRing<4>` (in-process) | 320 | 64 | `head_` (line 0), `tail_` (line 1) | 4..64 and 4..64 below each word; `buf_` starts line 2 | Producer/consumer each mutate their own line; the buffer and the condvar state follow |

Perf records (not line-padded, producer-only, dump-written):

| Struct | sizeof | Padding | Why |
|---|---|---|---|
| `PerfSampler::Sample` | 12 | 2 B (`pad[2]`) | 10 B wire record + 2 B pad; the in-memory layout **is** the perf-file ABI |
| `PerfWorkLog::Rec` | 12 | 2 B tail | Same rule for the 10 B work-log record |

## Findings

The audit records the intentional line couplings and declared padding/reserved
areas below. REP-10 v6 expanded only `RecordMirrorRingDesc` to give each
struct's replay gate a separate cache line:

1. **`epoch` + `epoch_waiters` share a line** (`ShmSpmcRing`, `ShmSpscRing`).
   Deliberate: the producer bumps `epoch` and immediately reads
   `epoch_waiters` to decide whether to issue a wake syscall; readers
   `fetch_add`/`fetch_sub` the same count around `FutexWait`. Splitting them
   would turn one line transfer into two on every publish (PERF-14 measured
   this path). The registration traffic on the shared line is bounded by the
   number of `WaitUntil` calls, not by publish rate.
2. **`LatestSlot` control tail sits on the `seq_` line.** `tap_page_` /
   `tap_struct_id_` are written by `SetRecordTap()` during wiring and only
   read on the (already off-hot-path) record tap. The alternative — a fourth
   line — would grow every `LatestSlot` region by 64 B per channel and change
   the region size recorded in the manifest, for no runtime benefit.
3. **`RecordMirrorRingPage::Header` has 56 B of tail headroom** (fields end at
   offset 72, `sizeof` rounds to 128). Reusing that hole for a new counter
   would keep `sizeof`/offsets stable (allowed by
   [abi-compatibility.md](abi-compatibility.md)), but it is still a page
   change: update this table and the layout asserts in the same commit.
4. **`MpControl` groups its counters after one super-header line.** The page is
   intentionally not padded per field: no field on it is touched per frame.
   The explicit 32 B `reserved[4]` tail keeps the page at 320 B and gives a
   future counter a declared slot instead of silently moving an existing one.
5. **`LatestSlot` process-local tail** (38 B at 154..192) is not part of the
   wire data; it exists so `seq_` stays 8-aligned at layout v2 (RT-03).
6. **Perf records carry 2 B of tail padding** because the 10 B record layout
   is the perf-file ABI; they are written by one sampler thread and never
   shared.
7. **`SpscRing`'s condvar/mutex block shares a line with the buffer tail** in
   small-capacity instantiations. It is only touched on the slow path
   (`WaitUntil`/`Notify`), never in `push_latest`/`try_pop`, so no change was
   made.

## Automation

`tests/test_cacheline_audit.cpp` (**mr_gate**) pins every number in this table
with `static_assert` (plus the "hot word owns a line" invariants), and
`--table` reprints the table. New padding — a field that pushes a later field
off its line, or that grows a page — fails that test, which is the signal to
update this document and the ABI notes together. The `-Wpadded`/`pahole` scan
stays a manual review step (procedure above, and in
[test-matrix.md](../guides/test-matrix.md)) because it flags the intentional holes.
