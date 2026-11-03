# Evidence-package integrity (SEC-03 + REC-18 + REC-23)

Status: **implemented** for sealed trigger captures and continuous slices
(SEC-03), invalidated `.incomplete` packages via partial manifests (REC-18),
and configurable full/metadata/off verification scopes (REC-23).
Scope: the on-disk packages written by `TriggeredRecorder`; the verification
tool `thyme-replay integrity`; the optional HMAC signature.
Related: `docs/security/threat-model.md` (rows 4/10/16 and §4),
`docs/reference/abi-compatibility.md` (manifest + control-protocol versions).

## 1. What is written at a clean seal

A package that seals successfully (wal seal row + every file fsync'ed) gains
two files at its root:

```
<pkg>/meta.tbin            package data (unchanged)
<pkg>/small.tbin           package data (unchanged)
<pkg>/frames/*.raw|*.tbin  package data (unchanged)
<pkg>/wal.log              drain journal, last row = seal (unchanged)
<pkg>/schema.json          ABI-09: record schema sidecar (contracts + manifest)
<pkg>/integrity.manifest   SEC-03: file list + size + scoped sha256 + chain
<pkg>/integrity.hmac       SEC-03: detached HMAC-SHA256 (only when signed)
```

`integrity.manifest` covers **every regular file** in the package except the
manifest and signature themselves (recursively; `frames/` included). Empty
directories are not evidence and are ignored.

ABI-09: `schema.json` is written **before** the manifest, so it is part of the
file set the chain covers (a tampered schema is caught by `verify`/`integrity`,
and removing it leaves an unlisted/missing entry). A partial (`.incomplete`)
package gets a `"partial": true` sidecar over the same surviving records.
`schema.json.tmp` never survives a successful write (atomic rename); a crash
leftover is covered/reported like any other file.

REC-18 — **partial manifests for `.incomplete` packages.** When a package is
actively invalidated (seal-path failure or `AbortCapture`: wal abort row +
`.incomplete` rename), the recorder writes a manifest over the *surviving*
file set with the header's **partial flag** set (`seal_time_ns` is then the
invalidation time). The retention startup sweep (`Manager::MarkIncomplete`)
does the same for crash-leftover packages it renames, recovering the session
id from the wal header (kind from the leaf name, `trigger_id` / `slice_seq`
from the header id; the trigger window fields stay 0 — the sweep does not
know them). Both paths are best-effort: the usual abort cause is ENOSPC, in
which case the manifest write can fail too and the `.incomplete` package
simply has no manifest (reported `absent`, as before).

The manifest is finalized once per package — at seal, at invalidation, or
during the startup sweep. In full mode it is the default integrity boundary
for bulk payloads (point clouds / images / H264): per-frame CRC32 is a
`--verify`-time recheck, not a record-time cost. REC-23 non-full modes narrow
that boundary exactly as described below.

## 2. Manifest format

Binary, little-endian, versioned. Fixed 120-byte header, then `file_count`
variable-length entries sorted bytewise by the UTF-8 relative path.

| offset | size | field |
| --- | --- | --- |
| 0 | 8 | magic `THYINTG1` |
| 8 | 2 | version (`1`) |
| 10 | 1 | kind: `1` trigger capture, `2` continuous slice |
| 11 | 1 | flags: bit0 = HMAC-SHA256 signature present; bit1 = **partial** (REC-18); bit2..3 = **integrity mode** `0=full`, `1=metadata`, `2=off`, `3=mixed` (REC-23) |
| 12 | 4 | `file_count` |
| 16 | 8 | `seal_time_ns` (CLOCK_REALTIME; wall clock for audit correlation; the invalidation/manifest time when partial) |
| 24 | 8 | `session_id` (the `meta.tbin` / `wal.log` session id) |
| 32 | 8 | `trigger_id` (`0` for continuous slices) |
| 40 | 8 | `t0_ns` (trigger; `0` in sweep-written partial manifests) |
| 48 | 8 | `before_ns` (trigger; `0` in sweep-written partial manifests) |
| 56 | 8 | `after_ns` (trigger; `0` in sweep-written partial manifests) |
| 64 | 8 | `slice_seq` (continuous) |
| 72 | 8 | `begin_ns` (continuous; `0` in sweep-written partial manifests) |
| 80 | 8 | `end_ns` (continuous; `0` in sweep-written partial manifests) |
| 88 | 32 | chain root |

Each entry:

| size | field |
| --- | --- |
| 4 | `path_len` (1..4096) |
| 8 | `size` (bytes) |
| 32 | `sha256(file bytes)`, or 32 zero bytes when payload hashing is disabled |
| `path_len` | relative path, `/` separators, no leading `/`, no `..` |

The entry layout is unchanged. A zero digest is a scope marker, not a computed
hash: `full` requires real digests everywhere; `metadata` requires them except
for `frames/*.raw`; `off` requires zero digests everywhere; `mixed` allows each
raw entry to be full or size-only while all non-raw entries remain hashed. A
manifest whose digest shape contradicts its header mode is invalid.

The binary format was chosen over JSON to avoid escaping rules for channel
names and to make the signed bytes and the parsed bytes identical (the HMAC
covers the file verbatim). Paths are the only variable-length field, and the
manifest is re-written atomically (`.tmp` + fsync + rename + directory fsync),
so a crash cannot leave a parseable half-manifest.

## 3. Hash chain

Fixed genesis tag and per-file chaining:

```
GENESIS   = sha256("thyme-evidence-package-v1")
chain[-1] = GENESIS
chain[i]  = sha256(chain[i-1] || path_bytes || u64le(size) || sha256_bytes)
root      = chain[file_count-1]
```

The root is stored in the header. Verification recomputes three things:

1. the manifest is self-consistent: recomputing the chain from its own
   declared entries must reproduce the stored root (a hand-edited path, size
   or digest changes the root → `invalid`);
2. every listed file exists and has the declared size; entries with a real
   digest must also match that digest;
3. the chain recomputed over the **declared verification scope** reproduces
   the stored root (unchecked entries contribute their zero digest).

In `full`, a single-byte change therefore fails both the per-file comparison
and the chain comparison. In every mode, deleting a file fails the missing
count and the chain, changing a size fails, and adding an unlisted file fails
the `unlisted` count. Same-size content changes to zero-digest entries are
**deliberately undetectable**; the verifier rejects extra files because the
declared evidence set must be exactly what was sealed.

### 3.1 REC-23 verification scopes and configuration

```ini
[security]
manifest = full       # full | metadata | off; default full

[channel./cloud_front]
kind = pinned_slot
name = cloud_front
manifest = metadata   # optional bulk-only full/metadata/off override
```

- `full`: every file's bytes are hashed. This is the default and produces the
  same package bytes, manifest and HMAC as REC-22.
- `metadata`: `meta.tbin`, `small.tbin`, `frames/*.index.tbin`, `wal.log`,
  `schema.json` and all other non-raw files retain full hashes; `.raw` records
  only file list+size. Small-struct/control channels cannot select a non-full
  per-channel override.
- `off`: the whole package records only file list+size.
- `mixed`: at least one bulk channel differs from the package default. Each
  raw entry's zero/non-zero digest records its actual scope.

Non-full modes require explicit configuration and emit startup audit records
in `<socket>.audit` (`req=config`, `reason=integrity_downgrade`, and
`integrity_mode`). Status and metrics expose `integrity_mode`. Offline output
never prints a bare `OK` for these modes: success is `OK(metadata)`,
`OK(off)`, or `OK(mixed)` with `UNVERIFIED(payload)`, and JSON includes
`integrity_mode`, `files_unverified`, and per-file `hash_checked`.

The chain and optional HMAC still cover the mode, path list, sizes, and every
declared digest. **HMAC authenticates the downgraded manifest; it does not
restore verification of zero-digest payload bytes.** Without HMAC, as before,
an attacker able to rewrite the package can also rewrite a self-consistent
manifest.

## 4. Signing (optional, off by default)

Configuration:

```ini
[security]
signing_key = /etc/thyme/evidence.key   ; empty/unset = unsigned manifests
```

The key file contains raw key bytes (a trailing newline is trimmed). When set:

* `TriggeredRecorder::Init` validates that the key is readable and non-empty
  (a misconfigured deployment refuses to start rather than silently sealing
  unsigned packages);
* each clean seal writes `integrity.hmac` containing the lowercase hex
  HMAC-SHA256 of the exact `integrity.manifest` bytes, one line;
* any manifest/signature write failure invalidates the package
  (abort row + `.incomplete` rename), exactly like a seal-path failure.

Verification requires the key: `thyme-replay integrity <pkg> --key <file>`.
A signed manifest without `--key`, a missing `integrity.hmac`, or a wrong key
all fail closed. An unsigned manifest verifies normally (integrity without
authenticity) and `--key` is ignored. See
[Key rotation](../guides/key-rotation.md) for atomic replacement and old-key
retention (SEC-08).

**Choice and limitation.** HMAC-SHA256 was chosen because the tree has no
crypto dependency and adding one is out of scope. The implementation is a
small FIPS 180-4/RFC 2104 header (`src/record/sha256.h`). HMAC is symmetric:
anyone who can verify can also forge, and a same-uid/root attacker who can
read the key file can re-sign a modified package. The key must therefore live
outside the package tree and outside the writer's uid if that attacker model
matters.

**Ed25519 / public-key ADR.** Public-key signatures would let analysts verify
with a public key while the recorder holds only a secret key, and would remove
the "verifier can forge" property. That needs a library (OpenSSL or
libsodium) and key provisioning/rotation, so it is **not implemented** this
round. Manifest flags bit0 is "signed", bit1 is REC-18 "partial", and
bit2..3 are the REC-23 integrity mode. Remaining bits are rejected as unknown;
adding Ed25519 later is a versioned flag/manifest-version change, not a
redesign.

## 5. Encryption — explicitly out of scope this round

Packages remain plain files inside a `0700` directory. **At-rest
confidentiality is not provided**: any process running as the package owner
(or root) can read every payload after the fact. Encrypting payloads would
require key management, a decision about where decryption happens (analysis
host vs. recorder), and would complicate the zero-copy drain path; none of
that is in this round. The threat this leaves open is exactly threat-model
row 10's **I** (information disclosure) for stolen disks, backups and
same-uid readers. File permissions and the `high/` retention exclusion remain
the only controls.

## 6. Crash / abort behaviour

| Situation | Manifest | Verifier output |
| --- | --- | --- |
| Clean seal (trigger or continuous) | present (signed if configured) | full: `integrity: OK`; non-full: `integrity: OK(mode); UNVERIFIED(payload)`, exit 0 |
| `AbortCapture` (ENOSPC etc.) | **partial** manifest over the surviving files (wal.log includes the abort row) | `integrity: PARTIAL`, exit 1 |
| Seal-path failure / fsync failure | partial manifest, package renamed `.incomplete` | `integrity: PARTIAL`, exit 1 |
| Recorder crash before seal | partial manifest written by the startup sweep (`MarkIncomplete`) at the next recorder start | `integrity: PARTIAL`, exit 1 |
| Partial-manifest write itself fails (e.g. ENOSPC), or pre-REC-18 `.incomplete` package | absent | `manifest : absent`, exit 1 |
| Manifest/signature write failure on the seal path | partial manifest after invalidation (or absent, same fallback) | PARTIAL / absent, exit 1 |
| Manifest or signature edited | present but invalid | `manifest : INVALID` / `signature: FAILED`, exit 1 |
| Surviving file of an `.incomplete` package tampered | partial manifest present | `integrity: FAILED`, exit 1 |

`PARTIAL` is deliberately distinct from both `OK` and `FAILED`: the package
was never sealed (it is forensic evidence of an aborted/invalidated capture),
but its declared scope verifies — file list, size, any declared sha256, chain,
and signature when signed. Partial always exits 1, including metadata/off;
non-full partial output also says `UNVERIFIED(payload)`. Only a sealed package
whose declared scope verifies prints `OK` / exits 0, and only full prints a
bare `OK`. Tampering within the declared scope prints `FAILED` exactly like a
tampered sealed one. The JSON report carries `partial` and `intact` booleans
so scripts can distinguish the outcomes.

The verifier never crashes on a missing or malformed manifest: it returns a
`ManifestState` (`kOk` / `kAbsent` / `kInvalid`) with a diagnostic.

## 7. Offline verification

```text
thyme-replay integrity <pkg> [--key <signing-key>] [--json]
thyme-replay verify <pkg> --integrity [--key <signing-key>]
```

No daemon, no sockets, no privileges: the tool only reads the package. Exit
codes: `0` all checks pass on a **sealed** package; `1` mismatch/missing/
unlisted/invalid/absent, a signature failure, **or a partial package**
(reported as `integrity: PARTIAL (...)` when the surviving files are intact,
`FAILED` when any of them is not); `2` usage. Example:

```text
$ thyme-replay integrity /data/high/7_1234567890
package  : /data/high/7_1234567890
manifest : present (unsigned)
kind     : trigger
seal_ns  : 1761205953123456789
session  : 7
partial  : no
trigger  : id=7 t0=1234567890 before=10000000000 after=5000000000
chain    : OK
files    : 3 ok, 0 missing, 0 mismatched, 0 unlisted
signature: absent (unsigned)
file     : OK      meta.tbin (412 B)
file     : OK      small.tbin (18432 B)
file     : OK      wal.log (264 B)
integrity: OK
```

`--json` emits the same fields as one JSON object (per-file states included).

## 8. Performance and residual risk

* REC-22 hashes bytes incrementally as the recorder writes them, so clean and
  actively invalidated seals do not re-read package content. REC-25 persists
  those hash contexts in an atomic `integrity.checkpoint` at most once per
  second and at package boundaries. Startup recovery validates its format,
  CRC, session and file sizes, rehashes only the small mutable WAL, and reuses
  unchanged payload contexts. A missing, partial, corrupt or stale checkpoint
  falls back to a full scan; checkpoint and temporary files are excluded from
  the manifest. REC-23 metadata/off raw entries skip SHA updates but retain
  incremental byte counts; non-raw files keep the incremental full-hash path.
  Signing remains detached and can be verified offline.
* REC-24 makes the append contract explicit: `ThymeBinFileSink` and the shared
  package writer require `ftell == incremental_size` before writing, so
  seek+overwrite must reset with `Replace` instead of being mislabeled as an
  append. Manifest generation `stat`s every listed file and rejects size
  mismatches without reading the content again.
* Incremental state and checkpoints are not a forensic trust boundary for
  same-size external payload tampering: an outside writer can change bytes
  after the recorder hashed them, and a checkpoint's size check cannot detect
  that change. The small WAL is deliberately re-read during recovery because
  seal/abort may overwrite it at a constant size. Independent
  `VerifyPackageManifest` / `thyme-replay integrity` re-reads and remains
  mandatory when external changes are possible; incremental hashing does not
  replace that verification.
* The manifest cannot stop deletion or replacement of the **whole** package:
  a complete-looking package with a valid manifest can be dropped in by
  anyone who has the signing key (or by an unsigned deployment). Policy must
  treat "complete package without a verifiable manifest" as untrusted and
  retention/audit must cover directory-level changes.
* There is no anti-rollback/sequence anchor across packages; retaining the
  newest sealed manifest out-of-band (or the audit log) is the mitigation.
* Same-uid/root can still read payloads (no encryption) and, for unsigned
  manifests, rewrite a package into a self-consistent one.
* `integrity.manifest` itself is not in its own file list (it cannot hash
  itself); its authenticity comes from the HMAC, and its integrity is checked
  by the self-consistency chain.

## 9. Tests

`tests/test_package_integrity.cpp` (ctest `test_package_integrity`):

* a real `TriggeredRecorder` capture seals with a manifest that verifies OK
  (trigger id / window / session / seal time included);
* one modified byte → per-file mismatch + chain failure, CLI exit 1;
* one deleted file → missing + chain failure;
* an unlisted extra file → unlisted + exit 1;
* an edited manifest → `INVALID`;
* absent manifest (pre-SEC-03 package) → `absent`, no crash;
* HMAC: correct key OK, wrong key fails, no key fails closed, missing
  `integrity.hmac` fails closed;
* big-frame channel: `frames/<channel>.raw` + `frames/<channel>.index.tbin`
  are covered by the manifest, and a flipped byte in the `.raw` bulk payload
  is detected (REC-18);
* REC-18 partial manifests: a fault-injected seal failure produces an
  `.incomplete` package whose partial manifest verifies as PARTIAL (files
  intact, exit 1), a tampered survivor flips it to FAILED, and the retention
  startup sweep manifests a crash-leftover package (session id recovered
  from the wal header).
* REC-25 checkpoints: round-trip and repeated digest, wrong session, partial
  and corrupt files, size drift, same-size WAL changes and crash-leftover temp
  files; read counting proves the valid fast path avoids a 1 MiB payload and
  the corrupt fallback reads it. A real `SIGKILL` during simultaneous trigger
  and continuous recording leaves two recoverable partial packages.
