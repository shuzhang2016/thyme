# Replay (`thyme::replay` / `thyme-replay`) and real-time producer gating

This document covers replay runtime modes, target-channel contracts, and
producer gating. Package format:
[ADR 0005](../design/adr/0005-thyme-bin-record-format-and-package-layout.md);
header rewrite policy: `include/thyme/replay/header_policy.h`.

## 1. Replay pipeline

| Component | Location | Responsibility |
|---|---|---|
| `ReplayPackage` | `src/replay/replay_reader.cpp` | Open a package read-only and k-way merge into one `mono_ns` timeline |
| `ReplaySession` | `src/replay/replay_session.cpp` | Pacing / pause / step / seek / stop |
| `PinnedSlotInjector` | `src/replay/pinned_slot_injector.cpp` | Explicit `channel → PinnedSlotWriter*` table for large frames |
| `SmallStructInjector` | `src/replay/small_struct_injector.cpp` | Explicit `(channel, struct_id) → ring slot / synchronous publish sink` table; geometry, sequence, header rewriting |
| `ReplayPlayer` | `src/replay/replay_player.cpp` | Synchronous driver for one session plus both injectors |
| `ReplayPlaylist` | `src/replay/replay_playlist.cpp` | Continuous-slice discovery/ordering, absolute continuation, cross-package seek |
| `thyme-replay` | `tools/replay/thyme-replay/` | `list/info/play/verify/integrity/schema/status/export` CLI |

Replay is a **fake producer**: the replay process owns the target channels, and
`ReplayPlayer` enforces injection order and non-decreasing `mono_ns`. Real
drivers (cloud/image/h264 and `demo_producer`) must stop or be gated during
replay.

### 1.1 Generated standalone replay

The generated `replay_tool` still accepts a single `.tbin` file, not a package
directory. It treats it as local channel 0 and uses
`ThymeRecordFillStructDescs` + `ResolveTopologyMapping` to map file source ids
to current generated ids; package channel numbering is independent. Mapped ids
are used for topic/stage filtering and publication.

All mapped structs go through one `SmallStructInjector`: geometry checks,
embedded `FrameHeader` mono/session/sequence rewriting, and reordered sequence
semantics match the ring path. Each target's synchronous sink capacity is the
generated descriptor's `payload_size`; callbacks still call `ReplayInjectOne`,
so pool/latest/`NotifPipe`/broadcast publication paths are unchanged. A sink
must consume the payload before returning and must not retain its pointer; the
injection hot path does not allocate. Bare POD has no embedded protocol header
and is passed through without faking mono/sequence. Sequence advances only after
successful publication.

Version/payload/layout/ambiguous conflicts fail before injection (zero
injection); missing sources in the target are skipped per row and counted in
`entries_skipped`. `SeekToMonoNs`, `--from-mono`/`--to-mono`, full-filter
fallback without a footer, row ordering, pacing, pause, and stop semantics are
unchanged.

### 1.2 Library-level absolute/stitch multi-package replay

`ReplayPlaylist::Open(root, cfg)` discovers direct `continuous_*` child
directories when `root` is not itself a package, or takes an explicit array of
package directories. `Open` fully preflights every package, orders by the WAL
header's continuous `slice_seq` (falling back to package `first_mono_ns`), and
validates channel id/kind/registry contracts. Any
version/payload/layout/ambiguous/missing or source/target struct-id
inconsistency fails before target injection and gate acquisition.

`Pump()` continues via `ReplayPlayer::OpenNextPackage()` at a package end;
`Seek()` switches packages by their first/last time range. The playlist holds
one `PinnedSlotInjector`, one `SmallStructInjector`, and one PinnedSlot gate;
rate/as-fast and the header policy are preserved across packages.

Absolute mode forces `skip_overlaps`: records in later packages whose `mono_ns`
is below the previous package's `last_event_ns` are counted as
`join_duplicates`, not injected and not counted as `mono_regressions`; batches
exactly at that timestamp are kept. `PlayerStats` also exposes `slice_joins` and
`join_holes` (later package's first timestamp greater than the previous
package's last). Seeking across packages does not add joins/holes.

`thyme-replay play` uses the same plan: multiple explicit package directories,
or one root without `meta.tbin` containing `continuous_*` children. The CLI
preflights all slices with a target-less playlist before creating targets;
switching slices replaces only `ReplaySession`, not injectors/gates. `--from` /
`--seek` can cross packages; `--to` closes the window before an out-of-range
event. Summary/JSON/status expose `timeline`, `slice_joins`, `join_duplicates`,
`join_holes`.

`--timeline=absolute` is the default. With `cfg.stitch_timeline=true` /
`--timeline=stitch`, `BuildReplayStitchPlan()` scans all slices before injection
and records each slice's `offset_ns`, `timeline_first_ns`, `timeline_last_ns`.
The first offset is 0; each later slice's first valid event is one sample period
after the previous non-empty slice's last valid event. The period is the
smallest positive delta between adjacent distinct `mono_ns` values among **data**
events (including metadata data) on that slice's merged timeline, not a fixed
100 ns assumption. Mixed rates therefore use the smallest cross-channel
interval; a single sample or no usable delta inherits the previous slice's
period, falling back to 1 ns. Empty packages neither lose samples nor move the
previous anchor; later non-empty slices still stitch from that anchor.

Offsets may be positive or negative; target times, offsets, and event shifts are
checked for `uint64_t` overflow, and a failed preflight means zero injection.
Stitch does not perform absolute overlap skipping, so `join_duplicates` is
always 0; `join_holes` still describes source-package boundary gaps and is not
required to be 0 after stitching. Seek and `--to` use stitched boundaries and
map back to the current package's source time at runtime. The offset is applied
to `ReplayEvent` before injection, so both PinnedSlot `Commit()` and embedded
`FrameHeader::mono_ns` use stitched time, not just summary time. Setting
`skip_overlaps` and `stitch_timeline` together is rejected. Single-package and
absolute behavior are unchanged; stitch rebuilds the logical timeline only.

## 2. Producer gating

### 2.1 Why in the core / shared memory

Real drivers mostly live in other processes or repositories. In-process
`replay_mode::Active()` can constrain only generated nodes in the same process;
it cannot stop another process's driver from writing the same SHM. The gate
therefore lives in the **channel header**: every writer goes through
`PinnedSlotWriter::Begin/Commit`, and the check is on the core write path, with
no per-driver adaptation.

### 2.2 Gate object and ABI

`include/thyme/runtime/producer_gate.h` defines `ProducerGate`, embedded in
`PinnedSlotPageHeader`:

- `owner_token`: unique holder token (0 = unclaimed). A second replay gets
  `GateResult::kBusy` and does not share the channel.
- `owner_pid` / `owner_start`: process identity plus `/proc/<pid>/stat`
  starttime (protects against pid reuse).
- `heartbeat_ns` / `timeout_ns`: monotonic heartbeat lease (default 2 s; the
  CLI heartbeats every 250 ms).
- `generation`: incremented on acquire/release.
- `rejects` / `steals`: blocked write attempts / reclaimed expired leases,
  read-only observability.

The current pinned-slot page is **v4** (v2→v3 introduced gating, v4 added the
super-header). The small-struct `RecordMirrorRingPage` is **v6**, with an
independent per-struct `ProducerGate` in each ring descriptor. v4 channels / v6
mirrors do not mix with old pages: readers/writers reject by
magic/version/layout hash, and `Create()` returns
`CreateStatus::kIncompatible`. Explicit `force_replace=true` /
`thyme-replay --force` only allows post-shutdown recreation; it does not migrate
data or bypass a live gate, and an old writer cannot use it to write a new
channel.

**Upgrade coupling:** out-of-tree drivers (cloud/image/h264) must be rebuilt
from the same tree; old-header drivers cannot write v4 channels or v6 mirror
pages. Replay/injectors must also match. The upgrade order (stop producers →
stop consumers → replace binaries → recreate SHM → start producer/consumer) is
in [`upgrade.md`](upgrade.md).

### 2.3 Error semantics (explicit, never silent corruption)

| Scenario | Result |
|---|---|
| Real producer `Begin()` while replay holds the gate | `BeginView::status == BeginStatus::kGated`, `payload/header == nullptr`, no slot/head touch; `gate.rejects++`. Distinct from `kOversize`/`kPinnedSkip` |
| Real producer `Begin()` succeeds but replay takes the gate before `Commit()` | `Commit()` does not publish (slot stays `seq==0`, head does not advance), `gate_aborted++` |
| A second replay/injector attaches the same target | Large-frame channel or the same `(ring, target_struct_id)` `Open` returns `ReplayStatus::kBusy`; different structs on the same ring may coexist. `PinnedSlotWriter::Create` under a live gate returns `CreateStatus::kGated` (even with `force_replace`) |
| Small-struct real producer writes the target struct while replay holds the gate | Default `writer_token=0` `TryWriteRing` returns false, `gate.rejects++`, ring head does not advance; replay writes with its own token |
| Replay loses the gate while running | Both injectors' `Inject` return `InjectStatus::kGated` and count `failed`/`gated`; never overwrite the writer that took the gate, never miscount as an ordinary write failure |
| Replay calls `Close()` / exits normally | Releases the gate (writer destruction / `ReleaseProducerGate`); real producers resume immediately; the small injector releases only the target structs it still holds |

At the end, the CLI reads `gate.rejects`/`gate.steals` and warns when nonzero:
`rejects` means a real driver is still running but double writes were blocked;
`steals` means an expired lease was reclaimed.

### 2.4 Recovery (no permanent gate after a crash)

The gate is a lease, not a boolean lock:

1. Replay process killed: the next writer's `Begin()`/`TryWriteRing()` sees the
   owner pid gone from `/proc` (or a starttime mismatch) and reclaims
   immediately (`steals++`).
2. Replay thread wedged/paused while the process lives: after the heartbeat
   stops for `timeout_ns`, the writer reclaims. **A paused replay must keep
   heartbeating**; `thyme-replay` uses an independent heartbeat thread for
   pause/step waits, and a `ReplayPlayer` host should call
   `ReplayPlayer::Heartbeat()` during long pauses, renewing both big-frame and
   small-struct targets.
3. Normal exit: both injectors' `Close()` release their tokens; a non-holder
   release is `kNotHeld` and cannot clear another owner's gate.

### 2.5 CLI

```
thyme-replay play <pkg> --pinned_slot cloud_1=/cloud1 ...
```

- Default: hold gates and start the heartbeat thread. `--no-gate` is an
  explicit diagnostic escape hatch for same-version targets only; it prints a
  warning and disables double-write detection. It is **not** a compatibility
  mode for old channels/pages.
- `--force` allows recreating a version-incompatible old channel after
  shutdown; it does not migrate data and does **not** override a live gate.
  Stop the replaying process to change targets. Replay/injectors and real
  producers must still match the target page version.
- An existing live channel is still rejected before `--force` by the normal
  channel rules; the gate check is a second layer at `Create`, so a caller that
  skips probe cannot silently take a replay target.
- By default, producer `sequence` is renumbered from 0 per target channel so a
  different recorded topology or seek start does not carry old sequence numbers
  into a new session; use `--preserve-sequences` for forensic comparison. This
  changes only sequence policy; `mono_ns`, replay session, and CRC rewriting are
  unchanged.

## 3. Record schema and manifest

A package's decoding contract is not in each `.thyme.bin` record; it is
`<pkg>/schema.json` (written at seal) plus the manifest block in `meta.tbin`.
Both are written by the recorder and are **independent**:

```
schema.json   format=thyme-record-schema v2, partial,
              record_manifest{hash,version,channels[{struct_id,channel,
              record_key,contract_version,payload_size,layout_hash}]},
              frame_header{40 B, fields[]}, records[{struct_id,name,
              contract_version,payload_size,channel,record_key,layout_hash,
              payload_offset,contract_bytes,layout,fields[]}]
meta/small    v4 header + 128 B ThymeStructDesc v2 registry + 32 B THYMANI1
              manifest block (manifest_version + kRecordManifestHash + CRC32)
              (v1..v3 packages still use the 40 B legacy registry)
```

Field offsets are **absolute within the record payload**
(`payload_offset + field.offset`); `payload_offset` is 0 for a bare contract and
`align_up(36, alignof(T))` for `sizeof(Frame<T>)` (`Frame` is not packed).
Records with `layout=unavailable` guarantee only
`struct_id/name/contract_version/payload_size`; tools must treat them as raw
bytes.

Rules:

| Scenario | Behavior |
|---|---|
| Current build × sidecar + manifest block, consistent | `list/info/verify/schema/play` work; `manifest: 0x… version=1` |
| Sidecar and block hash/version differ, or only one exists | `Open()` fails (`manifest-mismatch`), zero events; no half-trusted decoding |
| Sidecar unparseable / layout inconsistent / unknown key | `Open()` fails (`bad-schema`) |
| `--expect-manifest-hash 0x…` mismatch | `list/info/verify` exit 1; `play` refuses before injection; `export` writes nothing |
| Legacy package (v1/v2, no sidecar, no block) | Reads/verifies normally; `schema` reports absent; `--expect-manifest-hash` fails closed |

```bash
thyme-replay schema <pkg>
thyme_probe  schema <pkg> [--json]

thyme-replay verify <pkg> --expect-manifest-hash 0x9914c81f74037d59
thyme-replay play   <pkg> --expect-manifest-hash 0x9914c81f74037d59 --as-fast

thyme_log_merge a.bin b.bin --expect-hash 0x9914c81f74037d59   # mismatch/legacy -> exit 4
```

`schema` is read-only: no SHM attach, no file writes.

**Decoding without compiling thyme** (uses only `schema.json` + documented
`.tbin` layout):

```python
import json, struct

FMT = {"uint64": "Q", "uint32": "I", "int32": "i", "float32": "f"}
sc = json.load(open("<pkg>/schema.json"))
rec = {r["struct_id"]: r for r in sc["records"]}
data = open("<pkg>/small.tbin", "rb").read()
# thyme_bin header = 40 B (struct_count at offset 32), registry entry = 40 B.
off = 40 + struct.unpack_from("<I", data, 32)[0] * 40
while off + 32 <= len(data):
    typ = data[off]
    sid = struct.unpack_from("<H", data, off + 12)[0]
    size = struct.unpack_from("<I", data, off + 28)[0]
    payload = data[off + 32: off + 32 + size]
    r = rec.get(sid)
    if typ == 1 and r and r["layout"] == "known":
        base = r["payload_offset"]
        values = {
            f["name"]: struct.unpack_from(
                "<" + FMT[f["type"]], payload, base + f["offset"])[0]
            for f in r["fields"]
        }
        print(r["name"], values)
        break
    off += 32 + size + 4
```

When layout is unavailable, split only by `payload_size`; `partial: true` means
the package was invalidated and is still forensically readable.

### 3.1 Topology compatibility mapping

`RecordStructId` is a manifest row number and changes when recorded channels are
added/removed/reordered. Stable identity is written into each
`ThymeStructDesc`:

```
layout_hash   FNV-1a 64 over (cpp_type, payload_bytes, field_count, align,
              fields[name,type,offset,size,count,stride])
channel       recorded channel name
record_key    canonical key (currently equal to channel)
```

`thyme-replay play` automatic mapping omits target struct ids and resolves by
`(record_key/channel, contract_version, payload_size, layout_hash)` in the target
`RecordMirrorRingPage` registry:

```bash
thyme-replay play <pkg> --small small=/thyme_recr_1234 --as-fast
thyme-replay play <pkg> --small small=/thyme_recr_1234 --check-compat
thyme-replay play <pkg> --small small=/thyme_recr_1234 --dry-run --json
```

| Case | Behavior |
|---|---|
| Unique key hit, triple matches | Map to target `struct_id` (independent of source id) |
| Key hit but version/size/layout differs | Explicit failure (`version-mismatch` / `payload-mismatch` / `layout-mismatch`); never silently remap |
| Key hit but one side has `layout_hash==0` | Allowed, reported as `mapped-unverified` |
| Source channel/struct missing in target | Fails by default; `--allow-missing` skips and counts |
| Legacy source (no channel/key, v1..v3) | Map by `struct_id`, report `mapped-legacy` |
| Duplicate target key | `ambiguous`, failure |

`--check-compat` / `--dry-run` only resolve and print the mapping table, creating
and injecting nothing; `--json` outputs `mappings[]` and `mapping_counts`.
`list`/`info --json` include `structs[]` per channel.

## 4. Seek/index metadata

`ReplayPackage::Open()` records seek/index metadata per source, exposed by
`ReplayChannel` and `list --json`/`info --json`:

| Field | Meaning |
|---|---|
| `first_mono_ns` / `last_mono_ns` | First/last record timestamp for the source (including trigger/gap) |
| `record_count` / `data_count` / `gap_count` | Total / data / GAP counts |
| `data_offset` / `records_end_offset` | First record offset / end of record stream (footer entries start when indexed) |
| `first_record_offset` / `last_record_offset` | Absolute file offsets |
| `min_data_len` / `max_data_len` | Payload geometry |
| `first_raw_offset` / `last_raw_end` | Referenced `.raw` range for large frames (0 otherwise) |
| `seek_entries` / `seek_stride` | Available seek block points / spacing |
| `record_crc_checked` | Records CRC-checked during Open |
| `index_crc_checked` | Whether the footer index CRC was checked (1/0) |
| `frame_crc_declared` / `frame_crc_absent` | Frame index rows with nonzero/zero `payload_crc` |

`list --json` groups these and keeps `indexed`:

```json
"seek":     {"index":"footer","indexed":true,"entries":5,"stride":64,
             "complexity":"O(log n)"},
"offsets":  {"tbin_data":40,"records_end":12345,"first_record":40,
             "last_record":12200,"records_bytes":12305,
             "first_raw":0,"last_raw_end":98304},
"geometry": {"min_payload_bytes":512,"max_payload_bytes":1024,
             "tbin_bytes":12400,"raw_bytes":98304},
"crc":      {"records":"ok","record_checked":300,"index":"ok",
             "frames_declared":200,"frames_absent":0}
```

`crc.records = ok` means Open checked all record CRCs; `crc.index = absent`
means no footer (legacy/streaming/unfinished crash), not an error;
`frames_absent > 0` means the recorder disabled per-frame CRC, so `verify`
counts those frames as skipped and export counts them as unverified by default.

### 4.1 O(log n) and fallback

`SeekMonoNs(t)` per source:

1. `upper_bound` on the block table for the last point with `mono_ns <= t`
   (O(log n));
2. parse sequentially from that point, crossing at most one block:
   - with a footer: `kThymeBinIndexBlockSize = 64` records;
   - without: Open builds a `kScanBlockRecords = 256` sparse table during its
     validation scan, using the same path.

There is **no "scan the whole file on every seek"** path; the only full scan is
Open's validation (and `verify`/`info` event iteration).
`seek_calls()`/`seek_records_scanned()` expose the last seek's parsed record
count; tests assert an upper bound rather than wall-clock time.

## 5. Status and runtime control

`thyme-replay` is a thin shell: the core has `ReplaySession` and injectors;
argument parsing, status publishing, and export live in
`tools/replay/thyme-replay/`.

**Startup controls:** `--rate`, `--as-fast`, `--pause`, `--step`, `--seek`
(= `--from`), `--to`, `--status-file`, `--status-interval-ms`.

**Runtime controls** (tty, with `--pause`; one command per line):

| Input | Behavior |
|---|---|
| `p` / `r` | Pause / resume |
| `s` | Step one event and stay paused |
| `q` | Stop (same as SIGINT/SIGTERM) |
| `rate <r>` | Change rate at runtime (> 0) |
| `seek <t>` | Runtime seek (same form as `--seek`, relative to the first package event) |

Control-thread requests are queued and applied by the playback thread between
events (`Seek()` is playback-thread-only). Requests issued while paused wake the
wait and take effect immediately without losing events.

**Status document:** `play --status-file <path>` atomically writes JSON via
tmp+rename (default every 200 ms, forced on state change and exit):

```json
{"tool":"thyme-replay","command":"play","version":1,
 "state":"paused","package":"/data/pkg","pid":123,"mode":"paced",
 "rate":1.000,"events":42,"injected":40,"skipped":0,"ignored":2,
 "failed":0,"unmapped":0,"position_mono_ns":1500,"first_mono_ns":1000,
 "last_mono_ns":1500,"package_first_mono_ns":1000,
 "package_last_mono_ns":900000,"window":{"from_ns":1200,"to_ns":1500},
 "setup_ns":900000,"elapsed_ns":250000000,"stopped":false,
 "window_closed":false,"step_done":false,"error":null,"warnings":[],
 "updated_realtime_ns":...,"updated_mono_ns":...,
 "channels":[{"id","name","kind","events","injected","skipped","ignored",
              "failed","unmapped","bytes","last_mono_ns"}]}
```

`state` is `running | paused | finished | failed | stopped`; the last frame is
the final summary. `play` stdout/`--json` also report
`state`/`mode`/`rate`/`status_file`.

`thyme-replay status --status-file <path> [--json]` prints a text summary or
echoes the JSON for `jq`/watchdog. Missing/unreadable/non-status files exit 1;
missing `--status-file` exits 2. One status path allows one `play` writer.

## 6. Export / republish

`thyme-replay export <pkg> --out <file> [--format mcap|ndjson]` is read-only,
streaming, and CLI-only: it does not enter the core or the replay hot path. Both
formats write events in `mono_ns` order; `--from/--to` use the same forms as
`play` and `SeekMonoNs`.

```bash
thyme-replay export <pkg> --out pkg.mcap
thyme-replay export <pkg> --out pkg.ndjson --format ndjson
thyme-replay export <pkg> --out pkg.mcap --schema-out schema.json \
    --from 250ms --to 2s --no-verify-crc
thyme-replay export <pkg> --out pkg.mcap --decode-profile
thyme-replay export <pkg> --out pkg.thysd --encode cbor
```

### 6.0 Self-describing encoding

`--encode=cbor|msgpack|tlv` writes each event with field names so the decoder
does not need `schema.json`. The 16-byte header contains magic `THYSD01`,
version 1, encoding (1=cbor, 2=msgpack, 3=tlv), reserved 0, and record count.
Default `raw` leaves MCAP/NDJSON bytes unchanged.

Small/meta fields are expanded only when the sidecar says `layout: known`;
otherwise export fails and suggests `--encode=raw` (no guessing). Large frames
have no POD field table, so they export as `payload` bytes plus `profile` (or
null). `optional` is encoded as null. Unknown tags/versions, CBOR tags, and
MessagePack float32 are rejected. Encoding happens in `thyme-replay`, not on the
recording/replay hot path, and does not replace `.tbin`. Size and CPU are in the
text summary's `encode` line. Python decoding:
`thyme_py.selfdesc.decode_document`.

### 6.0.1 Contract encoding

`--encode=protobuf|cdr` still writes MCAP or NDJSON but replaces data-message
bytes. Schemas come from the sidecar's known field table: MCAP protobuf uses a
`FileDescriptorSet` (encoding `protobuf`), CDR uses ros2msg text (encoding
`ros2msg`, message encoding `cdr`), and `<out>.proto` / `<out>.msg` is written
alongside. `layout_hash` goes into channel metadata as identity only. Unknown
layouts or schema versions outside 1..3 fail and suggest `--encode=raw`. Large
frames have no field table and are rejected on this path; gap/trigger/session
stay JSON. Default `raw` leaves MCAP/NDJSON unchanged and introduces no ROS 2 or
DDS dependency.

### 6.1 MCAP

Uses `McapWriter` (unchunked subset: Header/Schema/Channel/Message/Statistics/
Footer, no chunk/index/attachment):

| Event | MCAP topic | Message encoding | Notes |
|---|---|---|---|
| Each data (small/meta) | `thyme/<channel>/<struct>` | `thyme` | Raw record bytes |
| Each data (pinned_slot) | `thyme/<channel>/raw` | `thyme.frame` | `.raw` segment; `FrameIdx` flags/offset/crc are per-frame values not copied into the payload |
| kGap | `thyme/<channel>/gap` | `json` | `{event,channel,struct_id,reason,missed_count}` |
| kTrigger | `thyme/<channel>/trigger` | `json` | `{event,channel,trigger_id,pre_roll_ns,post_roll_ns}` |
| kSession | `thyme/<channel>/session` | `json` | `{event,channel,session_id,reason}` |
| Package metadata | `thyme/package` | `json` | First message, links schema/manifest |

Each data channel's metadata carries `thyme.channel`, `thyme.channel_kind`,
`thyme.struct_id`, `thyme.contract_version`, `thyme.payload_size`,
`thyme.schema_ref`. The `thyme/package` message (at the window's first event
timestamp) contains:

```json
{"event":"package","format":"thyme-replay-mcap","version":1,
 "schema":{"ref":"schema.json","state":"ok","version":1,"records":5},
 "manifest":{"declared":true,"hash":"0x...","version":1},
 "first_mono_ns":...,"last_mono_ns":...}
```

`--schema-out <path>` copies `<pkg>/schema.json` verbatim; `schema.ref` points
there. Legacy packages without a sidecar report `state=absent`, `ref=null`, and
`--schema-out` fails explicitly (exit 1).

Known limits: MCAP is unchunked, so Foxglove can open and browse it but large
files have no index (linear seek); `mono_ns` is monotonic, not Unix epoch, so
the axis shows relative time; `FrameIdx` fields are not exported with raw
payloads.

### 6.2 NDJSON

One JSON object per line, suitable for scripts/pipelines that do not link thyme:

```json
{"record":"header","format":"thyme-replay-ndjson","version":1,
 "package":"/data/pkg","schema":{"state":"ok","ref":"schema.json",...},
 "manifest":{...},"window":null,
 "channels":[{"id":1,"name":"small","kind":"small",
              "registry":[{"struct_id":0,"name":"Pod16",
                           "contract_version":1,"payload_size":16}]}]}
{"record":"event","t":1000,"channel":1,"channel_name":"small","kind":"data",
 "struct_id":0,"record_sequence":0,"source_sequence":0,"len":16,
 "payload_kind":"record","schema_ref":"schema.json#/records/0",
 "payload":"<base64>"}
{"record":"summary","ok":true,"events":206,"data":204,"gaps":1,
 "triggers":1,"sessions":0,"payload_bytes":63392,
 "crc_checked":80,"crc_bad":0,"crc_skipped":0,
 "first_mono_ns":1000000000,"last_mono_ns":1000159500}
```

`kind=gap|trigger|session` lines have no payload but their own scalar fields.
Large-frame lines use `payload_kind=raw_frame`, `schema_ref=null`, plus
`raw_offset`, `flags`, `payload_crc`. `schema_ref` points into the sidecar
(`schema.json#/records/<struct_id>` or the `--schema-out` path); without a
sidecar, `schema.state=absent` and `schema_ref=null`.

### 6.3 Memory and CRC

- MCAP uses `McapWriter`'s fixed buffer (64 KiB default,
  `--mcap-buffer-bytes` 256 B..16 MiB); larger messages are written directly to
  the fd without a full-frame copy/cache.
- NDJSON base64-encodes payloads through a 3–4 KiB stack buffer, line by line;
  neither format buffers the whole file.
- Export first verifies every frame with `payload_crc != 0` (recomputing the
  `.raw` segment); a mismatch fails immediately and deletes the partial output.
  `payload_crc == 0` (CRC off) counts as `skipped`, never as verified.
  `--no-verify-crc` skips only this recomputation; Open still validates `.tbin`
  record CRCs.

Exit codes: 0 success; 1 open/corruption/CRC/write failure or `--schema-out`
without a sidecar; 2 usage error (missing `--out`, unknown `--format`, buffer
out of range).

### 6.4 Foxglove

```bash
thyme-replay export <pkg> --out pkg.mcap
thyme-replay export <pkg> --out pkg.mcap --schema-out schema.json
```

In Foxglove:

1. **Open connection → Local file**, choose `pkg.mcap` (`profile=thyme`,
   `library=thyme-mcap`).
2. The **Topics** panel shows `thyme/<channel>/<struct>`,
   `thyme/<channel>/raw`, `thyme/<channel>/{gap,trigger,session}`, and
   `thyme/package`. The package message is first and contains `schema.state/ref`
   and manifest identity.
3. **Raw Messages** shows raw payloads for `thyme` / `thyme.frame` channels and
   JSON text for `json` channels. The axis is `mono_ns`, not Unix epoch.
4. **Plot** works only on JSON scalars or manually parsed fields; MCAP's
   `jsonschema` is an opaque-payload description, and Foxglove does not decode
   thyme struct fields. Use the Python reader or `thyme_probe schema` for field
   decoding.

Known limits (matching the unchunked writer): no chunk/message index/chunk
index (linear seek); no compression/attachment/metadata records; no per-frame
`FrameIdx` metadata; MCAP `sequence` is renumbered per channel; `mono_ns` is a
monotonic clock, not wall time; `kSession` has no in-file timestamp and is
stamped with the previous observed `mono_ns` (or header `start_mono_ns`);
`--schema-out` only copies the sidecar.

Structural verification: `test_mcap_verify` independently checks magic/record
boundaries, Data End/Footer positions, Data/Summary CRC, Statistics and
per-channel counts, Summary Offset groups, and the unchunked shape;
`test_replay_export` byte-compares payloads/topics/timestamps.

## 7. record → replay → record determinism boundary

`test_record_replay_record` uses a fixed `mono_ns` data grid and fixed re-record
trigger parameters (`t0=140 ms / before=100 ms / after=0`); capture does not wait
on wall clock or rewrite samples through a test-only producer path. With fixed
inputs, repeated re-recording must produce byte-identical `meta.tbin`,
`small.tbin`, per-frame `.index.tbin`/`.raw`, and `schema.json`. TRIGGER,
`TriggerMetaRecord`, and `CaptureStatsRecord` in `meta.tbin` are compared against
the new capture's window, config hash, and statistics, not against the source
package's envelope. WAL descriptor and seal rows are byte-compared except
`mono_ns`; clock-anchor rows are validated separately and excluded from
cross-round wall-clock equality.

GAP and TRIGGER are **explicitly non-reinjectable control rows**, not data
injection targets: one source GAP and one TRIGGER each count as `ignored`, and
the re-recorded package contains no source GAPs and only TRIGGERs from the new
capture request. The two meta data rows have explicit scratch-ring mappings and
count as `injected`; the new package's meta is still generated by the real
recorder.

Real capture packages are not promised byte-identical as a whole:

- the WAL seal row's `mono_ns` is the journal commit's `CLOCK_MONOTONIC`; clock
  anchor values and counts vary across runs; `integrity.manifest`'s
  `seal_time_ns` is `CLOCK_REALTIME` at seal. These envelope fields are covered
  by integrity checks, not byte-equality assertions.
- `Trigger(t0_mono_ns=0)` uses the current `CLOCK_MONOTONIC`, so package
  directory names, TRIGGER, meta, and manifest t0 vary; deterministic comparison
  must pass a fixed t0.
- By default, replay generates a new session id and rewrites the embedded
  `FrameHeader`; byte comparison of such payloads must fix
  `ReplayHeaderPolicy::session_id` and the mono/sequence policy.
- Real producer timestamps, capture scheduling, slot overwrite/tearing, and quota
  eviction change the sample set, GAPs, and `CaptureStatsRecord`
  holes/torn/evicted/flags. Byte identity is expected only when inputs, windows,
  mappings, and these capture results are all fixed; wall-clock pacing only
  guarantees event order.

## 8. Related tests

| Test | Coverage |
|---|---|
| `test_record_replay_record` (mr_gate) | Fixed mono-grid data/meta/trigger comparison, GAP/TRIGGER exemptions, byte-identical re-record files, WAL wall-clock envelope limits, integrity checks |
| `test_replay_producer_gate` (mr_gate) | Gated Begin, owner write, second acquire busy, Create rejection under live gate, release recovery, timeout reclaim, fork+SIGKILL reclaim, v2 header rejection/force recreation |
| `test_pinned_slot_injector` | Open acquires gate, second injector `kBusy`, producer resumes after Close |
| `test_replay_cli` | End-to-end replay (including `--force`/target rejection), status/runtime control, absolute/stitch multi-package, dedup/holes, cross-package seek, `--to`, JSON/text, busy/Stop release |
| `test_replay_seek_index` (mr_gate) | Per-channel seek metadata; indexed/unindexed suffix equality; scanned-record upper bound |
| `test_replay_playlist` (mr_gate) | Three-slice absolute/stitch, varied periods/metadata, embedded header mono, negative offsets, single-sample fallback, overflow failure, cross-package seek, one SHM gate, double-write rejection, incompatible-contract zero injection |
| `test_replay_export` (mr_gate) | MCAP counts/timestamps/payload bytes/topics/schema sidecar; NDJSON header/event/summary/base64/`schema_ref`; 256 B writer buffer with large frames; `--from/--to`; CLI usage/exit codes/partial deletion |
| `test_mcap_verify` (mr_gate) | Independent MCAP structure validation |
| `test_mcap_official_python` (mr_gate) | Official Python `mcap` parses real export and matches topics/schemas/timestamps/counts; skips with a reason when absent |
| `test_thyme_py` (mr_gate) | Python reader for v4/v1–v3 packages, manifest/footer index, CRC, schema v2/v1, facade, CLI |
| `test_record_schema` (mr_gate) | Sidecar write/strict parse, honest fallback, missing/tampered, schema view |
| `test_record_manifest` (mr_gate) | `.tbin`/perf manifest round-trip/tampering, legacy behavior, `--expect-manifest-hash` rejection, partial schema |
| `test_topology_map` (mr_gate) | layout_hash stability/change, key mapping/reordered ids, version/payload/layout mismatch, ambiguous, legacy fallback |
| `test_topology_replay` | Package A → topology B with reordered ids, added/deleted channels; `--check-compat`/`--allow-missing`, layout rejection, header rewrite, two-run determinism |
