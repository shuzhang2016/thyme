# ADR 0005: thyme_bin record format and recording package layout

- Status: Accepted (as-built)
- Date: 2026-10-24
- Scope: `include/thyme/record/thyme_bin.h`, `triggered_recorder.h`, `package_integrity.h`, `clock_anchor.h`; `src/record/thyme_bin.cpp`, `triggered_recorder.cpp`, `clock_anchor.cpp`

## Context

The recording format must serve four readers at once: the online recorder
(append-only hot path), offline replay/troubleshooting tools (random access,
tolerant of truncation), retention (deciding whether a package is complete and
safe to delete), and forensics (tamper detection). A single-file format is
simple but cannot separate "fully written" from "half written"; per-frame CRC
on megabyte-scale bulk data is too expensive (see ADR 0004).

## Decision

1. **`.tbin` / `.thyme.bin` base format** (wire format == on-disk format):
   - File header: `THYM` magic + `file_abi_version` + device_id/session_id/
     start_mono_ns + a struct registry (`ThymeStructDesc{struct_id,
     contract_version, payload_size, name[32]}`, shared with the SHM mirror
     page).
   - **struct_id source:** `RecordStructId` is the row number of the
     generation-time `RecordManifest` (unique recorded channels, sorted by
     channel name; `generated/record_manifest.h`). A channel with
     `record: false` has no struct_id, no mirror slot, and no arena bytes.
     Adding or removing a recorded channel renumbers later ids, which is an ABI
     change exposed through the manifest hash and `gen_graph --check`. Old
     packages remain independently decodable through their self-describing
     struct table.
   - Record types `kData/kGap/kTrigger/kSession`; each row carries
     record_sequence, struct_id, mono_ns, source_sequence, payload_size, and
     crc32. `kTrigger` rows carry pre/post_roll_ns (readers expose it;
     `test_thyme_bin_trigger`).
   - Payload limit `kThymeBinMaxPayloadSize = kAbiMaxPayloadBytes` (512 KB;
     formerly 17024). Recordings above the old limit are readable only by new
     readers; raising the codec limit is intentionally not backward-compatible.
   - **Reader tri-state:** `ReadResult{kOk, kEof, kCorrupt}`. A bad record
     (CRC error or truncated tail) reports offset and expected/actual CRC; it is
     not folded into EOF.
   - **Footer index (v2):** seekable sinks write
     `[block entries × block_count][ThymeBinIndexFooter]` (`THYIDX1`) at the end
     of the file, one index entry per 64 records, so a seek scans at most one
     block. Streaming sinks (TCP/fd) keep the v1 no-index layout, and old
     readers remain usable.
2. **Trigger package layout** (one directory per capture,
   `<out_dir>/<trigger_id>_<t0>/`):
   - `meta.tbin` — header + TRIGGER records (t0/before/after) +
     `TriggerMetaRecord` (with config_hash for configuration-drift forensics) +
     per-source manifest + a trailing `CaptureStatsRecord` (bit 0 = truncated by
     a higher-priority capture);
   - `small.tbin` — small-structure samples plus GAP hole markers, written in
     ascending mono_ns order across structs (a zero-allocation sort of the
     descriptor ring before drain; replay readers depend on per-source
     monotonicity);
   - `frames/<channel>.raw` + `<channel>.index.tbin` — raw large-frame segments
     plus a `FrameIndexRecord{offset, len, flags, payload_crc}` index;
   - `wal.log` — drain journal (`TRIGWAL1` header plus fixed 24-byte rows,
     `static_assert(sizeof(WalRow)==24)`, package ABI). The package is complete
     only when the last row is `kWalOpSeal` (`PackageComplete()` == seal
     present). A failed seal path writes `kWalOpAbort` and renames the directory
     to `.incomplete`, so an already-cached seal line can never produce a
     self-sealed partial package. A `kWalOpClockAnchor` row stores monotonic time
     in `index` and the corresponding wall clock in `mono_ns`, written at
     creation, periodically, and before seal. It is not a data descriptor, does
     not change the final seal/abort decision, and does not enter the replay
     stream.
3. **Continuous recording slice model** (design §6.6): an unbounded window on
   the same pipeline rolls sealed `continuous_<seq>_<ts>/` slices on
   `slice_bytes` (default 512 MB) or `slice_ns`. H.264 channels align to GOP
   boundaries (payload flag bit 0 = IDR); a timeout forces a roll and counts
   `roll_forced`. Closing a slice writes `ContinuousMetaRecord`
   (slice_seq/begin/end/samples/frames/holes/evicted). The WAL uses
   `kContinuousWalMark`, a high marker that never collides with a real
   trigger_id. Continuous and trigger queues are bucketed separately and share
   `pending_bytes_quota`; under quota pressure the **continuous bucket is
   evicted first**, prioritizing trigger packages.
4. **Package-level integrity:** on seal (or invalidation), write
   `integrity.manifest` (120 B fixed header + path-sorted `{path, size,
   sha256}` entries + a chain root: a per-entry chained SHA-256 from a genesis
   tag, so deletion, reordering, and insertion are detectable). When
   `[security].signing_key` is configured, also write `integrity.hmac`
   (detached HMAC-SHA256 hex, verifiable offline; a configured but unreadable key
   fails Init/seal closed). For a package invalidated or swept at startup, write
   a partial manifest over the surviving files (flags bit 1). The manifest is
   computed only at seal/invalidation, **never on the per-frame hot path**.
   Details: [security/evidence-integrity.md](../../security/evidence-integrity.md).
5. **Record schema sidecar and manifest identity:**
   - On seal (or invalidation), write `<pkg>/schema.json` containing
     `format=thyme-record-schema`, `schema_version=1`, `partial` (true for
     `.incomplete` packages), `record_manifest{hash,version,channels:null}`, the
     40 B `frame_header` field table, and for each registry entry
     `struct_id/name/contract_version/payload_size/payload_offset/
     contract_bytes/layout/fields[]`. A sidecar is chosen over an embedded
     header blob so the recording hot path and the existing `.tbin` layout stay
     unchanged, and a non-thyme tool can consume one file. The sidecar is written
     before `integrity.manifest`, so the package hash chain covers it.
   - Field-level layout comes from `contract_layout.h`, generated by
     `gen_contracts` (declared through `template/contracts_layout.txt`). Values
     come from the schema `compute_layout()` and are pinned by
     `offsetof/sizeof/alignof` static_asserts in generated headers.
     `payload_offset` distinguishes a bare payload (0) from
     `sizeof(Frame<T>)` (`align_up(36, alignof(T))`, `Frame` is not packed).
     Without a table, or when the table disagrees with the package registry,
     write `"layout": "unavailable"` and `"fields": null` — never guess. Channel
     names are not in `ThymeStructDesc`, so `record_manifest.channels` is null
     and the manifest hash is the recorded-set identity.
   - **Manifest in the package:** `RecordStructId` is a manifest row number, so
     a manifest-aware host writes `meta.tbin` as file version 3, adding a 32 B
     `THYMANI1` block (version + `kRecordManifestHash` + CRC32) after the struct
     registry. Hosts without a manifest keep v2/v1 bytes unchanged: old packages
     and new readers both take the "manifest absent" legacy path, with no
     guessing. Perf logs write a type-7 block per file generation with the same
     hash/version. `thyme_log_merge` validates across files;
     `thyme-replay list/info/verify` compares against
     `--expect-manifest-hash`; `play` rejects a mismatch before injection.
     **No silent decoding:** id meaning is defined by the manifest, and packages
     or logs from different manifests must not be mixed.

## Alternatives

- **A. One large `.thyme.bin` plus external chunking:** locating a record can
  require reading 512 MB sequentially. The package-directory model makes
  "complete/partial/invalid" a directory-level fact (WAL seal plus
  `.incomplete` naming), so retention and forensics need no data parsing.
  Rejected.
- **B. Compress inside the recorder:** compression on the recording hot path
  would break the drain latency budget; trigger packages are compressed by an
  external tool after sealing. Rejected.
- **C. MCAP as the primary format:** MCAP exists through `mcap_export` for
  export/interoperability. The primary format stays thyme_bin (fixed rows,
  tri-state reader, shared struct registry with the SHM mirror).
- **D. Always write a footer index:** streaming sinks (TCP/fd) cannot carry a
  tail index. Version 1/2 is chosen per sink capability, and the reader
  tolerates `IndexStatus::kAbsent` (old recordings, crash truncation,
  non-seekable capture). A corrupt index or probe I/O failure rejects seek.
- **E. Embed the schema in the `.tbin` header or a meta record:** would require
  a file-version bump, force unsupported tools to parse a new block, and
  duplicate the schema in every data file. The sidecar is written once, is
  naturally consumable by non-thyme tools, and `.tbin` only gains an optional
  manifest block.
- **F. Hardcode field layouts in the core:** the core does not know application
  contracts and would inevitably drift. A generated, compile-time-asserted table
  with an honest fallback when absent is the only option that does not lie.
- **G. Ed25519 signatures instead of HMAC:** HMAC is symmetric — whoever can
  verify can forge, and a same-uid/root attacker who reads the key can re-sign.
  Ed25519 gives public-key verification but adds OpenSSL/libsodium
  dependencies; kept as a separate ADR.

## Consequences

- **Benefits:** completeness is decided with zero parsing (WAL seal plus
  manifest); partial packages are forensically recoverable (abort row plus a
  partial manifest over survivors); replay can seek randomly and tolerate
  truncation; continuous and trigger capture share one drain pipeline and one
  quota system.
- **Costs (explicitly accepted):**
  - after raising the codec limit (17024 → 512 KB), new recordings are
    incompatible with old readers; toolchains must be deployed together;
  - cross-struct monotonicity in `small.tbin` is a format contract. Late samples
    (mono_ns below the written watermark) are downgraded to GAP and counted as
    `out_of_order`; they are never written out of order;
  - package encryption is **not** provided: packages are plaintext files in a
    0700 directory, so anyone who can read the filesystem can read the payload;
  - the symmetric-HMAC limitation above applies (alternative G);
  - the manifest covers every regular file in the package. Whole-package hashing
    runs outside the real-time thread, but seal-time hashing cost is part of the
    drain budget ledger;
  - a manifest-aware package's `meta.tbin` is v3: a pre-manifest reader rejects
    it as a future version (an explicit error, not a misread). Hosts without a
    manifest still write v1/v2 with identical bytes. If the schema sidecar is
    missing, tools take the legacy path; field-level decoding requires the host
    to wire `contract_layout.h`. An out-of-tree recorder CLI must forward
    `ThymeRecordTable::manifest` to `Config::record_manifest`; without that
    one-line migration, functionality degrades but recording still works.
