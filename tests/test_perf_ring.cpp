// test_perf_ring.cpp — OBS-03: rate-derived ring capacity, dump-then-reset
// window semantics, and crash safety of the periodic dump.
//
// Covered:
//   * PerfRingCapForRate sizing math: default floor, power-of-two rounding,
//     margin, max cap, overflow-proof inputs.
//   * PerfSampler at a non-default capacity: eviction and Flush of a full
//     oversized ring through a capacity-matched PerfLogWriter.
//   * Dump-then-reset: after each PerfLogDumper dump the rings are empty;
//     consecutive dump windows are disjoint and lossless.
//   * Dumper capacity budgets: oversized sampler/work rings abort before an
//     append; matched non-default capacities honor a non-default interval.
//   * Watermark contract preserved: samples evicted before a dump are
//     dropped, never duplicated.
//   * Crash safety: a SIGKILL between dumps leaves a fully parseable file
//     containing exactly the dumped windows.
#include "thyme/check.h"
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/event_thread.h"
#include "thyme/runtime/perf_dump.h"
#include "thyme/runtime/perf_log.h"
#include "thyme/runtime/proc_sampler.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

std::vector<uint8_t> ReadFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  CHECK(f != nullptr);
  std::vector<uint8_t> out;
  uint8_t buf[8192];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out.insert(out.end(), buf, buf + n);
  }
  std::fclose(f);
  return out;
}

// Walks a T9P1 buffer; appends every sample-record ts (type 1) to samples,
// every work-record ts (type 3) to work, counts sys records, and skips
// OBS-09 histogram blocks. Returns the number of unconsumed tail bytes
// (0 = clean parse).
struct Parsed {
  struct Anchor {
    uint64_t mono_ns;
    uint64_t realtime_ns;
  };
  std::vector<uint32_t> samples;
  std::vector<uint32_t> work;
  std::vector<Anchor> anchors;
  uint32_t sys_records = 0;
  uint32_t epoch_blocks = 0;
  uint32_t hist_blocks = 0;
  uint32_t series_name_blocks = 0;
  uint32_t budget_blocks = 0;
};
std::size_t Parse(const std::vector<uint8_t>& d, Parsed* out) {
  CHECK(d.size() >= 4);
  CHECK(std::memcmp(d.data(), "T9P1", 4) == 0);
  std::size_t p = 4;
  while (p + 6 <= d.size()) {
    const uint8_t typ = d[p];
    const uint32_t cnt = ReadU32(&d[p + 2]);
    p += 6;
    std::size_t rec = (typ == 2)  ? 20
                      : (typ == 4) ? 8
                      : (typ == 6) ? kPerfHistRecordBytes
                      : (typ == 7) ? kPerfManifestRecordBytes
                      : (typ == 8) ? kPerfSeriesNameRecordBytes
                      : (typ == 9) ? kPerfNodeBudgetRecordBytes
                      : (typ == kPerfClockAnchorBlockType)
                          ? kPerfClockAnchorRecordBytes
                          : 10;
    if (typ < 1 || typ > kPerfClockAnchorBlockType) break;  // unknown tail
    if (p + cnt * rec > d.size()) break;                   // truncated tail
    if (typ == 1) {
      for (uint32_t i = 0; i < cnt; ++i) out->samples.push_back(ReadU32(&d[p + i * 10]));
    } else if (typ == 3) {
      for (uint32_t i = 0; i < cnt; ++i) out->work.push_back(ReadU32(&d[p + i * 10]));
    } else if (typ == 4) {
      out->epoch_blocks += cnt;
    } else if (typ == 5) {
      out->sys_records += cnt;
    } else if (typ == 6) {
      out->hist_blocks += cnt;
    } else if (typ == 8) {
      out->series_name_blocks += 1;
    } else if (typ == 9) {
      out->budget_blocks += cnt;
    } else if (typ == kPerfClockAnchorBlockType) {
      CHECK(cnt == 1 && d[p - 5] == 0);
      Parsed::Anchor anchor{0, 0};
      for (uint32_t i = 0; i < 8; ++i) {
        anchor.mono_ns |= static_cast<uint64_t>(d[p + i]) << (8 * i);
        anchor.realtime_ns |= static_cast<uint64_t>(d[p + 8 + i]) << (8 * i);
      }
      out->anchors.push_back(anchor);
    }
    p += cnt * rec;
  }
  return d.size() - p;
}

void ExpectCapacityAbort(const char* path, bool work_mismatch) {
  std::remove(path);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    PerfSampler sampler(work_mismatch ? PerfSampler::kCap : 8192);
    PerfWorkLog work(work_mismatch ? 8192 : PerfWorkLog::kCap);
    PerfLogDumper dumper;
    if (!dumper.Open(path, 1000)) _exit(2);
    sampler.Note(1000, 10, 1, 0);
    work.Note(1000, 20, 0, 0, 0);
    dumper.DumpIfDue(0, sampler, &work, 1);
    _exit(3);
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
  CHECK(ReadFile(path).empty());
  std::remove(path);
}

struct DummyCyclic : CyclicThread {
  DummyCyclic(const char* name, uint32_t hz)
      : CyclicThread(name, hz, &health) {}
  void Loop() override {}
  HealthSlot health;
};
struct DummyEvent : EventThread {
  DummyEvent(const char* name, uint32_t hz) : EventThread(name, &health, hz) {}
  void Dispatch() override {}
  HealthSlot health;
};

}  // namespace

int main() {
  // --- sizing math ----------------------------------------------------------
  {
    // No declared rate (or a quiet edge) keeps the 4096 default.
    CHECK(PerfRingCapForRate(0, kPerfRingDefaultWindowNs) == 4096);
    CHECK(PerfRingCapForRate(100, kPerfRingDefaultWindowNs) == 4096);
    // 1000 Hz x 10 s x 1.5 = 15000 -> next pow2.
    CHECK(PerfRingCapForRate(1000, kPerfRingDefaultWindowNs) == 16384);
    // 1000 Hz x 60 s x 1.5 = 90000 -> 131072.
    CHECK(PerfRingCapForRate(1000, 60000000000ull) == 131072);
    // 1 MHz x 10 s x 1.5 = 15M -> clamped to the 262144 max (~3MB @12B).
    CHECK(PerfRingCapForRate(1000000, kPerfRingDefaultWindowNs) ==
          kPerfRingMaxCap);
    // Absurd inputs cannot overflow the u64 product (clamped first).
    CHECK(PerfRingCapForRate(~0ull, ~0ull) == kPerfRingMaxCap);
    // Always a power of two within [default, max].
    for (uint64_t hz : {1ull, 7ull, 100ull, 999ull, 12345ull, 100000ull}) {
      for (uint64_t w : {1000000ull, 10000000000ull, 3600000000000ull}) {
        const std::size_t c = PerfRingCapForRate(hz, w);
        CHECK(c >= kPerfRingDefaultCap && c <= kPerfRingMaxCap);
        CHECK((c & (c - 1)) == 0);
      }
    }
    // Constructor clamps/rounds the same way.
    CHECK(PerfSampler(16384).capacity() == 16384);
    CHECK(PerfSampler(100).capacity() == 128);
    CHECK(PerfSampler(PerfSampler::kMaxCap * 4).capacity() ==
          PerfSampler::kMaxCap);
    CHECK(PerfSampler().capacity() == PerfSampler::kCap);
    CHECK(PerfWorkLog(8192).capacity() == 8192);
  }

  // --- ring at non-default capacity: eviction + full flush ------------------
  {
    PerfSampler s(16384);
    for (uint32_t i = 0; i < 20000; ++i) {
      s.Note(static_cast<uint64_t>(i) << 14, static_cast<uint64_t>(i) << 13,
             7, 0);
    }
    CHECK(s.count() == 16384);
    PerfLogWriter w(s.capacity(), 0);
    s.Flush(w, 3);
    Parsed p;
    CHECK(Parse({w.data(), w.data() + w.size()}, &p) == 0);
    CHECK(p.samples.size() == 16384);
    // Oldest 3616 samples evicted: the first retained sample is i=3616.
    CHECK(p.samples.front() == 3616);
    CHECK(p.samples.back() == 19999);
    CHECK(std::is_sorted(p.samples.begin(), p.samples.end()));
  }

  // --- dump-then-reset: disjoint, lossless windows --------------------------
  {
    char path[96];
    std::snprintf(path, sizeof(path), "/tmp/t9ring_%d.bin",
                  static_cast<int>(getpid()));
    std::remove(path);
    PerfSampler s;
    PerfWorkLog wl;
    PerfLogDumper d;
    CHECK(d.Open(path, 0));
    for (uint32_t i = 1; i <= 5; ++i) {
      s.Note(static_cast<uint64_t>(i) << 14, 100, 1, 0);
      wl.Note(static_cast<uint64_t>(i) << 14, 50, 0, 0, 0);
    }
    CHECK(d.DumpIfDue(0, s, &wl, 1));
    CHECK(s.count() == 0);   // ring reset after the dump
    CHECK(wl.count() == 0);
    CHECK(!d.DumpIfDue(1, s, &wl, 1));  // nothing new: no empty block
    for (uint32_t i = 6; i <= 9; ++i) {
      s.Note(static_cast<uint64_t>(i) << 14, 100, 1, 0);
      wl.Note(static_cast<uint64_t>(i) << 14, 50, 0, 0, 0);
    }
    CHECK(d.DumpIfDue(2, s, &wl, 1));
    CHECK(s.count() == 0);
    d.Close();

    Parsed p;
    CHECK(Parse(ReadFile(path), &p) == 0);
    CHECK(p.samples.size() == 9);
    CHECK(p.work.size() == 9);
    std::vector<uint32_t> expect{1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::sort(p.samples.begin(), p.samples.end());
    CHECK(p.samples == expect);  // no duplicates, no gaps across windows
    CHECK(p.epoch_blocks == 2);
    CHECK(p.anchors.size() == 2);  // one refreshed anchor per non-empty dump
    CHECK(p.anchors[0].mono_ns == 0 && p.anchors[1].mono_ns == 2);
    CHECK(p.anchors[0].realtime_ns > 0 && p.anchors[1].realtime_ns > 0);
    std::remove(path);
  }

  // --- capacity mismatch: abort before any append ---------------------------
  {
    char path[96];
    std::snprintf(path, sizeof(path), "/tmp/t9ring_cap_%d.bin",
                  static_cast<int>(getpid()));
    ExpectCapacityAbort(path, false);
    ExpectCapacityAbort(path, true);
  }

  // --- matched non-default capacities and interval dump normally ------------
  {
    char path[96];
    std::snprintf(path, sizeof(path), "/tmp/t9ring_cap_match_%d.bin",
                  static_cast<int>(getpid()));
    std::remove(path);
    PerfSampler s(8192);
    PerfWorkLog wl(8192);
    PerfLogDumper d;
    CHECK(d.Open(path, 1000, s.capacity(), wl.capacity()));
    s.Note(1 << 14, 100, 1, 0);
    wl.Note(1 << 14, 50, 0, 0, 0);
    CHECK(d.DumpIfDue(1000, s, &wl, 1));
    const std::size_t first_size = ReadFile(path).size();
    CHECK(first_size > 4);
    s.Note(2 << 14, 100, 1, 0);
    wl.Note(2 << 14, 50, 0, 0, 0);
    CHECK(!d.DumpIfDue(1500, s, &wl, 1));
    CHECK(ReadFile(path).size() == first_size);
    CHECK(s.count() == 1 && wl.count() == 1);
    CHECK(d.DumpIfDue(2000, s, &wl, 1));
    d.Close();
    Parsed p;
    CHECK(Parse(ReadFile(path), &p) == 0);
    CHECK(p.samples == std::vector<uint32_t>({1, 2}));
    CHECK(p.work == std::vector<uint32_t>({1, 2}));
    std::remove(path);
  }

  // --- watermark contract: eviction before a dump is a loss, not a dupe -----
  {
    char path[96];
    std::snprintf(path, sizeof(path), "/tmp/t9ring_wm_%d.bin",
                  static_cast<int>(getpid()));
    std::remove(path);
    PerfSampler s;  // 4096 default
    PerfLogDumper d;
    CHECK(d.Open(path, 0));
    for (uint32_t i = 0; i < 5000; ++i) {
      s.Note(static_cast<uint64_t>(i) << 14, 1, 1, 0);
    }
    CHECK(d.DumpIfDue(0, s, nullptr, 1));  // only the retained 4096 can dump
    CHECK(s.count() == 0);
    for (uint32_t i = 0; i < 10; ++i) {
      s.Note(static_cast<uint64_t>(5000 + i) << 14, 1, 1, 0);
    }
    CHECK(d.DumpIfDue(1, s, nullptr, 1));
    d.Close();
    Parsed p;
    CHECK(Parse(ReadFile(path), &p) == 0);
    CHECK(p.samples.size() == 4096 + 10);
    CHECK(p.samples.front() == 904);  // 5000 - 4096: oldest evicted
    CHECK(p.samples.back() == 5009);
    std::remove(path);
  }

  // --- crash safety with dump-reset: SIGKILL between dumps ------------------
  {
    char path[96];
    std::snprintf(path, sizeof(path), "/tmp/t9ring_crash_%d.bin",
                  static_cast<int>(getpid()));
    std::remove(path);
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      PerfSampler s;
      PerfLogDumper d;
      if (!d.Open(path, 0)) _exit(2);
      for (uint32_t i = 1; i <= 5; ++i) {
        s.Note(static_cast<uint64_t>(i) << 14, 100, 1, 0);
      }
      if (!d.DumpIfDue(0, s, nullptr, 1)) _exit(3);
      for (uint32_t i = 6; i <= 8; ++i) {  // undumped partial window
        s.Note(static_cast<uint64_t>(i) << 14, 100, 1, 0);
      }
      raise(SIGKILL);
      _exit(1);
    }
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    Parsed p;
    CHECK(Parse(ReadFile(path), &p) == 0);  // fully parseable, no corrupt tail
    CHECK(p.samples.size() == 5);  // the partial window is lost, dumped is safe
    CHECK(p.samples.front() == 1 && p.samples.back() == 5);
    std::remove(path);
  }

  // --- writer budget covers every block one dump appends (incl. epoch) -----
  {
    // A dump carries magic + manifest + series names + node budget + epoch +
    // clock anchor + sample/percentile/histogram + work + type-5. Fill each
    // ring to its cap and append every metadata block once: a missing budget
    // term aborts (PerfLogWriter::Overflow), so this is the regression for
    // the explicit epoch reservation.
    constexpr std::size_t kCap = 8;
    PerfSampler lat(kCap);
    PerfWorkLog work(kCap);
    ProcSampler proc;
    CHECK(proc.Start(ProcSampler::Options{}));
    for (std::size_t i = 0; i < kCap; ++i) {
      lat.Note(i * 1000000ull, 900000, 3, 1);
      work.Note(i * 1000000ull, 400000, 1, 1, 3);
    }
    for (std::size_t i = 0; i < kCap; ++i) (void)proc.SampleNow(i * 1000000ull);

    PerfLogWriter w(lat.capacity(), work.capacity());
    CHECK(w.ok());
    w.U8(kPerfManifestBlockType);
    w.U8(0);
    w.U32(1);
    w.U16(1);
    w.U16(0);
    w.U64(0x0123456789abcdefull);
    proc.FlushMetadata(w, 3);
    WritePerfNodeBudget(w, 3, 10000000, 8000000);
    w.U8(PerfSampler::kEpochBlockType);
    w.U8(3);
    w.U32(1);
    w.U64(0);
    WritePerfClockAnchor(w, 1000000000ull, 1700000000000000000ull);
    lat.Flush(w, 3);
    work.Flush(w, 3);
    proc.FlushNew(w, 3);
    CHECK(w.size() <= PerfLogWriter::BufSizeFor(lat.capacity(), work.capacity()));
    CHECK(w.size() > 4);
    // The epoch payload is eight bytes; the budget must reserve header+payload
    // rather than lean on the histogram margin.
    CHECK(PerfSampler::kEpochRecordBytes == 8);
    CHECK(PerfLogWriter::BufSizeFor(1, 1) >=
          4 + (6 + kPerfManifestRecordBytes) + (6 + PerfSampler::kEpochRecordBytes));
  }

  // --- ProcSampler reset via the dumper (ring cleared, baseline kept) -------
  {
    ProcSampler ps;
    CHECK(ps.Start(ProcSampler::Options{}));
    PerfSampler lat;
    char path[96];
    std::snprintf(path, sizeof(path), "/tmp/t9ring_proc_%d.bin",
                  static_cast<int>(getpid()));
    std::remove(path);
    PerfLogDumper d;
    CHECK(d.Open(path, 0));
    d.SetNodeBudget(10000000, 8000000);
    ps.SampleNow(NowNs());
    lat.Note(NowNs(), 100, 3, 0);
    CHECK(d.DumpIfDue(NowNs(), lat, nullptr, 3, &ps));
    CHECK(ps.count() == 0);       // sys ring reset too
    CHECK(lat.count() == 0);
    // Sampling still works after the reset (delta baseline survived).
    CHECK(ps.SampleNow(NowNs()) == 16);
    CHECK(ps.count() == 16);
    CHECK(d.DumpIfDue(NowNs() + 1, lat, nullptr, 3, &ps));
    CHECK(ps.count() == 0);
    d.Close();
    Parsed p;
    CHECK(Parse(ReadFile(path), &p) == 0);
    CHECK(p.sys_records == 32);  // two dumped windows of 16 series each
    CHECK(p.series_name_blocks == 1);
    CHECK(p.budget_blocks == 1);
    std::remove(path);
  }

  // --- thread constructors size rings from the declared rate -----------------
  {
    DummyCyclic hot("hot", 1000);    // 1000 Hz -> 16384-sample rings
    CHECK(hot.perf().capacity() == 16384);
    CHECK(hot.perf_edge().capacity() == 16384);
    CHECK(hot.work_log().capacity() == 16384);
    DummyCyclic quiet("quiet", 100);  // below the default floor
    CHECK(quiet.perf().capacity() == PerfSampler::kCap);
    DummyEvent ev("ev", 100);
    CHECK(ev.perf().capacity() == PerfSampler::kCap);
    DummyEvent ev_hot("ev_hot", 10000);  // 10000*10*1.5 = 150000 -> 262144
    CHECK(ev_hot.perf().capacity() == 262144);
  }

  return 0;
}
