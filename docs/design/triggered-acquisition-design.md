# Triggered acquisition design (on the thyme middleware)

> Status: design review draft
> Scope: trigger-driven capture for camera, LiDAR, and every struct on the chain; on-disk format, trigger center, quota and IO control
> Target platform: a downstream vehicle platform (ARM Linux, BusyBox init, no systemd)
> Naming note: the original design draft called this component `PinnedSlotStore`; no such class exists in the shipped code. The API is `PinnedSlotWriter` / `PinnedSlotReader` in `include/thyme/runtime/pinned_slot_store.h`; chapter 12 records what was actually built.

## 1. Background and current inventory

### 1.1 Recording infrastructure in thyme

thyme already has every low-level building block recording needs, but they are not yet composed into a triggered-acquisition system:

| Building block | Location | Current state |
|---|---|---|
| RecordTap (publish-point recording tap) | `tools/gen/gen_graph/emit_record.cpp`, `emit_integration.cpp` | gen_graph emits RecordTap helper functions, but **hand-written nodes are expected to call them explicitly after `publish()`** — today's recording is visible to the node and depends on nobody forgetting the call; large-channel nodes have no tap at all |
| record_mirror (process-level mirror page) | `include/thyme/runtime/record_mirror.h`, `include/thyme/app/record_mirror.h` | one `/thyme_rec_<pid>` shm page per process (2 MB arena, 64 slots, seqlock), **keeps only the latest value, no time-window history**; payload limit 512 KB (`kAbiMaxPayloadBytes`) |
| Time-window ring | `include/thyme/runtime/record_mirror_ring.h` | `RecordMirrorRingPage`: a per-struct indexed sample ring whose depth follows the configured window; `HarvestWindow(t_begin, t_end)` returns the descriptors inside `[t0-before, t0]` |
| Drain to disk | `include/thyme/record/recorder_drain.h`, `src/record/triggered_recorder.cpp` | `RecorderDrain` tick thread; the capture engine (`TriggeredRecorder`) writes samples to disk with WAL, backpressure, and hole markers |
| record_control (control plane) | `include/thyme/record/record_control.h` | Unix datagram socket `/tmp/thyme_recorder.sock`, `Trigger/Arm/Disarm/Status` commands, `Armed/Post/Settle` phase machine, uid authentication |
| thyme_bin (container format) | `include/thyme/record/thyme_bin.h`, `thyme_bin_stream.h` | custom binary container, 512 KB payload limit |
| recorder / recorderctl tool skeleton | `tools/gen/gen_graph/scaffold/tools/recorder/`, `recorderctl/` | scaffold, generated per graph |
| Large channels (cloud/img) | contract headers of a downstream vehicle platform | dedicated shm SPMC large-slot rings, two copies of the same pattern: `/cloud_<lidar>` 4 × 19.2 MB, `/img_<camera>` 4 × ~3.1 MB; **slot count hard-coded to 4, covering only 0.4 s / 0.13 s** |

### 1.2 Key mechanism facts (the footing of this design)

1. **RecordTap is currently a manual convention and does not satisfy "node-transparent"**. This design pushes the tap down into thyme transport primitives (`ShmSpmcRing::Push`, `SpscRing`/`LatestSlot`, each channel writer's `Commit`, see 4.1): cross-process channels and **in-process channels** (`MpRoleChannels::Open` creates no shm when `from_process == to_process`, `src/app/mp_runtime.cpp`, and uses the in-process `spsc_ring`/`latest_slot`) are both tapped inside the primitive — "record in-process traffic too" is therefore satisfied structurally, with zero changes to node source.
2. **Large payloads do not go through record_mirror**. cloud (≤19.2 MB) / img (~3.1 MB) exceed the mirror's 512 KB slot limit and fall into `oversize_drop_cnt`. A large channel's dedicated shm ring is already the sender-side cache; it only needs to be deepened and brought into the swap protocol.
3. **Server-side time base**: `RecordMirrorRingPage` stores `mono_ns` (CLOCK_MONOTONIC), and `HarvestWindow` cuts the pre-window against the monotonic trigger instant — avoiding cross-process clock skew.
4. **Vehicle state is available**: `/can_chassis` (chassis report, 104 B) carries `park_brk_status` (1 = Engaged), `vehicle_speed`, `drv_mode_status`, readable from the vehicle CAN process's mirror page — the data source for the "idle" decision (this vehicle has no P gear; parking relies on EPB).
5. **No precedent for IO priority**: the project uses no ionice/cgroup anywhere; the executive module only uses nice/CPU pinning/RLIMIT_AS, and its comments state that cgroup availability on the target platform is unverified.

### 1.3 Where the legacy ROS 2 package fits

The legacy ROS 2 implementation (dynamic subscription → SerializedMessage time-window ring → rosbag2) belongs to the ROS 2 era. The project has switched to the thyme middleware, and this design **is rebuilt entirely on thyme**; the legacy package remains only as a stopgap for topics still on ROS 2 and is out of scope for this phase (see open question 4 in chapter 11).

### 1.4 Gaps against the requirements

| # | Requirement | Current state | Gap |
|---|---|---|---|
| 1 | Trigger API + unified trigger center | recorderctl can only Trigger manually; no notion of a trigger source and no config lookup | Trigger center process, trigger API, per-source configuration |
| 2 | Zero-copy swap of the sender-side cache | mirror keeps only the latest value; cloud/img rings have only 4 slots; recorder drain is "read out and copy" | Time-window rings, configurable slot count, swap descriptor protocol |
| 3 | Configurable capture duration (10 s before, 5 s after) | the mirror ring supports a pre-window, but only one global value | Per-source/per-channel before/after |
| 4 | Persist while idle / persist with a real-time rate limit | None | Idle detection, ioprio/rate limit, cgroup verification |
| 5 | Mute interval | None | Trigger center state machine |
| 6 | Trigger-source priority, preemption by higher priority | None | Priority model and preemption semantics |
| 7 | Count/memory/disk quotas, overridable by high priority | None | Three-level quota and disk reservation |

## 2. Requirement mapping

| Requirement | Design chapter |
|---|---|
| 1. Trigger API + unified trigger center | Chapter 5 |
| 2. Distributed sender-side cache + zero-copy swap | Chapter 4 |
| 3. Configurable capture duration | 4.5 (capacity), 5.3 (configuration), 6.1 (window close-out) |
| 4. Idle / rate-limited write to disk | 6.2, 6.3 |
| 5. Mute interval | 5.4 |
| 6. Priority and preemption | 5.4, 6.4 |
| 7. Count/memory/disk quotas | 4.5 (memory budget), 6.4 |

## 3. Overall architecture

### 3.1 Components and data flow

```
┌─────────────┐                          ┌────────────────────────────┐
│ business    │  trigger(source, ts?)    │  trigger_center            │
│ modules     │ ───────────────────────▶ │  · per-source config lookup│
│ (header-only│   Unix datagram          │  · mute/priority/preemption│
│  client lib)│                          │  · quota accounting        │
└─────────────┘                          └────────────────────────────┘
                                                        │ parameterized Trigger
                                                        ▼ (record_control socket)
┌──────────────────────────────────────────────────────────────────────┐
│ recorder: attaches each process's read-only mirror page and          │
│           cloud/img channels                                         │
│  Armed ──Trigger──▶ Post(after s) ──▶ Settle(pending write)          │
│         zero-copy swap descriptors ◀── producer shm                  │
│         idle or rate-limited drain ──▶ /opt/usr/triggered_data/*.tbin│
└──────────────────────────────────────────────────────────────────────┘
        ▲ AttachReadOnly                        ▲ AttachReadOnly
        │                                      │
┌─────────────────┐                  ┌──────────────────┐
│ process A mirror│  …              │ /cloud_* ring /  │
│ page (small     │                  │ /h264_* ring     │
│ structs, window │                  │ (large-frame     │
│ ring slots)     │                  │  sender cache)   │
└─────────────────┘                  └──────────────────┘
```

### 3.2 Component responsibilities

| Component | Process | Responsibility |
|---|---|---|
| trigger client (header-only) | embedded in each business module | `Trigger(source_name, timestamp?)` sends a datagram; never blocks the business thread (fire-and-forget, result queryable asynchronously) |
| trigger_center | separate lightweight process | the single trigger entry point: authenticate → look up source config → mute/priority decision → quota accounting → issue a parameterized Trigger to the recorder |
| recorder | separate process (the existing recorder skeleton, extended) | permanently attaches every mirror page and large channel; maintains per-channel sample rings; harvests trigger windows; manages the Settle queue; writes to disk and applies retention |
| record_mirror (extended) | inside each business process | sender-side time-window cache for small structs (shm) |
| cloud/img channels (extended) | inside the driver process | sender-side time-window cache for large frames (shm, configurable slot count) |
| Configuration file | `/etc/thyme/trigger_config.yaml` (deployment) | trigger sources, channels, windows, priorities, quotas, write policy |

### 3.3 Design principles

1. **Sender-side cache, fully distributed**: history stays in the producer's own shm; the recorder normally reads only descriptor-level metadata and harvests by window at trigger time. Steady-state CPU/memory cost is minimal, and a producer crash loses only its own cache.
2. **Publish-point taps live inside the primitives, covering in-process and cross-process alike, transparent to nodes**: the tap is inside `Push`/`Commit`, so it does not depend on the node calling any recording API, nor on whether an shm channel exists.
3. **Zero copy until the write**: after a trigger only descriptors are swapped; the serialization write reads shm directly. The only copy happens in the write syscall itself.
4. **Recording is a side path**: a failed or overloaded recorder must not apply backpressure to business channels — every swap may degrade to a "hole marker" instead of blocking the producer (following the existing drain semantics).
5. **Every threshold is configurable**: windows, frame-decimation ratio, quotas, mute, and write policy all live in configuration; contract constants only provide upper bounds.

## 4. Detailed design of the sender-side cache and zero-copy swap

### 4.1 Tap points: pushed down into thyme transport primitives (transparent to nodes)

Current state: gen_graph generates `RecordTap` helpers, but **hand-written nodes are expected to call them explicitly after `publish()`** (`emit_integration.cpp` checklist: "every output calls the matching RecordTap* after publish()") — recording is visible to the node and depends on nobody forgetting the call. Large-channel nodes such as the LiDAR driver write the channel directly with `Begin()`/`Commit()` and have no RecordTap at all.

Design change: **push the tap down into thyme transport primitives**, so node business code is entirely unaware and needs no modification:

| Transport primitive | Coverage | Tap location |
|---|---|---|
| `ShmSpmcRing<T, Cap>::Push` (`runtime/shm_spmc_ring.h`) | cross-process small-struct channels | inside `Push`: after the ring write, append to the mirror ring |
| `ShmSpscRing::Push` (`runtime/shm_spsc_ring.h`) | cross-process value FIFO channels | inside `Push`, same as above |
| `SpscRing` / `LatestSlot<T>` (`runtime/spsc_ring.h`, `latest_slot.h`) | **in-process channels** (between roles of the same process) | inside `Push`/`Store`, same as above |
| legacy downstream cloud/image channel writers: `Commit` | LiDAR point cloud / camera large-frame channels | inside `Commit`: append frame/descriptor to the recording ring (replaced by `PinnedSlotWriter::Commit` in 4.3) |
| `/h264_<camera>` new channel `PinnedSlotWriter::Commit` | camera H.264 stream | same as above |

Implementation points:

- **struct_id injection**: the primitives are templates and do not know a struct_id. The generated code / app startup passes it in a **constructor argument** when wiring (the record table already generates `payload_size`/`priority` metadata per struct_id, `record/record_table.h`); for the node this only changes the port type's constructor arguments, never business logic.
- **Layering**: runtime primitives must not depend back on the app singleton; the assembly layer (`MpRoleSession`/app main) injects this process's mirror-ring pointer into the constructors of channels/rings. A default of nullptr means "do not record" (zero cost for unit tests and tool processes).
- **Tap cost**: small structs = one memcpy of ≤512 KB (in practice usually tens to hundreds of bytes) into the process's shm; large frames = descriptor write only. A tap failure only increments a counter and never blocks the business path.
- The codegen RecordTap convention is retired with it (the corresponding integration-checklist entry is removed), avoiding a double tap.
- The only "change" on the node side is a recompile (headers are inlined) — **no source modification**.

### 4.2 Small structs: record_mirror grows from "latest value" to a "time-window sample ring"

Current slot layout (`record_mirror.h`): one slot per struct_id, seqlock + fixed-size payload slot, latest value only, 2 MB arena hard-coded.

Extension design (`RecordMirrorRingPage`, a separate page type with its own magic/version so the two ABIs stay distinguishable; as built in `include/thyme/runtime/record_mirror_ring.h`, magic "THYMERRG" v6):

```
RecordMirrorRingPage (shm, one per process)
├── Header: magic/version/session/slot_count/ring_epoch/drop_cnt/oversize_cnt
├── StructDesc table[64]                    # unchanged, contract-generated
└── one SampleRing per struct_id:
    ├── ring_head(atomic) / slot_count / slot_capacity(=round_up(payload,64))
    ├── slots[N]: {seqlock, mono_ns, source_sequence, payload[slot_capacity]}
    └── N follows the configured window: N = ceil(window_s × rate_hz), bounded by the byte budget
```

- The slot count N is no longer a compile-time constant: it is derived from the `max(before)+after` window of the trigger source that owns the struct and from that channel's rate, and the page is laid out from configuration at process start (configuration and contract both come from the gen_graph record table, so the two sides cannot drift).
- The arena size changes from a fixed 2 MB to a configured calculation (see 4.5); the page header carries `config_hash`, and the recorder verifies configuration consistency when attaching.
- Writes remain data-plane lock-free with a single-writer seqlock; the reader (recorder) is read-only. "Lock-free" refers only to the data plane — for the precise statement on wait/wakeup paths (same-process mutex/condvar, cross-process futex) see [glossary.md](../reference/glossary.md) §4.

### 4.3 Large-frame channels: unified into the payload-agnostic pinned-slot component

Currently `cloud_channel.h` and `image_channel.h` in the downstream contract headers are two copies of the same pattern (seqlock + head + futex, `Begin`/`Commit` writing directly into shm slots), differing only in parameters and metadata location. **This design unifies them into thyme runtime's payload-agnostic pinned-slot component — `PinnedSlotWriter` / `PinnedSlotReader`, `include/thyme/runtime/pinned_slot_store.h`** (shipped as part of P1, see 12.1; the API below is what the implementation provides):

```
PinnedSlotWriter::Create(name, slot_bytes, slot_count, meta_blob_bytes)
slot layout: [PinnedSlotHeader{seq(seqlock), mono_ns, data_len, flags, crc32}]
             [payload area: slot_bytes][PinnedSlotFooter magic(written last, completion marker)]
page header: magic/version/slot_count/slot_bytes/head/epoch(futex)/meta_blob_bytes
API:         Writer: Begin(len) -> {payload*, header*} / Commit()
             Reader: TryRead(view) / WaitView(deadline)     # zero-copy view
```

- `slot_bytes`/`slot_count` are **runtime creation arguments** — this natively satisfies "configurable slot count" (4.5) and removes the need to change a contract compile-time constant.
- Type safety is kept by thin typed wrappers in the downstream contract headers (cloud point-cloud parameters, image width/height/format, h264 bitrate/GOP), while shm names stay `/cloud_*`, `/img_*`, `/h264_*`.
- Unified parameters for the three channels:

| Channel | slot_bytes | slot_count (default / recording) | meta_blob |
|---|---|---|---|
| `/cloud_<lidar>` | 19.2 MB upper bound (variable `data_len`) | 4 (consumer) / recording ring sized separately (4.5) | point stride, coordinate frame |
| `/img_<camera>` | w×h×1.5 + footer | 4 (consumer) | width/height/format/frame_id |
| `/h264_<camera>` | 64 KB (one AU within a GOP) | derived from window × bitrate (4.5) | bitrate/gop_s/resolution |

- **Recording benefits**: the tap has a single point, `PinnedSlotWriter::Commit`; the swap descriptor format is unified as `(shm_name, slot_idx, seq, mono_ns, len, flags)`; there is one set of unit tests (`test_pinned_slot_channel*.cpp`).
- Migration: existing `/cloud_*` consumers (preprocess, the LiDAR-match thyme node) and `/img_*` (no in-chain consumer yet) move to the new component as thyme rolls out; the old headers keep a compatibility alias for one version and are then deleted. The cloud-header follow-up about "handle+arena upgrade" is merged into this unification.

#### 4.3.1 camera: H.264 stream recording (default) — one instance of the unified component

Rationale: the downstream camera driver already has a hardware H.264 encoder built in (default 4000 kbps, with an existing file-saving precedent), and the `/img_*` NV12 channel header also notes that in-chain consumption of raw frames has not landed. Recording H.264 is attractive because 15 s × 5 cameras is only ~37 MB (versus 3.9 GB for NV12), needs no frame decimation, and gives better evidence usability (playable directly).

Design:

1. `/h264_<camera>` is a "many small variable-length slots" instance of the pinned-slot component (64 KB per slot × hundreds of slots): one Annex-B access unit per slot, with `PinnedSlotHeader.flags` carrying `is_idr`. Producer = the camera driver (`Begin(len)` + write + `Commit()` in `H264EncodedCallback`, one memcpy; that output already exists, it just goes only to DDS today).
2. **GOP-aligned window splitting**: H.264 is a streaming codec, so the pre-window start must be IDR-aligned: when harvesting `[t0-before, t0]`, the recorder walks back to the **last IDR inside the window** (configured `gop_s` upper bound 2 s; the actual start is at most 2 s earlier); the post window runs to the next IDR after `t0+after`, completes the GOP, and then truncates, so the result is decodable.
3. Bitrate is configurable (default 4 Mbps; memory/disk budget follows directly from bitrate × window).
4. NV12 raw frames remain available as a `format: nv12` debug profile (charged against the raw-frame budget in 4.5) and are off in the default configuration.

### 4.4 Zero-copy swap protocol

At trigger instant `t0`, the recorder does the following for every participating channel:

1. **Snapshot harvest (pre window)**: read `ring_head`, select the slots in `[t0-before, t0]`, and append each slot's descriptor `(shm_name, struct_id/channel, slot_idx, seq, mono_ns, len)` to the **pending-write queue** (in recorder process memory). Payloads are not copied.
2. **Post-window continuation**: enter the Post phase and keep appending descriptors for new slots in `(t0, t0+after]` (large-frame channels read the channel ring head; small structs read the mirror ring).
3. **Settle / write**: the drain thread `pread`s each queued slot in order and verifies the seqlock before writing:
   - seq unchanged → data is valid, serialize and write;
   - seq changed (producer overwrote it) → write a **hole marker** (struct_id, mono_ns, len, reason=overrun), following the existing drain semantics.
4. When the queue is fully consumed (or truncated by a higher-priority preemption), the capture is complete.

Tear prevention and protection:

- While a slot is referenced the producer **keeps writing** (backpressure on business is not allowed); protection comes from the margin "window depth × rate ≫ write duration", with hole markers as the fallback. The measured on-target write duration feeds the capacity calculation (4.5).
- Recorder crash: the descriptor queue has a WAL (reusing the existing drain WAL); after restart it can resume or mark the package incomplete.

### 4.5 Ring capacity calculation (10 s before + 5 s after)

**Current state**: cloud 4 slots = 0.4 s at 10 Hz; img 4 slots = 0.13 s at 30 fps — far from covering the window.

**Memory for the 15 s target window** (slot count = ceil(window × recording rate); camera records H.264 by default):

| Channel | Per frame / bitrate | 15 s memory | × channel count | Notes |
|---|---|---|---|---|
| `/h264_*` ×5 | 4 Mbps ≈ 0.5 MB/s | 7.5 MB per camera (including ~2 s GOP-alignment margin) | 5 → **~37 MB** | full frame rate, no decimation; IDR alignment adds at most 2 s |
| `/cloud_*` ×6 | actual ~0.5–2 MB (upper bound 19.2 MB) | 10 Hz → 75–300 MB per LiDAR | 6 → **0.45–1.8 GB** (typical 1 MB/frame ≈ 0.9 GB) | no decimation, variable-length arena |
| Small structs ×~60 | ≪1 KB–a few KB | 15 s of everything <100 MB | **<0.1 GB** | negligible |
| (optional) `/img_*` NV12 debug profile | 3.11 MB at 1080p | 10 s before at 10 fps + 5 s after at 30 fps ≈ 778 MB per camera | 5 → ~3.9 GB | only for the `format: nv12` debug profile |

**Recommended budget tiers** (total record shm quota is configurable; tmpfs is memory):

| Tier | camera (H.264) | lidar | small struct | Total | Suitable for |
|---|---|---|---|---|---|
| Standard 1 GB | 5 cameras × 15 s ≈ 37 MB | typical 0.9 GB (trim the window by quota in worst-case frame scenarios) | 0.1 GB | **~1.0 GB** | **recommended baseline** |
| Comfortable 2 GB | same + bitrate raised to 8 Mbps (~75 MB) | full rate + stitched 1.2 GB | 0.1 GB | ~1.4 GB | high-bitrate needs |
| Debug 4 GB+ | add NV12 raw frames (tiered window: 10 s before at 10 fps + 5 s after at 30 fps, +3.9 GB) | 0.9 GB | 0.1 GB | ~5 GB | development vehicles |

Deployment rules:

1. At startup, calculate each channel's slot count and total bytes from configuration; **refuse to start if the quota is exceeded and print the over-budget breakdown** (never silently shorten a window).
2. The plan for trimming by channel priority lives in configuration (`channel_priority`); high-priority channels are trimmed last.
3. On-target measurements: actual cloud frame-size distribution, actual H.264 bitrate, drain write throughput — feed these back into the calculation table before fixing the tier.

**Continuous mode (6.6) disk-growth accounting** (implemented as `trigger_cfg::ValidateContinuousPlan`, run at host startup):

```
sustained_bps = Σ (within the continuous channel whitelist):
    h264_stream:  bitrate_kbps × 1000 / 8                     (GOP stream, no decimation)
    pinned_slot:  rate_hz × avg_frame_bytes / dec             (dec = decimation ratio)
    small_struct: rate_hz × avg_frame_bytes                   (contract payload approximation)
disk_gb_per_hour   = sustained_bps × 3600
quota_coverage_h   = disk_quota_bytes / sustained_bps / 3600
```

Validation rule: when `io_rate_limit < sustained_bps`, act according to `[continuous] rate_check` — `enforce` refuses to start, `warn` logs a warning (default), `off` skips the check. The lag that rate limiting pushes into continuous traffic is counted as `pinned_slot_drops` by the streaming drain's gap-skip (not charged against the pending quota — streaming large frames never enter the descriptor queue).

**Continuous-mode disk tiers** (camera H.264 full recording + decimated LiDAR scenario):

| Tier | Continuous content | Disk growth | 20 GB quota coverage | Suitable for |
|---|---|---|---|---|
| Minimal | all small structs (~60 channels, ≪1 KB at ≤100 Hz) | <1 GB/h | >20 h | default floor |
| Standard | + 5 H.264 streams at 4 Mbps | +0.9 GB/h ≈ **~1.9 GB/h** | ~10 h | recommended |
| Comfortable | + point cloud decimated 1/10 (6 LiDARs × 1 MB × 10 Hz → 1 Hz) | +2.2 GB/h ≈ ~4 GB/h | ~5 h | long-distance review |
| Debug | full-rate point clouds | ~220 GB/h | ~5 min | short tests only (fast rolling slices + retention) |

## 5. Detailed design of the trigger center

### 5.1 Trigger API

Reuse the Unix datagram mechanism from record_control, on a separate socket `/tmp/thyme_trigger.sock` (separate from the recorder control plane, which simplifies authentication and upgrades):

```
# request (fixed-size header + variable-length source name, little-endian; proposed shape)
struct TriggerRequest {
  uint32_t magic;          # 'TRIG'
  uint16_t version;
  uint16_t flags;          # bit0: has_timestamp; bit1: force (manual preemption confirmation)
  int32_t  priority_hint;  # -1 = use the configured default
  uint64_t timestamp_ns;   # optional, default server CLOCK_MONOTONIC now
  char     source[64];     # trigger source name, the configuration lookup key
};
# response
struct TriggerResponse {
  uint32_t magic; uint16_t version; uint16_t code;
  # code: 0 accepted; 1 muted; 2 quota_exceeded; 3 unknown_source;
  #       4 preempted_by_higher; 5 recorder_offline
  uint64_t trigger_id;
};
```

The shipped protocol lives in `record/trigger_protocol.h` (88 B request / 16 B response); the sketch above is the design-era shape.

Client library: header-only (in the style of `record_mirror_client.h`), `trigger("collision")` is one send/receive with a 100 ms timeout; failures are only logged — the business thread never blocks and does not depend on the recorder being alive.

### 5.2 Trigger-source configuration lookup

trigger_center loads `trigger_config.yaml` at startup (chapter 7) and resolves each source as:

```
source → { channels: [...], before_s, after_s, priority, mute_s,
           flush_policy: idle|realtime, quota_class }
```

On a trigger → look up configuration → build a parameterized Trigger and send it to the recorder over record_control (extending the record_control message with trigger_id, window, channel list, flush_policy, priority). An unknown source is handled according to the `default_source` configuration or rejected (configurable).

(Implementation note: `trigger_center` forks the recorder by default. The parent's hooks write trigger/preempt/evict into a shared page and wake the child through an inherited eventfd; the page contains only channel names, while frames stay in the mirror or a large-frame lease. `--in-process` still uses same-process hooks. Continuous Arm/Disarm/Status is still handled by the record_control socket in the child. Host wake-up and first-frame comparison is covered by the trigger-center host tests (`test_trigger_center_fsm`, `test_trigger_e2e`), not by on-target results.)

### 5.3 Trigger-center state machine (mute and preemption)

```
                 ┌────────────────────────────────────────┐
                 ▼                                        │
   ┌──────┐  accept (priority trigger)  ┌───────────┐  capture done + mute expired
   │ IDLE │ ──────────────────────────▶ │ CAPTURING │ ─────────────────────┘
   └──────┘                            └─────┬─────┘
        ▲                                    │ after seconds captured
        │                                    ▼
        │                              ┌───────────┐   mute_s expires
        └──────────────────────────────│  MUTED    │ ──────────▶ IDLE
                                       └───────────┘
   Rules:
   · IDLE:      any source trigger → accept
   · CAPTURING: priority > current → preempt (below); ≤ current → reject (code=1)
   · MUTED:     priority == highest class (crash level) → accept immediately;
                otherwise → reject (code=1) (ensures "the next trigger is allowed
                only after capture completes")
```

**Preemption semantics (high priority, e.g. crash)**:

1. Send `PreemptTrigger` to the recorder: the current capture is truncated immediately — already harvested pre data is kept and marked `truncated`, the post window is abandoned;
2. Clear mute and start a new capture immediately, with the window recomputed from the new trigger instant;
3. The new capture forces `flush_policy: realtime` (no waiting for idle) and raises the drain to the highest IO priority (see 6.3);
4. Quotas allow overriding the count and memory limits (see 6.4).

**Concurrent multiple sources**: configuration `max_concurrent_captures` (default 1). When >1, different sources may capture in parallel (meaningful only when their channel sets are disjoint; overlapping channels are de-duplicated by time window, and the recorder admits a channel to only one capture queue at any instant — overlapping trigger sources should be avoided during configuration review).

### 5.4 Quota accounting (trigger-center side)

- Count: trigger counts per `quota_class` in a sliding window (e.g. 1 h); over the limit is rejected (code=2); the crash class is unlimited.
- Memory: the pending-write queue is capped by the recorder's `pending_bytes_quota`; the center reads the byte count through the `pending_bytes` hook. On overflow, new triggers are rejected only for low priority; a crash-level trigger evicts the oldest low-priority pending data (writing hole markers and discarding it).

## 6. Write-to-disk subsystem

### 6.1 Hybrid format: thyme_bin container + raw frame segments

Each trigger produces one capture package directory:

```
/opt/usr/triggered_data/<trigger_id>_<source>_<ts>/
├── meta.tbin          # thyme_bin: trigger metadata (source, priority, window, config hash, channel list, hole statistics)
├── small.tbin         # thyme_bin: small-struct sample records (struct_id, mono_ns, seq, payload)
├── frames/            # large-frame segments: one index.tbin + raw frame files per channel
│   ├── cloud_w1_1_forward_b1.index.tbin   # (mono_ns, seq, offset, len, flags) sequence
│   ├── cloud_w1_1_forward_b1.000123.raw   # raw point-cloud bytes
│   ├── h264_front.index.tbin              # (mono_ns, seq, offset, len, is_idr, flags)
│   └── h264_front.h264                    # Annex-B stream segment, starts at an IDR, directly muxable for playback
└── wal.log            # on-disk WAL (resume/incomplete detection after a crash), sealed when writing finishes
```

- Small messages use the thyme_bin record stream (reusing `thyme_bin_stream.h`); large frames use raw binary segments plus an index — a hybrid of "bag + raw binary": the index carries the bag's time-ordered lookup role, while raw segments avoid container overhead and the 512 KB limit.
- The camera H.264 segment is cut on GOP boundaries (4.3.1) and can be muxed into mp4 with `ffmpeg` for playback; the NV12 debug profile stores `.nv12` raw frames in the same directory.
- Index flags carry `hole/overrun/truncated/decimated` markers; replay tools can degrade gracefully based on them.

### 6.2 Idle-time write to disk (default mode)

- The pending-write queue from the Settle phase resides in memory (bounded by the recorder's `pending_bytes_quota`).
- **Idle detector** (a thread inside the recorder): periodically reads the latest `/can_chassis` value from the vehicle CAN process's mirror page (`park_brk_status == 1` (EPB Engaged) and `vehicle_speed < 0.1 m/s` for `idle_hold_s`, default 30 s) → declares idle and starts draining.
- If the mirror is unreadable (CAN node not started), degrade according to `idle_fallback: always_busy|assume_idle`, default `always_busy` (do not write; rely on the quota-pressure fallback in 6.4).
- Idle draining is also rate-limited (6.3), so a long drain does not steal disk bandwidth from the next drive's logs.

(Implementation note: idle detection is implemented as the library component `idle::IdleDetector` (`include/thyme/record/idle_detector.h`) with an injectable predicate; engine integration is `TriggeredRecorder::Trigger(..., defer_settle)` plus a one-shot `AllowFlush()` (triggered mode) and the level-based `SetContinuousGate()` (continuous mode). The quota-pressure fallback (allow flushing even when busy once the pending ratio reaches 0.8) is already implemented inside the detector. The chassis field mapping belongs to the downstream contract package and will be wired during on-target bring-up.)

### 6.3 Real-time rate-limited write (optional mode + mandatory for crash)

- The drain thread sets `ioprio_set(IOPRIO_WHO_PROCESS, tid, IOPRIO_CLASS_IDLE)` — it is scheduled only when IO is idle, minimizing system stalls;
- A token bucket additionally shapes the write rate (`io_rate_limit_mbps`, default 20 MB/s);
- **cgroup v2 `io.max` as a supplement**: if on-target validation shows the platform kernel has the cgroup v2 io controller enabled, the deployment script moves the recorder process into a dedicated cgroup and sets `io.max` write bandwidth. Validation steps:
  1. `mount | grep cgroup2`, `cat /sys/fs/cgroup/io.controllers`;
  2. create `/sys/fs/cgroup/thyme_recorder/`, write `io.max`, then run fio and compare;
  3. if unavailable, keep only the ioprio + token-bucket path (configuration `use_cgroup: auto|on|off`, default `auto`).
- Crash-level trigger: the drain thread is temporarily raised to the highest best-effort `IOPRIO_CLASS_BE` level and the token bucket is lifted; both are restored after the write.

### 6.4 Quota management (count/memory/disk)

| Resource | Configuration | Normal priority | Crash level |
|---|---|---|---|
| Trigger count | `max_triggers_per_hour` (per quota_class) | reject over the limit | **overridable** (still accounted and warned) |
| Pending memory | `pending_bytes_quota` | Center checks via the `pending_bytes` hook and rejects new triggers over the limit | **overridable**: evict the oldest low-priority data (hole marker) |
| Total disk | `disk_quota_mb` | retention deletes the oldest capture packages | **may not enter the reserved area** |
| Disk reservation | `reserved_mb` + dedicated subdirectory `high/` | low-priority retention never deletes `high/` | exclusive use; when its own reservation is exceeded, delete the oldest crash packages |

Retention scans directories as units (not by file suffix, avoiding the class of failure where legacy ROS 2 `.mcap` packages became invalid); it runs after every write and periodically (every 5 min).

### 6.5 Disk full and write failures

- On ENOSPC: seal the current package as incomplete, run retention, retry once; if that still fails, reject subsequent low-priority triggers (crash level writes to the reserved area).
- The whole drain path carries a WAL; on restart the recorder first scans the WAL to recover or seal incomplete packages.

### 6.6 Continuous capture mode (shares the pipeline with triggered capture)

thyme's existing drain path — a per-slot cursor over the mirror page that polls and keeps appending to a thyme_bin stream, with a RAM WAL structure to absorb backpressure — is already continuous capture. This design unifies it as two modes of the same recorder rather than a separate component:

| | Triggered (the main line of this design) | Continuous (reuses the existing drain) |
|---|---|---|
| Data source | the same sender-side caches (mirror ring + pinned-slot store) | same |
| Write timing | swap and write by window after Trigger | drain continuously after `Arm` (= an unbounded window), rolling slices by time/size |
| Write pipeline | descriptor queue → thyme_bin/raw segments → retention | same, reusing ioprio/rate limit/quota/retention |
| Large-frame channels | written on trigger | by default **not written continuously** (disk cannot sustain it); a configurable whitelist may opt in |

- Configuration: `recorder.continuous: { enable: true, channels: [all small structs], slice_mb: 512 }`; large-frame channels are excluded by default.
- Combined use: continuous drain keeps all small structs as a baseline, while triggered capture snapshots windows (including large frames) of the same channels; both share the disk quota, continuous slices and trigger packages participate in retention together, but trigger packages (especially the crash reserved area) are kept preferentially.
- Control plane: continuous capture uses the existing record_control `Arm/Disarm` and does not block Trigger; the trigger center does not take part in continuous mode.

## 7. Complete configuration schema example

(Implementation note: the repository has no general YAML parser and we do not add a new dependency; the actual on-disk format is a minimal INI dialect parsed in `src/record/trigger_config.cpp`. Fields map one-to-one to the schema in this chapter (mapping table in the header comment of `include/thyme/record/trigger_config.h`): `[trigger_center]`/`[recorder]`/`[channel.<shm name>]`/`[source.<name>]`/`[quota.<class>]`/`[continuous]`. The YAML below is the semantic authority.)

> **Implementation differences** (per-field table in [trigger-config.md](../guides/trigger-config.md)): `recorder.continuous.enable` has been renamed `auto_arm` (the old name is still accepted with a warning) and only controls automatic Arm at startup; `rate_check` runs on every Arm (automatic or manual). `recorder.flush.use_cgroup` is not implemented — configuring it is a parse error. `channels[].gop_s` is effective (default 0, no window extension; `1..60` is allowed only for `h264_stream`; the pre-window starts at the last IDR inside the window, and the post-window ends at the next IDR without including that frame). `[source.*].channels` is effective as a per-trigger channel filter (empty = all; selectors must match `[channel.*]` and support `*` wildcards). `slice_mb` must satisfy `0 < slice_mb <= disk_quota_mb`.

```yaml
# /etc/thyme/trigger_config.yaml
trigger_center:
  socket: /tmp/thyme_trigger.sock
  max_concurrent_captures: 1
  default_source: manual        # unknown sources fall back to this config; absent = reject

recorder:
  socket: /tmp/thyme_recorder.sock
  output_dir: /opt/usr/triggered_data
  pending_bytes_quota: 67108864 # sole quota for the pending-write queue; owned by the recorder, Center coordinates via the pending_bytes hook
  disk_quota_mb: 20000          # total disk quota
  high_priority:
    dir: high                   # dedicated subdirectory (not implemented/future: only reserved_mb is implemented, as high_reserved_mb)
    reserved_mb: 4000           # disk reservation that low-priority retention may not enter (implemented as high_reserved_mb)
  flush:
    policy: idle                # idle | realtime (a trigger source may override)
    idle_hold_s: 30             # duration of EPB Engaged + speed ≈ 0
    idle_fallback: always_busy  # degradation when the chassis mirror is unreadable
    io_rate_limit_mbps: 20
    use_cgroup: auto            # not implemented: cgroup v2 io.max is a deployment-side concern; configuring it is a parse error
  continuous:                   # continuous capture (6.6); shares quota and the write pipeline with triggered capture
    enable: true                # renamed auto_arm (old name still accepted with a warning)
    channels: ["small_struct:*"]    # by default only small structs are continuous; large-frame channels are written only on trigger
    slice_mb: 512                   # rolling slice size (slice_s/wait_idle/demo_idle/rate_check are also implemented)

# Channel registration (generated from the same source as the gen_graph record table; only recording parameters are given here)
channels:
  - name: /h264_front
    kind: h264_stream           # variable-length byte ring (INI spelling: h264_stream)
    bitrate_kbps: 4000          # encoder bitrate; window memory = bitrate × (window + GOP margin)
    gop_s: 2                    # H.264 trigger-window IDR margin (seconds); 0 = no extension. pre starts at most this much earlier; post truncates at the next IDR
    window_cap_s: 20            # per-channel window cap (not implemented/future)
  - name: /img_front            # optional NV12 debug profile, off by default
    kind: pinned_slot             # INI spelling: pinned_slot
    format: nv12                # not implemented/future: pixel format is not parsed from recording config
    record_rate_hz: 10          # pre-window frame decimation; post-window full rate (implemented as rate_hz, used only for bitrate accounting; pre-window decimation is not implemented/future)
    slot_bytes: 3112960         # not implemented/future: transport geometry comes from the contract binding
    window_cap_s: 20            # not implemented/future
    enable: false               # not implemented/future: per-channel switch (to disable a channel, remove its [channel.*] section)
  - name: /cloud_w1_1_forward_b1
    kind: pinned_slot_varlen      # INI spelling: pinned_slot (variable-length is the default)
    record_rate_hz: 10          # implemented as rate_hz
    avg_frame_bytes: 1048576    # fill in after on-target measurement
    window_cap_s: 20            # not implemented/future
  # ... the other 5 LiDARs, stitched, the other cameras
  - name: /can_chassis
    kind: small_struct          # INI spelling: mirror_ring
    rate_hz: 50
    window_cap_s: 60            # not implemented/future
  # ... the remaining small-struct channels (recorded by default; use exclude to remove)

# Trigger sources
trigger_sources:
  - name: crash                 # collision (input source still to be connected, see chapter 11)
    channels: ["/h264_*", "/cloud_*", "/can_chassis", "/guardian_estop"]
    before_s: 10
    after_s: 5
    priority: 100               # highest: preempt + write immediately + override count/memory quotas
    mute_s: 0
    flush_policy: realtime
    quota_class: safety
  - name: estop                 # emergency stop
    channels: ["/h264_front", "/cloud_*", "/can_chassis", "/guardian_estop"]
    before_s: 10
    after_s: 5
    priority: 80
    mute_s: 30
    quota_class: safety
  - name: perception_anomaly    # perception anomaly (example; business integration comes later)
    channels: ["/cloud_stitched", "/pred_perception"]
    before_s: 10
    after_s: 5
    priority: 40
    mute_s: 60
    quota_class: debug
  - name: manual                # manual (recorderctl / UI)
    channels: ["/h264_*", "/cloud_*", "/can_chassis"]
    before_s: 10
    after_s: 5
    priority: 10
    mute_s: 30
    flush_policy: idle
    quota_class: manual

quotas:
  max_triggers_per_hour: { safety: 0, debug: 20, manual: 30 }   # 0 = unlimited
```

## 8. Exceptions and edge cases

| Scenario | Behavior |
|---|---|
| Producer process crash | the attached shm becomes invalid → data in that channel's window is written as hole markers; after the mirror page is recreated (`session_id` changes) the recorder re-attaches automatically |
| seqlock verification fails (slot overwritten) | write a hole marker for that frame and continue; warn when the hole rate exceeds a threshold (configurable, default 5%) — it means window depth or write speed is insufficient and the tier must be adjusted |
| Trigger storm | trigger-center mute plus count quota give two layers of limit; the datagram receive queue is bounded, and overflow is rejected and counted |
| Crash trigger during mute | accept immediately (mute does not apply to the highest class, see 5.3) |
| Low-priority drain in progress when high priority preempts | pause the low-priority drain (keeping its WAL position) and resume after the high-priority write finishes |
| Pending memory over the limit and the trigger is not high priority | reject with code=2; the caller may retry later |
| Recorder offline | the trigger center responds with code=5; trigger requests are not cached (avoiding a storm after recovery) |
| Disk full | see 6.5 |
| Configuration hot reload | the trigger center reloads `trigger_sources`/`quotas` on SIGHUP; changes to the `channels` section require a restart (shm layout cannot change online) |
| Clocks | window decisions always use CLOCK_MONOTONIC; each package also records REALTIME for cross-device alignment |

## 9. Verification plan

1. **Unit tests** (`tests/`, following the existing `test_record_*` style):
   - `test_record_mirror_ring_window.cpp`: eviction and window boundaries of the per-struct time-window ring (trigger exactly on a window edge, empty window, wrap-around);
   - `test_record_mirror_ring_abi.cpp`: `RecordMirrorRingPage` layout/version/`config_hash` validation;
   - `test_trigger_center_fsm.cpp`: all state-machine transitions (including preemption, mute, quota rejection);
   - `test_swap_hole.cpp`: construct overwrite scenarios and verify hole markers;
   - `test_pinned_slot_channel.cpp`: the unified large-frame component's Begin/Commit/variable length/seqlock tearing/footer validation/three-channel parameterization (cloud/img/h264 configurations);
   - `test_tap_in_primitives.cpp`: publish without calling any recording API, only through `ShmSpmcRing::Push`/`SpscRing`/`LatestSlot`/channel `Commit`, and assert that samples reach the mirror ring (regression test for node transparency); with the tap pointer nullptr, behavior and performance match the status quo.
2. **Harness integration**: use the gen_graph scaffold to generate a two-node graph (one in-process channel and one cross-process channel) and assert that **samples from both channel types reach the mirror ring** (regression test for in-process recording); after a simulated trigger, verify package contents and window integrity.
3. **On-target acceptance**:
   - total record shm usage versus quota under real load; feed the actual cloud frame-size distribution back into the 4.5 calculation table;
   - IO usage in real-time write mode: observe `iostat`/stalls under full driving load and verify the effect of ioprio + rate limiting; verify cgroup v2 `io.max` availability (steps in 6.3);
   - trigger → write-complete latency (crash level must be under a configurable threshold, default 30 s); preemption correctness; restart recovery.
4. **Long soak**: 72 h field-duty soak, tracking hole rate, quota hits, and the disk-growth curve.

## 10. Phased implementation roadmap

| Phase | Content | Depends on |
|---|---|---|
| P1 | Ring-ize record_mirror (`RecordMirrorRingPage`); **push taps down into transport primitives** (inside `ShmSpmcRing::Push`/`ShmSpscRing::Push`/`SpscRing`/`LatestSlot`/channel `Commit`, struct_id injected at assembly time, zero node changes); **unify cloud/img/h264 on the pinned-slot component `PinnedSlotWriter`/`PinnedSlotReader`** (slot_bytes/slot_count are runtime parameters, thin typed wrappers keep type safety, the LiDAR driver's `Begin`/`Commit` call sites are unchanged; the camera driver connects `/h264_*` in `H264EncodedCallback`) | none |
| P2 | Trigger-aware recorder: AttachReadOnly on all channels, pre/post window harvesting, swap descriptor queue, hybrid write format, WAL | P1 |
| P3 | trigger_center: API/socket, config lookup, state machine (mute/priority/preemption), count quota | P2 (can be developed in parallel; integration depends on P2) |
| P4 | Write policy: idle detection, ioprio + rate limit, disk quota and reservation, retention | P2 |
| P5 | On-target validation and capacity tiering (4.5/9.3); extend recorderctl with a trigger entry point | P1–P4 |
| P6 (later) | Connect business trigger signals (estop/DM faults, etc.), replay/export tools, UI integration | P5 |

## 11. Open questions

1. **Collision signal source**: the on-vehicle `/CAN/BCU/DiMsg1` (safety edge) has no producer; the crash input must be defined first (add BCU to the CAN DBC, or approximate it with `vcu_err_level`/`/guardian_estop`). This phase only reserves the configuration and priority mechanics.
2. **Availability of the cgroup v2 io controller on the target kernel**: unverified; 6.3 provides two paths and validation steps.
3. **Camera evidence quality tier**: H.264 by default (4 Mbps, adjustable); if accident analysis needs raw NV12, enable the `format: nv12` debug profile (budget in the 4.5 debug tier).
4. **Legacy ROS 2 package migration**: topics still on ROS 2 (if any) are temporarily covered by the legacy package; it is retired once everything moves to thyme.
5. **VPC/DVPP direct write into the recording ring**: camera frames currently take one memcpy into the channel slot; having the recording ring also receive DMA writes would require a DVPP interface change, listed as a later optimization (the `image_channel.h` header already marks this follow-up).
6. **Multiple concurrent captures**: the channel-intersection de-duplication policy for `max_concurrent_captures > 1` must be refined during implementation.
7. **H.264 window start offset**: when `gop_s` is configured, the pre window aligns to the nearest IDR, so the actual start is at most `gop_s` earlier; the default `0` does not change the nominal window. If an exact 10.0 s start is required, shorten the GOP or do not set `gop_s`.

## 12. Implementation status (as-built)

> This chapter is maintained by the implementation side and records "what was actually built". The design discussion (chapters 1–11) is left as written; where it no longer matches, a short "(implementation note: …)" is added in place.

### 12.1 Phase completion

| Phase | Status | Key landing files | Tests |
|---|---|---|---|
| P1 sender-side cache and tap | **Done** | `include/thyme/runtime/record_mirror_ring.h` (RecordMirrorRingPage, magic "THYMERRG" v6, per-struct ProducerGate), `include/thyme/record/record_mirror_ring_client.h` + `src/record/record_mirror_ring_client.cpp` (RingClient, `/thyme_recr_<pid>`); taps pushed down via `SetRecordTap` in `runtime/shm_spmc_ring.h` / `shm_spsc_ring.h` / `spsc_ring.h` / `latest_slot.h`; `runtime/pinned_slot_store.h` (PinnedSlotWriter/PinnedSlotReader) | test_record_mirror_ring / test_tap_in_primitives / test_pinned_slot_channel / test_tap_cross_process / test_record_mirror_ring_mp / test_crash_injection |
| P2 recorder capture engine | **Done** | `include/thyme/record/triggered_recorder.h` + `src/record/triggered_recorder.cpp` (TriggeredRecorder: Armed/Post/Settle, pre `HarvestWindow` + post `PeekRingSample` continuation, swap descriptor queue + quota, 6.1 package format, `wal.log` sealing, `PackageComplete`); API gaps filled: PinnedSlotReader `head()`/`ReadFrame`/`View::slot_seq`, ThymeBinReader exposes pre/post_roll | test_triggered_recorder / test_trigger_capture_pipeline / test_thyme_bin_trigger |
| P3 trigger center | **Done** | `record/trigger_protocol.h` (88 B request / 16 B response), `record/trigger_config.*` (INI dialect), `record/trigger_center.*` (Center FSM + quota + socket), `record/trigger_client.h` (header-only client), `tools/apps/trigger_center/{trigger_center_main,triggerctl}.cpp` | test_trigger_center_fsm / test_trigger_protocol / test_trigger_e2e |
| P4 write policy | **Done** | `record/idle_detector.h` (injectable predicate + fallback + pressure fallback), `record/token_bucket.h`, `record/io_priority.h` (`ioprio_set`), `record/retention.*` (quota/incomplete marker/high reserved area), engine `defer_settle`/`AllowFlush`/`Preempt`/`EvictOldest`/`disk_full`/`high_priority`, Center `disk_full` coordination | test_idle_flush / test_rate_limit / test_retention / extended test_trigger_e2e |
| Continuous capture (6.6) | **Done** | engine `ArmContinuous`/`DisarmContinuous` (bucketed queues + shared quota, continuous bucket evicted first), rolling slices `continuous_<seq>_<ts>/`, retention type priority; control plane reuses record_control arm/disarm/status + `triggerctl` subcommands; `tools/apps/trigger_center/demo_producer.cpp` | test_continuous + host smoke test (arm → slice → disarm and seal → trigger package coexists) |
| P5 on-target validation | **Pending on-target** | see the 12.3 to-do list | — |
| P6 follow-up | Not started | replay tools, large-frame whitelist accounting for continuous capture, multiple concurrent captures, etc. | — |

Acceptance baseline: `cmake --build` with zero warnings (all of `-Wall -Wextra -Wpedantic -Wconversion` and friends enabled); `ctest` 75/75 green, with repeated parallel runs showing no flakiness. Multi-process, crash injection, and fault injection (SIGKILL, ENOSPC, session restart) each have dedicated tests.

### 12.2 Cumulative deviation list (relative to the chapters 1–11 design)

Each entry is "deviation — reason":

1. RingClient is a new class rather than a `CreateRing` overload on the old client — the two pages have different ABIs/shm names, isolation is cleaner, and the old API is untouched byte for byte.
2. The SpscRing tap supports an optional payload-pool base address (by default it records the packed `(idx,gen)` word) — the index ring carries no payload itself, and the assembly-layer wiring comes later.
3. LatestSlot gains a tap member (+16 B in `sizeof`) — gen_graph computes `sizeof` from the same header on both sides, so the two agree; this requires recompiling all processes at the same version (the "node only recompiles" requirement).
4. The tearing counter is process-local (out-param) rather than an shm counter — a read-only mapping writer that writes to shm would SIGSEGV (observed in fault-injection testing); this follows the `drops_` convention of the cloud/image channels.
5. Large-frame pre-window harvest collects, reverses, and then enqueues — enqueueing directly in backtracking order made the on-disk mono sequence non-monotonic (a real bug exposed by tests, already fixed).
6. Post-window peek failure distinguishes "definitely evicted" from "being written/torn" — so a transient tear is not permanently recorded as a hole.
7. `TriggerRequest.source` uses a fixed 64 B field — the design struct was already `char[64]`, so "variable-length" was a typo-level contradiction.
8. Response code 4 (`preempted_by_higher`) is reserved and unused — the FSM rules reject low priority with code=1; 4 is left for later asynchronous notification.
9. The `force` flag semantics were decided as "preempt when priority >= current" (manual confirmation) — the design gave no decision formula.
10. The trigger center forks the recorder by default (shared page + eventfd); `--in-process` still uses same-process hooks. The continuous-capture control plane still reuses record_control (see the implementation note in 5.2).
11. Configuration is an INI dialect rather than YAML (fields correspond one-to-one) — the repository has no general YAML parser and we do not add a new dependency (gen_graph's `yaml.cpp` is specific to the Graph schema).
12. `meta.tbin` additionally writes two `kMetaStructId` records, `TrigMeta`/`CaptureStats` — the design only said "trigger metadata" without defining a format, and statistics are known only once settle finishes.
13. Quota eviction descriptors are only counted, not written as holes — by task agreement; replay shows a tail truncation.
14. Source session scanning happens only during Armed — in-flight captures stay mapped to the old generation, and overwrites are turned into holes by the drain verification, preventing new-session data from mixing in.
15. Continuation uses bounded polling (the Tick model) instead of futex blocking — the library stays thread-free and embeddable; futex waiting is left to the host main loop.
16. The engine's rate-limit bucket applies only to idle-flushed captures (realtime/crash never rate-limit); bytes are approximated as descriptor `len + 32 B`.
17. The ENOSPC "retry once" is implemented by the host loop (`disk_full` → retention → `ClearDiskFull` → retry on the next trigger) — the engine stays policy-free.
18. The ENOSPC test uses a fault-injection hook (part of the public Config, commented as test-only) — trading for deterministic coverage without filesystem trickery.
19. Incomplete packages are marked with an `.incomplete` suffix and still participate in quota deletion — "not automatically deleted" refers to the marking action itself; quota semantics treat incomplete packages as occupying disk too (consistent with sealing and retention in 6.5).
20. Continuous capture collects through the descriptor queue (not a direct drain-style write) — this satisfies "bucketing + shared quota accounting"; the cost is that under quota pressure a continuous stream may be dropped wholesale (counted, with visible mono discontinuities across slices), a behavior the design did not specify.
21. Continuous slice metadata uses two records (`ss=2` ContinuousMetaRecord / `ss=1` CaptureStatsRecord) rather than a new entry type — reusing the `kMetaStructId` channel.
22. On ENOSPC, continuous capture stops the whole continuous mode (the host re-arms after retention) — more conservative than aborting a single triggered package.
23. The continuous-capture control plane reuses record_control string commands — §6.6 specified reusing the existing Arm/Disarm.
24. `max_concurrent_captures` keeps a configuration slot but is implemented as 1 — multi-capture channel de-duplication is open question 6 in chapter 11.
25. TokenBucket allows a single over-burst entry to borrow into negative credit — prevents a giant-frame livelock.
26. `pending_bytes()` returns the combined total of the trigger and continuous buckets — the direct implementation of "shared quota accounting" in §6.6.

### 12.3 Deployment / on-target to-do list

**shm lifecycle**
- `/dev/shm` stale-object cleanup policy (the startup script sweeps `thyme_*`/channel names by prefix; crash-left objects are verified semantically safe by crash-injection tests, but they occupy tmpfs memory and must be reclaimed).
- Session-change detection is the reader's responsibility (periodically compare `session_id`/`config_hash` or pid liveness); producer orphan-process protection follows the tests' `PR_SET_PDEATHSIG` pattern.

**I/O and resources**
- fallback/privilege-dropping strategy for ioprio in scenarios that require `CAP_SYS_ADMIN`; on-target validation of the cgroup v2 io controller (validation steps in 6.3; the `use_cgroup` configuration placeholder is already in place).
- capacity tiering feedback: after measuring the actual cloud frame-size distribution, H.264 bitrate, and drain write throughput on target, feed the values back into the 4.5 calculation table and fix the tier; continuous-capture traffic must be counted in the quota baseline; the continuous-mode large-frame whitelist must be sized separately as `slice_mb × disk quota`.

**Contracts and business integration**
- wire the chassis predicate to the downstream contract package's chassis report field mapping (EPB + speed); the fallback when the mirror is unreadable is to be measured on target.
- define the crash trigger input (chapter 11 open question 1); generate the `[channel.*]` list from the same source as the gen_graph record table (currently hand-filled).
- retire the gen_graph RecordTap convention and wire assembly-layer tap injection (MpRoleSession injects this process's mirror ring into each channel's constructor).

**Architecture follow-ups**
- the recorder/center process split landed as the default fork (shared page + eventfd, see the implementation note in 5.2). On-target re-testing is not part of this item.
- channel-intersection de-duplication for `max_concurrent_captures > 1`.
- asynchronous notification to the preempted party (response code 4 reserved).
- quota sliding-window persistence and a crash-override warning channel (currently in-memory counters).
- replay tools: the package-format ABI is stable (wal structure, meta records, raw+index), but the read-side tool is not implemented.
