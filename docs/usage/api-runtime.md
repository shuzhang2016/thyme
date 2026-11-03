# Runtime primitive API

This page is the API reference for the `thyme::runtime` data path: in-process queues,
shared-memory ring queues, latest-value slots, the object pool, the notification pipe,
the `Frame<T>` wire format, and the lease / producer gate of the large-frame channel.
All types live in `namespace thyme`, with headers under `include/thyme/runtime/`
(`Frame` / `FrameHeader` are in `include/thyme/contracts/`).

None of these primitives allocate memory or carry global state; capacities, file
descriptors and shared-memory names are supplied by the caller at construction or
`Create` time. The concurrency contract is specified per type; misuse (multiple
producers, writing a `LatestSlot` across processes, and so on) is a logic error that
the primitive does not detect on the caller's behalf.

## 1. `SpscRing`

`include/thyme/runtime/spsc_ring.h`

```cpp
template <std::size_t Cap>
class SpscRing : public Waitable;
```

An in-process single-producer/single-consumer ring queue that moves only packed
`(index, generation)` words; the payload is owned by the caller's own pool. `Cap` must
be a power of two >= 2 (compile-time `static_assert`). `head_` and `tail_` each occupy
their own cache line.

| Member | Signature | Semantics |
|---|---|---|
| `kInvalidIndex` | `static constexpr uint32_t` = `0xFFFFFFFF` | no eviction / invalid index |
| `push_latest` | `uint32_t push_latest(uint32_t idx, uint32_t gen, uint32_t* dropped_gen = nullptr)` | evicts the oldest entry when full and returns its index; returns `kInvalidIndex` when nothing was evicted |
| `try_push` | `bool try_push(uint32_t idx, uint32_t gen)` | returns `false` when full and overwrites nothing |
| `try_pop` | `bool try_pop(uint32_t& idx, uint32_t& gen)` | returns `false` when empty |
| `size` | `uint64_t size() const` | instantaneous depth, may differ by 1 from concurrent push/pop |
| `drop_count` | `uint32_t drop_count() const` | cumulative evictions by `push_latest` |
| `waiter_count` | `uint32_t waiter_count() const` | number of registered `WaitUntil` callers |
| `Notify` / `WaitUntil` | `void Notify()` / `void WaitUntil(uint64_t deadline_ns)` | `Waitable` interface; the deadline is an absolute `CLOCK_MONOTONIC` nanosecond value |
| `SetRecordTap` | `void SetRecordTap(RecordMirrorRingPage*, uint16_t struct_id, const void* payload_base = nullptr, uint32_t payload_stride = 0)` | optional recording tap; a failure only bumps an in-page counter and never affects push |

Drop semantics: `push_latest` drops the oldest. When the consumer falls behind it may
skip entries and may also see the same "latest" value more than once; `try_pop`
returns **survival order**, not production order. A path that must consume every
entry has to size `Cap` for the worst-case lag, or switch to a queue that never
overwrites, such as `NotifPipe` / `ShmSpscRing`.

**Do not use a ring when you only keep the latest value.** If the consumer only cares
about the newest value and may skip intermediate ones, the head/tail/epoch fields and
`Cap` slots of `SpscRing` are pure surplus history. Use `LatestSlot<T>` for small
values and `PinnedLatestStore<T>` for large payloads (zero copy plus multi-reader
pinning). `push_latest` is "latest-wins with bounded history" (the most recent `Cap`
entries, so a consumer can occasionally catch up), not "keep a single index". If a
single index really is all you need, a one-slot `LatestSlot<index+generation>` is
semantically more direct; but you must also solve the lifetime of the pool slot (pin
before copying out, double buffering, or reference counting), otherwise the writer may
reuse that index before the reader takes the payload.

```cpp
#include "thyme/runtime/spsc_ring.h"

thyme::SpscRing<1024> ring;

// producer
uint32_t dropped_gen = 0;
const uint32_t dropped =
    ring.push_latest(pool_index, generation, &dropped_gen);
if (dropped != thyme::SpscRing<1024>::kInvalidIndex) {
  // the oldest entry was evicted; use dropped_gen to avoid a double release
}

// consumer
uint32_t idx = 0, gen = 0;
if (ring.try_pop(idx, gen)) {
  const auto& msg = pool.at(idx);
  pool.release(idx, gen);
}
```

`Notify()` first runs one seq_cst fence so that the publish and the waiter probe form
a store-buffering pair, then skips the condvar when `waiter_count() == 0`; with no
waiters the wake path is a single atomic read, and wake cost is charged per publish
rather than per waiter.

## 2. `ShmSpscRing` / `ShmSpmcRing`

### 2.1 `ShmSpscRing`

`include/thyme/runtime/shm_spsc_ring.h`

```cpp
template <typename T, std::size_t Cap>
class ShmSpscRing : public Waitable;
```

A cross-process SPSC queue that delivers every item by value. `T` must be trivially
copyable and `Cap` must be a power of two >= 2. The page format version is
`kShmSpscRingVersion == 4` and the magic is `kShmSpscRingMagic` (`"THYMSSR1"`). A v4
page starts with a 64 B `ShmSuperHeader`, followed by `cap`@64, `head`@128, `tail`@192,
`epoch`@256 and payload@320; `layout_hash` binds `sizeof(T)`/`alignof(T)`/`Cap` and all
offsets. Each payload occupies one 64-byte-aligned cacheline cell, with a slot stride
of `RoundUp64(sizeof(T))`. v3 pages are rejected at `Open` by the descriptor-page
check; there is no dual read.

Creating and opening:

| Member | Signature | Notes |
|---|---|---|
| `Prepare` | `static bool Prepare(const char* name)` | for the supervisor: unlinks the old name, creates the descriptor page, writes the cap and super-header (magic last) and unmaps; the producer/consumer `Open` the object afterwards |
| `Create` | `bool Create(const char* name)` | creates the descriptor page and writes the super-header (magic last as a release-store), then re-verifies |
| `Open` | `bool Open(const char* name, uint64_t timeout_ns)` | `OpenDescribed` validates magic/version/layout hash/page bytes/features and then checks the in-page `cap`; returns `false` on any mismatch |
| `Close` | `void Close()` | unmaps |
| `RegionBytes` | `static constexpr std::size_t RegionBytes()` | `sizeof(Layout)`, used for the manifest's `slot_bytes` entry |

Publishing:

| Member | Signature | Notes |
|---|---|---|
| `BeginWrite` | `T* BeginWrite()` | returns the next publish slot; when full it bumps `push_fail` and returns `nullptr` — do not spin and retry |
| `CommitWrite` | `void CommitWrite()` | release-stores `head`, bumps the epoch and wakes the consumer; one publish bumps exactly once |
| `AbortWrite` | `void AbortWrite()` | an explicit no-op: `head` does not advance before `CommitWrite`, and an uncommitted slot is invisible to the consumer |
| `Push` | `bool Push(const T& v)` | legacy interface; a wrapper around `BeginWrite` + assignment + `CommitWrite`, returns `false` when full. New code should use the in-place path |
| `TryPop` | `bool TryPop(T& out)` | `false` when empty |
| `push_fail_count` | `uint32_t push_fail_count() const` | number of times `BeginWrite` / `Push` found the ring full |
| `epoch` / `epoch_waiters` | `uint32_t epoch() const` / `uint32_t epoch_waiters() const` | wake-path observation; exactly one epoch bump per publish |

`Push` never overwrites: when full it returns `false` and bumps `push_fail`, leaving
the drop policy to the caller. `WaitUntil` registers in the shared `epoch_waiters`
first, then checks `empty()` and the epoch; the producer skips the futex syscall when
the waiter count is zero. Linux uses futexes; other platforms degrade to 1 ms polling.
`SetWakeWord(std::atomic<uint32_t>*)` attaches a cross-process shared wake word to the
channel, while `SetNotify(Waitable*)` only affects the forwarding target in this
process.

```cpp
#include "thyme/runtime/shm_spsc_ring.h"

struct Sample {
  uint64_t mono_ns;
  float value[4];
};

thyme::ShmSpscRing<Sample, 256> ring;

// producer process
if (!ring.Open("/thyme_sample_ring", 1000000000ull)) return -1;
Sample* slot = ring.BeginWrite();
if (slot == nullptr) {
  // full: record push_fail and drop or slow down per the channel policy
} else {
  *slot = sample;          // single producer, plain writes suffice
  ring.CommitWrite();
}

// consumer process
Sample out{};
if (ring.TryPop(out)) { /* use out */ }
```

### 2.2 `ShmSpmcRing` / `SpmcReader`

`include/thyme/runtime/shm_spmc_ring.h`

```cpp
template <typename T, std::size_t Cap>
class ShmSpmcRing;

template <typename T, std::size_t Cap>
class SpmcReader final : public Waitable;

enum class SpmcRead { Follow, Latest };
```

A single-writer, multi-reader shared-memory broadcast ring. The page version is
`kShmSpmcRingVersion == 4` with magic `"THYMSPM1"`; v4 likewise starts with a 64 B
`ShmSuperHeader` (`cap`@64, `head`@128, `epoch`@192, `reader_drop_slots`@256,
payload@320), `layout_hash` binds T/Cap and the offsets, and v3 pages are rejected.
`Push` **never fails**: when full it overwrites the oldest slot. Each slot is
`{std::atomic<uint64_t> seq; T value;}`, and `seq == 0` means "write in progress /
hole": a reader that sees 0 does not read the payload but skips according to the
follow/latest rule.

| Member | Signature | Notes |
|---|---|---|
| `Prepare` / `Create` / `Open` / `Close` | same as `ShmSpscRing` (`Open` takes `timeout_ns`) | rejected when descriptor-page version/hash/page size/cap do not match |
| `PushWithResult` | `PushResult PushWithResult(const T& v)` | same publish path as `Push`; `ok` is always true and `overwritten` is the number of old slots overwritten this time (0/1), not the drop each reader has accounted for |
| `Push` | `void Push(const T& v)` | compatibility wrapper around `PushWithResult`; returns no status |
| `BeginWrite` | `T* BeginWrite()` | marks the slot `seq = 0` and returns the payload; the writer fills it with plain writes |
| `CommitWrite` | `void CommitWrite()` | `seq = index + 1`, release-stores `head`, optional record tap, `Notify()` |
| `AbortWrite` | `void AbortWrite()` | no-op; the slot keeps `seq == 0` (its previous contents are sacrificed) |
| `AddNotifyTarget` | `bool AddNotifyTarget(Waitable* t)` | in-process notify registration; capped at `kAbiMaxSpmcReaders` (8), returns `false` when full or on a null pointer |
| `layout` / `head` / `epoch` | `Layout* layout() const` and friends | for use by an in-process `SpmcReader` |

`SpmcReader::Bind(ShmSpmcRing<T, Cap>* ring, SpmcRead mode)` binds a cursor;
`SpmcRead::Follow` starts at 0 and chases every item (overwritten ones count as drops),
while `SpmcRead::Latest` takes only the newest item and does not count ordinary skips
of older values as drops. The optional out-parameters of `TryPop(T&,
dropped_since_last)` / `ReadLatest(T&, dropped_since_last)` report the drops added since
the previous call; the default `nullptr` takes the fast path with no delta snapshot. A
failed seqlock re-check is retried within a budget of `kSpmcReadRetries == 16`, after
which the call returns `false` and never hands out a torn payload. `drop_count()`,
`retry_count()`, `torn_count()` and `waiter_count()` are diagnostic counters.
Reader drops accumulate both in the SHM `reader_drop_slots[reader_slot()]` and in the
page-level `reader_drops` (v3). Slots are allocated from a shared bitmap across
wrappers/processes and are not reused for the lifetime of the page; probe and exporter
expose both per-reader and totals, and `overwritten` must never be added into them.
`CreditSpmcReaderDrops(reader, health, seen)` books the delta as a full 64-bit value
into the consumer `HealthSlot`, which status/probe/exporter expose per edge.
`HealthSlot::drop_cnt` is a precise 64-bit counter (SharedHealthPage v3): deltas above
`2^32` are no longer truncated or saturated, and only `2^64` wraps. `Bind` returns
`false` and leaves the reader unbound when the registry is full.

`AbortWrite` differs from SPSC: an overwritten old frame cannot be restored (the writer
has already touched the payload, and restoring `seq` would resurrect torn bytes), so a
follow reader counts it as one drop instead of reading half a frame.

```cpp
#include "thyme/runtime/shm_spmc_ring.h"

// writer
thyme::ShmSpmcRing<Sample, 64> ring;
ring.Create("/thyme_broadcast");
Sample* slot = ring.BeginWrite();
*slot = sample;
ring.CommitWrite();          // overwrites the oldest slot, never blocks

// reader
thyme::SpmcReader<Sample, 64> reader;
reader.Bind(&ring, thyme::SpmcRead::Follow);
Sample out{};
while (reader.TryPop(out)) { /* ... */ }
```

## 3. `LatestSlot`

`include/thyme/runtime/latest_slot.h`

```cpp
template <typename T>
struct LatestSlot;
```

A single-writer, double-buffered seqlock latest-value slot that can live in-process or
as a whole page in shared memory. `T` must be trivially copyable. The in-process body
layout is layout v2; the v3 SHM page is a 64 B `ShmSuperHeader` + 64 B
`LatestSlotPageGeometry` + the original body, with page version
`kLatestSlotPageAbiVersion == kLatestSlotLayoutVersion == 3` and a read retry budget of
`kLatestSlotReadRetries == 16`.

| Member | Signature | Notes |
|---|---|---|
| `Write` | `void Write(const T& v)` | legacy interface; `AtomicPayloadCopyIn` + `CommitWrite`, new code uses the in-place path |
| `BeginWrite` | `T* BeginWrite()` | opens the write critical section (`seq_` becomes odd) and returns the inactive slot; the writer fills it with plain writes |
| `CommitWrite` | `void CommitWrite()` | publishes the new slot (`published_idx_`), `seq_` becomes even |
| `AbortWrite` | `void AbortWrite()` | closes the critical section without flipping `published_idx_`; readers keep seeing the old value |
| `Read` | `bool Read(T& out, LatestSlotReadStats* stats = nullptr) const` | consistent snapshot; returns `false` when the retry budget is exhausted |
| `ReadIfNew` | `bool ReadIfNew(uint64_t& seen, T& out, LatestSlotReadStats* stats = nullptr) const` | reads only when `seq_` differs from `seen`; updates `seen` on success |
| `HasNew` | `bool HasNew(uint64_t seen) const` | compares `seq_` only, never samples the payload |
| `SetRecordTap` | `void SetRecordTap(RecordMirrorRingPage*, uint16_t struct_id)` | writer-process-local tap; cross-process readers must not dereference it |

`seq_` is a `uint64_t`: 0 means "never written", odd means a write is in progress, even
means published. `Read` / `ReadIfNew` retry while `seq_` is odd and re-check `seq_`
after copying; only two matching reads return success. When the retries run out they
return `false` and **never** return a torn value. The payload copy uses
`detail::AtomicPayloadCopyOut`, so a writer filling the inactive slot while a reader
copies is race-free (see ADR 0001).

Side effect of `AbortWrite`: `seq_` advanced but `published_idx_` did not, so
`ReadIfNew` may report **the same old value** again. Callers should be idempotent on
"did the value change", not on "did I receive an event".

ABI note: the in-process body has no in-page version word; its layout is pinned by the
`static_assert`s in `detail::LatestSlotLayout<T>`. A v3 SHM page carries a
`ShmSuperHeader` (magic/version/layout_hash/page_bytes/feature_bits) and a geometry
block (`payload_bytes`/`cell_stride`/`slot_count`), and `CreateLatestSlotPage` /
`OpenLatestSlotPage*` together with `LatestSlotLayoutModel<T>` are the only
create/open/decode path; `payload_layout_hash` must be non-zero and is taken by the
generator from the contract layout. Probe, supervisor and roles share the same
model/header; bare v2 body pages are rejected with no dual read (see
`docs/reference/abi-compatibility.md`).

```cpp
#include "thyme/runtime/latest_slot.h"

thyme::LatestSlot<VehicleState> slot;

// writer
VehicleState* w = slot.BeginWrite();
*w = state;                  // plain writes
slot.CommitWrite();

// reader
thyme::LatestSlotReadStats stats;
VehicleState snapshot{};
if (slot.Read(snapshot, &stats)) {
  // consistent snapshot; stats.torn > 0 means an attempt was interrupted by a publish
}
```

## 3.1 `PinnedLatestStore`

`include/thyme/runtime/pinned_latest_store.h`

```cpp
template <typename T, std::size_t Slots = 4>
class PinnedLatestStore;
```

An in-process, zero-copy latest-value primitive for large payloads: one writer, any
number of readers. Unlike `LatestSlot`, readers get a `Borrowed<T>` (an RAII pin) and
never copy the payload; the writer may only fill a slot that is neither pinned nor the
current latest, and when every slot is taken the publish is dropped and counted
instead of overwriting data under a reader.

| Member | Signature | Notes |
|---|---|---|
| `BeginWrite` | `T* BeginWrite()` | takes an unpinned, non-latest slot; returns `nullptr` and bumps `dropped_publishes` when none is available |
| `CommitWrite` | `void CommitWrite()` | release-stores the new latest; readers cannot see a half-written payload before the writer commits |
| `AbortWrite` | `void AbortWrite()` | abandons this write without changing the latest |
| `Publish` | `bool Publish(const T& v)` | wrapper around `BeginWrite` + assignment + `CommitWrite` |
| `TryRead` | `bool TryRead(Borrowed* out)` | pins the latest slot and returns a view; returns `false` when there is no value or the retry budget is exhausted |
| `Borrowed` | move-only, RAII | `get()`/`operator->` read the payload; destruction or `Reset()` releases the pin |
| `dropped_publishes` / `read_retries` | counters | publishes dropped on slot exhaustion, reader retry-budget exhaustion |

`Slots >= 2`, and the slots are allocated inline in the object (fixed at construction,
zero allocation on the hot path). `T` still has to be trivially copyable and standard
layout; the writer never reuses a slot while a `Borrowed` is alive. The single writer
must serialize its `BeginWrite/CommitWrite` calls. This is a **process-local heap
primitive**: it creates no SHM page and has no `ProducerGate`/futex/timeout steal; use
the `PinnedSlotWriter` / `PinnedSlotReader` lease when you need cross-process access or
crash reclamation.

```cpp
#include "thyme/runtime/pinned_latest_store.h"

thyme::PinnedLatestStore<PointCloud, 4> latest;

if (!latest.Publish(cloud)) {
  // every slot is pinned by readers: drop this publish, per latest-value semantics
}

thyme::PinnedLatestStore<PointCloud, 4>::Borrowed view;
if (latest.TryRead(&view)) {
  use(view->points);  // zero copy; the pin is released when view is destroyed
}
```

## 4. `ObjectPool`

`include/thyme/runtime/object_pool.h`

```cpp
template <typename T, std::size_t N, bool kFreeList = false>
class ObjectPool;
```

A fixed-capacity, cacheline-isolated slot pool whose lifetime is
`FREE -> WRITING -> READY -> FREE`. `acquire()` hands out an exclusive index, the owner
fills it and calls `publish()`, and the consumer calls `release()` with the generation
it saw at acquire time. A generation mismatch rejects a stale release (a double
release, a release after abort, or a release by a previous owner).

| Member | Signature | Notes |
|---|---|---|
| `kInvalidIndex` | `static constexpr uint32_t` = `0xFFFFFFFF` | returned when the pool is full |
| `acquire` | `uint32_t acquire()` | CAS one FREE slot; `kInvalidIndex` when full |
| `publish` | `void publish(uint32_t idx)` | `WRITING -> READY` |
| `abort_write` | `void abort_write(uint32_t idx)` | `WRITING -> FREE`, without rolling back the generation |
| `generation` | `uint32_t generation(uint32_t idx) const` | current generation |
| `release` | `bool release(uint32_t idx, uint32_t gen)` | only when the state is READY and the generation matches |
| `at` | `T& at(uint32_t idx)` / `const T& at(uint32_t idx) const` | access the payload |

Two `acquire()` strategies:

- Default `kFreeList == false`: scan up to N slots starting at `cursor_` and CAS the
  first FREE one. Worst case O(N), O(1) when the next position is free.
- Opt-in `kFreeList == true`: pop from a tagged Treiber LIFO stack, and push back on
  `release` / `abort_write`; O(1), at the cost of one extra CAS per release and one
  successor word per slot. `ObjectPool` allocates its slots on the heap, not in an SHM
  page, so this option is per-instantiation and process-local, not part of the ABI.

The generation is a `uint32_t` that increments on every `acquire()` (`abort_write` does
not roll it back). Wrap-around is well-defined unsigned arithmetic; a slot reused at a
steady 50 Hz wraps only after about 2.72 years, far longer than any process lifetime.
`release()` compares the full value and performs no ordering arithmetic.

```cpp
#include "thyme/runtime/object_pool.h"

thyme::ObjectPool<Image, 32> pool;

const uint32_t idx = pool.acquire();
if (idx == thyme::ObjectPool<Image, 32>::kInvalidIndex) {
  // pool full: drop the frame or degrade
} else {
  pool.at(idx) = image;
  pool.publish(idx);
  const uint32_t gen = pool.generation(idx);
  // consumer: pool.release(idx, gen)
}
```

## 5. `NotifPipe`

`include/thyme/runtime/notif_pipe.h`

```cpp
template <typename T, std::size_t Cap>
class NotifPipe : public Waitable;

struct PushWaitGate {
  bool (*aborted)(void* ctx) = nullptr;
  void* ctx = nullptr;
  uint64_t slice_ns = 100000000ULL;  // 100 ms
};
```

An in-process SPSC queue that delivers every item: internally two `SpscRing<Cap>`
instances manage free and ready indexes, the payloads live in a fixed `slots_[Cap]`
array, and nothing is allocated after construction. `Push` never overwrites and returns
`false` when full; the consumer's `TryPop(T&)` moves the payload out and returns the slot
to the free list.

| Member | Signature | Notes |
|---|---|---|
| `Push` | `bool Push(const T& v)` | `false` when full; legacy interface |
| `BeginWrite` | `T* BeginWrite()` | takes a slot from the free list; `nullptr` when full, and the caller records `HealthSlot::AddDrops` instead of spinning |
| `CommitWrite` | `void CommitWrite()` | pushes the slot onto the ready ring and `Notify`s the extra target |
| `AbortWrite` | `void AbortWrite()` | returns the uncommitted slot to the free list |
| `PushOrWait` | `bool PushOrWait(const T& v, const PushWaitGate& gate)` | when full, waits `slice_ns` at a time for a free slot, checking `gate.aborted` before each slice |
| `TryPop` | `bool TryPop(T& out)` | `false` when empty |
| `Depth` / `waiter_count` / `slot_waiter_count` | `uint64_t` / `uint32_t` | published depth / ready waiters / producers blocked in `PushOrWait` |
| `SetNotify` | `void SetNotify(Waitable* target)` | extra notification after every successful push (used by the EventThread wake hub) |

Blocking semantics belong to `PushOrWait` alone, and its comment states explicitly that
it serves the replay injector's cooperative backpressure, not the real-time `Push`
path. `PushOrWait` waits on the free list and is woken when the consumer recycles a
slot; when the abort callback returns `true` it gives up immediately and returns
`false`.

```cpp
#include "thyme/runtime/notif_pipe.h"

thyme::NotifPipe<Command, 64> pipe;

thyme::PushWaitGate gate;
gate.aborted = [](void* ctx) { return *static_cast<bool*>(ctx); };
gate.ctx = &stop_requested;

if (!pipe.PushOrWait(cmd, gate)) {
  // stop/pause: this event was not queued
}

Command out{};
if (pipe.TryPop(out)) { /* ... */ }
```

## 6. `Frame<T>` / `FrameHeader`

`include/thyme/contracts/frame.h`, `include/thyme/contracts/frame_header.h`

`FrameHeader` is a packed 40-byte wire-format header; `Frame<Payload>` is
`{FrameHeader hdr; Payload payload;}`, which requires `Payload` to be trivially
copyable and standard layout. `Frame` itself is **not** packed: if `Payload` has an
alignment greater than 1, the compiler may insert padding between the header and the
payload. That padding is not part of the wire image, and the CRC covers only the
`payload` member.

| Field | Type | Semantics |
|---|---|---|
| `magic` | `uint32_t` | product/wire-format identifier, which must match the reader's expectation exactly |
| `version` | `uint16_t` | wire-format version |
| `size` | `uint16_t` | wire byte count (header + payload span) |
| `session_id` | `uint64_t` | session identifier |
| `mono_ns` | `uint64_t` | producer `CLOCK_MONOTONIC` timestamp |
| `sequence` | `uint32_t` | producer sequence number |
| `valid` | `uint8_t` | **data-valid**: 1 means the payload was fully written and is stable |
| `reserved[3]` | `uint8_t[3]` | `reserved[0]` bit0 = `kFrameFlagCrcDisabled`; every other bit must be 0 |
| `crc32` | `uint32_t` | covers the header with `crc32` zeroed plus the payload |
| `reserved2` | `uint32_t` | explicit tail, must be 0; makes the header 40 B / a multiple of 8 bytes |

| Function | Signature | Notes |
|---|---|---|
| `FrameInit` | `template <typename T> void FrameInit(T& msg, uint32_t seq, uint64_t mono_ns, uint64_t session)` | zeroes the message and fills sequence/mono_ns/session_id/size; `valid` stays 0 |
| `FrameSeal` | `template <typename T> void FrameSeal(T& msg, uint32_t magic, uint16_t version)` | writes magic/version/size, sets `valid = 1` and computes the CRC |
| `FrameSealNoCrc` | `template <typename T> void FrameSealNoCrc(T& msg, uint32_t magic, uint16_t version)` | no-CRC seal: sets the CRC-disable bit, `crc32 = 0`, and still sets `valid = 1` |
| `FrameMarkValid` | `template <typename T> void FrameMarkValid(T& msg)` | no seal, only sets `valid` to 1; use it solely on channels that do not check `FrameHeaderOk` |
| `FrameVerify` | `template <typename T> bool FrameVerify(const T& msg, uint32_t magic, uint16_t version)` | data-boundary validation: structure, `valid == 1`, size, CRC or the CRC-disable contract |

`FrameHeaderOk` rejects `valid != 1` at the data boundary; readers that only inspect
metadata should use `FrameHeaderStructurallyOk`, which checks neither `valid == 1` nor
the CRC. `kFrameFlagCrcDisabled` lets a reader skip the CRC but still requires
`crc32 == 0`; a CRC value that happens to be 0 is not enough to skip validation.

When an external producer builds a `FrameHeader` by hand, the normal path must be
`FrameInit` → write the payload completely → `FrameSeal` / `FrameSealNoCrc`. Use
`FrameMarkValid` only on channels that explicitly do not seal, and even then only after
the payload is fully written; it is not a substitute for the seal/CRC publish step of a
normal channel. `valid > 1` is rejected by structural validation.

Reserved-field validation is always strict: only bit0 of `reserved[0]`
(`kFrameFlagCrcDisabled`) is defined, and the remaining bits of `reserved[0]`,
`reserved[1..2]` and `reserved2` must be 0. There is **no** `--allow-legacy-reserved`
switch in the implementation; do not leave uninitialized bytes in place or expect the
runtime to relax validation. Recordings made with the old 36 B header must be
re-recorded. The authoritative contract is the `FrameHeader.valid` section and rules
3/4 in [`abi-compatibility.md`](../reference/abi-compatibility.md); the external
upgrade checklist is in [`upgrade.md`](../guides/upgrade.md).

Size limits:

- `hdr.size` is a `uint16_t`, so a `Frame` is capped at 64 KB on the wire, and
  `FrameInit` rejects oversized types at compile time with
  `static_assert(FramePodWireBytes<T> <= 0xFFFFu)`. Larger contracts (perception maps,
  for example) go through the pinned-slot channel instead of a graph edge.
- 512 KB (`kAbiMaxPayloadBytes == 524288`) is the limit of the **record layer** (mirror
  ring / `thyme_bin`), not of `Frame` transport.

```cpp
#include "thyme/contracts/frame.h"

thyme::Frame<VehicleState> msg;
thyme::FrameInit(msg, sequence++, mono_ns, session_id);
msg.payload = state;
thyme::FrameSeal(msg, kProductMagic, kProductVersion);  // sets valid + CRC
if (!thyme::FrameVerify(msg, kProductMagic, kProductVersion)) {
  // reject at the data boundary
}
```

## 7. `PinnedSlotWriter` / `PinnedSlotReader` and the lease

`include/thyme/runtime/pinned_slot_store.h`

`PinnedSlotWriter` and `PinnedSlotReader` are the two ends of a payload-agnostic
large-frame shared-memory channel; `slot_bytes`, `slot_count` and `meta_blob_bytes` are
runtime arguments to `Create`. The channel page version is
`kPinnedSlotStoreVersion == 4` (super-header + producer gate) and the lease table
version is `kPinnedSlotLeaseVersion == 6`. The lease table lives in a **separate**
shared-memory object, `"<name>.lease"`: readers map the payload region read-only and the
lease region read-write.

### 7.1 Writer

| Member | Signature / default | Notes |
|---|---|---|
| `Create` | `bool Create(const char* name, uint32_t slot_bytes, uint32_t slot_count, uint32_t meta_blob_bytes, uint64_t steal_timeout_ns = kPinnedSlotLeaseDefaultStealTimeoutNs, bool force_replace = false)` | `steal_timeout_ns == 0` is rejected; with a live gate even `force_replace` returns `false` |
| `CreateStatus` | `kOk` / `kInvalidArgument` / `kCreateFailed` / `kGated` / `kIncompatible` | read through `create_status()`, with details in `last_error()` |
| `RegionBytes` | `static std::size_t RegionBytes(uint32_t slot_bytes, uint32_t slot_count, uint32_t meta_blob_bytes)` | channel region size; `LeaseRegionBytes(slot_count)` is the lease region size |
| `Begin` | `BeginView Begin(uint32_t data_len)` | returns `{payload, header, status}` |
| `BeginStatus` | `kOk` / `kOversize` / `kPinnedSkip` / `kGated` | the three reasons for an empty payload must be told apart |
| `Commit` | `void Commit()` / `void Commit(uint64_t mono_ns)` | writes the footer first (magic last), then publishes the seqlock / `head` / futex `FutexWakeAll` |
| `SetCrcEnabled` | `void SetCrcEnabled(bool enabled)` | off by default (the whole-package manifest is the integrity boundary); when enabled, every frame carries a `Crc32` |
| Counters | `dropped_oversize()` / `pinned_skip_count()` / `gate_rejects()` / `gate_steals()` / `gate_aborted()` | for observability |

`Begin` runs: gate check → length check → `slot.seq = 0` (write in progress) → pin
check. When the target slot is pinned by a lease, the writer **skips** that slot:
`head` advances, the frame is not published, readers see a hole, and `pinned_skip`
increments; the writer never waits for or overwrites a lease. There is no explicit
abort API after `Begin` — not calling `Commit` leaves `seq == 0`, so readers never see
half a frame.

`kPinnedSlotLeaseDefaultStealTimeoutNs` defaults to 5 s. A pin left behind by a dead or
killed reader is reclaimed by `StealIfExpired` after the timeout: `pin_gen`'s
generation increments and the pin is cleared, and the old lease's `Release` is skipped
because the generation no longer matches.

The `View::CopyPayloadTo` path first sets copy-active with the same `pin_gen` CAS while
the lease is valid, then copies with a plain `memcpy`; when it finishes it clears
active and keeps the pin/gen. Bit31 is not part of the pin count, and both the
acquire/release pin±1 updates preserve active. An expired active flag cannot be taken
by time alone: the writer reclaims it only when `copy_owner_pid/copy_owner_start` prove
the copier is dead; a live copier keeps being skipped. When the owner identity is
unavailable, or another copier already holds active, it falls back to an atomic copy
only while the lease is still valid; a stale generation is rejected outright and
nothing is copied. The non-lease path still uses an atomic copy.

### 7.2 Reader and `Lease`

| Member | Signature | Notes |
|---|---|---|
| `Open` | `bool Open(const char* name, uint64_t timeout_ns, bool lease_read_only = false)` | probes the header read-only for the geometry, then remaps at the real size; the lease table is RW by default |
| `Next` / `NextLease` | `BorrowedFrame Next(bool verify_crc = true)` | advances the follow cursor one step; verifies under the pin first, then advances |
| `Acquire` | `Lease Acquire(uint64_t frame_index, ReadResult* status = nullptr, bool verify_crc = true)` | random access; holds a pin only on success |
| `AcquireLease` | `ReadResult AcquireLease(uint64_t frame_index, View& out, uint32_t* gen_out, bool verify_crc = true)` | low-level variant that outputs the generation |
| `ReleaseLeaseStatus` | `LeaseReleaseResult ReleaseLeaseStatus(uint64_t frame_index, uint32_t gen)` | unpins only when the generation matches |
| `ReadFrame` | `ReadResult ReadFrame(uint64_t frame_index, View& out, bool verify_crc = true)` | does not move the cursor |
| `ReadResult` | `kOk` / `kNotPublished` / `kOverrun` / `kTorn` | `kOverrun` is an evicted hole, `kTorn` a seqlock/footer/CRC mismatch |
| `LeaseReleaseResult` | `kOk` / `kAlreadyReleased` / `kStolen` | `kStolen` means the writer stole the slot while it was held |

`Lease` is an RAII guard. `valid()` says the acquire succeeded (a pin was once held);
`still_valid()` is the actual liveness check, returning `false` once the generation no
longer matches or the pin is 0, so long operations must re-check it. `held_ns()` /
`deadline_ns()` come from the `steal_timeout_ns` cached at acquire time, and the
deadline is conservative. `view().payload` is only safe while `still_valid()` holds;
to keep bytes beyond the lease you must call `CopyPayloadTo()` into your own buffer
while the lease is still valid. A live `Lease` must be destroyed or `Reset()` before
the reader `Close()`; debug builds abort otherwise.

### 7.3 Producer gate

The gate is embedded in the channel header (`ProducerGate`, occupying its own
cacheline), and every writer goes through `Begin`/`Commit`, so gating lives on the core
write path instead of being reimplemented by each driver.

| Member | Signature / default | Notes |
|---|---|---|
| `AcquireProducerGateWithNewToken` | `GateResult AcquireProducerGateWithNewToken(uint64_t timeout_ns = kProducerGateDefaultTimeoutNs)` | 2 s by default; generates a unique token |
| `AcquireProducerGate` | `GateResult AcquireProducerGate(uint64_t token, uint64_t timeout_ns = kProducerGateDefaultTimeoutNs)` | a second injector on the same writer object with a different token gets `kBusy` |
| `ReleaseProducerGate` | `GateResult ReleaseProducerGate()` / `(uint64_t token)` | only the holder can release; a non-holder gets `kNotHeld` |
| `HeartbeatProducerGate` | `bool HeartbeatProducerGate()` / `(uint64_t token)` | a paused replay must keep heartbeating (interval `kProducerGateHeartbeatIntervalNs` = 250 ms) |
| `owns_producer_gate` / `producer_gate_held_by_other` | `bool` | observability |

`GateResult` values: `kOk` / `kBusy` / `kStolen` / `kNotHeld` / `kInvalidArgument`.
What a writer observes:

- Someone else holds the gate: `Begin()` returns `BeginStatus::kGated`, payload/header
  are `nullptr`, neither slot nor head is touched, and `gate.rejects` increments.
- `Begin()` succeeded but the gate was taken before `Commit()`: `Commit()` publishes
  nothing, the slot keeps `seq == 0`, and `gate_aborted` increments.
- The holder crashes or stops heartbeating: the next writer reclaims the gate in
  `Begin()` (immediately if `/proc` proves the process is dead, otherwise after
  `timeout_ns`), and `gate.steals` increments.

```cpp
#include "thyme/runtime/pinned_slot_store.h"

thyme::PinnedSlotWriter writer;
writer.Create("/h264_cam0", 256 * 1024, 32, 4096);  // 256 KB x 32 slots

const auto bv = writer.Begin(frame_len);
if (bv.status == thyme::PinnedSlotWriter::BeginStatus::kOk) {
  std::memcpy(bv.payload, frame, frame_len);
  bv.header->flags = is_idr ? 1u : 0u;
  writer.Commit(mono_ns);
} else if (bv.status ==
           thyme::PinnedSlotWriter::BeginStatus::kPinnedSkip) {
  // the frame is pinned by a reader: skip and count, do not roll head back
} else if (bv.status == thyme::PinnedSlotWriter::BeginStatus::kGated) {
  // replay owns the channel: the real producer steps aside
}
```

Reader side:

```cpp
thyme::PinnedSlotReader reader;
reader.Open("/h264_cam0", 2000000000ull);

while (true) {
  auto frame = reader.Next();
  if (!frame.ok()) break;                 // kEmpty / kLost / kTorn
  if (frame.still_valid()) {
    // frame.data()/size() are valid while still_valid() holds
  }
  const auto rr = frame.Release();        // kOk / kStolen / kAlreadyReleased
  (void)rr;
}
```

## 8. Choosing a primitive

| Primitive | Typical use | Cost / limits |
|---|---|---|
| `SpscRing` | passing indexes/handles between threads with the payload in an external pool | drop-oldest; moves only `(index, generation)`; single producer, single consumer |
| `ShmSpscRing` | cross-process delivery of every small struct | fails when full and counts `push_fail`; `T` must be trivially copyable; stored by value |
| `ShmSpmcRing` / `SpmcReader` | broadcasting one data item to several processes | the writer never blocks but overwrites; follow readers may lose frames; at most 8 registered readers |
| `LatestSlot` | state mirroring, last-value semantics (vehicle state, for example) | single writer; the in-process body has no version word, v3 SHM pages carry version + geometry; returns `false` when retries run out |
| `ObjectPool` | payload slots that need an explicit lifetime | O(N) scan by default; u32 generation; slots on the heap |
| `NotifPipe` | in-process event queues that must deliver every item | `false` when full; `PushOrWait` is for replay backpressure only; single consumer |
| `Frame<T>` | wire format for small messages on graph edges | 64 KB cap; one CRC per frame; `Frame` is not packed |
| `PinnedSlotWriter` / `PinnedSlotReader` + `Lease` | MB-scale image/point-cloud/H.264 frames | single writer + per-slot lease; the writer skips pinned slots; readers must manage lease lifetimes |

## Related documents

- [Recording and trigger API](api-record-trigger.md)
- [Replay API](api-replay.md)
- [Probe and observability](probe.md)
- [Performance baselines](perf.md)
