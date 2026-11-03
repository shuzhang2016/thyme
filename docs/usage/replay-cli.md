# thyme-replay and triggerctl: replay and recording control

This document covers two command-line tools: `thyme-replay` handles **package
reading and replay**, and `triggerctl` handles **recorder control, package seal
verification, and manual triggers**. Both work only through public file/socket
contracts and do not link against business nodes.

Binary locations:

```text
build/tools/replay/thyme-replay/thyme-replay
build/tools/apps/trigger_center/triggerctl
```

## 1. Tool roles

`thyme-replay` is a thin shell over the replay core (`ReplayPackage` /
`ReplaySession` / `ReplayPlayer`). `play` is a **fake producer**: it injects
events into explicitly named target channels along the package's `mono_ns`
timeline. By default it holds the in-header producer gate, which keeps real
drivers from double-writing during replay; `--force` can recreate an existing
PinnedSlot channel, but it never steals the gate from another live replay.

`triggerctl` is the recorder's unified CLI: `arm`/`disarm` control continuous
recording, `status` queries recorder state, `verify` validates a sealed package
and its evidence manifest, and any `<source>` sends one manual trigger through
the trigger center. Package structure/CRC validation still belongs to
`thyme-replay verify`.

## 2. `thyme-replay` commands

```text
thyme-replay <command> <package-dir> [options]
thyme-replay repair <input.tbin> --out <output.tbin> [--json]
thyme-replay status --status-file <path> [--json]
```

Every command accepts `--json`. `play` accepts one or more package directories;
the other package-reading commands accept exactly one directory, `status` takes
no directory, and `repair` takes one `.tbin` file. A wrong argument count
returns 2.

### 2.1 `list` / `info` / `schema`

| Command | What it reads | Event traversal |
| --- | --- | --- |
| `list` | channels, kinds, record counts, time span, geometry, index state, integrity | no |
| `info` | `list` information + per-channel rate/byte counts | once |
| `schema` | Schema sidecar: contract, field layout, manifest identity | no |

```bash
build/tools/replay/thyme-replay/thyme-replay list /data/captures/trigger_000012
build/tools/replay/thyme-replay/thyme-replay info /data/captures/trigger_000012 --json
build/tools/replay/thyme-replay/thyme-replay schema /data/captures/trigger_000012
```

Each channel in `list --json` carries `structs[]` (`struct_id`, `name`,
`channel`, `record_key`, `contract_version`, `payload_size`,
`layout_hash`) and groups this information into `seek` / `offsets` / `geometry` /
`crc`; `indexed` is retained as a compatibility field.

`schema` exits 1 when the sidecar is missing (packages that predate the schema
sidecar) or unparsable; it never guesses the field layout. Field offsets are
absolute offsets within the record payload.

### 2.2 `verify` / `integrity`

| Command | Validation layers |
| --- | --- |
| `verify` | Open-time record CRC/registry/footer/offset validation + per-frame CRC over every PinnedSlot payload + timeline monotonicity + schema/manifest identity |
| `integrity` | Evidence manifest: per-file SHA-256, hash chain, optional HMAC signature; does not decode payloads |

```bash
build/tools/replay/thyme-replay/thyme-replay verify /data/captures/trigger_000012
build/tools/replay/thyme-replay/thyme-replay verify /data/captures/trigger_000012 \
    --integrity --key /etc/thyme/signing.key
build/tools/replay/thyme-replay/thyme-replay integrity /data/captures/trigger_000012 \
    --key /etc/thyme/signing.key --json
```

`--integrity` applies only to `verify` and runs the evidence manifest once more
after structural validation; `--key <path>` supplies the raw HMAC-SHA256 key.
`integrity` concludes OK / PARTIAL / FAILED: only "sealed and with a complete
manifest" is OK; `.incomplete` with intact surviving files is PARTIAL, which
together with FAILED exits non-zero (PARTIAL is not a complete package either).

`verify` always performs per-frame CRC validation; `--no-verify-crc` applies
only to `play`/`export`, for recordings known to have `crc=off`, or to inspect
structure first while troubleshooting.

### 2.3 `play`

```bash
build/tools/replay/thyme-replay/thyme-replay play <pkg> \
    --pinned_slot cloud_1=/thyme_cloud_1234 \
    --small small:0=/thyme_recr_1234:9 \
    --rate 1.0 --status-file /tmp/thyme-play.json
```

Multi-slice absolute/stitch replay can pass an explicit package list, or a root
directory containing `continuous_*` sub-packages:

```bash
build/tools/replay/thyme-replay/thyme-replay play \
    /data/captures/continuous_000020 /data/captures/continuous_000021 \
    --timeline=absolute --pinned_slot cam=/thyme_cam_1234 --as-fast
build/tools/replay/thyme-replay/thyme-replay play /data/captures/continuous \
    --pinned_slot cam=/thyme_cam_1234 --as-fast
```

`--timeline=absolute` is the default. Explicit packages and sub-packages
discovered from a directory are sorted by the WAL `slice_seq` (or by
first-event time when there is no WAL), and every package's channel/registry
contract is pre-checked before any target is created or gate acquired. Slice
changes reuse the same injector and gate set. In absolute mode, records in later
packages whose `mono_ns` is less than the last delivered event of the previous
package are skipped and counted in `join_duplicates`; identical timestamps are
kept. Time holes are counted in `join_holes`.

`--timeline=stitch` places the first event of a later slice one sampling period
after the last event of the previous non-empty slice. The period is the smallest
positive delta between adjacent distinct `mono_ns` values across all data events
of that slice (including metadata data, merged across channels); when no delta
is available it reuses the previous slice's period, and 1 ns if there is none
either. Single-sample and mixed-rate slices follow the same rule, and empty
packages do not move the anchor. Stitch performs no absolute overlap skip, leaves
ordinary payloads untouched, and shifts the transport and embedded-frame-header
mono values together. Offsets may be negative, and any time/offset overflow
fails explicitly before injection.

Cross-package seeks in either mode add no joins or holes, and `--to` still stops
before the first out-of-range event is injected; in stitch mode, seeks and
windows use the shifted timeline. `join_holes` is always counted at source-package
boundaries, so it can be non-zero after stitching too.

Targets must be named explicitly; the tool **never guesses by name**:

| Target form | Meaning |
| --- | --- |
| `--pinned_slot <channel>=<shm-name>` | Inject a package PinnedSlot channel into the named channel; the tool creates and owns the channel |
| `--small <channel>:<struct_id>=<ring>:<target-struct-id>` | Explicit small-struct mapping |
| `--small <channel>=<ring>` | Automatic mapping: resolve each struct by `(record_key, contract_version, payload_size, layout_hash)` |

PinnedSlot targets are created by the replay tool and kept after it exits;
small-struct rings must already exist and are attached read-write, never created
or overwritten. `--pinned_slot-slots <n>` sets the ring depth of newly created
channels, default 64, range 1..65536.

Target and safety options:

| Option | Effect |
| --- | --- |
| `--strict-targets` | Fail immediately (exit 3) when a data channel has no target |
| `--allow-missing` | With automatic mapping, skip and count channels/structs absent from the target topology; a hard error by default |
| `--check-compat` | Resolve and print the mapping table, then exit; no create/attach/inject |
| `--dry-run` | Resolve and report exactly like `--check-compat` without touching live targets |
| `--force` | Allow replacing an existing PinnedSlot channel; **does not** override a live replay gate |
| `--no-gate` | Do not hold the producer gate (unsafe: real producers may double-write) |
| `--no-verify-crc` | Skip per-frame CRC validation before injection |

`--check-compat` and `--dry-run` differ only in the `state` label in the report
(`check-compat` / `dry-run`); both exit 0 to mean resolution succeeded, and
resolution/target errors return 3. In the mapping rules, a key hit with an
inconsistent tuple (version/payload/layout) is always fatal and never silently
remaps; duplicate keys are `ambiguous` and also fail.

Pacing and windows:

| Option | Default/value |
| --- | --- |
| `--timeline=absolute` | Replay single packages/multi-slices on the original `mono_ns`; default |
| `--timeline=stitch` | Close inter-slice gaps using the measured sampling period without rewriting ordinary payloads |
| `--rate <r>` | Speed multiplier, default `1.0`, must be > 0 |
| `--as-fast` | Disable pacing |
| `--pause` | Start paused; on a tty, `r`/`s`/`p`/`q` and `rate`/`seek` are available |
| `--step` | Deliver exactly one event, then stop |
| `--seek <t>` / `--from <t>` | Start at a timeline offset (mutually exclusive, synonyms) |
| `--to <t>` | Stop before the first event beyond `t` (`t` itself is included) |

Time forms: a bare integer is an ns offset; `250ms`, `1.5s`, `2m`, `1h` scale by
unit; `HH:MM:SS.mmm` is an offset from the first event of the first package;
`@<mono_ns>` is an absolute timeline time.

Status document:

```bash
build/tools/replay/thyme-replay/thyme-replay play <pkg> --pinned_slot cam=/thyme_cam_1234 \
    --status-file /tmp/thyme-play.json --status-interval-ms 200
build/tools/replay/thyme-replay/thyme-replay status --status-file /tmp/thyme-play.json --json
```

`--status-file` publishes JSON atomically via tmp+rename, by default every
200 ms; state changes and termination force a flush. `--status-interval-ms`
ranges from 1..3600000. A status path allows only one `play` writer.

Key fields of the status document:

| Field | Meaning |
| --- | --- |
| `tool`, `command`, `version` | Fixed as `thyme-replay` / `play` / `1` |
| `state` | `running`, `paused`, `finished`, `failed`, `stopped` |
| `package`, `pid` | Package directory and process id |
| `mode`, `rate`, `timeline` | `paced`, `as-fast`, `step`; runtime-variable rate; `absolute` / `stitch` |
| `events`, `injected`, `skipped`, `ignored`, `failed`, `unmapped` | Event counters |
| `slice_joins`, `join_duplicates`, `join_holes` | Natural slice changes, overlap skips, time holes |
| `position_mono_ns` | Position of the last delivered event (or seek target) |
| `first_mono_ns`, `last_mono_ns` | First/last event delivered by this run |
| `package_first_mono_ns`, `package_last_mono_ns` | Overall package time range |
| `window` | `{"from_ns","to_ns"}` or `null` |
| `setup_ns`, `elapsed_ns` | Target setup time, run time |
| `stopped`, `window_closed`, `step_done` | Termination-reason flags |
| `error`, `warnings` | Error string or null; warning array |
| `updated_realtime_ns`, `updated_mono_ns` | Time of the most recent publication |
| `channels[]` | Per-channel `id/name/kind/events/injected/skipped/ignored/failed/unmapped/bytes/last_mono_ns` |

`status` exits 1 when the file is missing, unreadable, or not a status document
(missing `tool`/`state`); without `--status-file` it exits 2.

### 2.4 `export`

```bash
build/tools/replay/thyme-replay/thyme-replay export <pkg> --out pkg.mcap
build/tools/replay/thyme-replay/thyme-replay export <pkg> --out pkg.ndjson \
    --format ndjson --schema-out schema.json --from 250ms --to 2s
```

`export` is a read-only streaming export that writes events along the unified
`mono_ns` timeline; `--from/--to` accept the same time forms as `play`.

| Option | Default/constraint |
| --- | --- |
| `--out <file>` | Required (missing exits 2) |
| `--format mcap\|ndjson` | Default `mcap` |
| `--schema-out <path>` | Copy `<pkg>/schema.json` and reference it in the output |
| `--mcap-buffer-bytes <n>` | Default 65536; range 256..16777216 |
| `--no-verify-crc` | Skip per-frame payload CRC validation |
| `--from/--to` | Export window |

MCAP output reuses the MCAP writer (Header/Schema/Channel/Message/Statistics/
Footer, no chunk/index/attachment), with topics shaped as
`thyme/<channel>/<struct>`, `thyme/<channel>/raw`, `thyme/<channel>/gap`.
NDJSON is a generic base64 republish that references the schema sidecar.

### 2.5 `repair`

```bash
build/tools/replay/thyme-replay/thyme-replay repair /data/captures/small.tbin \
    --out /data/captures/small.repaired.tbin --json
```

`repair` works on **one `.tbin` file only**. It first validates
header/registry/manifest with `ThymeBinReader`, then reads sequentially and
rewrites the valid prefix. It stops at the first `kCorrupt` (including a record
CRC error or truncated data): no resync, no synthesized GAPs, and no fabricated
lost records. A corrupt header/registry/manifest returns 1 and produces no
output; when the first record is corrupt, a header-only PARTIAL file is allowed.
A missing or corrupt footer whose records are still readable also permits
salvage, and the output always rebuilds a seekable footer index.

The input is read-only; `--out` is required and by default refuses an existing
file or the same path as the input. The tool writes a temporary file in the
output directory, re-reads it completely to `kEof`, verifies footer/count, and
publishes it atomically via hard link, so a publish race cannot overwrite an
existing output; failures clean up the temporary file. Successful salvage
returns 0 and the text explicitly marks `PARTIAL (valid prefix only)`; usage
errors return 2 and other failures return 1.

`--json` includes at least `input_corrupt`, `recovered_records`,
`corrupt_offset`, `discarded_tail_bytes`, `output_bytes`, and `partial`. A
completely intact input can also be rewritten, in which case `partial=false`; a
missing footer alone does not mean the records are corrupt.

The rewrite preserves device/session/start, the registry, the manifest
hash/version, and the fields, opaque payloads, and sequence numbers of
data/gap/trigger/session records. The writer recomputes record CRCs, the
manifest CRC, and the index/footer; older files may be upgraded in
file/registry version, so this is **semantic preservation, not a byte copy**.
Session records have no on-disk `mono_ns`, so their index time reuses the
preceding record and the first session uses 0. The command does not handle
package directories, `.raw`, WAL/seal, signature continuation, or the package
manifest.

### 2.6 `--expect-manifest-hash`

```bash
build/tools/replay/thyme-replay/thyme-replay verify <pkg> \
    --expect-manifest-hash 0x9914c81f74037d59
```

The option requires the package's record manifest to match this build, in the
form `0x<16 hex digits>`. `list`/`info`/`verify` exit 1 on a mismatch; `play`
refuses before injecting any event; `export` writes no output. Packages without
a manifest block (legacy) **fail closed** (unknown is not a match).

## 3. `thyme-replay` exit codes

| Exit code | Meaning |
| --- | --- |
| `0` | Success |
| `1` | Package unreadable, validation failed, manifest mismatch, schema missing/corrupt |
| `2` | Usage error (unknown option, argument count, missing `--out`/`--status-file`, etc.) |
| `3` | Returned only by `play` when target configuration/topology checks fail |

## 4. `triggerctl`

```text
triggerctl arm|disarm [--socket PATH] [--timeout MS] [--json]
triggerctl status    [--socket PATH] [--timeout MS] [--json]
triggerctl verify <package-dir> [--key PATH] [--json]
triggerctl <source> [--socket PATH] [--priority N] [--force]
           [--timestamp NS] [--timeout MS] [--json]
```

| Command | Socket | Behavior |
| --- | --- | --- |
| `arm` / `disarm` | control | Arm/disarm continuous recording; datagram, fire-and-forget |
| `status` | control | Request/response snapshot |
| `verify <pkg>` | none | Validate the WAL seal + evidence manifest |
| `<source>` | trigger | Send one manual trigger through the trigger center |

Socket defaults: `/tmp/thyme_recorder.sock` for control and
`/tmp/thyme_trigger.sock` for trigger. `--socket` applies to the surface of the
current command; `--control-socket` is a legacy alias for recorder commands and
wins when both are given.

| Option | Description |
| --- | --- |
| `--timeout MS` | Reply timeout, 1..60000; default 1000 ms for control and 100 ms for trigger; `--timeout-ms` is an alias |
| `--json` | Emit a single JSON object on stdout; errors still go to stderr |
| `--key PATH` | `verify` only: HMAC-SHA256 key for the signed manifest |
| `--priority N` | Priority hint for trigger |
| `--force` | Forced preemption for trigger |
| `--timestamp NS` | Explicit timestamp for trigger |

`arm`/`disarm` only guarantee that the datagram reached the socket; they do not
wait for execution. Use `status` to confirm.

### 4.1 `status --json`

```json
{
  "schema": 1,
  "kind": "thyme-recorder-status",
  "ok": true,
  "socket": "/tmp/thyme_recorder.sock",
  "armed": true,
  "phase": "armed",
  "next_trigger_id": 12,
  "auth_rejects": 0,
  "captures_done": 3,
  "slices_done": 0,
  "pending_bytes": 0,
  "disk_full": false
}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `schema` | int | Document schema, currently 1 |
| `kind` | string | Fixed `thyme-recorder-status` |
| `ok` | bool | Whether the recorder answered; `false` with `error` on transport failure |
| `socket` | string | Control socket that answered |
| `armed` | bool | Continuous-recording arm flag |
| `phase` | string | `armed`, `post`, `settle` (capture FSM) |
| `next_trigger_id` | int | Next trigger id |
| `auth_rejects` | int | Datagrams rejected because the peer uid did not match |
| `captures_done` | int | Completed triggered captures (trigger_center host) |
| `slices_done` | int | Completed continuous slices (trigger_center host) |
| `pending_bytes` | int | Pending queue accounted in bytes |
| `disk_full` | bool | Recorder's ENOSPC latch |

`verify --json` additionally reports `sealed`, `manifest`
(`ok`/`absent`/`invalid`), `intact`, `partial`,
`signed`/`signature_checked`/`signature_ok`, `chain_ok`, and
`files_ok/files_missing/files_mismatch/files_unlisted`. Only "sealed and
complete" yields `ok:true`. `triggerctl verify` does not validate per-frame CRC;
run `thyme-replay verify` when that is needed.

### 4.2 Trigger result

On success the text output is `code=accepted trigger_id=<n> priority=<p>`;
`--json` emits `kind=thyme-trigger-result` with fields `ok`, `source`, `code`,
`transport_ok`, `trigger_id`, `priority`, `priority_clamped`.

Common `code` values: `accepted`, `muted`, `quota_exceeded`, `unknown_source`,
`preempted_by_higher`, `busy`, `recorder_offline`, `invalid_timestamp`,
`transport_timeout`. Only `accepted` returns 0.

### 4.3 Exit codes

| Exit code | Meaning |
| --- | --- |
| `0` | Command accepted / status returned / package validation passed |
| `1` | Recorder rejected, trigger not accepted, or package validation failed |
| `2` | Usage error (unknown flag/command, missing value, wrong positional argument) |
| `5` | Transport failure (socket unreachable or timed out) |

## 5. Typical workflows

### 5.1 Record → validate → export → replay

```bash
# 0) Confirm the recorder is online and arm it
triggerctl status --json
triggerctl arm

# 1) Trigger a capture (source names are defined by the trigger config)
triggerctl manual --priority 3 --json
triggerctl disarm

# 2) Seal and evidence validation
triggerctl verify /data/captures/trigger_000012 --json
thyme-replay verify /data/captures/trigger_000012
thyme-replay integrity /data/captures/trigger_000012 --key /etc/thyme/signing.key

# 3) Export (optional)
thyme-replay export /data/captures/trigger_000012 --out /tmp/pkg.mcap \
    --schema-out /tmp/schema.json

# 4) Replay into the live targets while continuously publishing status
thyme-replay play /data/captures/trigger_000012 \
    --pinned_slot cloud_1=/thyme_cloud_1234 \
    --status-file /tmp/thyme-play.json
thyme-replay status --status-file /tmp/thyme-play.json --json
```

If a real driver is still running during replay, the producer gate blocks its
writes and prints this at the end:

```text
warning: <n> real-producer write(s) were blocked by the replay gate (double-write prevented)
```

This is not an error but evidence that gating is working; only use `--no-gate`
when you genuinely need to coexist with an older target that cannot recognize
the gate, and it prints
`--no-gate: real producers are NOT blocked during replay (double-write detection disabled)`.
A non-zero `gate_steals` means stale replay leases left by an expired or crashed
replay were reclaimed.

### 5.2 Replay after a topology change

When a recorded package no longer agrees with the target ring's struct
ids/manifest row numbers, first use automatic mapping for a read-only check:

```bash
thyme-replay play <pkg> --small small=/thyme_recr_1234 --check-compat
thyme-replay play <pkg> --small small=/thyme_recr_1234 --check-compat --json
```

The mapping table resolves by `(record_key, contract_version, payload_size,
layout_hash)`. A key hit with an inconsistent tuple must be fixed in the package
or the target changed; `--allow-missing` cannot bypass it. Only "the target
genuinely has no such key" counts as a missing target:

```bash
thyme-replay play <pkg> --small small=/thyme_recr_1234 --allow-missing
```

`--allow-missing` applies only to the automatic mapping form and counts skipped
channels/structs in `mappings_missing`. With the explicit `<channel>:<struct_id>`
form, a missing target is still a hard error.

## 6. Safety notes

- `play`'s PinnedSlot channels are **created by the tool** and kept after the
  run; a repeated run refuses because they already exist unless `--force` is
  given. Even `--force` does not override a live replay gate.
- Small-struct rings are attached only, never created or overwritten; start the
  target process first.
- Only one replay may target the same destination at a time; the second gets a
  busy/gated error.
- `--no-gate` is an escape hatch, not the default path; in production, stop the
  real driver during replay.
- One status file is written by one `play` only; pollers should treat a file
  missing `tool`/`state` as not a status document.
- `--expect-manifest-hash` is a fail-closed identity check; do not "skip
  validation first" to handle a version mismatch.

## Related documentation

- [thyme_probe read-only observation](probe.md)
- [Other tools](tools-other.md)
- [Replay API](api-replay.md)
- [Triggered acquisition](triggered-acquisition.md)
