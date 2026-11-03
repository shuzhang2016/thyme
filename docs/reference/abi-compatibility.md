# thyme ABI / SHM compatibility matrix

> Only versions that reject mixed runs are listed. Layout changes bump the
> version; **semantic changes also bump it**, even when the byte layout is
> unchanged.

## Current versions

| Object | Constant | Current | Notes |
|---|---|---|---|
| `FrameHeader` | `sizeof == 40` | v1 | `reserved[0]` bit 0 = `kFrameFlagCrcDisabled`; `crc32 = 0` means disabled only when that flag is set; all other reserved bits must be 0, and `FrameHeaderOk` rejects frames with undefined bits. `valid` is **data-valid** (payload fully written and stable), not header structural validity; `FrameHeaderOk` requires `valid == 1`, while `FrameHeaderStructurallyOk` checks only the header. |
| PinnedSlot payload region | `kPinnedSlotStoreVersion` | **4** | v4: 64 B `ShmSuperHeader` plus the existing gated body; `page_bytes == sizeof(PinnedSlotPageHeader) == 384`; the meta blob and slots follow `MetaOffset()`. `layout_hash` binds the super/body offsets and `ProducerGate` size; v3 and older are rejected. `PinnedSlotWriter::Create` defaults transport CRC **off** (`SetCrcEnabled(true)` overrides); readers accept both flag values. Not a mixed-run ABI. |
| PinnedSlot lease table | `kPinnedSlotLeaseVersion` | **6** | v6: 64 B `ShmSuperHeader` plus the v5 lease body; `page_bytes == sizeof(PinnedSlotLeaseHeader) == 128`; lease cells follow the header. v5 copy-active/pid/start recovery semantics are unchanged; v5/v4/v3/v2 are rejected. |
| `ShmSpscRing` | `kShmSpscRingVersion` | **4** | v4: 64 B super-header; `cap`@64, `head`@128, `tail`@192, `epoch`@256, payload@320; `layout_hash` binds `sizeof(T)`/`alignof(T)`/`Cap` and all offsets; v3 pages are rejected with no dual-read. |
| `ShmSpmcRing` | `kShmSpmcRingVersion` | **4** | v4: 64 B super-header; `cap`@64, `head`@128, `epoch`@192, `reader_drop_slots`@256, payload@320; `layout_hash` binds size/alignment/Cap and all offsets. v3 `reader_slot_mask`/per-reader drop semantics are unchanged; v3 pages are rejected. |
| `LatestSlot<T>` SHM page | `kLatestSlotPageMagic` / `kLatestSlotPageAbiVersion` | **3** (described page); in-process body remains layout v2 | v3 wraps the unchanged v2 seqlock body after a 64 B `ShmSuperHeader` and a 64 B `LatestSlotPageGeometry` (payload_bytes/cell_stride/slot_count/reserved): payload cells@128, `published_idx_`/`seq_` follow the body, region = `128 + 2*RoundUp64(sizeof(T)) + 64`. Geometry lets a probe that sees only an opaque `layout_hash` read geometry directly. `layout_hash` binds page geometry and the caller's explicit `payload_layout_hash` (0 rejected; generated code takes it from `contract_layout.h` and asserts nonzero). Generator/supervisor/role/probe all use `LatestSlotLayoutModel<T>`; probe identifies via `THYLSLOT` magic; bare v2 bodies are rejected with no dual-read. |
| `RecordMirrorRingPage` | magic/version | **6** | v6: each `RecordMirrorRingDesc` grows from 64 B to 128 B and embeds a `ProducerGate` at offset 64; the header stays 128 B; fixed tables are `192 + 256*N`; the arena follows `ArenaBase()`. `layout_hash` binds page/ring descriptor sizes, gate offset/size, table offsets, and `kRecordMirrorRingMaxStructs`. `TryWriteRing` optionally takes a `writer_token`; a zero token is rejected by a live gate, the owner may write, and a failed pre-publish re-check returns the seqlock to even without advancing `ring_head`/`write_generation`. Statistics reuse each gate's `rejects`/`steals`; no new abort field. `RecordMirrorPage` stays at **v4**; v5 and older ring pages are rejected. |
| `ThymeStructDesc` registry entry | `kThymeStructDescVersion` | **2** | v1 is 40 B (struct_id/contract_version/payload_size/name); v2 is 128 B and adds `layout_hash` (FNV-1a 64 over the field model), `channel[32]`, and `record_key[48]`. Container version implies entry version: `.tbin < 4` and old mirror pages use v1; `.tbin 4` / `RecordMirrorPage 4` / `RecordMirrorRingPage 6` use v2. `layout_hash == 0` means unknown (host has no `contract_layout.h`); two nonzero values that differ mean incompatible layouts and mapping is forbidden. |
| `SharedHealthPage` | `kVersion` | **3** | v3: super-header at 0..63, a 64 B health identity block at 64 (`session_id`/`worker_count`/`control`), `slots[64]` from 128 (cacheline aligned), `sizeof == 4224`. `HealthSlot::drop_cnt` becomes `atomic<uint64_t>` (offset 24; `error_code` moves to 32), so increments above `2^32` accumulate exactly. Attach validates magic/abi/layout_hash/page_bytes/feature_bits/reserved; v2 (`sizeof == 4160`, no super-header) is rejected. |
| `MpSessionIdentityPage` | `kMpSessionIdentityVersion` | **1** | A separate description page `<prefix>_id`: super-header plus `config_hash`/`graph_hash`/`supervisor_pid`/`supervisor_start_ns`/`health_n`; roles compare each field after attach; a missing page only falls back with a warning for pre-super-header sessions. Does not change the frozen health/control page layouts. |
| `SessionLeasePage` | `kSessionLeaseVersion` | **1** | Topology writer lease, 384 B (super-header + holder row + 32 channel members). A live second writer gets `kBusy` and does not replace the holder; a dead process or expired heartbeat is reclaimed as `kStolen`. Does not change `ProducerGate` layout. |
| `MpControl` | `kMpControlAbiVersion` | **3** | v3: explicit `THYME_MP` magic@0 and super-header@0..63; scalars/counters move after offset 64 (`stop`@64, trigger relay@68..95, `proc_loops`@96, `cross_frames`@224, explicit `reserved[4]`@288), `sizeof == 320`, `alignof == 64`. Supervisor creates with `CreateDescribed`, roles open with `OpenDescribed` and validate magic/abi/layout/page_bytes/features/reserved; v2 (bare `abi_version`@0, 232 B, no magic) is rejected. |
| `ShmPageSpec` / `ShmSuperHeader` | `ShmPageSpec` (no single version constant) | Unified page description | `magic`, `abi_version`, `layout_hash`, `page_bytes`, `feature_bits`. `CreateDescribed/OpenDescribed` first maps only the 64 B header and validates it completely (`kNotReady` retries within a timeout; other errors are precise), then maps the declared logical page and rechecks the full header, so page replacement between mappings is rejected. Errors: `kBadMagic/kBadAbiVersion/kBadLayoutHash/kBadPageBytes/kBadFeatureBits/kBadReserved/kBadGeometry/kTooSmall`; `FormatShmPageError` prints expected/actual. `SharedHealthPage`, `MpControl`, and v3 `LatestSlot` are migrated; other pages follow in batches. |
| Trigger control socket | `trigger_proto::kTriggerVersion` | **4** | v4 adds `request_id` (8 B) and a new CRC after v3; requests 104→120 B, responses 32→48 B; the old v3 CRC slot becomes reserved-zero and existing offsets are unchanged. Identical messages with the same `(uid,pid,request_id)` hit a bounded dedup window and return the first result without re-execution; an ID conflict is rejected. v3 still uses sequence+CRC replay protection; v1/v2 require explicit compatibility flags and gain no idempotent retry. Cancel/wait flags work in v3/v4. |
| `integrity.manifest` | `integrity::kManifestVersion` | 1 | 120 B header + sorted entries (path/size/sha256) + chain root; little-endian, atomic write. v1 layout enables flags bit 1 = partial; bit 2..3 = full/metadata/off/mixed; entry layout is unchanged, unhashed entries use a 32-byte zero digest, and the file list + size still enter the chain/HMAC. Full mode bits are 0, so the default manifest/HMAC is byte-identical to previous output. Old verifiers reject unknown flags rather than misreporting a degraded package as full. Missing = `absent`; parse/chain failure = `invalid`. Read-only and not a mixed-run ABI. |
| `.tbin` / `.thyme.bin` | magic/version | See `thyme_bin.h` | Persistent format with a tri-state reader. Version 0 is undefined and >4 is future; both reject with `ok() == false`. v1 (no footer index) reads sequentially; v2 has an index; v3 adds the manifest block after the struct registry (registry still 40 B v1); v4 uses 128 B `ThymeStructDesc` v2 and always carries the manifest block. Writers emit v4 only when the host provides a manifest; otherwise v2/v1 bytes are unchanged. v1/v2/v3 packages use the legacy registry (`channel`/`record_key` empty, `layout_hash = 0`, mapped by struct_id). Registry `contract_version == 0` is rejected. |
| `FrameIdx` in `frames/*.index.tbin` | `sizeof(FrameIndexRecord)` | **2** (32 B) | v2 = 32 B (`offset u64` / `len u32` / `flags u32` / `payload_crc u32` / `reserved u32` / `producer_frame_index u64`); v1 = 24 B. A v2 reader accepts 24 or 32 bytes according to the record's `payload_size`; a v1 row's `producer_frame_index` reads as 0; other sizes are rejected. `producer_frame_index` is the `PinnedSlotReader::View::frame_index` (monotonic from 0, wraps across rings); `source_sequence` remains a slot index that wraps and is not a frame number. |
| `frames/*.raw` header | `detail::kRawFileVersion` | **2** | `.raw` starts with a 128 B header (magic `THYRAW1`, version, header_bytes, channel_key_bytes, flags, `channel_key[32]`, `profile_version`/`profile_bytes`/`params_digest`/`profile[56]`). v1 is 64 B (no profile slot); readers skip by `header_bytes`. Index `offset` remains an absolute file offset. The header identifies the channel and an optional opaque profile, not `layout_hash`/`struct_id`. Headerless old packages (first `offset=0`) remain readable. |
| `wal.log` | `kWalOpClockAnchor` | additive op 1 | `WalRow` stays 24 B; an anchor row has `index=CLOCK_MONOTONIC`, `mono_ns=CLOCK_REALTIME`, and zero elsewhere. Written at creation, periodically, and before seal; the last row is still seal/abort and completeness is unchanged. Old replay/retention only inspect descriptors or the last op and can read new packages; old packages have no anchor, and the read API returns an empty list. Truncated rows, invalid anchor fields, or mono rollback are rejected. |
| manifest block in `.tbin` | `kThymeBinManifestBlockVersion` | **1** | 32 B (`THYMANI1` magic + manifest_version + hash + CRC32) immediately after the struct registry, carried by file version 3+. Any magic/version/reserved/CRC mismatch = `ManifestState::kInvalid` and the reader rejects the whole file. v1/v2 files without the block = `kAbsent`; v4 files always have it. |
| record manifest identity | `schema::kManifestVersion` | **1** | `kRecordManifestHash` (ordered recorded-channel table) + manifest version, written into the `.tbin` block, `schema.json`, and perf type-7 block. **`RecordStructId` is a manifest row number; adding/removing recorded channels renumbers later ids, an ABI change.** Readers and perf analysers must reject a mismatch (`manifest-mismatch` / exit 4), never decode with different ids. |
| package schema sidecar | `schema::kVersion` | **3** | `<pkg>/schema.json` (`thyme-record-schema` + `schema_version` + `partial` + `record_manifest` + `frame_header` + `records[]` + `payload_profiles[]`). v1 has `record_manifest.channels = null`; v2 writes `channels[]` sorted by struct_id; v3 requires `payload_profiles[]` (channel/profile or null, profile_version, params_digest or null); the core stores only opaque identifiers. Readers accept v1/v2/v3; v1/v2 must not carry non-empty `payload_profiles`. Written at seal before the integrity manifest; `.incomplete` packages get a partial version; missing = pre-ABI-09 legacy; unparseable/tampered = `kInvalid` → `bad-schema`. |
| perf log | block type 7 (`kPerfManifestBlockType`) | 12 B record layout | `manifest_version u16 / reserved u16=0 / hash u64`, once per file generation. `thyme_log_merge` pre-scans all inputs: different hash/version in one merge = exit 4; `--expect-hash` mismatch or a legacy input (no block) also exits 4; old parsers stop at an unknown block type. |
| perf log | block type 10 (`kPerfClockAnchorBlockType`) | additive, 16 B record layout | `mono_ns u64 / realtime_ns u64`, `node=0`, `count=1`, refreshed on every non-empty periodic dump and carried by each file generation and the first dump after rotation. `thyme_log_merge` attaches `realtime_ns` per input file using its latest anchor; the mono timeline and old sample fields are unchanged; legacy files without an anchor or overflow emit no wall clock. Truncation, bad headers, or mono rollback stop at a corrupt tail; unknown types follow the stop rule. |

Record-mirror assembly boundary: generated scaffold selects
`RecordMirrorRingPage` **v6** through a non-empty
`MpSupervisorConfig::record_ring_specs`; an empty value keeps the older
`RecordMirrorPage` **v4**. A role's six-argument
`Open(..., record_ring=true)` and the five-argument old entry preserve that
boundary. The two magic/version pairs must not mix on one SHM; old attachers
reject new pages. The ring region is
`RequiredBytes(recorded specs) = sizeof(RecordMirrorRingPage) + recorded arena`,
not the fixed 2 MiB arena. Migrating does not change sealed `.tbin` reading or
old-package compatibility.

**Sequence semantics (layout unchanged):**
`RecordMirrorRingSlotHeader::source_sequence` stays `uint32_t`. It is a
producer-local wire-provenance sequence that wraps at `2^32`; it is not a global
unique sample key and must not be used for sorting, deduplication, or
newness. Each struct's `ring_head` and the slot's `publish_index` are absolute
`uint64_t` publication numbers; `PeekRingSample`/`ReadRingSample` locate and
validate samples only by `publish_index`, so `UINT32_MAX → 0 → 1` is a
continuous stream. Widening only the mirror field would not remove the
32-bit wrap in `FrameHeader::sequence` and other transports; a full 64-bit
sequence requires a coordinated Frame/transport version bump.

**`layout_hash`:** FNV-1a 64 over `schema::ContractLayout` — mix `cpp_type`,
`payload_bytes`, `field_count`, `align`, then each field's
`name`/`type`/`offset`/`size`/`count`/`stride` in declaration order; strings
terminate with `0xFF`. Same layout model → same value across builds; any field
name/type/offset/size/count/stride change → different value. 0 is reserved for
unknown.

### Build-time capacity profile

Two **table-size** limits may be overridden at build time because they change
only fixed SHM tables, not the wire format:

| Macro / CMake variable | Default | Range | Affected pages |
|---|---|---|---|
| `THYME_ABI_MAX_STRUCT_COUNT` | 64 | 1..1024 | `struct_table[]` in `RecordMirrorRingPage`/`RecordMirrorPage`, `.tbin` registry limit, mirror slot count |
| `THYME_ABI_MAX_THREADS` | 64 | 1..1024 | `SharedHealthPage::slots[]`, `App::kMaxThreads` |

`THYME_ABI_MAX_STRUCT_COUNT` is the number of contract kinds per recording
package / mirror page (`ThymeStructDesc` entries), not the total number of C++
types in the product. Multi-process recording unions all recorded process
registries (deduplicated by `struct_id`), so 10 processes × 30 contracts = 300
entries.

At 1024, `RecordMirrorRingPage` uses 256 B per contract (128 B descriptor +
128 B ring descriptor with an embedded 64 B gate), about 256 KB per process
page; the `.tbin` registry uses 128 B per contract (about 128 KB per full
package); `schema.json` `records[]` grows linearly (about 200–400 KB at 1024).
**The payload arena is independent of this number**, so larger profiles do not
squeeze payload capacity.

Per-frame paths are unaffected: publish/consume/record/probe iterate over the
runtime `struct_count` or `worker_count`, never over the limit; these tables are
read only during init and diagnostics. The only cost that scales is creation:
`RecordMirrorClient::Create` and `RingClient::CreateRing` use
`ThymeStructDesc descs[N]` stack buffers (128 KB at 1024). The default 8 MB
stack is safe, but if a smaller thread stack is ever allowed, move these to
members/heap or first fix the `fill_struct_descs(out, cap)` contract.

`THYME_ABI_MAX_THREADS` is the per-process thyme-managed worker limit (cyclic +
event threads), not the board-wide thread count. Multiple processes each have
their own health page; a 128/256/512 profile costs 64 B per worker
(`128 + 64 × N` bytes per process page): about 16 KB at 256 and 64 KB at 1024.
Third-party thread pools are outside this limit; the hard boundary is the
process cgroup cpuset/cpu.max. If managed threads approach the limit, first
check whether the "few workers, many callbacks" model is being violated.

Upper-layer aliases (`kRecordMirrorRingMaxStructs`,
`kThymeBinMaxStructCount`, `App::kMaxThreads`,
`SharedHealthPage::kMaxWorkers`) alias these two constants, so there is no
"ABI value changed, old literal elsewhere" drift.

**Not configurable** (fixed because they bind encoding or fixed offsets):

- `kAbiMaxPayloadBytes = 512 KB`, `kRecordMirrorMaxArenaBytes = 2 MB`: coupled
  to `.thyme.bin` and mirror encoding;
- `kAbiMaxTransportBytes = 0xFFFF`: `FrameHeader.size` is `uint16_t`; larger
  payloads use the pinned-slot store;
- `kAbiMaxEventWaitInputs` / `kAbiMaxCyclicWaitInputs`: generation-time
  contracts shared with `gen_graph`, not in SHM pages;
- `kAbiMaxSpmcReaders = 8`: the SPMC payload offset is fixed at
  `kShmSpmcPayloadOffset = 320` (five 64 B cells), and eight per-reader drop
  slots exactly fill the fifth cell. More readers requires moving payload
  offsets — a separate page-layout change, not a profile knob; `limits.h`
  asserts the value is 8.

**Mixing is rejected.** Each page embedding these limits mixes the values into
`ShmLayoutHash()`, so processes with different profiles fail at `layout_hash`
validation instead of reading tables at the wrong width. This still follows
"bump only, no dual-read".

**Deployment constraint:** one deployment (same `<prefix>` SHM set and same
`abi_version`) must use one profile. Changing any value is an ABI change: bump
the page version and rebuild **all** processes. The default profile is
byte-identical to pre-profile builds.

**Validation:** `test_abi_profile` (mr_gate) pins ranges, default literals,
distinct page identities, and `ShmLayoutHash` sensitivity; CMake prints the
current profile at configure time. Cross-profile rejection is covered by the
attach-rejection matrix (`test_abi_cross_version`).

### SHM page `layout_hash`

`ShmLayoutHash(parts...)` is a separate FNV-1a 64 over ABI-relevant values in
declaration order: page/substructure tag, version, `sizeof`, `alignof`, each
field's offset/size, array count/stride, and nested sub-layout hashes, mixed
little-endian. Names do not participate (numeric tags replace them), so the same
layout yields the same value on any build/compiler; any field addition,
movement, width change, or array-length change changes it. Runtime
configuration (`health_n`, channel tables) and `feature_bits` do **not**
participate: `feature_bits` is optional behavior negotiation and is compared
separately.

Page-side implementations: `SharedHealthPageLayoutHash()` (including
`HashHealthSlotLayout()`) and `MpControlLayoutHash()` live in their headers;
`Spec()` returns `{magic, version, layout_hash, sizeof(Page), feature_bits}`.
`page_bytes` and `sizeof(Page)` share one source, and `layout_hash` also covers
`sizeof`, so a page-size change cannot move only one field. Attach failures
produce `ShmPageError{code, expected, actual}` and a diagnosable
`FormatShmPageError()` message.

Migrated: `SharedHealthPage` v3, `MpControl` v3,
`MpSessionIdentityPage` v1, `LatestSlot<T>` v3 described page, `ShmSpscRing` /
`ShmSpmcRing` v4, `RecordMirrorPage` v4 / `RecordMirrorRingPage` v6; no old
page is dual-read. `LatestSlot` additionally mixes `payload_layout_hash`, so
same-size different-contract pages are rejected. Other pages keep their own
magic/version validation and will reuse the same described-page path.

## Trigger control socket: v2 / v3 layout

Request (little-endian; offsets in bytes):

| Offset | v2 (96 B) | v3 (104 B) |
|---|---|---|
| 0 | `magic` (4) | same |
| 4 | `version` (2) = 2 | `version` (2) = 3 |
| 6 | `flags` (2) | same |
| 8 | `priority_hint` (4) | same |
| 16 | `timestamp_ns` (8) | same |
| 24 | `source[64]` | same |
| 88 | `sequence` (8) | same |
| 96 | — | `response_timeout_ms` (4, 1..60000) |
| 100 | — | `crc32` (4, covers 0..99) |

Response:

| Offset | v2 (24 B) | v3 (32 B) |
|---|---|---|
| 0 | `magic` (4) | same |
| 4 | `version` (2) = 2 | `version` (2) = 3 |
| 6 | `code` (2) | same |
| 8 | `trigger_id` (8) | same |
| 16 | `effective_priority` (4) | same |
| 20 | `flags` (4) | same |
| 24 | — | `reserved0` (4, must be 0, covered by CRC) |
| 28 | — | `crc32` (4, covers 0..27) |

The server selects the layout by datagram size and requires a matching version
field; when rejecting v1/v2 it replies with the corresponding short layout
(16/24 B) and `kUnsupportedVersion`, so clients do not wait for a timeout. The
`crc32` field itself is excluded from the calculation, so the receiver need not
zero it before recomputing.

### Trigger control socket: v4

v4 keeps all v3 offsets, turns the v3 CRC slot into reserved-zero, and appends:

| Offset | v4 request (120 B) | v4 response (48 B) |
|---|---|---|
| 0..99 | same as v3 (version=4) | same as v3 0..23 (version=4) |
| 100 | `reserved0=0` (4) | — |
| 104 | `request_id` (8, nonzero) | — |
| 112 | `crc32` (4, covers 0..111) | — |
| 24 | — | `reserved0=0` (4) |
| 28 | — | `reserved1=0` (4, former v3 CRC slot) |
| 32 | — | `request_id` (8, must match the request) |
| 40 | — | `crc32` (4, covers 0..39) |

An identical retry with the same `(uid,pid,request_id)` within the dedup
time/count window returns the first result with `kRespFlagDeduplicated` and
never enters the FSM. Same ID with a different CRC is `request_id_conflict`.
Outside the window, existing sequence replay protection applies. v1/v2/v3 do
not enter the dedup window; their accept flags, response sizes, and duplicate
rejection are unchanged.

## Trigger control socket: cancel / outcome notification (v3 layout unchanged)

This reuses v3's `flags` and `timestamp_ns` fields without adding bytes or
bumping the version:

- `kFlagCancel` (bit 2): cancel the trigger_id in `timestamp_ns` (must also set
  `kFlagHasTimestamp`). Replies: `kCancelled` / `kUnknownTrigger` (unknown or
  already complete, explicit rejection) / `kBusy` (rollback hook failed,
  retryable).
- `kFlagWaitOutcome` (bit 3): wait for an outcome. `timestamp_ns` filters by
  trigger_id (`kWaitFilterAny` = UINT64_MAX means any). A hit in the fixed
  64-entry outcome ring replies immediately; otherwise the request becomes a
  deferred waiter (fixed 8 slots) and replies when the outcome occurs
  (`kRespFlagOutcomeNotification`, code `kCaptureSealed` / `kPreemptedByHigher` /
  `kStartFailed` / `kPreemptFailed` / `kCancelled`) or is silently dropped after
  `response_timeout_ms` (the client reports its own `kTransportTimeout`). A full
  waiter table replies `kBusy` immediately.
- Backward compatibility: both flags require `kFlagHasTimestamp`; trigger_id is
  far below CLOCK_MONOTONIC nanoseconds and `kWaitFilterAny` far above any
  future tolerance, so pre-change v3 servers reject them as
  `kInvalidTimestamp` instead of misrecording a trigger. v1/v2 datagrams
  carrying these or any unknown flag bits are rejected as `malformed` with no
  response. Flags are the only extension point inside the v3 layout, so **v3
  datagrams with flag bits undefined in this build are also rejected as
  malformed**; a future opcode without a version bump must not be silently
  treated as an ordinary trigger.
- `Accepted` still has no late error code; outcome notification is a separate
  semantic (`kRespFlagOutcomeNotification`), not a second `TriggerResponse`.

## `FrameHeader.valid`: data-valid

The 40 B header contains independent checks that cannot substitute for each
other:

| Field/mechanism | Question answered | Set by | `FrameHeaderOk` (data boundary) | `FrameHeaderStructurallyOk` (metadata only) |
|---|---|---|---|---|
| `magic`/`version` | Is this our product/format? | Producer (per-product constants) | Exact match required | Exact match required |
| `size` | Wire span of header + payload | `FrameInit` | Must equal `sizeof(FrameHeader) + n` | Must be ≥ `sizeof(FrameHeader)` |
| `reserved` | Does it carry flags this build does not know? | Seal helper | Only bit 0 (CRC-disabled) allowed; all others 0 | Same |
| `crc32` | Are the bytes complete? | Seal helper | CRC over header (`crc32` zeroed) + payload | Not checked (no payload) |
| `valid` | Is the payload fully written and safe to read? | Producer after payload completion (seal) | Must be `== 1` | Requires 0/1; 0 allowed |

Conventions:

- `valid == 1` means the payload bytes under the header are fully written and
  stable (data-valid), not "header structurally valid", and not a copy of magic.
  `valid == 0` means "do not read the payload": the slot is being filled, the
  producer has not finished, or the frame was never sealed.
- Writer: `FrameInit` only zeroes and fills metadata, leaving `valid` at 0;
  after the payload is written, `FrameSeal` / `FrameSealNoCrc` /
  `FrameHeaderSeal` set it to 1. Channels that skip CRC (for example
  same-process event transport) call `FrameMarkValid`. `valid` is covered by
  CRC, so tampering after seal also breaks the CRC.
- Reader: a data boundary that consumes the payload must use `FrameHeaderOk`,
  which requires `valid == 1`. Header-only metadata checks (probe/routing/early
  drop) use `FrameHeaderStructurallyOk`, which does not validate CRC and allows
  `valid == 0`; **never** consume payload based on it.
- `valid` is orthogonal to CRC policy: a CRC-disabled frame
  (`kFrameFlagCrcDisabled`, `crc32 == 0`) still has `valid == 1`; no-CRC does
  not mean no-data.
- `valid > 1` is undefined and rejected by `FrameHeaderStructurallyOk`.
- Backward compatibility: since `FrameHeader` was introduced, writers have
  published `valid == 1`. Enforcing it in `FrameHeaderOk` rejects no existing
  recording; `FrameInit` no longer setting it early only moves the assignment to
  the seal phase, and the final wire image is unchanged, so the frame version is
  not bumped. A `valid == 0` frame in an old package was never readable data by
  contract (payload incomplete or never sealed); rejection is expected.

## Cross-version compatibility matrix

`tests/test_abi_cross_version.cpp` drives each combination below, asserts the
**exact result**, and confirms rejection delivers no partial data (no reply, no
FSM entry, no attach, no record). Formats no longer produced are rebuilt from
documented layouts (old structs plus `offsetof` assertions or synthesized byte
images), not kept as dead code.

| Format | Combination | Expected | Test |
|---|---|---|---|
| trigger v3 | v3 reader × v3 writer | `Accepted`, 32 B response + CRC | `TestTriggerV3Reader` |
| trigger v3 | v3 reader × v2 datagram (legacy off) | 24 B V2 layout, `kUnsupportedVersion`; FSM/counters unchanged | `TestTriggerLegacyV2` |
| trigger v3 | v3 reader × v2 datagram (`allow_legacy_v2`) | `Accepted`, 24 B V2 response (no CRC tail); cancel/notify bits still `malformed` | `TestTriggerLegacyV2` |
| trigger v3 | v3 reader × v1 datagram (legacy off) | 16 B V1 layout, `kUnsupportedVersion` | `TestTriggerLegacyV1` |
| trigger v3 | v3 reader × v1 datagram (`allow_legacy_unsequenced`) | `Accepted`, 16 B V1 response; unknown flags still `malformed` | `TestTriggerLegacyV1` |
| trigger v3 | v1/v2 reader × v3 datagram (104 B truncated to 88/96 B) | Version mismatch → `malformed`, no reply | `TestOldReaderVsV3Datagram` |
| trigger v3 | v3 message changed to version=2 (still 104 B) / 96 B V2 image changed to version=3 | `malformed` (size selects layout; version must match) | `TestOldReaderVsV3Datagram` |
| trigger v3 | Any version carrying an undefined flag bit (including v3) | `malformed`, no reply, trigger not recorded | `TestTriggerV3Reader` / `TestTriggerLegacyV1` / `TestTriggerLegacyV2` |
| trigger v3 | Short/truncated (0,1,8,16,50,87,88,95,96,103,105 B) and oversized | `malformed` | `TestTriggerV3Reader` |
| trigger v3 | Socket reply size | V1/V2/V3 exactly 16/24/32 B; no bytes written beyond the layout | `TestTriggerSocketReplySizes` |
| PinnedSlot header | v3 reader × v2 header image (192 B) | `Open()==false`, no header/lease | `TestPinnedSlotV2Header` |
| PinnedSlot header | v3 writer × v2 channel | `CreateStatus::kIncompatible` (explicit `force_replace` required) | `TestPinnedSlotV2Header` |
| PinnedSlot header | Rebuilt v2 reader × v3 header | `kIncompatible`; `MetaOffset 192≠320` | `TestPinnedSlotV2Header` |
| PinnedSlot header | v3 reader × channel version=2 (stomp) | `Open()==false` | `TestPinnedSlotLeaseOldVersions` |
| PinnedSlot lease | v6 reader × v5/v4/v3 lease header | `Open()==false`, no header/lease | `TestPinnedSlotLeaseOldVersions` |
| `ShmSpscRing` | v4 described open × v3 page/version | `Open()==false` (`kBadAbiVersion`/`kBadMagic`) | `TestRingVersions` |
| `ShmSpmcRing` | v4 described open × v3 page/version | `Open()==false` (`kBadAbiVersion`/`kBadMagic`) | `TestRingVersions` |
| `RecordMirrorRingPage` | v6 described attach × v5 page/version | `AttachReadOnly()==false`, no page; v5 fixed tables incompatible | `TestRingVersions` |
| `SharedHealthPage` | v3 described open × v2 image (4160 B, no super-header) | `Open()==false`, `kBadAbiVersion`; probe reports `[incompatible]` | `test_shm_page_header` / `test_thyme_probe` |
| `MpControl` | v3 described open × v2 image (232 B, bare `abi_version`@0) | `Open()==false`, `kBadMagic` (v2 has no magic) | `test_shm_page_header` / `test_mp_supervisor` |
| `SharedHealthPage` / `MpControl` | Same version but layout_hash/page_bytes/feature_bits/reserved changed | Described open rejects; `error.code` points to `kBadLayoutHash`/`kBadPageBytes`/`kBadFeatureBits`/`kBadReserved`; probe reports a note | `test_shm_page_header` |
| `SharedHealthPage` / `MpControl` | Role attach × tampered magic/worker_count/layout_hash | `MpRoleSession::Open()==false`, log contains expected/actual from `FormatShmPageError` | `test_mp_supervisor` |
| `LatestSlot` v3 page | v3 described open × v2 body (no super-header) | `kBadMagic` (unwritten v2 is `kNotReady`); never reads at v2 offsets | `test_latest_slot_page`, `test_latest_slot_pipeline` |
| `LatestSlot` v3 page | Same version with wrong payload hash / page_bytes / feature / version / geometry / short mapping | `kBadLayoutHash`/`kInvalidSpec`/`kBadPageBytes`/`kBadFeatureBits`/`kBadAbiVersion`/`kBadGeometry`/`kTooSmall` | `test_latest_slot_page` |
| `LatestSlot` wiring | Generator/supervisor/role/probe all v3 | `kGraphShmKindSpecs` uses `LatestSlotLayoutModel<T>`; probe reads `THYLSLOT` + geometry; old v2 pages are rejected and require SHM recreation | `test_latest_slot_pipeline` |
| `.tbin` | v2 reader × v1 package (stream sink, no index) | `ok()`; index `kAbsent`; sequential read; `SeekToMonoNs` false | `TestThymeBinV1` |
| `.tbin` | v2 reader × v2 package | `ok()`; index `kOk` | `test_thyme_bin_index` |
| `.tbin` | Current reader × version 0 / version 4 | `ok()==false` (0 undefined, 4 future) | `TestThymeBinVersions` |
| `.tbin` | v3 reader × v1/v2 package (no manifest block) | `ok()`; `manifest_state()==kAbsent` (legacy); sequential/indexed read unchanged | `TestThymeBinVersions` / `test_record_manifest` |
| `.tbin` | v3 reader × v3 package | `ok()`; `manifest_state()==kOk`, hash/version from the block | `test_record_manifest` |
| `.tbin` | v4 reader × v3 package (40 B legacy registry) | `ok()`; `manifest_state()==kOk`; registry `channel`/`record_key` empty, `layout_hash==0`, mapped by struct_id | `test_record_manifest` / `test_topology_map` |
| `.tbin` | v4 reader × v4 package (128 B keyed registry) | `ok()`; registry has key/layout_hash; `ThymeBinManifestBlock` present and CRC valid | `test_topology_replay` |
| registry | v4 package keyed to a target topology with reordered ids | Key hit + triple match → new target id; missing `--allow-missing` skips, otherwise fails; layout_hash mismatch fails explicitly | `test_topology_map` / `test_topology_replay` |
| `.tbin` | v3 reader × v3 package with tampered block CRC/hash | `ok()==false`, `manifest_state()==kInvalid`; no records delivered | `test_record_manifest` |
| `.tbin` | v3 header without a manifest block / block `manifest_version==0` | `ok()==false` (corrupt/unset, not legacy) | `test_record_manifest` |
| `.tbin` | v2 reader × v3 package | Version > 2 → future format rejected (v3 layout not parsed) | Rule 2 |
| package sidecar | Current reader × no `schema.json` (v1/v2 package) | `kAbsent`; package readable and verifiable; `--expect-manifest-hash` fails explicitly | `test_record_manifest` |
| package sidecar | Current reader × tampered/inconsistent `schema.json` | `kBadSchema`, `Open()` fails, zero events | `test_record_schema` / `test_record_manifest` |
| package sidecar | meta.tbin declares a manifest but the sidecar is missing / hashes differ | `kManifestMismatch`, `Open()` fails | `test_record_manifest` |
| package sidecar | Sidecar declares a manifest but `.tbin` has no block (v1/v2) | `kManifestMismatch` (unverifiable combination) | `test_record_manifest` |
| perf log | Two files with different hashes / `--expect-hash` mismatch / legacy + expect | Explicit error, exit 4, no incorrect timeline | `test_record_manifest` |
| `.tbin` | Current reader × `struct_count>64` / truncated registry | `ok()==false` | `TestThymeBinVersions` |
| `.tbin` | Record `payload_size` above the codec limit | `ReadNext==kCorrupt` plus `corrupt_offset`, no partial payload | `TestThymeBinVersions` |
| registry | `ThymeStructDesc::contract_version==0` | Reader `ok()==false`, empty registry | `TestContractRegistry` |
| registry | Unknown `contract_version` (7) | Passed through unchanged (id/version/size/name not truncated), payload intact | `TestContractRegistry` |
| `FrameHeader` | Future frame version | Exact version match → `false` | `TestFrameHeaderCrossVersion` |
| `FrameHeader` | No-CRC flag | Current reader accepts; an old reader without the flag rejects by CRC | `TestFrameHeaderCrossVersion` |
| `FrameHeader` | Undefined reserved bits | `FrameHeaderOk==false` | `TestFrameHeaderCrossVersion` |
| `FrameHeader` | `valid==0` with a self-consistent CRC | Data boundary `FrameHeaderOk==false`; metadata check `FrameHeaderStructurallyOk==true` | `TestFrameHeaderCrossVersion` |

### Silent acceptances fixed at the same time

1. **Trigger v3 unknown flag bits:** the v3 path previously recognized only
   cancel/wait combinations and **recorded other undefined bits as ordinary
   triggers**. It now rejects them as `malformed`, matching v1/v2, after CRC/
   timeout validation and before the replay window.
2. **`.tbin` version 0:** `ThymeBinReader` used to parse undefined version 0 as
   v1 (the replay reader always rejected it). Both now reject. Registry
   `contract_version==0` is also rejected.
3. **`FrameHeader` reserved bits:** rule 3 said other bits must be 0, but the
   implementation did not check, silently accepting future flags.
   `FrameHeaderOk` now rejects them explicitly.

## Record manifest and schema sidecar

`RecordStructId` is not a stable channel id: it is the row number of the
generated `RecordManifest` (unique recorded channels sorted bytewise by name;
`record: false` channels take no row). **Adding/removing/reordering recorded
channels changes later ids** — an ABI change, not a generator detail:

- Generation: `kRecordManifestHash` + `kRecordManifestCount` are written by
  `gen_graph` into `generated/record_manifest.h`; `gen_graph --check` makes any
  recorded-set drift visible in CI.
- On disk: a manifest-aware host
  (`triggered::Config::record_manifest = GetThymeRecordTable().manifest`)
  makes each package's `meta.tbin` v3 and carry a 32 B manifest block; at seal
  it also writes `schema.json` with the same hash/version. The two sources are
  independent: tampering with one is caught by the other.
- Consumption: `ReplayPackage::Open()` validates sidecar/block agreement
  (`kManifestMismatch`); `thyme-replay list/info/verify` can compare
  `--expect-manifest-hash 0x...` against this build and fail on mismatch;
  `play` rejects before injecting any event. `thyme_log_merge` applies the same
  rule to perf logs (exit 4).
- Legacy: pre-manifest packages (v1/v2, no block, no sidecar) remain readable
  and verifiable; they simply have no manifest identity, and
  `--expect-manifest-hash` fails closed for them (unknown is never treated as a
  match). This is documented legacy behaviour, not a silent downgrade.
- Decoders (including out-of-tree ones) must not infer compatibility from a
  matching struct_id name: the registry name is the contract type name, and id
  meaning is defined by the manifest. Mixing old perf logs with a new build is
  rejected by `thyme_log_merge`.

`schema.json` field layouts are available only when the host provides the
`gen_contracts`-generated `contract_layout.h` (declared through
`template/contracts_layout.txt`). The `offset/size/count/stride` values come
from the schema `compute_layout()` and are pinned with
`offsetof/sizeof/alignof` static_asserts in generated headers. Without the
table, or when its `payload_bytes/align` disagrees with the package registry's
`payload_size`, write `"layout": "unavailable"` and `"fields": null` — **never
guess**. `payload_offset` is the contract's start inside the record payload
(0 for a bare payload; `align_up(36, alignof(T))` for a `Frame<T>`, which is not
packed); a field's absolute offset is `payload_offset + field.offset`.

## Endianness and layout assertions

- **Endianness:** every cross-process and on-disk format is little-endian.
  `FrameHeader`, the trigger protocol, `.thyme.bin`, the integrity manifest, and
  all SHM structs are memcpy'd/written in **host byte order** with no swap
  layer, so `contracts/abi.h` asserts at compile time that
  `__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__` (GCC/Clang; other compilers set
  `kAbiHostEndianChecked == false`). Big-endian builds fail to compile; there
  is no runtime conversion. Byte-wise encoders such as `perf_log`/manifest write
  LSB-first explicitly and are still bound by the same contract.
- **Layout assertions:** every SHM page struct uses
  `static_assert(sizeof(...) == N)` plus `offsetof(...)` to pin field offsets
  and cites its version constant; template pages assert payload-independent
  invariants (header offsets, cell stride `RoundUp64(sizeof(T))`, `sizeof(Layout)`
  formula). `tests/test_abi_layout.cpp` covers all pages, version constants, and
  endianness. **Never edit an assertion to bypass a failure**: first determine
  whether the layout really changed — if it did, bump the version; if not, it is
  a bug.
- **Contract generation assertions:** `gen_contracts` emits, after each struct
  in `thyme_types.h`, `sizeof(T)`, `alignof(T)`, and `offsetof(T, field)` for
  every field; arrays also assert `sizeof(T::field) == count * stride` and
  `sizeof(T::field[0]) == stride`. Numbers come from the generator's single
  natural-alignment `compute_layout()` model. A mismatch with declared `bytes`
  fails generation rather than emitting uncompilable headers. **A failing
  assertion means the layout really changed** — bump the contract/frame version,
  do not edit the assertion. Generated headers must compile warning-free under
  `-Wall -Wextra -Wconversion -Werror` (only `static_assert`, no runtime code).
  Covered by `test_gen_contracts` and `test_contracts_layout_asserts`.
- **Contract schema validation:** `gen_contracts` performs a collect-all
  validation before generation: every violation, with its JSON path
  (`types[3].fields[2]`), is reported to stderr and the process exits 2 without
  writing a file — a bad `datatypes.json` cannot produce half a header set.
  Rules cover structure (known keys, required fields/types, non-empty types,
  prior-definition-only references, non-empty fields, `bytes` matching
  natural layout, valid `layout_of`), uniqueness (type/field names, a field not
  named like its struct), identifier legality (no keywords, no
  implementation-reserved spellings, no collisions with generated names or
  standard aliases), semantics (`payload` must be the only field in an opaque
  envelope), and explicit ceilings:
  `MAX_TYPES=4096`, `MAX_FIELDS_PER_STRUCT=256`, `MAX_ARRAY_COUNT=2^25`,
  `MAX_NESTING_DEPTH=16`, `MAX_STRUCT_BYTES=64 MiB`, `MAX_TOTAL_BYTES=128 MiB`.
  Size/depth limits use the same `compute_layout()` model after structural
  validation. Python integers do not overflow, so these are semantic gates
  ensuring generated array dimensions and constexpr assertions stay within what
  compilers and readers handle. Covered by `SchemaValidationTests` and a seeded
  fuzz-lite pass (one structural mutation per case; exit ∈ {0,2}, no traceback,
  no output on rejection).
- **LatestSlot body vs described page:** the in-process seqlock body remains
  layout v2, pinned by `detail::LatestSlotLayout<T>`; the v3 SHM page adds a
  64 B geometry block after the super-header. Generator, supervisor, role, and
  probe all use `LatestSlotLayoutModel<T>`; old v2 body pages are rejected with
  no dual-read, and upgrade requires recreating SHM.

## Rules

1. **Layout change:** bump magic/version and reject old peers; a region-size
   mismatch also rejects. Contract structs are additionally locked by generated
   layout assertions; a failed assertion follows the same version bump.
2. **Semantic change:** bump the version even when the layout is unchanged.
   Examples: the lease v3→v4 epoch invariant and the v4→v5 copy-active/owner
   recovery protocol. Old writers do not understand active and could overwrite
   payload during a plain `memcpy`, so v4/v3/v2 and v5 must not mix.
3. **CRC flag:** `FrameHeader.reserved[0]` bit 0 is ABI; all other reserved bits
   must be 0. Old readers do not know the no-CRC flag and reject such frames;
   no-CRC is opt-in and producers/readers upgrade together.
4. **CRC policy matrix** (`[channel.*] crc = package|frame|off`):

   | Channel | Default | Per-frame drain verify | Index `payload_crc` | Integrity boundary |
   |---|---|---|---|---|
   | pinned_slot / h264_stream | `package` | No (`verify_crc=false`) | Recalculated during drain (for `--verify`) | Package-level manifest |
   | mirror_ring (small/control `Frame<T>`) | `frame` | `.tbin` record CRC always kept | — | Record CRC + package manifest |
   | Any pinned_slot with explicit `crc=off` | — | No | `0` (no per-frame anchor) | Package manifest only |

   Backward compatibility: packages from before the flag existed validate by
   the original semantics — `crc32==0` without the flag is a real CRC value and
   must never be treated as "CRC disabled"; a no-CRC frame must have `crc32==0`
   to pass `FrameHeaderOk`.
5. **Wakeup counting:** `epoch_waiters` in `ShmSpscRing` v4 / `ShmSpmcRing` v4
   is advisory; a process crash leaks +1 (an extra wake, not a lost wake).
   SPMC v4 also has `reader_slot_mask` and 8 per-reader drop slots, allocated
   across wrappers/processes and never reused within the page lifetime; older
   versions reject them and cannot be mixed.
6. **Upgrade order:** stop producers → upgrade all readers/writers → recreate
   SHM objects; never mix old/new in one session. See
   [`upgrade.md`](../guides/upgrade.md): preflight `thyme_probe abi`
   (`--matrix` prints this build's constants, `--mismatches` lists
   incompatible pages) and recheck after startup.
7. **Endianness:** cross-process/on-disk formats are little-endian (host byte
   order, no swap layer); `abi.h` rejects big-endian builds at compile time.
   New cross-process/on-disk structs need the same `sizeof`/`offsetof`
   assertions and a version note.
8. **Producer gate:** v4 `PinnedSlotPageHeader` embeds `ProducerGate`; v6
   `RecordMirrorRingPage` embeds a per-struct gate in each ring descriptor.
   Writers check the lease in `Begin()`/`Commit()` or `TryWriteRing()` and
   reject publication when gated. After a replay crash, a writer reclaims the
   gate by `/proc` liveness or heartbeat timeout, so the channel is never
   permanently locked. `Create()` rejects overwriting an old header or live
   gate; `force_replace` bypasses version incompatibility only, never a live
   gate or data migration. Producers built on old headers cannot write to v4
   channels or v6 mirror pages, and replay/injectors must match, so upgrades
   switch the whole set per rule 6.
9. **`FrameHeader.valid`:** `valid == 1` means the payload is fully written and
   stable (data-valid), set by the writer after completion (seal or
   `FrameMarkValid`); `valid == 0` is not readable data. `FrameHeaderOk` checks
   magic/version, defined reserved bits, size/span, CRC, and `valid == 1`;
   `FrameHeaderStructurallyOk` checks only the header (no CRC, `valid == 0`
   allowed, never use it to consume payload). `valid` does not replace
   magic/version/size/CRC and does not mean structural validity; a CRC-disabled
   frame still has `valid == 1`. Existing writers always published `valid == 1`,
   so this rule does not bump the frame version.
10. **Record manifest:** `RecordStructId` is a manifest row number, not a
    stable channel id. Adding/removing recorded channels renumbers ids (ABI
    change); the host must pass `GetThymeRecordTable().manifest` to
    `triggered::Config::record_manifest`, and readers/perf analysers must
    compare the manifest hash and reject mismatches explicitly
    (`manifest-mismatch` / exit 4). Old packages without a manifest are legacy
    (no id guarantee), and `--expect-manifest-hash` fails closed for them. A
    missing `schema.json` is also legacy; present but inconsistent with the
    `.tbin` block means tampering and is rejected. `schema_version` /
    `kManifestVersion` only bump for new semantics; "same version, changed
    semantics" is forbidden.

## Evidence

- `include/thyme/runtime/pinned_slot_store.h`, `shm_spsc_ring.h`,
  `shm_spmc_ring.h`, `contracts/frame_header.h`
- `include/thyme/runtime/latest_slot.h`, `latest_slot_page.h`,
  `tests/test_latest_slot_page.cpp`, `tests/test_latest_slot_pipeline.cpp`,
  `tests/test_latest_slot_seq_wrap.cpp`
- `tests/test_abi_layout.cpp` (all SHM page sizes/offsets, version constants,
  little-endian assertions)
- `include/thyme/app/mp_shm_kind.h`, `tests/test_mp_session_identity.cpp`
- `tests/test_shm_page_header.cpp`, `tests/test_shared_health.cpp`,
  `tests/test_mp_supervisor.cpp`
- `tests/test_shm_spmc_ring.cpp`
- `tests/test_abi_cross_version.cpp`
- `include/thyme/contracts/abi.h`
- `tests/test_pinned_slot_lease_abi.cpp`, `tests/test_frame_header.cpp`
- `tests/test_crc_policy.cpp`
- `tests/test_trigger_cancel.cpp`, `tests/test_trigger_notify.cpp`
- `tests/test_replay_producer_gate.cpp`, `tests/test_pinned_slot_injector.cpp`
- `include/thyme/probe/abi_matrix.h`, `src/probe/abi_check.cpp`
- `include/thyme/record/record_schema.h`, `src/record/record_schema.cpp`,
  `tests/test_record_schema.cpp`
- `tests/test_record_manifest.cpp`,
  `tools/gen/gen_contracts/gen_contracts.py`,
  `tools/gen/gen_graph/emit_record.cpp`
- `tools/inspect/thyme_probe/main.cpp`,
  `include/thyme/probe/schema_view.h`
- `tools/ops/upgrade/thyme_upgrade.sh`,
  `tools/ops/upgrade/abi_matrix.golden.json`, `docs/guides/upgrade.md`
