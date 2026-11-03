# Continuous recording

Continuous recording is a record-always, roll-by-slice mode: it does not wait
for a trigger. After `Arm`, whitelisted channels are written to disk
continuously, and each slice is an independent package that can be validated on
its own. It shares the same sender cache, descriptor queue, rate limiting, and
retention as triggered acquisition; the only difference is that the window is
unbounded and cut by the slicing policy.

## Concepts

- **Data sources**: the mirror ring (`RecordMirrorRingPage`, small structs)
  records everything by default; PinnedSlot / H264 big-frame channels do **not**
  stream to disk by default (the disk cannot sustain it) and must be explicitly
  whitelisted. Big frames go through a streaming drain and never enter the
  descriptor queue.
- **Starting point**: `Arm` starts from the current `head` and does not
  backfill history. History belongs to triggered acquisition's pre window.
- **Shared quota**: triggered and continuous pending bytes share
  `[recorder].pending_bytes_quota`; under quota pressure the **continuous queue
  is evicted first**, and trigger packages take priority.
- **Non-interference**: continuous collection and triggered capture are two
  descriptor buckets; `Arm` can be called in any phase and does not terminate an
  in-flight trigger package.
- **Control plane**: continuous mode is driven by the recorder's record-control
  socket (`triggerctl arm|disarm|status`); the trigger center is not involved.
  `auto_arm` merely makes the host send one Arm at startup.

## Configuration

Continuous keys live in `[continuous]`, output location and quota in
`[recorder]`, and channel parameters in `[channel.*]`. The full key table is in
`docs/guides/trigger-config.md`; only the parts continuous mode actually uses
are listed here.

```ini
[recorder]
output_dir = /opt/usr/triggered_data
disk_quota_mb = 20480
high_reserved_mb = 4096
pending_bytes_quota = 67108864
incomplete_policy = keep
incomplete_max_age_s = 0

[continuous]
auto_arm = true
channels = small_struct:*
slice_mb = 512
slice_s = 0
wait_idle = false
demo_idle = false
rate_check = warn

[channel./thyme_recr_can]
kind = mirror_ring
name = can_chassis
rate_hz = 50
avg_frame_bytes = 128
crc = frame
```

| Key | Default | Meaning |
| --- | --- | --- |
| `auto_arm` | `false` | Arm continuous recording automatically at startup; does not enable/disable manual `arm` |
| `channels` | `small_struct:*` | Whitelist/exclusion/decimation syntax, see below |
| `slice_mb` | `512` | Slice byte limit; with a quota it must satisfy `0 < slice_mb <= disk_quota_mb` |
| `slice_s` | `0` | Time-based rolling, in seconds (fractions allowed), `0` = roll by size only |
| `wait_idle` | `false` | Use the idle detector as the gate for continuous drain; requires a real `[chassis]` or `demo_idle = true` |
| `demo_idle` | `false` | Explicitly enable the demo idle predicate when there is no `[chassis]` |
| `rate_check` | `warn` | Bitrate accounting handling: `enforce` rejects, `warn` warns, `off` skips the check |

`slice_s` is `ContinuousConfig::slice_ns` (nanoseconds) in the library; the
config file uses seconds and allows fractions, and `slice_mb` likewise maps to
`slice_bytes`. `enable` is the old spelling of `auto_arm`; it is still accepted
but prints a deprecation warning.

### `channels` selector syntax

| Form | Meaning |
| --- | --- |
| `small_struct:*` | All mirror sources (default) |
| `small_struct:/thyme_recr_*` | Select mirror sources by shm-name glob |
| `pinned_slot:/cloud_*@dec=10` | Select big frames by glob, recording 1 frame in every 10 |
| `/cloud_w1@dec=10` | Bare channel/shm-name compatibility form, also supporting decimation |
| `!small_struct:/thyme_recr_noisy` | Exclusion, which takes precedence over inclusions |

`source` and `continuous` share this syntax. `dec=N` genuinely applies to both
small structs and big frames, keeping samples whose index satisfies
`index % N == 0`; an explicit include rule overrides the decimation of a `*`
include rule. h264 is a GOP stream and cannot be frame-decimated, so an
`h264_stream` selected by an `@dec>1` rule is rejected at config time. Every
positive selector in a source must match a configured `[channel.*]`; the final
selection of either mode must not be empty, otherwise `--check-config`, startup,
or Arm fails — a typo never silently becomes "record everything" or "record
nothing".

### Rate accounting and `rate_check`

`--check-config`, startup, and **every Arm** (automatic or manual) run
`ValidateContinuousPlan()` once:

```text
sustained_bps = Σ whitelisted channels:
    h264_stream:  bitrate_kbps × 1000 / 8
    pinned_slot:     rate_hz × avg_frame_bytes / dec
    small_struct: rate_hz × avg_frame_bytes
disk_bytes_per_hour = sustained_bps × 3600
quota_coverage_h = disk_quota_bytes / sustained_bps / 3600
```

If `io_rate_limit_mbps` is below `sustained_bps`: `enforce` rejects startup/Arm
outright, `warn` prints a warning and continues, and `off` does not check. The
accounting result is printed as
`trigger_center: continuous plan: sustained N B/s (M MB/h), quota coverage X h`.
Lag caused by rate limiting is counted as `ContinuousStats::pinned_slot_drops`
through the streaming drain's gap-skip and does not consume the pending quota.

## Slice layout and rolling

Each slice is an independent package with the directory name
`continuous_<seq>_<ts>/`:

```text
<output_dir>/continuous_0_1761205953123456789/
├── meta.tbin
├── small.tbin
├── frames/
│   ├── h264_front.raw
│   └── h264_front.index.tbin
├── wal.log
├── integrity.manifest
├── integrity.hmac          # only when [security].signing_key is configured
└── schema.json             # schema sidecar, when a record manifest is configured
```

- On close, `meta.tbin` writes a `ContinuousMetaRecord` (`slice_seq`,
  `begin/end_mono_ns`, `samples`, `frames`, `holes`, `evicted`, `detail`) and a
  `CaptureStatsRecord`. `detail` holds the mono ns of the IDR that a GOP-aligned
  roll was aligned to, and is 0 for slices cut on byte/time boundaries.
- The `wal.log` header's `trigger_id` uses `kContinuousWalMark | slice_seq`; the
  high-bit mark guarantees it can never pair with a real trigger id.
- `frames/<channel>.raw` is the raw frame segment and `.index.tbin` is the
  `(offset, len, flags, payload_crc)` sequence; the index also contains
  `FrameGap` records (`struct_id = 61`) marking holes.

There are three roll conditions, and they can be configured together:

1. **By size**: `cont_slice_bytes` accumulates `len + 32 B` overhead per
   descriptor/frame; reaching `slice_mb` sets roll pending.
2. **By time**: `slice_s` expiring sets roll pending.
3. **GOP alignment**: an h264 channel does not roll immediately; it waits for
   the next frame with `is_idr`, ensuring the new slice starts at a decodable
   IDR. If no IDR arrives within `gop_roll_timeout_ns` (default 5 s), it forces
   the roll and counts `roll_forced`.

Non-h264 rolls execute as soon as the descriptor queue drains, so half a slice
never spans two files. `Disarm` first streams out big frames, drains the queues,
and then seals the current slice.

## Ordering, watermark, and GAP

- **Monotonic on-disk order**: `small.tbin` rows are ascending by `mono_ns` and
  globally sorted across structs; the pending window is re-sorted before every
  drain, so each source file a replay sees is monotonic.
- **Watermark**: each slice maintains `cont_small_last_mono_`. If a collected
  sample's `mono_ns` is below the watermark (a late publish from another struct,
  i.e., an out-of-order tick), it does not write an out-of-order row; it degrades
  to a GAP and counts `out_of_order`.
- **Overwritten**: if collection finds `head - next` beyond the ring depth, the
  sample was overwritten by the producer before it was read — write a GAP with
  `reason = kGapReasonOverrun (1)` and use the mono of the last sample collected
  for that struct (0 before the first collection), keeping the hole inside the
  slice's time order. These holes count in `holes_written`.
- **Torn reads**: a seqlock tear or a slot currently being written is not
  treated as a permanent hole; the next Tick retries. Tear counts go to
  `torn_reads`.
- **Queue pressure**: when the shared `pending_bytes_quota` is full, the oldest
  continuous descriptor is evicted and counted in `queue_evicted`; nothing is
  silently lost.
- **Big-frame lag**: when the streaming drain cannot keep up, the difference in
  `PinnedSlotReader::drop_count` is counted in `pinned_slot_drops`; frames
  overwritten while blocked by the `wait_idle` gate take the same path.

GAP is the closest evidence of a "drop" on the replay side: both `small.tbin`
GAP rows and `frames/*.index.tbin` `FrameGap` carry offset/length semantics, and
the replay tool degrades by marker instead of guessing.

## Retention (`Enforce`)

Retention manages packages **per directory**, ignoring file suffixes:

```text
<out>/<trigger_id>_<t0>/             low-priority package
<out>/high/<trigger_id>_<t0>/        crash-level package (exclusive reserved area)
<out>/continuous_<seq>_<ts>/         continuous slice (reclaimed along with trigger packages)
```

Rules:

- When total bytes exceed `[recorder].disk_quota_mb`, delete oldest
  **low-priority** packages until below quota; `high/` is never touched by
  low-priority cleanup.
- When `high/` itself exceeds `high_reserved_mb`, delete the oldest crash
  package inside `high/`.
- `.incomplete` packages are **never** deleted by quota cleanup; only
  `incomplete_policy` handles them: `keep` (default; a partial package is
  incident evidence), `delete_after_age` (delete only after
  `incomplete_max_age_s`), `delete_immediately` (delete on the next `Enforce`).
- Active packages are passed explicitly by the caller as an exclude set;
  `Enforce` also rechecks seal state internally — any directory that is not
  sealed and not marked `.incomplete` is skipped.
- The whole walk uses `lstat` and does not follow symlinks (skipped entries
  count in `symlinks_skipped()`); recursion depth is capped at 8, and anything
  deeper is treated as corrupt and left alone.
- At startup `MarkIncomplete()` reads the package's WAL lifecycle; for a
  parseable non-terminal package it first appends and fsyncs
  `aborted(crash_recovery)`, then renames to `<name>.incomplete`, and
  best-effort writes a partial manifest (`kFlagPartial`). Thus an
  `armed/capturing/sealing` left by `kill -9` converges to a single terminal
  state after startup recovery; a corrupt WAL still follows the existing
  fail-closed partial-package policy.

The host runs `Enforce()` after every capture and on a 5-minute tick; when it
frees space it prints `trigger_center: retention freed N bytes`.
`disk_quota_mb = 0` means unlimited, leaving only the `.incomplete` policy and
the disk itself as constraints.

## Disk-full handling

Continuous recording is more conservative than triggered capture on ENOSPC: the
current slice is actively invalidated (abort row + `.incomplete` rename), **the
whole continuous mode stops**, and the `disk_full` latch is set.

```text
trigger_center: disk full during drain; running retention and re-arming
```

Handling order:

1. Use `df -h <output_dir>` and `du -sh <output_dir>/* | sort -h | tail` to
   confirm usage.
2. When the host sees `disk_full`, it automatically runs `Enforce()` once and
   then `ClearDiskFull()`; if there is nothing deletable within quota
   (`.incomplete`, the active slice, and `high/` are all protected), delete
   sealed low-priority packages by hand.
3. After space is freed, run `triggerctl arm` again; continuous mode does not
   recover on its own.
4. Use `triggerctl status --json` to see whether `disk_full` is cleared and
   `armed` is back to `true`.

Prevention: set both `disk_quota_mb` and `high_reserved_mb`, and use
`rate_check = enforce` so a config whose bitrate exceeds `io_rate_limit_mbps` is
rejected at startup and on every Arm.

## Validating slice packages

Continuous slices and trigger packages use the same tools:

```bash
# Seal state + evidence manifest (no payload decoding)
build/tools/apps/trigger_center/triggerctl verify /opt/usr/triggered_data/continuous_0_1761205953123456789 --json

# Structure + per-frame CRC + timeline monotonicity + index
build/tools/replay/thyme-replay/thyme-replay verify /opt/usr/triggered_data/continuous_0_1761205953123456789

# Evidence manifest: per-file sha256 + hash chain + optional HMAC
build/tools/replay/thyme-replay/thyme-replay integrity /opt/usr/triggered_data/continuous_0_1761205953123456789 \
    --key /etc/thyme/signing.key

# Field schema sidecar
build/tools/replay/thyme-replay/thyme-replay schema /opt/usr/triggered_data/continuous_0_1761205953123456789
```

`triggerctl verify` reports `kind` `thyme-recorder-verify`, with key fields
`sealed`, `manifest` (`ok/absent/invalid`), `intact`, `partial`,
`signed/signature_checked/signature_ok`, `chain_ok`, and
`files_ok/files_missing/files_mismatch/files_unlisted`. Only "sealed, complete,
manifest passed" yields `ok: true`; a `.incomplete` package or one without a
manifest exits 1.

`thyme-replay integrity` prints a conclusion line:
`OK` / `PARTIAL (not sealed; listed files intact)` / `FAILED`. `PARTIAL` is also
a non-zero exit — it is forensic incident residue, not a complete package.

## Complete example: configure → run → validate

The example below uses `demo_producer` to generate data on a development machine
and does not depend on a real vehicle.

```ini
# /tmp/thyme_demo.ini
[trigger_center]
socket = /tmp/thyme_trigger.sock
default_source = manual
crash_priority = 100

[recorder]
output_dir = /tmp/thyme_demo_out
socket = /tmp/thyme_recorder.sock
disk_quota_mb = 256
pending_bytes_quota = 16777216
incomplete_policy = keep

[channel./thyme_recr_can]
kind = mirror_ring
name = can_chassis
rate_hz = 100
avg_frame_bytes = 16

[channel./h264_front]
kind = h264_stream
name = h264_front
bitrate_kbps = 4000

[source.manual]
channels = /thyme_recr_can
before_s = 2
after_s = 1
quota_class = manual

[quota.manual]
max_triggers_per_hour = 10

[continuous]
auto_arm = true
channels = small_struct:*
slice_mb = 64
slice_s = 10
rate_check = warn
```

```bash
mkdir -p /tmp/thyme_demo_out

# 1) Config self-check: unknown keys, invalid selectors, and oversubscribed
#    slices all fail here
build/tools/apps/trigger_center/trigger_center --check-config \
    --record-manifest /etc/thyme/record_manifest.h /tmp/thyme_demo.ini
build/tools/apps/trigger_center/trigger_center --dump-config \
    --record-manifest /etc/thyme/record_manifest.h /tmp/thyme_demo.ini

# 2) Producer: RecordMirrorRingPage + ShmSpmcRing + big-frame channel, 100 Hz
build/tools/apps/trigger_center/demo_producer \
    /thyme_recr_can /thyme_demo_ring /h264_front 100 &
PROD=$!

# 3) Trigger center: auto_arm makes it Arm continuous recording at startup
build/tools/apps/trigger_center/trigger_center \
    --record-manifest /etc/thyme/record_manifest.h /tmp/thyme_demo.ini &
TC=$!
sleep 2

# 4) Confirm it is armed and watch slices roll
build/tools/apps/trigger_center/triggerctl status --json
sleep 25
ls -1 /tmp/thyme_demo_out
```

You should see directories such as `continuous_0_<ts>/`, `continuous_1_<ts>/`,
and so on (rolling at 10 s or 64 MiB, whichever comes first). Each slice should
contain `meta.tbin`, `small.tbin`, `wal.log`, and `integrity.manifest`.

```bash
# 5) Arm/Disarm manually once to confirm the control plane works
build/tools/apps/trigger_center/triggerctl disarm --json --timeout 1000
build/tools/apps/trigger_center/triggerctl status --json
# disarm drains and seals the current slice first, incrementing slices_done

# 6) Validate the most recent slice
PKG=$(ls -d /tmp/thyme_demo_out/continuous_* | tail -1)
build/tools/apps/trigger_center/triggerctl verify "$PKG" --json
build/tools/replay/thyme-replay/thyme-replay verify "$PKG"
build/tools/replay/thyme-replay/thyme-replay integrity "$PKG"

# 7) Check whether the in-slice timeline is monotonic and how many holes there are
build/tools/replay/thyme-replay/thyme-replay info "$PKG" --json
```

Cleanup: `kill $TC $PROD`, then handle leftover pages according to the
runbook's SHM reclamation rules; on a normal exit `demo_producer` releases the
pages it created.

## Related documentation

- [`triggered-acquisition.md`](triggered-acquisition.md) — trigger windows, preemption, and the trigger→seal SLO
- [`config-deploy.md`](config-deploy.md) — config sections, hot-reload matrix, and deployment gates
- [`replay-cli.md`](replay-cli.md) — `thyme-replay` verify/integrity/schema/play
- [`probe.md`](probe.md) — determine whether a producer is actually publishing (head/epoch/hz)
