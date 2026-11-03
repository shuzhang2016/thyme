// Perf sample log wire format (feature/perf-log):
//   sample record (10B): ts u32 (16.384us units = 2^14 ns, ABSOLUTE
//                        CLOCK_MONOTONIC >> 14 truncated to u32 — one global
//                        time axis across all processes/files, no per-sampler
//                        base; wraps at ~19.5h uptime, log rotation keeps
//                        files shorter) | lat u24 (8.192us units, clamped) |
//                        id u8 | io u8 (hi = out frame nibble, lo = in frame
//                        nibble; consume taps carry the in-frame nibble as
//                        the frame-join key) | rsvd u8
//   percentile block (20B): tag 0xA1 | node u8 | n u16
//                        p50/p95/p99/max 4 x u24 (8.192us units)
//                        window_end_ts u32 (16.384us units)
//   work block (type 3): 10B records — ts u32 (absolute, as above) |
//                        dur u24 (1.024us units = 2^10 ns) | in/out frame
//                        nibbles u8 | node u8 | flags u8
// File: "T9P1" u32 magic, then blocks: [type u8][node u8][count u32]
// followed by count records (type 1 = samples, type 2 = one percentile rec,
// type 3 = work records). Type 10 is a 16B full-u64 mono/realtime anchor.
#include "thyme/check.h"
#include "thyme/runtime/perf_log.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace thyme;

int main() {
  PerfSampler s;

  // Empty: no blocks beyond header.
  {
    PerfLogWriter w;
    s.Flush(w, 7);
    CHECK(w.size() == 4);  // magic only
    CHECK(std::memcmp(w.data(), "T9P1", 4) == 0);
  }

  // Quantization: ts at 2^14ns (16.384us) units, absolute (no base
  // subtraction); lat at 2^13ns (8.192us) units. Shift-only conversion.
  s.Reset();
  s.Note(1000000ULL, 0);        // ts = 1000000 >> 14 = 61
  s.Note(1000000ULL + 16384ULL, 8192);   // ts 62; 8192ns -> lat 1
  s.Note(1000000ULL + 49152ULL, 20480, PerfSampler::kNoId, 0x05);  // ts 64; lat 2; io 0x05
  {
    PerfLogWriter w;
    s.Flush(w, 7);
    // header + block header (1+1+4) + 3 samples*10B + percentile block hdr +
    // record + OBS-09 histogram block (hdr + fixed-size record)
    CHECK(w.size() == 4 + 6 + 30 + 6 + 20 + 6 + kPerfHistRecordBytes);
    const uint8_t* p = w.data() + 4;
    CHECK(p[0] == 1 && p[1] == 7);  // sample block, node 7
    const uint32_t cnt = ReadU32(p + 2);
    CHECK(cnt == 3);
    const uint8_t* r = p + 6;
    CHECK(ReadU32(r) == 61);                      // ts absolute
    CHECK(ReadU24(r + 4) == 0 && r[7] == 7);      // lat 0, id 7
    CHECK(r[8] == 0 && r[9] == 0);                // io 0, rsvd 0
    CHECK(ReadU32(r + 10) == 62);                 // +16.384us
    CHECK(ReadU24(r + 14) == 1);                  // 8192ns -> 1
    CHECK(ReadU32(r + 20) == 64);                 // +3 units
    CHECK(ReadU24(r + 24) == 2);                  // 20480ns -> 2
    CHECK(r[28] == 0x05);                         // io nibble byte
  }

  // Absolute ts wraps at 2^32 units (~19.5h uptime): values are truncated,
  // not rebased.
  s.Reset();
  s.Note((1ULL << 46) + (7ULL << 14), 0);
  {
    PerfLogWriter w;
    s.Flush(w, 1);
    const uint8_t* r = w.data() + 4 + 6;
    CHECK(ReadU32(r) == 7);
  }

  // Latency clamp: beyond 2^24 * 2^13 ns (~=137s) clamps to 0xFFFFFF.
  s.Reset();
  s.Note(0, 200000000000ULL);  // 200s -> 24.4M units > 16.7M -> clamp
  {
    PerfLogWriter w;
    s.Flush(w, 1);
    const uint8_t* r = w.data() + 4 + 6;
    CHECK(ReadU24(r + 4) == 0xFFFFFF);
  }

  // Percentile block values come from the same samples (sorted nearest-rank).
  s.Reset();
  for (uint32_t i = 1; i <= 100; ++i) s.Note(i * 16384ULL, i * 8192ULL);
  {
    PerfLogWriter w;
    s.Flush(w, 3);
    const uint8_t* p = w.data();
    // find the type-2 block: samples block is 6 + 100*10 = 1006 bytes after
    // magic
    const uint8_t* b = p + 4 + 1006;
    CHECK(b[0] == 2 && b[1] == 3);
    const uint8_t* rec = b + 6;
    CHECK(rec[0] == 0xA1 && rec[1] == 3);
    CHECK((rec[2] | (rec[3] << 8)) == 100);
    CHECK(ReadU24(rec + 4) == 50);    // p50 = 50us
    CHECK(ReadU24(rec + 7) == 95);    // p95
    CHECK(ReadU24(rec + 10) == 99);   // p99
    CHECK(ReadU24(rec + 13) == 100);  // max = 100us
  }
  // Edge-id sampling: one sampler, per-id blocks in Flush output.
  s.Reset();
  s.Note(0, 8192, 20, 0x03);       // edge 20, lat 1 unit, in-frame 3
  s.Note(16384, 16384, 21, 0x04);  // edge 21, lat 2 units, in-frame 4
  s.Note(32768, 8192, 20, 0x03);   // edge 20 again
  {
    PerfLogWriter w;
    s.Flush(w, 7);
    const uint8_t* p = w.data() + 4;
    // first block: edge 20 with 2 samples
    CHECK(p[0] == 1 && p[1] == 20);
    CHECK(ReadU32(p + 2) == 2);
    CHECK(p[6 + 8] == 0x03);  // io of first sample
    // then its percentile block, then edge 21's two blocks
    const uint8_t* q = p + 6 + 20;   // skip sample block
    CHECK(q[0] == 2 && q[1] == 20);
    const uint8_t* r = q + 6 + 20;   // skip percentile block
    CHECK(r[0] == 1 && r[1] == 21);
    CHECK(ReadU32(r + 2) == 1);
    CHECK(ReadU24(r + 6 + 4) == 2);  // edge 21 lat = 2 units
    CHECK(r[6 + 8] == 0x04);         // edge 21 in-frame nibble
  }
  // Work records (block type 3): 10B each — ts u32 (2^14ns, absolute) +
  // dur u24 (2^10ns = 1.024us units) + in/out nibble u8 + node u8 + flags u8.
  {
    PerfWorkLog wl;
    wl.Note(1000000ULL, 2048ULL, 3, 4, 0);   // ts 61, 2us dur -> 2 units
    wl.Note(1000000ULL + 16384ULL, 1024ULL, 4, 5, 0x1);  // batch-span flag
    PerfLogWriter w;
    wl.Flush(w, 7);
    const uint8_t* p = w.data() + 4;
    CHECK(p[0] == 3 && p[1] == 7);           // work block, node 7
    CHECK(ReadU32(p + 2) == 2);              // 2 records
    const uint8_t* r = p + 6;
    CHECK(ReadU32(r) == 61);                 // ts absolute
    CHECK(ReadU24(r + 4) == 2);              // 2048ns >> 10 = 2
    CHECK(r[7] == 0x43);                     // out=4 hi nibble, in=3 lo nibble
    CHECK(r[8] == 7 && r[9] == 0);           // node, flags
    const uint8_t* r2 = r + 10;
    CHECK(ReadU32(r2) == 62);                // +16.384us
    CHECK(ReadU24(r2 + 4) == 1);             // 1024ns >> 10 = 1
    CHECK(r2[7] == 0x54 && r2[9] == 0x1);    // in=4 out=5, span flag
  }
  // Explicit id 0 is a valid edge id (RecordStructId::kObstacle): it must
  // NOT be replaced by the node id — only kNoId samples get substituted.
  s.Reset();
  s.Note(0, 8192, 0);   // explicit edge id 0
  s.Note(16384, 8192);  // kNoId -> substituted with node id
  {
    PerfLogWriter w;
    s.Flush(w, 7);
    const uint8_t* p = w.data() + 4;
    CHECK(p[0] == 1 && p[1] == 0);  // first block: edge id 0 (not node 7)
    CHECK(ReadU32(p + 2) == 1);
    const uint8_t* q = p + 6 + 10 + 6 + 20;  // skip sample + percentile block
    CHECK(q[0] == 1 && q[1] == 7);  // second block: the kNoId sample -> node 7
    CHECK(ReadU32(q + 2) == 1);
  }
  // Worst-case flush: all 255 usable ids present (0xFF is the kNoId
  // sentinel) with kCap samples in total. The writer buffer must fit magic +
  // kCap*10 sample bytes + 255 * (6B sample header + 6B percentile header +
  // 20B record) + one OBS-09 histogram block — and is sized with block
  // margin beyond that.
  s.Reset();
  for (uint32_t i = 0; i < 4096; ++i) {
    s.Note(i * 16384ULL, 8192, static_cast<uint8_t>(i % 255));
  }
  {
    PerfLogWriter w;
    s.Flush(w, 7);
    CHECK(w.size() == 4 + 4096 * 10 + 255 * (6 + 6 + 20) + 6 +
                          kPerfHistRecordBytes);
  }

  // OBS-09: the aggregate type-6 histogram block. 100 samples on edge 5 with
  // a known distribution; the block follows the sample + percentile blocks
  // of that id and carries the fixed-size bucket record.
  s.Reset();
  s.SetHistogramSeries(PerfSampler::kHistSeriesCallbackWork);
  for (uint32_t i = 0; i < 100; ++i) {
    s.Note(static_cast<uint64_t>(i) * 16384ULL, 1000 + static_cast<uint64_t>(i) * 100, 5);
  }
  {
    PerfLogWriter w;
    s.Flush(w, 7);
    const uint8_t* b = w.data() + 4 + (6 + 100 * 10) + (6 + 20);
    CHECK(b[0] == kPerfHistogramBlockType);
    CHECK(b[1] == 7);                // node
    CHECK(ReadU32(b + 2) == 1);      // one histogram record
    const uint8_t* r = b + 6;
    CHECK(r[0] == kPerfHistogramRecordTag);
    CHECK(r[1] == PerfSampler::kHistSeriesCallbackWork);
    CHECK((static_cast<uint32_t>(r[2]) |
           (static_cast<uint32_t>(r[3]) << 8)) ==
          LatencyHistogram::kBuckets);
    // count, exact max, window end ts
    uint64_t cnt = 0, max_ns = 0;
    for (int i = 0; i < 8; ++i) {
      cnt |= static_cast<uint64_t>(r[4 + i]) << (8 * i);
      max_ns |= static_cast<uint64_t>(r[12 + i]) << (8 * i);
    }
    CHECK(cnt == 100);
    CHECK(max_ns == 1000 + 99 * 100);  // exact max, not a bucket boundary
    CHECK(ReadU32(r + 20) == 99);      // last sample ts (2^14 ns units)
    // Bucket counts reconstruct the distribution; the 1000 ns sample sits in
    // the bucket whose bounds contain it.
    uint32_t bucket_sum = 0;
    for (uint32_t i = 0; i < LatencyHistogram::kBuckets; ++i) {
      bucket_sum += ReadU32(r + 24 + i * 4);
    }
    CHECK(bucket_sum == 100);
    CHECK(ReadU32(r + 24 + LatencyHistogram::BucketOf(1000) * 4) == 1);
  }
  // Reset clears the histogram: nothing to flush -> magic only.
  s.Reset();
  {
    PerfLogWriter w;
    s.Flush(w, 7);
    CHECK(w.size() == 4);
  }

  // ARCH-15: additive type-10 clock anchor preserves both full u64 clocks.
  {
    const uint64_t mono = 0xFEDCBA9876543210ull;
    const uint64_t realtime = 0x0123456789ABCDEFull;
    PerfLogWriter w;
    WritePerfClockAnchor(w, mono, realtime);
    CHECK(w.size() == 4 + 6 + kPerfClockAnchorRecordBytes);
    const uint8_t* b = w.data() + 4;
    CHECK(b[0] == kPerfClockAnchorBlockType && b[0] == 10);
    CHECK(b[1] == 0 && ReadU32(b + 2) == 1);
    uint64_t got_mono = 0;
    uint64_t got_realtime = 0;
    for (uint32_t i = 0; i < 8; ++i) {
      got_mono |= static_cast<uint64_t>(b[6 + i]) << (8 * i);
      got_realtime |= static_cast<uint64_t>(b[14 + i]) << (8 * i);
    }
    CHECK(got_mono == mono);
    CHECK(got_realtime == realtime);
  }
  return 0;
}
