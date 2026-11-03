# ADR 0004: Frame wire format and the Frame / PinnedSlot dual path

- Status: Accepted (as-built)
- Date: 2026-10-24
- Scope: `include/thyme/contracts/frame.h`, `frame_header.h`, `abi.h`; `include/thyme/runtime/pinned_slot_store.h`

## Context

Messages on graph edges must be self-describing, verifiable, and usable across
processes (shared memory carries raw bytes), while payload sizes span a huge
range: small control/state structs of a few dozen bytes through multi-megabyte
point clouds and images. Putting every size through one channel would force
small structs to pay fixed slot costs for large frames, or force large frames to
be split; compile-time fixed-capacity containers (`SpscRing<T,Cap>` and the
like) cannot hold a megabyte-scale payload whose size is known only at runtime.

## Decision

1. **Fixed 40-byte wire header** (`FrameHeader`, `#pragma pack(1)`,
   `static_assert(sizeof == 40)`):
   `magic u32 | version u16 | size u16 | session_id u64 | mono_ns u64 |
   sequence u32 | valid u8 | reserved[3] | crc32 u32 | reserved2 u32`.
   The trailing `reserved2` pads `sizeof` to a multiple of 8, preventing implicit
   padding in `Frame<T>` for payloads with alignment ≤ 8.
   `size` is the wire byte count (header + payload). CRC32 (IEEE 802.3,
   reflected polynomial) covers the header with `crc32` zeroed plus the payload;
   x86 uses PCLMULQDQ, ARMv8 uses the CRC32 extension (runtime HWCAP
   detection), otherwise a bitwise fallback is used.
2. **64 KB transport limit:** `size` is a `uint16_t`, and `FrameInit` rejects
   oversized contracts at compile time with
   `static_assert(FramePodWireBytes<T> <= 0xFFFF)`, so an oversized frame type
   cannot be silently truncated into a plausible-looking but wrong wire size.
3. **Dual-path routing:**
   - ≤ 64 KB: `Frame<Payload>` (POD payload plus a 40 B header) travels graph
     edges (`SpscRing`/`ShmSpscRing`/`ShmSpmcRing`/`LatestSlot`); the payload
     must be trivially copyable and standard-layout;
   - > 64 KB: the pinned-slot store (`PinnedSlotWriter`/`PinnedSlotReader`).
     `slot_bytes`/`slot_count`/`meta_blob` are runtime `Create` parameters, with
     a per-slot seqlock (0 = write in progress, otherwise frame_index + 1), a
     64 B footer (magic written last as the payload-complete marker), and a
     per-slot lease table in a separate `<name>.lease` shared-memory object.
     The writer **skips** a pinned slot (the head advances without publishing,
     readers see a hole, and `pinned_skip` is counted). A dead reader's pin is
     reclaimed by epoch timeout (default 5 s).
   - 512 KB (`kAbiMaxPayloadBytes`) is the **recording-layer** limit (mirror ring
     slots and the thyme_bin codec), not the Frame transport limit;
     `thyme/limits.h` pins module constants to the same ABI value with
     `static_assert`.
4. **Version/flags discipline:** magic and version must match exactly
   (`FrameHeaderOk`). Layout changes bump the version, and **semantic changes
   also bump it** (see [abi-compatibility.md](../../reference/abi-compatibility.md)).
   Flags use defined bits in `reserved[]`:
   - `reserved[0]` bit 0 = `kFrameFlagCrcDisabled`: an explicit no-CRC seal
     (`crc32 = 0`). An old reader that does not know the bit still validates
     CRC and rejects the frame — no-CRC is opt-in and fails toward rejection,
     not false acceptance;
   - pinned-slot payload region v2 (separate lease table), lease table **v4**
     (pin-before-validate plus epoch-after-validation; v2/v3 peers are rejected
     for mixed runs because the epoch invariant differs). The later evolution
     is **v5**: same layout, plus copy-active and copier pid/start death
     reclamation; v4/v3/v2 are rejected for mixed runs;
   - pinned-slot transport CRC defaults to **off**. Bulk-channel integrity is
     bounded by the package-level manifest; `SetCrcEnabled(true)` opts in per
     channel. Readers accept both values, and this is not a mixed-run ABI. Small
     `Frame<T>` payloads keep per-frame CRC by default and may use `crc=off` per
     channel.

## Alternatives

- **A. Widen `size` to `uint32_t` (ABI v2):** keeps large contracts on the Frame
  path, but every reader/writer must bump versions, and it does not solve the
  root problem that compile-time fixed-capacity containers cannot hold
  megabyte-scale payloads. Kept as a possible ABI v2, not done now.
- **B. Merge into one channel:** a single parameterized channel serving both
  40 B and 19 MB payloads either makes small structs pay for large slots or
  makes large frames use variable-length allocation with heap fragmentation.
  The dual path is an explicit trade-off: small structs need compile-time fixed
  sizes, zero allocation, and shared-memory rings; large frames need runtime
  sizing and leases. Rejected.
- **C. Per-frame CRC for every bulk payload:** CRC cost on a 19 MB frame is on
  the same order as the copy. The package-level manifest (per-file SHA-256 plus
  a chain root) is already a stronger integrity boundary, so per-frame CRC is
  on demand (the frame index keeps `payload_crc` for `--verify` recomputation).
  Disabled by default.

## Consequences

- **Benefits:** the small-struct path is compile-time fixed-size and
  zero-allocation on the hot path; the large-frame path is zero-copy in steady
  state (the lease pin covers the whole fwrite/index window). The size routing
  rule (≤ 64 KB Frame / > 64 KB pinned slot) is an ABI-level contract, and
  `limits.h` `static_assert`s prevent drift.
- **Costs (explicitly accepted):**
  - a contract above 64 KB cannot be a `Frame` on a graph edge; it must define a
    pinned-slot channel and accept its runtime-sized region planning;
  - pinned skips create holes (readers treat them as lost/GAP), and pin duration
    directly reduces writer capacity (N−R), bounded by drain bandwidth and the
    `pinned_skip` gate;
  - lease ABI v4 is incompatible with v2/v3: mixed-version processes are
    rejected (deliberately, because the epoch invariant differs);
  - no-CRC frames are rejected by old readers, so confirm consumers are upgraded
    before enabling `crc=off`;
  - naming: older documents call the primitive `PinnedSlotStore`, while the
    code exposes `PinnedSlotWriter`/`PinnedSlotReader`.
