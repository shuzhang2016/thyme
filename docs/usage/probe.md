# thyme_probe: read-only SHM observability

`thyme_probe` is a read-only observability tool for thyme SHM containers; the
binary is at `build/tools/inspect/thyme_probe/thyme_probe`. It answers questions
such as "which segments exist now, what is their geometry, what is the publish
rate, and are the versions compatible?" without attaching, subscribing or taking
over any cursor.

It is deliberately not called `monitor`: the system already has an
event-aggregation component with that name (chain health). `thyme_probe` is not a
monitoring system and provides no persistence; when metric export is needed, use
`thyme_metrics_exporter` from [Other tools](tools-other.md).

## session (session identity)

```bash
build/tools/inspect/thyme_probe/thyme_probe session /c9_1234 --json
```

The command opens `/c9_1234_id` read-only (`MpSessionIdentityPage` v1) and outputs
`config_hash`, `graph_hash`, `supervisor_pid`, `supervisor_start_ns`, `health_n`. A
missing page means a session created before session identity existed, exit code 1;
a page whose magic/version/layout does not match exits 3.

## 1. Read-only contract

Every `thyme_probe` operation maps the target page with `PROT_READ` and only loads
from it:

- it does not subscribe, acquire a PinnedSlot lease, or advance a shared consumer cursor;
- it does not call `Push` / `Write` / `Commit`, and does not `shm_unlink` any name;
- instead of `ShmSpscRing::TryPop`, which writes `tail`, it re-reads "the newest
  published slot" using the container's own seqlock/seq/footer validation.

Sampling may therefore miss frames, but it **never moves a producer or consumer
cursor**. It also means that `watch`/`hz`/`bw`/`delay` statistics are samples within
the observation window, not lossless counts.

## 2. Command overview

```text
thyme_probe <command> [options]
```

| Command | Purpose | Key options |
| --- | --- | --- |
| `list` | Enumerate thyme pages under `/dev/shm` | `--json` `--filter PAT` `--prefix P` `--manifest FILE` |
| `info <name>` | Single-page geometry, counters, struct registry summary | `--json` `--manifest FILE` |
| `watch <name>` | Sample the live value stream | `--count N` `--period MS` `--duration MS` `--json` |
| `hz <name>` | Publish frequency and jitter | `--window MS` `--period MS` `--json` |
| `bw <name>` | Payload byte rate | `--window MS` `--period MS` `--exact` `--json` |
| `delay <name>` | Age of the producer stamp at the observation point | `--window MS` `--period MS` `--json` |
| `echo <name>` | Decode and hexdump the newest payload | `--count N` `--max-bytes N` `--timeout MS` `--json` |
| `schema <pkg\|schema.json>` | Read the record schema sidecar | `--json` |
| `abi` | Pre-upgrade ABI preflight | `--json` `--matrix` `--mismatches` `--prefix P` `--manifest FILE` |

Two top-level aliases are equivalent to `abi`: `--check-abi` equals `abi`, and
`--abi-matrix` equals `abi --matrix`. `thyme_probe --help` or
`thyme_probe <command> --help` prints usage and exits 0.

### Time units and defaults

All time windows and sampling periods are in **milliseconds** (`--window` /
`--period` / `--duration` / `--timeout`). This build has no `--seconds` or
`--mib`: `hz`/`bw` output automatically selects `s/ms/us/ns` and `B/s`, `KiB/s`,
`MiB/s`, `GiB/s` by magnitude. Sampling periods are clamped internally to
1 ms..60 s.

| Scenario | Default |
| --- | --- |
| `list` scan prefix | `thyme_` (use `--prefix c9_` for MP supervisor session segments) |
| `watch` sampling period | 1000 ms |
| `watch` sample count | `0`, i.e. until Ctrl-C or `--duration` expires |
| `hz`/`bw`/`delay` window | 1000 ms |
| `hz`/`bw`/`delay` sampling period | 100 ms |
| `echo` sample count | 1 |
| `echo` maximum bytes | 4096 (internal ceiling 1 MiB per sample) |
| `echo` wait timeout | 2000 ms |

## 3. Enumeration and single-page inspection

### `list`

```bash
build/tools/inspect/thyme_probe/thyme_probe list
build/tools/inspect/thyme_probe/thyme_probe list --prefix c9_
build/tools/inspect/thyme_probe/thyme_probe list --prefix thyme_ --prefix c9_ --json
```

`--prefix` matches `/dev/shm` entry names (without the leading `/`) and is
repeatable; when omitted, only `thyme_` is scanned. The default segment name of the
MP supervisor is `/c9_<pid>`, so `--prefix c9_` (or several prefixes at once) must
be passed explicitly. `.lease` attachments are not listed separately; they belong to
their parent page.

`--filter PAT` applies a substring filter to page names; when nothing matches,
stderr reports `no page matches filter` and exits 2.

Example output (columns are aligned; the actual number of rows depends on the
machine):

```text
NAME                                   KIND                VER  SLOTS SLOT_BYTES   STRIDE           HEAD       DROP
/thyme_cloud_1                         pinned_slot             3     64       4096     4224             90          0
/thyme_recr_1234                       record_mirror_ring    4     64        512      576             17          0
/thyme_probe_2895602_big               pinned_slot             2     64        128      256              5          0  [incompatible]
```

`VER` is the in-page version word; `[incompatible]` means magic/version/geometry do
not match the current build. Pages whose geometry cannot describe itself print a
`note:` line at the end; if no thyme page exists under `/dev/shm`, exit 2.

### `info`

```bash
build/tools/inspect/thyme_probe/thyme_probe info /thyme_recr_1234
build/tools/inspect/thyme_probe/thyme_probe info /thyme_cloud_1 --json
```

`info` outputs `kind`, `abi_version`, `region_bytes`, `slots`, `slot_bytes`,
`stride`, `head`, optional `tail`/`epoch`, `drop_count`, the SPMC v3
`reader_drop_slots`, `pinned_skip`, `lease_steal`, `session_id`, `workers`,
`frame_shaped`, `geometry_source`, and a `structs` summary from the record
registry.

`geometry_source` is `page` or `page+manifest`. A v3 `LatestSlot` page carries its
own `THYLSLOT` magic, `ShmSuperHeader` and geometry block (payload bytes/cell
stride/slot count), and probe decodes directly from the in-page geometry; the
manifest is used only to supplement `frame_shaped`/struct information and can
verify that the declared slot_bytes match the page. For the two ring kinds, slot
stride still comes only from the manifest. A bare v2 LatestSlot body is not a
described page, so probe reports `bad_geometry` instead of reading at a guessed
offset.

`SharedHealthPage`, `MpControl` and `LatestSlot` (v3) all carry a 64 B
`ShmSuperHeader`: like a role, probe validates `abi_version`, `layout_hash`,
`page_bytes`, `feature_bits` and the reserved words before reading any field, and
reports an `expected/actual` diagnosis in the JSON `note` and the `[incompatible]`
line on mismatch. `MpControl` has `kind` `mp_control` (v2 has no magic, so old
pages are not misidentified). The health page's `drop_count` is an exact 64-bit
sum, and the JSON number is not truncated.

## 4. Rates, latency and payload

### `watch`

```bash
build/tools/inspect/thyme_probe/thyme_probe watch /thyme_cloud_1 --count 20 --period 100
build/tools/inspect/thyme_probe/thyme_probe watch /thyme_cloud_1 --duration 5000 --json
```

Text mode prints wall-clock time, `seq`, `size`, the `rate` relative to the previous
sample, and `age` when decodable. `--json` is NDJSON with **one object per line**:

```json
{"seq":41,"ts_ns":1234567890,"producer_ns":1234567000,"size":4096,"rate_hz":100.000}
```

A sample count of `--count 0` (default) means sample continuously; `--duration` is a
hard deadline.

### `hz` / `bw` / `delay`

```bash
build/tools/inspect/thyme_probe/thyme_probe hz /thyme_cloud_1 --window 2000
build/tools/inspect/thyme_probe/thyme_probe bw /thyme_cloud_1 --window 2000 --exact
build/tools/inspect/thyme_probe/thyme_probe delay /thyme_cloud_1 --window 5000
```

| Command | What it measures | Notes |
| --- | --- | --- |
| `hz` | avg/min/max Hz from the publish count in the window | plus jitter p50/p99/max |
| `bw` | Payload byte rate | defaults to "count × slot capacity"; `--exact` uses per-frame lengths for PinnedSlot only |
| `delay` | Age of `now_mono - producer_stamp` | this is **observation-point age**, not consumer latency |

`delay`'s `stamp_source` may be `frame_header`, `pinned_slot_slot` or `none`.
PinnedSlot slot headers carry their own timestamp; ordinary ring/latest payloads
need a `frame_header` declaration in the manifest entry to decode a timestamp,
otherwise exit 3 (bad geometry) with a message that the producer timestamp is
missing. FrameHeader timestamps use the same structural checks as `echo`, and
`mono_ns` must be non-zero; `valid == 0` does not prevent reading that metadata
timestamp, but does not prove the payload is complete or the CRC correct.

Example:

```text
channel:   /thyme_cloud_1
window:    2.000 s (published 200)
hz:        avg 100.00  min 98.50  max 101.00
jitter:    p50 1.200 ms  p99 4.800 ms  max 12.000 ms
```

`bw` text output notes where the bytes came from:

```text
bandwidth: avg 3.91 MiB/s  min 3.85 MiB/s  max 3.95 MiB/s  (exact per-frame lengths)
```

`--json` emits a single object: `hz` has `hz_avg`/`hz_min`/`hz_max`,
`jitter_p50_ns`/`jitter_p99_ns`/`jitter_max_ns`; `bw` has
`bytes_per_s`/`bytes_per_s_min`/`bytes_per_s_max`,
`jitter_p50_ns`/`jitter_p99_ns` and `exact_bytes`.

### `echo`

```bash
build/tools/inspect/thyme_probe/thyme_probe echo /thyme_cloud_1 --count 3 --max-bytes 256
build/tools/inspect/thyme_probe/thyme_probe echo /thyme_cloud_1 --json
```

`echo` reads the newest payload and, when it recognizes a `FrameHeader`, decodes
`magic/version/size/session_id/mono_ns/sequence/valid/crc32`. Recognition uses
`FrameHeaderStructurallyOk` and checks `size <= slot_bytes`: undefined reserved bits
and `valid > 1` are rejected; a structurally valid `valid == 0` frame can still show
its header but is not CRC-verified and does not mean the payload is consumable.
Probe does not hold the product's magic/version constants and can only validate
structure from the fields themselves, so it cannot identify a frame that is
structurally self-consistent but has the wrong product magic/version. It then prints
up to 16 lines of hexdump (bytes beyond `--max-bytes` are marked truncated). The
output includes the page's `drop_count`; an SPMC ring page aggregates drops from all
readers, and consumers also add their delta to `SharedHealthPage` to preserve
per-edge attribution. Running `echo --count N` on a health page shows `worker`,
`drop_count`, `memory_flags` and `memory_state` per worker; the last two JSON fields
are `null` for non-health pages. `memory_state` is
`ok/disabled/degraded/unsupported`, where `disabled` only means `THYME_NO_MLOCK` was
explicitly set, not that `mlockall` failed. It **does not include a schema**: use the
`schema` subcommand to inspect in-package field layouts.

## 5. ABI preflight

`abi` compares the pages actually present in `/dev/shm` with the ABI constants
compiled into the current binary. It includes stale segments left by old sessions,
so it is suitable as a gate before and after an upgrade, not for deciding whether
"the workload is running".

```bash
# 1) Constant matrix expected by this build (deterministic JSON, archivable/comparable)
build/tools/inspect/thyme_probe/thyme_probe abi --matrix > build-abi.json

# 2) Check pages under all relevant prefixes
build/tools/inspect/thyme_probe/thyme_probe abi --prefix thyme_ --prefix c9_ --json

# 3) Deployment scripts take only incompatible rows: <name> expected=<n> actual=<n>
build/tools/inspect/thyme_probe/thyme_probe abi --prefix thyme_ --prefix c9_ --mismatches
```

Typical text output:

```text
NAME                                   KIND              OBJECT                   ACTUAL EXPECTED RESULT
/thyme_cloud_1                         pinned_slot         PinnedSlotPageHeader         2        3 MISMATCH
abi: 0 compatible, 1 mismatch(es)
mixed versions cannot run together: stop producers, upgrade every
peer, then rebuild the SHM session (docs/guides/upgrade.md).
```

`--matrix` output has the top-level shape
`{"schema":1,"entries":[{"component","object","constant","version","surface","probe_kind","primary","in_page_version","note"}]}`.
`surface` is one of `shm_page`, `wire`, `file`, `layout`; `in_page_version=false`
means the object has no readable in-page version word (for example a `file` row such
as a manifest version), and such pages go into the report's `unverifiable` list
instead of being wrongly judged compatible. `LatestSlot<T>` is a `shm_page` row
since v3, and probe validates the version using the in-page `ShmSuperHeader`.

Decision rules:

- a missing in-page version word, mismatched geometry, or actual != expected → incompatible;
- `abi` exits 0 when all comparable pages are compatible, 2 when no page was found,
  3 when a version/geometry mismatch exists, and 1 on a usage error;
- `--matrix` does not access SHM and always exits 0.

See [Configuration and deployment](config-deploy.md) for the upgrade order and
rollback; for the full semantics of the ABI constant table see
`docs/reference/abi-compatibility.md` and `docs/guides/upgrade.md` in the
repository.

## 6. When to use `schema`

`thyme_probe schema` reads the record schema sidecar (`schema.json`) written when a
recording package is sealed. It **does not access SHM, write files, or need thyme's
generated headers**. Use it when you need to answer "what is the field offset/type
of a record in this package" or "what is the package's record manifest hash"
instead of guessing at the `.tbin` layout.

```bash
build/tools/inspect/thyme_probe/thyme_probe schema /data/captures/trigger_000012
build/tools/inspect/thyme_probe/thyme_probe schema /data/captures/trigger_000012/schema.json --json
```

Example output:

```text
file   : /data/captures/trigger_000012/schema.json
schema   : present (v2, sealed)
manifest : hash=0x9914c81f74037d59 version=1
frame    : 40 B, 10 fields
channels : 2
channel  : struct_id=0 channel=cam.pod16 record_key=cam.pod16 contract_version=1 payload_size=16 layout_hash=0x031e5529cb6c1a1c
record   : struct_id=0 name=Pod16 contract_version=1 payload_size=16 channel=cam.pod16 record_key=cam.pod16 layout_hash=0x031e5529cb6c1a1c layout=known (16 B, 3 fields)
  field  : a uint64 @0 (rel 0) size=8 count=1 stride=8
  field  : b uint32 @8 (rel 8) size=4 count=1 stride=4
  field  : c uint32 @12 (rel 12) size=4 count=1 stride=4
```

Field offsets are **absolute offsets within the record payload**
(`payload_offset + field.offset`); records with `layout=unavailable` can only be
sliced into raw bytes by `payload_size`.

Exit codes:

| Exit code | Meaning |
| --- | --- |
| `0` | The sidecar exists and is parsable |
| `1` | Usage error (wrong number of arguments) |
| `2` | The sidecar does not exist (a package created before the schema sidecar existed); it explicitly reports `absent` and is not treated as an empty schema |
| `3` | The sidecar exists but is unparsable/tampered with; reports the parse reason and does not partially trust it |

## 7. Exit codes

| Exit code | Meaning |
| --- | --- |
| `0` | Success |
| `1` | Usage error, or unparsable manifest |
| `2` | Page/channel not found; `list` also returns 2 when nothing matches |
| `3` | magic/version/geometry mismatch, or a required timestamp cannot be decoded |
| `4` | Insufficient permission (`EACCES`/`EPERM` from `shm_open`) |
| `5` | No new samples within the window (stale/no data) |

## 8. Safety rules and troubleshooting

- **Read-only**: `thyme_probe` never cleans up SHM. When stale segments are found,
  confirm liveness with the SHM cleanup procedure in `docs/guides/runbook.md` §2;
  do not manually `rm /dev/shm/thyme_*`.
- **Prefixes**: by default only `thyme_` is scanned; if an MP session is not
  visible, add `--prefix c9_` first instead of assuming the segment does not exist.
- **Stale segments**: `list` shows segments left by old sessions, and `abi` counts
  them too. Judge workload state together with `info`/`hz` exit codes (5 = no data
  in the window).
- **`delay` is not end-to-end latency**: it is the age of the producer timestamp at
  the moment probe reads it; the consumer's actual queueing/processing latency is an
  application-side metric.
- **`watch` misses frames**: it re-reads the newest slot, which suits trend
  observation but not counting audits.

The startup resource self-check (fd, memlock, disk, `/dev/shm`) is performed by
`trigger_center --check-resources`; see `docs/guides/runbook.md` §7 and
[Other tools](tools-other.md).

## Related documentation

- [Replay CLI and triggerctl](replay-cli.md)
- [Other tools](tools-other.md)
- [Runtime API](api-runtime.md)
- [Configuration and deployment](config-deploy.md)
