# Replay API

This document covers the package reading, paced playback, injection, and
topology mapping APIs of `thyme::replay`. Replay is a **fake producer**: the
target channels are owned by the replay process, and `ReplayPlayer` guarantees
injection order and non-decreasing `mono_ns`. Real drivers (cloud / image /
h264) must be stopped during replay, or blocked by the producer gate.

All types live in `namespace thyme::replay`, with headers under
`include/thyme/replay/`. The CLI (`thyme-replay`) is a thin shell over these
APIs; see [replay CLI](replay-cli.md) for exports and command-line arguments.

## 1. `ReplayPackage`

`include/thyme/replay/replay_reader.h`

`ReplayPackage` opens one trigger package or continuous slice read-only and
merges `meta.tbin`, `small.tbin`, and each `frames/<channel>.index.tbin` into a
single unified timeline sorted by `mono_ns`.

### 1.1 Opening and traversal

| Member | Signature | Description |
|---|---|---|
| `Open` | `ReplayStatus Open(const std::string& dir)` | Discovers, maps, and **fully validates** the package: per-record CRC, registry, footer index, frame offsets, and `.raw` sizes |
| `Close` | `void Close()` | Releases all mappings |
| `is_open` | `bool is_open() const` | Whether it is open |
| `NextEvent` | `ReplayStatus NextEvent(ReplayEvent* out)` | Returns `kOk` / `kEnd`, or an I/O integrity error encountered while advancing |
| `SeekMonoNs` | `ReplayStatus SeekMonoNs(uint64_t mono_ns)` | Positions every source at the first record with `mono_ns >= target`, then re-primes the merge |
| `first_mono_ns` / `last_mono_ns` | `uint64_t` | First/last event time in the package; 0 for an empty package |
| `channels` / `FindChannel` | `const std::vector<ReplayChannel>&` / `const ReplayChannel*` | Channel metadata |
| `status` / `error` | `ReplayStatus` / `const std::string&` | Latest status / `"path: reason"` |
| `events_delivered` | `uint64_t` | Events delivered since `Open` (`Seek` does not reset it) |
| `seek_calls` / `seek_records_scanned` | `uint64_t` | Seek observation: call count / records scanned by the most recent probe |
| `wal_present` / `package_complete` | `bool` | WAL present / carries a seal row; packages without a seal are still readable, these flags are informational |
| `schema_state` / `schema` / `schema_error` | `schema::ReadState` / `const schema::PackageSchema&` / `const std::string&` | Sidecar state and contents |
| `manifest_declared` / `manifest_hash` / `manifest_version` | `bool` / `uint64_t` / `uint16_t` | Record manifest identity declared by the package |

Validation in `Open()` is one full record scan, so it costs one pass per
`.tbin`; the scan also builds a sparse seek block table for files without a
footer index. A failed validation leaves no mappings open, and `error()` points
at the failing file and offset.

`ReplayStatus`:

| Value | Meaning |
|---|---|
| `kOk` | Success |
| `kEnd` | Normal end of stream (not an error) |
| `kNotADirectory` | Path does not exist or is not a directory |
| `kMissingMeta` | `meta.tbin` is missing |
| `kMissingRaw` | An `.index.tbin` exists but its `.raw` is missing |
| `kOrphanRaw` | A `.raw` exists but its `.index.tbin` is missing |
| `kBadHeader` | magic/version/registry rejected |
| `kTruncated` | File ends in the middle of a record/header/index |
| `kCorrupt` | CRC or layout violation |
| `kTooManyChannels` | Source count exceeds `kReplayMaxChannels` (64) |
| `kInvalidArgument` | Bad argument (such as `rate <= 0` or a null pointer) |
| `kStopped` | Stopped by the host's `Stop()` |
| `kBusy` | Another live replay already holds the target's gate |
| `kManifestMismatch` | `schema.json` and/or a `.tbin` manifest block disagree; decoding refused |
| `kBadSchema` | `schema.json` exists but is unparsable/tampered with |

### 1.2 Channels and events

Source enumeration is deterministic and channel ids are dense:

- channel 0: `meta.tbin`
- channel 1: `small.tbin` (a package with no samples does not create this file)
- subsequent: `frames/*.index.tbin`, sorted bytewise by file name

`ReplayChannel`:

| Field | Meaning |
|---|---|
| `id` / `kind` / `name` | Channel number / `kMeta`, `kSmall`, `kPinnedSlot` / `"meta"`, `"small"`, or the frame stem |
| `tbin_path` / `raw_path` | `.tbin` path / `.raw` path for big frames |
| `registry` | `ThymeStructDesc` table from the `.tbin` header |
| `first_mono_ns` / `last_mono_ns` | Minimum/maximum timestamp of this source |
| `record_count` / `data_count` / `gap_count` | All records / data / GAP records |
| `has_footer_index` | Footer index exists and passed validation |
| `data_offset` / `records_end_offset` / `first_record_offset` / `last_record_offset` | Byte offsets of the record stream and first/last record |
| `seek_entries` / `seek_stride` | Seek block table actually in use |
| `record_crc_checked` / `index_crc_checked` | Records validated by Open / whether the footer index was validated as a whole |
| `frame_crc_declared` / `frame_crc_absent` | Count of frame index rows with `payload_crc != 0` / `== 0` |
| `max_data_len` / `min_data_len` | Maximum/minimum payload of this source (raw segment length for big frames) |
| `first_raw_offset` / `last_raw_end` | `.raw` range actually referenced by big frames |

`ReplayEvent`:

| Field | Description |
|---|---|
| `mono_ns` / `channel` / `kind` | Timeline time / channel number / `kData`, `kGap`, `kTrigger`, `kSession` |
| `struct_id` | Record contract id |
| `record_sequence` | Record sequence number within the file; the corresponding id for trigger/session entries, 0 for GAP |
| `len` / `payload` | Payload byte count / read-only mapped pointer |
| `source_sequence` | data: producer sequence; gap: missed count |
| `gap_reason` | GAP only (such as `triggered::kGapReasonOverrun`) |
| `frame_flags` / `payload_crc` / `raw_offset` | Big frames only: flags such as IDR / raw segment CRC / offset within `.raw` |
| `trigger_id` / `pre_roll_ns` / `post_roll_ns` | `kTrigger` only |
| `session_id` / `session_reason` | `kSession` only |

Merge order is `(mono_ns, channel id, record_sequence, source ordinal)`; file
order is preserved within a source, so the same package always produces the
same event sequence. Records within a source are expected to be non-decreasing
in `mono_ns`; the merge never re-sorts a single source.

Payload lifetime: `event.payload` points into the read-only mapping of the
source file or `.raw` and remains valid until `Close()`/destruction (hence at
least until the next `NextEvent()`), and is never copied. Big frames are
zero-copy: `.raw` is mapped once in `Open()`, and the index row's `offset`/`len`
select the segment directly.

### 1.3 Schema sidecar and record manifest validation

`Open()` folds the sidecar and manifest identity into validation:

- `schema.json` unparsable/tampered with → `kBadSchema`;
- the hash/version of `schema.json` and of each `.tbin` manifest block must
  agree, and all `.tbin` files that declare a manifest must agree with each
  other, otherwise `kManifestMismatch`;
- legacy packages (no sidecar, no manifest block) open normally, with
  `schema_state()` reporting `kAbsent` and `manifest_declared()` `false`.

`RecordStructId` is a manifest row number, so different manifests mean different
channel id assignments; therefore never "decode as best it can" when manifests
disagree.

```cpp
#include "thyme/replay/replay_reader.h"

thyme::replay::ReplayPackage pkg;
const auto st = pkg.Open("/data/thyme_captures/7_1761205953123456789");
if (st != thyme::replay::ReplayStatus::kOk) {
  // pkg.error() gives "path: reason"
  return -1;
}

thyme::replay::ReplayEvent ev;
while (pkg.NextEvent(&ev) == thyme::replay::ReplayStatus::kOk) {
  if (ev.kind == thyme::replay::ReplayKind::kData) {
    // ev.payload[0..ev.len)
  }
}

pkg.SeekMonoNs(pkg.first_mono_ns() + 1000000000ull);  // +1 s
```

## 2. `ReplaySession`

`include/thyme/replay/replay_session.h`

`ReplaySession` adds wall-clock pacing on top of the unified timeline: after
Open/Seek the first event is delivered immediately, and each later event is
delayed by `(mono_ns - previous_mono_ns) / rate`.

| Member | Signature | Description |
|---|---|---|
| `Open` | `ReplayStatus Open(const std::string& dir)` | Opens the package and resets: rate 1.0, paced, not paused, stop cleared |
| `Close` | `void Close()` | Closes |
| `NextEvent` | `ReplayStatus NextEvent(ReplayEvent* out)` | Waits until the next event is due, then returns it; blocks while paused; `kEnd` at the end, `kStopped` after `Stop()` |
| `Step` | `ReplayStatus Step(ReplayEvent* out)` | Delivers exactly one event immediately without pacing, and stays paused |
| `Seek` | `ReplayStatus Seek(uint64_t mono_ns)` | Repositions; the next event is delivered immediately, later ones resume pacing |
| `SetRate` / `rate` | `ReplayStatus SetRate(double)` / `double rate() const` | `rate > 0` and finite; invalid values give `kInvalidArgument`. 2.0 = half the wall-clock time |
| `Pause` / `Resume` / `paused` | `void` / `void` / `bool` | No events are consumed while paused |
| `AsFast` / `as_fast` | `void AsFast(bool)` / `bool` | true = ignore `mono_ns` deltas and read as fast as possible |
| `Stop` / `stopped` | `void Stop()` / `bool` | Callable from any thread; interrupts a paused/sleeping `NextEvent()` |
| `status` / `state` / `delivered_events` | read-only | Current state |
| `package` | `const ReplayPackage&` | Access channel metadata and payloads |

`ReplaySessionState`: `kClosed` / `kPlaying` / `kPaused` / `kEnded` / `kStopped`.

Thread contract: `Open` / `NextEvent` / `Step` / `Seek` belong to the same
playback thread; `Pause` / `Resume` / `SetRate` / `AsFast` / `Stop` may be
called from any thread. `Stop()` is the only exit from a paused or sleeping
`NextEvent()` (other than `Resume()`), and every control call notifies waiters
so nothing blocks forever.

```cpp
#include "thyme/replay/replay_session.h"

thyme::replay::ReplaySession session;
if (session.Open(pkg_dir) != thyme::replay::ReplayStatus::kOk) return -1;
session.SetRate(2.0);          // 2x
session.AsFast(false);

thyme::replay::ReplayEvent ev;
while (session.NextEvent(&ev) == thyme::replay::ReplayStatus::kOk) {
  // consume
}
```

## 3. Injectors and `ReplayPlayer`

### 3.1 Explicit target tables

`include/thyme/replay/pinned_slot_injector.h`, `small_struct_injector.h`

Mappings are always explicit; nothing is guessed by name:

```cpp
struct PinnedSlotTarget {
  uint32_t channel = 0;          // channel id inside the package
  PinnedSlotWriter* writer = nullptr;
};

struct SmallStructTarget {
  uint32_t channel = 0;          // small/meta channel id inside the package
  uint16_t struct_id = 0;        // struct_id inside the package registry
  RecordMirrorRingPage* ring = nullptr;
  uint16_t target_struct_id = 0; // struct slot inside the target page
};
```

| Injector | Method | Description |
|---|---|---|
| `PinnedSlotInjector` | `Open(const ReplayPackage&, const PinnedSlotTarget*, uint32_t)` | Target channels must exist, be non-empty, be open, and not repeat; failure gives `kInvalidArgument` |
| | `SetGateEnabled(bool)` | Must be called before `Open()`; default true |
| | `Heartbeat()` | Renews the gate lease; hosts with long pauses must call it periodically |
| | `SetVerifyCrc(bool)` | Default true: validate `FrameIndexRecord::payload_crc` before injecting |
| | `SetHeaderPolicy(const ReplayHeaderPolicy&)` | See 3.3 |
| | `Inject(const ReplayEvent&)` | Returns `InjectStatus` |
| | `gate_rejects()` / `gate_steals()` / `gate_aborted()` | Double-write observations read from the target channel header |
| `SmallStructInjector` | `Open(const ReplayPackage&, const SmallStructTarget*, uint32_t)` | `target_struct_id` must exist in the ring page and `struct_id` in the package registry; duplicate `(channel, struct_id)` is rejected; a target struct with a live gate returns `kBusy` |
| | `SetGateEnabled(bool)` | Must be called before `Open()`; default true, publish sinks acquire no gate |
| | `Heartbeat()` | Renews the lease of every acquired `(ring, target_struct_id)`; hosts with long pauses must call it periodically |
| | `gate_rejects()` / `gate_steals()` | Aggregates double-write observations per unique target struct, without double-counting shared mappings |
| | `HasTarget(uint32_t channel, uint32_t struct_id)` | Distinguishes "control row, ignored" from "data row, unmapped" |
| | `SetHeaderPolicy` / `Inject` | Same as big frames; losing the gate on a target struct returns `kGated` and counts in `gated` |

`InjectStatus`:

| Value | Meaning |
|---|---|
| `kOk` | Injected |
| `kIgnored` | No injectable payload for this target (GAP/trigger/session marker); not counted as data loss |
| `kSkippedPinned` | The target slot is pinned by a reader and the writer skips it (head advances, nothing is published); expected with concurrent readers |
| `kOversize` | Payload exceeds the target slot capacity; nothing is written or truncated |
| `kUnmapped` | No target mapped for `(channel[, struct_id])` |
| `kBadEvent` | The event itself is unusable: empty data payload or `len == 0` |
| `kCrcMismatch` | The package's `payload_crc` does not match the `.raw` bytes; not injected when validation is enabled |
| `kWriteFailed` | The target refused the write for a non-geometry reason (such as a torn race in a small-struct ring) |
| `kGated` | The target gate refused the write. For an injector this means lost gate ownership; for a real producer it is the expected replay block |
| `kNotOpen` | The injector is not open |

Big-frame injection order: `Begin(ev.len)` → `memcpy` → embedded-header rewrite
(optional) → `header->flags = ev.frame_flags` → `Commit(ev.mono_ns)`. `Commit`
recomputes the slot CRC when the channel has CRC enabled; frames with
`payload_crc == 0` are not verified (recorder-side CRC off). Small structs are
injected through `TryWriteRing(target_struct_id, payload, ev.len, ev.mono_ns,
sequence, gate_token)`; the ring performs the thread-safe atomic copy and
seqlock publication itself.

`PinnedSlotInjector` is the sole writer of its target channels;
`SmallStructInjector` is the writer of each unique `(ring, target_struct_id)`.
When a second injector (same or another process) opens the same big-frame
channel or small-struct target struct, `Open()` returns
`ReplayStatus::kBusy`; different structs in the same ring use independent gates
and can coexist across injectors. When several sources map to one target
struct, the gate is acquired once. `SetGateEnabled(false)` is an explicit escape
hatch (disables double-write detection, for tests or legacy producers).

### 3.2 `ReplayPlayer`

`include/thyme/replay/replay_player.h`

```cpp
struct ReplayPlayerConfig {
  std::vector<PinnedSlotTarget> pinned_slots;
  std::vector<SmallStructTarget> small_structs;
  bool verify_crc = true;
  ReplayHeaderPolicy header_policy;  // session_id == 0: one shared id generated at Open
};
```

| Member | Signature | Description |
|---|---|---|
| `Open` | `ReplayStatus Open(const std::string& dir, const ReplayPlayerConfig& cfg)` | Opens the package and resolves every mapping; a missing channel/struct or unopened target makes `Open` fail and closes the session. Unmapped channels in the package are **not** an error |
| `SetGateEnabled` | `void SetGateEnabled(bool)` | Call before `Open`, sets both big-frame and small-struct; default true |
| `Heartbeat` | `void Heartbeat()` | Renews both injector kinds; during long pauses/step waits the host must call it periodically, and `Pause()` also renews once immediately |
| `Pump` | `ReplayStatus Pump()` | Takes one event through the session and injects it synchronously; `kOk` / `kEnd` / `kStopped` / error |
| `Run` | `ReplayStatus Run()` | Loops `Pump()` until the timeline ends; `kEnd` is normalized to `kOk` |
| `Step` | `ReplayStatus Step()` | Delivers and injects exactly one event, leaving the session paused |
| `Seek` / `SetRate` / `AsFast` / `Pause` / `Resume` / `Stop` | Forwarded to the session | Control plane |
| `stats` / `channels` | `const PlayerStats&` / `const std::vector<PlayerChannelStats>&` | Global/per-channel counters |
| `pinned_slot_injector` / `small_struct_injector` / `session` | read-only access | Diagnostics |
| `replay_session_id` / `header_policy` | read-only | Session id resolved at Open / effective policy |
| `error` | `const std::string&` | Latest error |

`PlayerStats`: `events`, `injected`, `skipped`, `ignored`, `failed`,
`mono_regressions` (must stay 0), `first_event_ns`, `last_event_ns`,
`last_skew_ns` (lag against the paced clock), `max_skew_ns`.
`PlayerChannelStats`: `channel`, `kind`, `events`, `injected`, `skipped`,
`ignored`, `bytes`, `last_event_ns`.

Synchrony is guaranteed by construction: the session already paces a single
merge stream, and the player injects the current event synchronously before
taking the next one, so injection order is non-decreasing in `mono_ns` across
channels; events sharing a `mono_ns` are injected consecutively. The player
starts no threads; `Run()` is just a convenience loop.

```cpp
#include "thyme/replay/replay_player.h"

thyme::replay::PinnedSlotTarget bf;
bf.channel = 2;
bf.writer = &h264_writer;

thyme::replay::SmallStructTarget ss;
ss.channel = 1;
ss.struct_id = 3;
ss.ring = &mirror_page;
ss.target_struct_id = 3;

thyme::replay::ReplayPlayerConfig cfg;
cfg.pinned_slots.push_back(bf);
cfg.small_structs.push_back(ss);

thyme::replay::ReplayPlayer player;
if (player.Open(pkg_dir, cfg) != thyme::replay::ReplayStatus::kOk) {
  // player.error() names the unresolved mapping
  return -1;
}
const auto rc = player.Run();
// player.stats().mono_regressions == 0
// non-zero player.pinned_slot_injector().gate_rejects() => a real producer tried to write during replay
```

### 3.3 Header rewrite policy

`include/thyme/replay/header_policy.h`

```cpp
struct ReplayHeaderPolicy {
  bool rewrite_embedded_frame = true;
  bool rewrite_mono_ns = true;
  bool rewrite_session = true;
  bool renumber_sequence = true;
  uint64_t session_id = 0;  // 0 = generated at Open
};
```

A replayed frame is a **new publication**, not a copy of the recorded slot
image:

- `mono_ns` is rewritten to the replay scheduling time (the event's position on
  the merge timeline);
- `session_id` is rewritten to this replay session id
  (`GenerateReplaySessionId()`, mixing realtime clock and pid); the target
  page/channel's own session identity belongs to the target and is not touched
  by the injector;
- `sequence` is renumbered from 0 per target channel by default;
  `renumber_sequence = false` keeps the recorded values, for byte-level
  record → replay → record comparison; the CLI equivalent is
  `thyme-replay play --preserve-sequences`;
- rewriting invalidates the CRC: ordinary frames recompute
  `Crc32HeaderPayload`, and CRC-disabled frames keep their flag and get
  `crc32 = 0`.

A payload is treated as an embedded frame only when it starts with a
**self-consistent `FrameHeader`**: `size` equals the payload length,
`valid == 1`, `magic != 0`, and the CRC contract holds (a full CRC for ordinary
frames, `crc32 == 0` for CRC-disabled frames). Every other payload is injected
byte for byte. A payload larger than 64 KB cannot be a `Frame` wire image
(`uint16 size`) and is rejected before any CRC computation. The pre-injection
`payload_crc` check applies to the **recorded bytes** and happens before any
rewriting.

### 3.4 Gate ownership

By default `PinnedSlotInjector` acquires the in-header producer gate of every
big-frame target during `Open()`; `SmallStructInjector` acquires the gate of
each unique `(ring, target_struct_id)`, while a publish sink has no ring and
therefore no gate. Both heartbeat on every `Inject()`, a failed `Open` rolls
back the gates acquired so far, and `Close()` releases only the tokens it still
holds.

Under a live gate, `PinnedSlotWriter::Create` returns `CreateStatus::kGated`
(even with `force_replace`); a real producer's `Begin()` gets `kGated`, and if
it is preempted before `Commit()` the `gate_aborted` counter increases. The
small-struct token0 `TryWriteRing` returns false under a live gate and
increments `rejects`. A crashed replay is reclaimed by the next writer based on
pid/timeout.

A paused replay must keep heartbeating: during long pauses the `ReplayPlayer`
host should call `player.Heartbeat()` periodically, covering both injector
kinds; the CLI uses a dedicated heartbeat thread to cover pause/step waits.

## 4. Topology mapping

`include/thyme/replay/topology_map.h`

`RecordStructId` is a manifest row number: adding, removing, or reordering
recorded channels changes the contract a given number refers to. Each
`ThymeStructDesc` carries a stable identity:

- `channel`: recorded channel name
- `record_key`: cross-build canonical key (currently equal to `channel` in
  generated code)
- `layout_hash`: FNV-1a of the field model; 0 = unknown
- `contract_version` / `payload_size`

`ResolveTopologyMapping(const ThymeStructDesc& source, const ThymeStructDesc*
target, std::size_t target_count, TopologyMapping* out)` is a pure function with
no I/O and no SHM access. `TopologyMapping` outputs `source_struct_id`,
`target_struct_id`, `channel`, `record_key`, `contract_version`,
`payload_size`, `layout_hash`, `status`, and `detail`.

| `TopologyMapStatus` | Behavior |
|---|---|
| `kMapped` | Unique key hit, contract tuple matches |
| `kMappedUnverified` | Key hit but one side has `layout_hash == 0` (no `contract_layout.h`): allowed, and the report marks it unverified |
| `kMappedLegacy` | Legacy source (no channel/key, v1..v3 packages): mapped by `struct_id` |
| `kMissing` | Target has no matching key/id; the caller decides whether to skip (`--allow-missing`) or fail |
| `kAmbiguous` | Target has multiple descriptors with the same key; fails |
| `kVersionMismatch` | Key hit but `contract_version` differs |
| `kPayloadMismatch` | Key hit but `payload_size` differs |
| `kLayoutMismatch` | Key hit and both `layout_hash` values are nonzero but differ |

`TopologyMapStatusMapped(status)` returns true for the first three. The rule is
"never silently remap": a key hit with an incompatible tuple is a hard failure,
and only `kMissing` can be skipped and counted under an explicit
`--allow-missing`.

### 4.1 When to use `--check-compat`

Run `--check-compat` (or the equivalent `--dry-run`) before a live play in
these situations:

- the package comes from an older build/topology while the target system has
  added, removed, or reordered recorded channels;
- the package has no stable keys (legacy) and can only be mapped by `struct_id`;
- the target ring's `layout_hash` is unknown and you need to confirm whether it
  is `mapped-unverified` or `layout-mismatch`.

`--check-compat` only resolves and prints the mapping table
(`CHANNEL SRC -> TGT KEY STATUS DETAIL`) and creates/injects nothing;
`--dry-run` has the same semantics. `--json` emits `mappings[]` and
`mapping_counts`. The automatic form is `--small <channel>=<ring>` (without an
explicit struct id), resolved against the target ring's descriptor table by
`(record_key/channel, contract_version, payload_size, layout_hash)`; the
explicit form `--small <channel>:<struct_id>=<ring>:<target-id>` keeps the old
semantics.

CLI exit codes: `0` success, `1` package/injection/validation failure, `2`
argument error, `3` target configuration or `--strict-targets` failure.
`--check-compat` exits `1` when it finds incompatible mappings; target
configuration errors exit `3`.

### 4.2 Export in the CLI

The core library `thyme::replay` contains no exporter. `thyme-replay export
<pkg> --out <file> [--format mcap|ndjson]` is a read-only streaming package
export (MCAP for Foxglove, NDJSON as base64 republish), supporting the same
`--from` / `--to` time windows and `--schema-out` as `play`; a manifest mismatch
writes no output at all. See [replay CLI](replay-cli.md) for the full argument
set, exit codes, and format contract.

## Related documentation

- [Runtime primitives API](api-runtime.md)
- [Recording and trigger API](api-record-trigger.md)
- [replay CLI](replay-cli.md)
- [Triggered acquisition](triggered-acquisition.md)
