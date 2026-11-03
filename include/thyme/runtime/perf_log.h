#pragma once
#include "thyme/runtime/histogram.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace thyme {

class PerfLogWriter;

// OBS-09: type-6 histogram record (fixed size, one record per block).
// Layout (little-endian):
//   tag u8 (0xA2) | series u8 | buckets u16 (must equal
//   LatencyHistogram::kBuckets) | count u64 | max_ns u64 |
//   end_ts u32 (2^14 ns units, absolute axis) | bucket counts u32[kBuckets]
// The series byte labels which sampler produced the histogram (see
// PerfSampler::kHistSeries*). Bucket counts are per-window; merging across
// processes is bucket-wise addition, while max needs a max.
inline constexpr uint8_t kPerfHistogramBlockType = 6;
inline constexpr uint8_t kPerfHistogramRecordTag = 0xA2;
inline constexpr std::size_t kPerfHistRecordBytes =
    1 + 1 + 2 + 8 + 8 + 4 + LatencyHistogram::kBuckets * sizeof(uint32_t);

// OBS-04: upper bound on type-5 (ProcSampler) records covered by one writer
// flush; ProcSampler's ring is exactly this size. Lives here because the
// writer budget below must account for it (proc_sampler.h includes this
// header, not the other way round).
inline constexpr std::size_t kProcSeriesMaxRecords = 1024;

// ABI-10: record-manifest block (type 7), written once per perf file
// generation right after the magic (PerfLogDumper writes it on the first dump
// that lands; rotation re-emits it). It pins the GEN-12 recorded-channel set
// the samples were produced against: RecordStructId is a row index into the
// sorted recorded-channel list, so an analyser must refuse to join a log
// against a different manifest.
//
// Wire record (12 B, one per block, little-endian):
//   manifest_version u16 | reserved u16 (0) | manifest_hash u64
// The block header's node byte is 0: the manifest is build-wide, not per node.
// Parsers that predate type 7 stop at it like any unknown block ("log
// truncated at an unknown block"); this build's analysers consume it.
inline constexpr uint8_t kPerfManifestBlockType = 7;
inline constexpr std::size_t kPerfManifestRecordBytes = 12;

// OBS-07-C: additive metadata. Type 8 maps type-5 series ids to stable names;
// type 9 carries the declared node period and dispatch deadline in ns.
inline constexpr uint8_t kPerfSeriesNameBlockType = 8;
inline constexpr std::size_t kPerfSeriesNameRecordBytes = 32;
inline constexpr std::size_t kPerfSeriesNameBytes = 31;
inline constexpr uint8_t kPerfNodeBudgetBlockType = 9;
inline constexpr std::size_t kPerfNodeBudgetRecordBytes = 16;

// ARCH-15: additive file-wide CLOCK_MONOTONIC / CLOCK_REALTIME anchor. One
// 16-byte record per block, emitted with every non-empty periodic dump.
inline constexpr uint8_t kPerfClockAnchorBlockType = 10;
inline constexpr std::size_t kPerfClockAnchorRecordBytes = 16;

// OBS-03: ring capacity is derived from the declared/expected sample rate and
// the dump window: capacity = rate_hz * window_s * 3/2 (margin), rounded up
// to a power of two, clamped to [kPerfRingDefaultCap, kPerfRingMaxCap].
// kPerfRingMaxCap caps one PerfSampler ring at 262144 samples ~= 3MB (12B
// samples) so even a maxed-out ring stays well inside the §3.23 default perf
// ring budget (<= 32MB per process covers ~3 fully maxed threads with
// latency+edge+work rings each). Hosts wiring a PerfLogDumper with a
// non-default interval should size samplers with the same window.
inline constexpr std::size_t kPerfRingDefaultCap = 4096;
inline constexpr std::size_t kPerfRingMaxCap = 1 << 18;
inline constexpr uint64_t kPerfRingDefaultWindowNs = 10000000000ull;  // 10 s

// Declared rate -> ring capacity (OBS-03). rate_hz is the expected sample
// rate (node period_hz for cyclic loops, expected_hz message rate for event
// nodes); window_ns is the periodic-dump interval the ring must survive
// without overwriting unflushed samples. Inputs are clamped (rate <= 1MHz,
// window <= 1h) so the product cannot overflow; the result is always a power
// of two in [kPerfRingDefaultCap, kPerfRingMaxCap].
inline std::size_t PerfRingCapForRate(uint64_t rate_hz, uint64_t window_ns) {
  if (rate_hz > (1ull << 20)) rate_hz = 1ull << 20;
  if (window_ns > 3600000000000ull) window_ns = 3600000000000ull;
  const uint64_t want = (rate_hz * window_ns / 1000000000ull) * 3 / 2;
  std::size_t cap = kPerfRingDefaultCap;
  while (cap < want && cap < kPerfRingMaxCap) cap <<= 1;
  return cap;
}

// Perf sample log: compact per-sample latency recording (10B wire records).
//
// Wire format (all little-endian):
//   file magic: "T9P1" (4B)
//   block header: type u8 (1 = samples, 2 = percentile, 3 = work) |
//                 node u8 | count u32
//   sample record (10B): ts u32 (16.384us units = 2^14 ns, ABSOLUTE
//                       CLOCK_MONOTONIC >> 14 truncated to u32 — one global
//                       time axis across all processes/files with no
//                       per-sampler base; wraps at ~19.5h uptime, so rotate
//                       log files well before that) | lat u24 (8.192us units
//                       = 2^13 ns, clamped at 0xFFFFFF ~= 137s) | id u8 |
//                       io u8 (hi = out frame nibble, lo = in frame nibble;
//                       consume taps carry the consumed frame's nibble as
//                       the frame-join key) | rsvd u8 (0)
//   percentile record (20B, one per type-2 block): tag 0xA1 | node u8 |
//                       n u16 | p50/p95/p99/max 4 x u24 (8.192us units) |
//                       window_end_ts u32 (16.384us units)
//   work block (type 3): 10B records — ts u32 (16.384us units, absolute as
//                       above) | dur u24 (1.024us units = 2^10 ns, clamped) |
//                       in/out frame nibbles u8 (hi=out, lo=in, mod-16
//                       sequence) | node u8 | flags u8 (bit0: batch spans
//                       multiple input frames; bit1: produced output; bit2:
//                       consumed input — validity lives in flags because
//                       nibble 0 is a legitimate frame, io == 0 does not mean
//                       "no attribution")
//   sys block (type 5, OBS-04): 10B records — ts u32 (same absolute axis) |
//                       value u32 (clamped) | series u8 | rsvd u8 (0).
//                       Series 0..15 are process counters sampled off the hot
//                       path by ProcSampler (see proc_sampler.h) and carry
//                       per-period DELTAS; series >= 16 are registered
//                       instant-value probes: queue depths (OBS-04) and, from
//                       SCH-08, overrun-reason counters registered via
//                       EventThread::RegisterObservability. Parsers that
//                       predate type 5 stop at it like any unknown block.
//   histogram block (type 6, OBS-09): one fixed-size record per block
//                       (kPerfHistRecordBytes = 1,340 B) — tag 0xA2 u8 |
//                       series u8 | buckets u16 | count u64 | max_ns u64 |
//                       end_ts u32 (2^14 ns units, absolute) |
//                       bucket counts u32[LatencyHistogram::kBuckets].
//                       Written once per Flush/FlushNew when the sampler
//                       recorded samples since the last Reset; percentiles
//                       are recomputed from the buckets (see
//                       include/thyme/runtime/histogram.h for the layout and
//                       the honest bucket-resolution error).
//   series-name block (type 8): series u8 | name char[31], zero padded.
//   node-budget block (type 9): period_ns u64 | dispatch_deadline_ns u64.
//   clock-anchor block (type 10, ARCH-15): mono_ns u64 | realtime_ns u64.
//                       The header node byte is 0 because the anchor describes
//                       the whole file, not one node. Full-width timestamps
//                       avoid the sample timestamp's u32 wrap and day boundary.
//
// In-memory the sampler keeps a ring of the last capacity() samples as 12B
// {ts_units, lat_units, id, io} entries; Flush writes one sample block per
// id plus one percentile block per id, then one aggregate type-6 histogram
// block (OBS-09, see histogram.h: 1,332 B per sampler, bucket-resolution
// p50/p90/p99/p999 plus exact max). The ring is allocated once at
// construction (capacity is fixed for the object's lifetime); Note() never
// allocates.
class PerfSampler {
 public:
  static constexpr std::size_t kCap = kPerfRingDefaultCap;  // default ring
  static constexpr std::size_t kMaxCap = kPerfRingMaxCap;
  static constexpr uint32_t kMaxLatUnits = 0xFFFFFF;  // 2^13 ns * 2^24 ~= 137s
  static constexpr uint32_t kTsShift = 14;  // 16.384us per unit
  static constexpr uint32_t kLatShift = 13;  // 8.192us per unit

  // OBS-09 histogram series labels (type-6 record `series` byte). The
  // histogram is per sampler, i.e. aggregate over sample ids: per-edge
  // percentiles stay in the type-2 blocks, not here.
  static constexpr uint8_t kHistSeriesLatency = 0;       // node consume latency
  static constexpr uint8_t kHistSeriesEdgeLatency = 1;   // per-edge latency
  static constexpr uint8_t kHistSeriesCallbackWork = 2;  // SCH-08 callback work

  // OBS-03: capacity is rounded up to a power of two and clamped to
  // [1, kMaxCap]; the ring is heap-allocated HERE (construction), never on
  // the Note() hot path. On allocation failure the sampler degrades to a
  // no-op (capacity() == 0) instead of throwing (core is -fno-exceptions).
  // hist_series labels the OBS-09 histogram for the offline analyzer.
  explicit PerfSampler(std::size_t capacity = kCap,
                       uint8_t hist_series = kHistSeriesLatency)
      : hist_series_(hist_series) {
    cap_ = ClampPow2(capacity);
    if (cap_ != 0) {
      samples_ = std::unique_ptr<Sample[]>(new (std::nothrow) Sample[cap_]{});
    }
    if (samples_ == nullptr) cap_ = 0;
    mask_ = cap_ - 1;
  }
  ~PerfSampler() = default;
  PerfSampler(const PerfSampler&) = delete;
  PerfSampler& operator=(const PerfSampler&) = delete;

  // OBS-03 sizing helper (see PerfRingCapForRate).
  static std::size_t CapacityForRate(uint64_t rate_hz, uint64_t window_ns) {
    return PerfRingCapForRate(rate_hz, window_ns);
  }

  std::size_t capacity() const { return cap_; }

  uint8_t hist_series() const { return hist_series_; }
  void SetHistogramSeries(uint8_t series) { hist_series_ = series; }
  const LatencyHistogram& histogram() const { return hist_; }
  LatencyHistogram& histogram() { return hist_; }

  // Drops all retained samples and the FlushNew watermark, and clears the
  // OBS-09 histogram. Used by the dump-then-reset contract (OBS-03): after a
  // periodic dump each ring holds only the current (not yet dumped) window.
  // Like the ring, the histogram reset is a dump-path operation and must not
  // race with Note() producers — a sample landing in the boundary window may
  // be lost (see histogram.h).
  void Reset() {
    idx_ = 0;
    cnt_ = 0;
    flushed_ = 0;
    hist_.Reset();
  }

  // id = kNoId means "no channel attribution"; Flush substitutes its
  // node-level default for such samples. 0xFF is reserved so that EVERY
  // RecordStructId (including 0) stays a valid explicit edge id — the
  // substitution can never mislabel an edge sample.
  static constexpr uint8_t kNoId = 0xFF;

  // Record one sample: ts_ns is the consume timestamp (CLOCK_MONOTONIC),
  // lat_ns the measured latency. id is the sample's channel/edge id (kNoId =
  // none; Flush substitutes its node-level default). io carries the frame
  // nibble(s) for frame-join analysis (0 = unattributed). OBS-09: also one
  // relaxed atomic histogram bucket increment (raw ns, finer than the 2^13 ns
  // wire unit), ~a few ns.
  void Note(uint64_t ts_ns, uint64_t lat_ns, uint8_t id = kNoId,
            uint8_t io = 0) {
    if (cap_ == 0) return;  // degraded (allocation failed at construction)
    hist_.Record(lat_ns);
    const uint32_t ts = static_cast<uint32_t>(ts_ns >> kTsShift);
    const uint64_t lat_units64 = lat_ns >> kLatShift;
    const uint32_t lat = lat_units64 > kMaxLatUnits
                             ? kMaxLatUnits
                             : static_cast<uint32_t>(lat_units64);
    Sample& s = samples_[idx_ & mask_];
    s.ts = ts;
    s.lat = lat;
    s.id = id;
    s.io = io;
    ++idx_;
    if (cnt_ < cap_) ++cnt_;
  }

  std::size_t count() const { return cnt_; }

  // Appends the file magic (on first use) and this node's blocks to w.
  void Flush(PerfLogWriter& w, uint8_t node_id) const;

  // Periodic-dump variant (crash-safe perf, design review, option 1): writes only
  // samples not covered by a previous FlushNew and advances the watermark.
  // Samples evicted from the ring before being flushed are silently gone —
  // size the ring (CapacityForRate) so idx_ never laps flushed_ by more than
  // capacity() between dumps.
  void FlushNew(PerfLogWriter& w, uint8_t node_id);

  // A sample ts is a u32 (mono>>14) and wraps every ~19.5h; the periodic dump
  // file carries the wrap base in an epoch block (wire format unchanged). See
  // PerfLogDumper / the parser in eval_chains.cpp.
  static constexpr uint8_t kEpochBlockType = 4;
  // Payload of one type-4 block: the u64 wrap base. Budgeted explicitly by
  // PerfLogWriter (a missing reservation would abort, not truncate).
  inline static constexpr std::size_t kEpochRecordBytes = 8;

 private:
  void FlushRange(PerfLogWriter& w, uint8_t node_id, uint64_t begin,
                  uint64_t end) const;

  static std::size_t ClampPow2(std::size_t capacity) {
    if (capacity == 0) return 0;
    if (capacity > kMaxCap) capacity = kMaxCap;
    std::size_t cap = 1;
    while (cap < capacity) cap <<= 1;
    return cap;
  }

  struct Sample {
    uint32_t ts;
    uint32_t lat;
    uint8_t id;
    uint8_t io;
    uint8_t pad[2];
  };
  // The dump writes ts/lat/id/io in this order as a 10 B record, so the
  // in-memory ring record and the pad are part of the perf-file ABI (the file
  // has no per-record header describing the layout).
  static_assert(sizeof(Sample) == 12,
                "10 B wire record + 2 B pad; the field order above is the "
                "wire order written by Flush");
  static_assert(offsetof(Sample, lat) == 4, "wire order: ts, lat");
  static_assert(offsetof(Sample, id) == 8, "wire order: ts, lat, id");
  static_assert(offsetof(Sample, io) == 9, "wire order: ts, lat, id, io");
  std::unique_ptr<Sample[]> samples_;
  LatencyHistogram hist_;  // OBS-09 aggregate histogram (1,332 B, no alloc)
  uint8_t hist_series_ = kHistSeriesLatency;
  std::size_t cap_ = 0;
  std::size_t mask_ = 0;
  uint64_t idx_{0};
  std::size_t cnt_{0};
  uint64_t flushed_{0};  // FlushNew watermark (absolute sample index)
};

// Grow-free writer: the buffer is sized at construction for the exact worst
// case of the rings it will flush (magic + sample_cap 10B samples + work_cap
// 10B work records + per-id block headers for all 256 ids + one type-5 sys
// block + up to four OBS-09 type-6 histogram blocks + one type-10 clock
// anchor), and every write goes
// through Reserve, so a future Flush bug aborts instead of corrupting memory.
// The default constructor covers two default-sized (kCap) rings, matching the
// pre-OBS-03 fixed 4096 rings. Construction allocates (dump/shutdown path
// only, never the Note() hot path); on allocation failure ok() is false and
// any write aborts.
class PerfLogWriter {
 public:
  PerfLogWriter() : PerfLogWriter(PerfSampler::kCap, PerfSampler::kCap) {}
  PerfLogWriter(std::size_t sample_cap, std::size_t work_cap)
      : PerfLogWriter(sample_cap, work_cap, 56) {}
  // `name_records` sizes the type-8 block for samplers with more series than
  // the process sampler (the OBS-14 host sampler emits one name per CPU).
  PerfLogWriter(std::size_t sample_cap, std::size_t work_cap,
                std::size_t name_records)
      : buf_(new (std::nothrow)
                 uint8_t[BufSizeFor(sample_cap, work_cap, name_records)]),
        cap_(buf_ != nullptr ? BufSizeFor(sample_cap, work_cap, name_records)
                             : 0) {
    if (buf_ != nullptr) {
      std::memcpy(buf_, "T9P1", 4);
      pos_ = 4;
    }
  }
  ~PerfLogWriter() { delete[] buf_; }
  PerfLogWriter(const PerfLogWriter&) = delete;
  PerfLogWriter& operator=(const PerfLogWriter&) = delete;
  PerfLogWriter(PerfLogWriter&& o) noexcept
      : buf_(o.buf_), cap_(o.cap_), pos_(o.pos_) {
    o.buf_ = nullptr;
    o.cap_ = 0;
    o.pos_ = 0;
  }
  PerfLogWriter& operator=(PerfLogWriter&& o) noexcept {
    if (this != &o) {
      delete[] buf_;
      buf_ = o.buf_;
      cap_ = o.cap_;
      pos_ = o.pos_;
      o.buf_ = nullptr;
      o.cap_ = 0;
      o.pos_ = 0;
    }
    return *this;
  }

  static constexpr std::size_t BufSizeFor(std::size_t sample_cap,
                                          std::size_t work_cap,
                                          std::size_t name_records = 56) {
    // OBS-09: room for four histogram blocks (latency, edge latency,
    // callback work, one spare) in case a host flushes several samplers
    // through one writer before draining it.
    // Series-name block: 16 fixed proc series (ProcSampler series 0..15),
    // up to 8 registered probes, `ext_caps` and the optional OBS-13 sources
    // (17 fixed fields + 8 registered threads) when enabled. Keep in step
    // with ProcSampler::kProbeBase/kMaxProbes and proc_ext.h; FlushMetadata
    // aborts past the budget.
    // Per-dump metadata: manifest, series names, node budget, epoch block and
    // clock anchor each get their own header + payload reservation. The epoch
    // term is small but mandatory: relying on histogram slack is fragile and
    // a shortfall aborts the dump.
    return std::size_t{4} + sample_cap * 10 + work_cap * 10 +
           std::size_t{256} * (6 + 6 + 20) + std::size_t{6} /*work header*/ +
           std::size_t{6} + kProcSeriesMaxRecords * 10 +
           std::size_t{4} * (6 + kPerfHistRecordBytes) +
           std::size_t{6} + kPerfManifestRecordBytes +
           std::size_t{6} + PerfSampler::kEpochRecordBytes +
           std::size_t{6} + name_records * kPerfSeriesNameRecordBytes +
           std::size_t{64} * (6 + kPerfNodeBudgetRecordBytes) +
           std::size_t{6} + kPerfClockAnchorRecordBytes;
  }

  bool ok() const { return buf_ != nullptr; }
  const uint8_t* data() const { return buf_; }
  std::size_t size() const { return pos_; }
  // Rewinds to just after the magic so the writer can be reused for the next
  // periodic dump without reallocating.
  void Rewind() { pos_ = buf_ != nullptr ? 4 : 0; }

  void U8(uint8_t v) {
    Reserve(1);
    buf_[pos_++] = v;
  }
  void U16(uint16_t v) {
    Reserve(2);
    buf_[pos_++] = static_cast<uint8_t>(v);
    buf_[pos_++] = static_cast<uint8_t>(v >> 8);
  }
  void U24(uint32_t v) {
    Reserve(3);
    buf_[pos_++] = static_cast<uint8_t>(v);
    buf_[pos_++] = static_cast<uint8_t>(v >> 8);
    buf_[pos_++] = static_cast<uint8_t>(v >> 16);
  }
  void U32(uint32_t v) {
    Reserve(4);
    buf_[pos_++] = static_cast<uint8_t>(v);
    buf_[pos_++] = static_cast<uint8_t>(v >> 8);
    buf_[pos_++] = static_cast<uint8_t>(v >> 16);
    buf_[pos_++] = static_cast<uint8_t>(v >> 24);
  }
  void U64(uint64_t v) {
    U32(static_cast<uint32_t>(v));
    U32(static_cast<uint32_t>(v >> 32));
  }

 private:
  [[noreturn]] static void Overflow() {
    std::fprintf(stderr, "PerfLogWriter: buffer overflow (bug in Flush)\n");
    std::abort();
  }
  void Reserve(std::size_t n) {
    if (pos_ + n > cap_) Overflow();
  }
  uint8_t* buf_ = nullptr;
  std::size_t cap_ = 0;
  std::size_t pos_{0};
};

inline void WritePerfNodeBudget(PerfLogWriter& w, uint8_t node_id,
                                uint64_t period_ns, uint64_t deadline_ns) {
  w.U8(kPerfNodeBudgetBlockType);
  w.U8(node_id);
  w.U32(1);
  w.U64(period_ns);
  w.U64(deadline_ns);
}

inline void WritePerfClockAnchor(PerfLogWriter& w, uint64_t mono_ns,
                                 uint64_t realtime_ns) {
  w.U8(kPerfClockAnchorBlockType);
  w.U8(0);  // file-wide: not attributed to a node
  w.U32(1);
  w.U64(mono_ns);
  w.U64(realtime_ns);
}

inline uint32_t ReadU24(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16);
}
inline uint32_t ReadU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline void PerfSampler::Flush(PerfLogWriter& w, uint8_t node_id) const {
  if (cnt_ == 0) return;  // empty sampler: file holds just the magic
  FlushRange(w, node_id, idx_ - cnt_, idx_);
}

inline void PerfSampler::FlushNew(PerfLogWriter& w, uint8_t node_id) {
  const uint64_t avail = idx_ - flushed_;
  if (avail == 0) return;
  FlushRange(w, node_id, avail > cap_ ? idx_ - cap_ : flushed_, idx_);
  flushed_ = idx_;
}

// Writes one sample block + one percentile block per effective id covering
// samples [begin, end) (absolute sample indexes; the ring keeps the last
// capacity() samples, so begin must satisfy begin >= idx_ - capacity()), then
// one aggregate OBS-09 histogram block for the whole [begin, end) window.
inline void PerfSampler::FlushRange(PerfLogWriter& w, uint8_t node_id,
                                    uint64_t begin, uint64_t end) const {
  const std::size_t cnt = static_cast<std::size_t>(end - begin);
  if (cnt == 0) return;
  // Group samples by effective id (sample.id, or node_id for kNoId
  // samples): one sample block + one percentile block per id present.
  auto eff = [&](const Sample& s) { return s.id != kNoId ? s.id : node_id; };
  auto at = [&](uint64_t i) -> const Sample& { return samples_[i & mask_]; };
  // Percentile sort scratch: stack for default-sized flushes, heap beyond
  // (Flush runs on the dump/shutdown path, so a rare allocation there is
  // acceptable; on failure the percentile blocks are skipped, sample blocks
  // are still written).
  constexpr std::size_t kStackSort = kPerfRingDefaultCap;
  uint32_t sorted_stack[kStackSort];
  uint32_t* sorted = sorted_stack;
  std::unique_ptr<uint32_t[]> sorted_heap;
  if (cnt > kStackSort) {
    sorted_heap = std::unique_ptr<uint32_t[]>(new (std::nothrow) uint32_t[cnt]);
    sorted = sorted_heap.get();
  }
  uint8_t present[256] = {};
  for (uint64_t i = begin; i < end; ++i) present[eff(at(i))] = 1;
  // OBS-09 histogram window end: the newest sample in the range (samples are
  // stored chronologically, so this is the range's last ts — not the last
  // block written, since blocks are grouped by id).
  const uint32_t end_ts = at(end - 1).ts;
  for (std::size_t k = 0; k < 256; ++k) {
    if (!present[k]) continue;
    const uint8_t bid = static_cast<uint8_t>(k);
    // Sample block (chronological order within this id).
    uint32_t cnt_id = 0;
    for (uint64_t i = begin; i < end; ++i) {
      if (eff(at(i)) == bid) ++cnt_id;
    }
    w.U8(1);
    w.U8(bid);
    w.U32(cnt_id);
    uint32_t last_ts = 0;
    for (uint64_t i = begin; i < end; ++i) {
      const Sample& s = at(i);
      if (eff(s) != bid) continue;
      w.U32(s.ts);
      w.U24(s.lat);
      w.U8(bid);
      w.U8(s.io);
      w.U8(0);  // rsvd
      last_ts = s.ts;
    }
    // Percentile block (nearest-rank) for this id; skipped if the sort
    // scratch allocation failed.
    if (sorted == nullptr) continue;
    std::size_t n = 0;
    for (uint64_t i = begin; i < end; ++i) {
      const Sample& s = at(i);
      if (eff(s) == bid) sorted[n++] = s.lat;
    }
    std::sort(sorted, sorted + n);
    auto pct = [&](double q) -> uint32_t {
      return n == 0 ? 0 : sorted[static_cast<std::size_t>(static_cast<double>(n - 1) * q)];
    };
    w.U8(2);
    w.U8(bid);
    w.U32(1);  // one percentile record
    w.U8(0xA1);
    w.U8(bid);
    w.U16(static_cast<uint16_t>(n > 0xFFFF ? 0xFFFF : n));
    w.U24(pct(0.50));
    w.U24(pct(0.95));
    w.U24(pct(0.99));
    w.U24(n == 0 ? 0 : sorted[n - 1]);
    w.U32(last_ts);
  }
  // OBS-09: one aggregate histogram block per flush window (fixed-size
  // record, no allocation). The histogram is cumulative since the last
  // Reset, so it also counts samples the ring evicted before this dump —
  // that is deliberate: p999 must not lose its tail to ring pressure.
  // Flush/FlushNew callers follow the dump-then-reset contract, so a block
  // covers exactly one window as long as a final Flush does not repeat an
  // already-dumped window. The offline analyzer merges bucket-wise (max with
  // max).
  const uint64_t hist_n = hist_.count();
  if (hist_n != 0) {
    w.U8(kPerfHistogramBlockType);
    w.U8(node_id);
    w.U32(1);  // one histogram record
    w.U8(kPerfHistogramRecordTag);
    w.U8(hist_series_);
    w.U16(static_cast<uint16_t>(LatencyHistogram::kBuckets));
    w.U64(hist_n);
    w.U64(hist_.max());
    w.U32(end_ts);
    for (uint32_t b = 0; b < LatencyHistogram::kBuckets; ++b) {
      w.U32(hist_.bucket_count(b));
    }
  }
}

}  // namespace thyme

namespace thyme {

// Work (compute-time) log: one 10B record per Loop / Dispatch batch.
// Duration uses 2^10 ns units (1.024us; 3B range ~= 17.2s) — finer than the
// latency unit because compute times are µs-scale. Timestamps are absolute
// (same axis as PerfSampler). Frame nibbles carry the mod-16 sequence of the
// input frame (first consumed) and output frame (last produced) so a work
// record can be frame-joined across nodes; flags bit0 marks event batches
// that spanned multiple input frames (not splittable).
//
// Same OBS-03 capacity contract as PerfSampler: the ring is sized at
// construction (default kCap; rate-sized via CapacityForRate) and Reset()
// after each periodic dump bounds the window the ring must hold.
class PerfWorkLog {
 public:
  static constexpr std::size_t kCap = kPerfRingDefaultCap;
  static constexpr std::size_t kMaxCap = kPerfRingMaxCap;
  static constexpr uint32_t kDurShift = 10;  // 1.024us per unit
  static constexpr uint32_t kMaxDurUnits = 0xFFFFFF;

  explicit PerfWorkLog(std::size_t capacity = kCap) {
    cap_ = ClampPow2(capacity);
    if (cap_ != 0) {
      recs_ = std::unique_ptr<Rec[]>(new (std::nothrow) Rec[cap_]{});
    }
    if (recs_ == nullptr) cap_ = 0;
    mask_ = cap_ - 1;
  }
  ~PerfWorkLog() = default;
  PerfWorkLog(const PerfWorkLog&) = delete;
  PerfWorkLog& operator=(const PerfWorkLog&) = delete;

  static std::size_t CapacityForRate(uint64_t rate_hz, uint64_t window_ns) {
    return PerfRingCapForRate(rate_hz, window_ns);
  }

  std::size_t capacity() const { return cap_; }

  void Reset() { idx_ = 0; cnt_ = 0; flushed_ = 0; }

  void Note(uint64_t ts_ns, uint64_t dur_ns, uint8_t in_nib, uint8_t out_nib,
            uint8_t flags) {
    if (cap_ == 0) return;
    const uint32_t ts = static_cast<uint32_t>(ts_ns >> PerfSampler::kTsShift);
    const uint64_t du = dur_ns >> kDurShift;
    Rec& r = recs_[idx_ & mask_];
    r.ts = ts;
    r.dur = du > kMaxDurUnits ? kMaxDurUnits : static_cast<uint32_t>(du);
    r.io = static_cast<uint8_t>((out_nib << 4) | (in_nib & 0xF));
    r.flags = flags;
    ++idx_;
    if (cnt_ < cap_) ++cnt_;
  }

  std::size_t count() const { return cnt_; }

  void Flush(PerfLogWriter& w, uint8_t node_id) const {
    if (cnt_ == 0) return;
    FlushRange(w, node_id, idx_ - cnt_, idx_);
  }

  // Periodic-dump variant: only records not covered by a previous FlushNew.
  void FlushNew(PerfLogWriter& w, uint8_t node_id) {
    const uint64_t avail = idx_ - flushed_;
    if (avail == 0) return;
    FlushRange(w, node_id, avail > cap_ ? idx_ - cap_ : flushed_, idx_);
    flushed_ = idx_;
  }

 private:
  void FlushRange(PerfLogWriter& w, uint8_t node_id, uint64_t begin,
                  uint64_t end) const {
    w.U8(3);
    w.U8(node_id);
    w.U32(static_cast<uint32_t>(end - begin));
    for (uint64_t i = begin; i < end; ++i) {
      const Rec& r = recs_[i & mask_];
      w.U32(r.ts);
      w.U24(r.dur);
      w.U8(r.io);
      w.U8(node_id);
      w.U8(r.flags);
    }
  }

  static std::size_t ClampPow2(std::size_t capacity) {
    if (capacity == 0) return 0;
    if (capacity > kMaxCap) capacity = kMaxCap;
    std::size_t cap = 1;
    while (cap < capacity) cap <<= 1;
    return cap;
  }

  struct Rec {
    uint32_t ts;
    uint32_t dur;
    uint8_t io;
    uint8_t flags;
  };
  // Work records are written as 10 B (ts u32, dur u24, io u8, node u8,
  // flags u8) in this field order; the in-memory layout is the wire ABI.
  static_assert(sizeof(Rec) == 12,
                "work record: 10 B wire + 2 B tail pad; field order is the "
                "wire order written by FlushRange");
  static_assert(offsetof(Rec, dur) == 4, "wire order: ts, dur");
  static_assert(offsetof(Rec, io) == 8, "wire order: ts, dur, io");
  static_assert(offsetof(Rec, flags) == 9, "wire order: ts, dur, io, flags");
  std::unique_ptr<Rec[]> recs_;
  std::size_t cap_ = 0;
  std::size_t mask_ = 0;
  uint64_t idx_{0};
  std::size_t cnt_{0};
  uint64_t flushed_{0};
};

}  // namespace thyme
