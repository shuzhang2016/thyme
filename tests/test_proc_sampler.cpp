// test_proc_sampler.cpp — OBS-04: low-frequency process CPU / ctx-switch /
// page-fault / resource / queue-depth sampling.
//
// Covers: proc series presence + monotonic ts + utime delta under known CPU
// burn; resource gauges (RSS/VSize/threads/fd) and deltas (io, cpu delay);
// fd gauge tracking real opens; probe registration (callback + atomic counter)
// carrying instant values; FlushNew wire blocks (type 5) and watermark;
// PerfLogDumper integration; ctx-switch series presence under forced
// contention (presence, NOT magnitude — roadmap H1); hot-loop decoupling
// (loop iterations must never cause a /proc read); ProcSampleThread smoke;
// writer budget with a full ring.
#include "thyme/check.h"
#include "thyme/runtime/perf_dump.h"
#include "thyme/runtime/proc_sampler.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
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

void SleepNs(uint64_t ns) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ull);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ull);
  while (nanosleep(&ts, &ts) != 0) {
  }
}

void BurnCpuMs(uint64_t ms) {
  const uint64_t end = NowNs() + ms * 1000000ull;
  volatile uint64_t x = 0;
  while (NowNs() < end) {
    for (int i = 0; i < 1000; ++i) x += static_cast<uint64_t>(i);
  }
}

// Sum of recorded deltas / first-seen instant for one series.
uint64_t SumSeries(const std::vector<ProcSampler::Record>& recs,
                   uint8_t series) {
  uint64_t sum = 0;
  for (const ProcSampler::Record& r : recs) {
    if (r.series == series) sum += r.value;
  }
  return sum;
}

uint32_t CountSeries(const std::vector<ProcSampler::Record>& recs,
                     uint8_t series) {
  uint32_t n = 0;
  for (const ProcSampler::Record& r : recs) {
    if (r.series == series) ++n;
  }
  return n;
}

// Most recently recorded value for one series (0 when absent). Records are
// copied oldest-first, so the last match is the newest sample.
uint64_t LastValue(const std::vector<ProcSampler::Record>& recs,
                   uint8_t series) {
  uint64_t v = 0;
  for (const ProcSampler::Record& r : recs) {
    if (r.series == series) v = r.value;
  }
  return v;
}

uint64_t ProbeFn(void* ctx) { return *static_cast<uint64_t*>(ctx); }

// Contention workers: yield + mutex ping-pong to provoke context switches.
std::atomic<bool> g_contend_stop{false};
pthread_mutex_t g_contend_mu = PTHREAD_MUTEX_INITIALIZER;
void* ContendMain(void*) {
  while (!g_contend_stop.load(std::memory_order_relaxed)) {
    pthread_mutex_lock(&g_contend_mu);
    pthread_mutex_unlock(&g_contend_mu);
    sched_yield();
  }
  return nullptr;
}

}  // namespace

int main() {
  ProcSampler s;
  CHECK(!s.started());
  CHECK(s.SampleNow(NowNs()) == 0);  // not started: no-op
  CHECK(s.Start(ProcSampler::Options{}));
  CHECK(s.started());

  // Probe registration: callback and atomic counter.
  uint64_t depth = 7;
  const int probe_cb = s.RegisterProbe(&ProbeFn, &depth);
  CHECK(probe_cb == ProcSampler::kProbeBase);
  std::atomic<uint64_t> pending{0};
  const int probe_atomic = s.RegisterCounter(&pending);
  CHECK(probe_atomic == ProcSampler::kProbeBase + 1);
  CHECK(s.probe_count() == 2);
  CHECK(s.RegisterProbe(nullptr, nullptr) == -1);

  // --- proc series: presence, monotonic ts, utime delta under CPU burn ----
  // Burn CPU until the sampler observes a nonzero utime delta (10ms tick
  // granularity makes one 50ms burn enough in practice; bounded retry loop).
  uint64_t utime_sum = 0;
  for (int attempt = 0; attempt < 100 && utime_sum == 0; ++attempt) {
    BurnCpuMs(50);
    CHECK(s.SampleNow(NowNs()) == 18);  // 16 proc series + 2 probes
    std::vector<ProcSampler::Record> recs(s.count());
    CHECK(s.CopyRecords(recs.data(), static_cast<uint32_t>(recs.size())) ==
          recs.size());
    utime_sum = SumSeries(recs, ProcSampler::kUtimeNs);
  }
  CHECK(utime_sum > 0);  // burned CPU must show up as user time

  std::vector<ProcSampler::Record> recs(s.count());
  CHECK(s.CopyRecords(recs.data(), static_cast<uint32_t>(recs.size())) ==
        recs.size());
  CHECK(recs.size() >= 18);
  // Every proc series present in every sample; ts non-decreasing per series
  // and the deltas sum to a non-decreasing (monotonic) cumulative series.
  for (uint8_t series : {ProcSampler::kUtimeNs, ProcSampler::kStimeNs,
                         ProcSampler::kMinFlt, ProcSampler::kMajFlt,
                         ProcSampler::kVolCtx, ProcSampler::kNonvolCtx,
                         ProcSampler::kRssKb, ProcSampler::kVsizeKb,
                         ProcSampler::kThreads, ProcSampler::kFdCount,
                         ProcSampler::kFdLimit, ProcSampler::kIoReadKb,
                         ProcSampler::kIoWriteKb, ProcSampler::kIoReadSyscalls,
                         ProcSampler::kIoWriteSyscalls,
                         ProcSampler::kCpuDelayMs}) {
    CHECK(CountSeries(recs, series) >= 1);
  }
  // Gauges must carry plausible values: the test process has at least one
  // thread, one open fd, a positive fd limit and a non-empty address space.
  CHECK(LastValue(recs, ProcSampler::kThreads) >= 1);
  CHECK(LastValue(recs, ProcSampler::kFdCount) >= 1);
  CHECK(LastValue(recs, ProcSampler::kFdLimit) > 0);
  CHECK(LastValue(recs, ProcSampler::kRssKb) > 0);
  CHECK(LastValue(recs, ProcSampler::kVsizeKb) > 0);

  // The fd gauge must track opens made by this process.
  {
    const uint64_t before = LastValue(recs, ProcSampler::kFdCount);
    int fds[4];
    for (int i = 0; i < 4; ++i) fds[i] = open("/dev/null", O_RDONLY);
    CHECK(s.SampleNow(NowNs()) == 18);
    std::vector<ProcSampler::Record> after(s.count());
    CHECK(s.CopyRecords(after.data(), static_cast<uint32_t>(after.size())) ==
          after.size());
    CHECK(LastValue(after, ProcSampler::kFdCount) >= before + 4);
    for (int i = 0; i < 4; ++i) {
      if (fds[i] >= 0) close(fds[i]);
    }
  }
  {
    uint32_t last_ts[256] = {};
    for (const ProcSampler::Record& r : recs) {
      CHECK(r.ts >= last_ts[r.series]);  // ts monotonic within a series
      last_ts[r.series] = r.ts;
    }
  }
  // Probe records carry the instant value, not a delta.
  depth = 42;
  pending.store(123, std::memory_order_relaxed);
  CHECK(s.SampleNow(NowNs()) == 18);
  recs.resize(s.count());
  CHECK(s.CopyRecords(recs.data(), static_cast<uint32_t>(recs.size())) ==
        recs.size());
  {
    bool saw_cb = false, saw_atomic = false;
    for (auto it = recs.rbegin(); it != recs.rend() && !(saw_cb && saw_atomic);
         ++it) {
      if (!saw_cb && it->series == static_cast<uint8_t>(probe_cb)) {
        CHECK(it->value == 42);
        saw_cb = true;
      }
      if (!saw_atomic && it->series == static_cast<uint8_t>(probe_atomic)) {
        CHECK(it->value == 123);
        saw_atomic = true;
      }
    }
    CHECK(saw_cb && saw_atomic);
  }

  // --- FlushNew wire format + watermark ------------------------------------
  {
    ProcSampler s2;
    CHECK(s2.Start(ProcSampler::Options{}));
    s2.SampleNow(NowNs());  // 16 records, no probes
    PerfLogWriter w;
    s2.FlushNew(w, 9);
    // Walk: magic + one type-5 block.
    CHECK(w.size() == 4 + 6 + 16 * 10);
    CHECK(w.data()[4] == 5);  // block type
    CHECK(w.data()[5] == 9);  // node
    CHECK(ReadU32(&w.data()[6]) == 16);
    uint32_t seen = 0;
    for (uint32_t i = 0; i < 16; ++i) {
      const uint8_t* rec = &w.data()[10 + i * 10];
      (void)ReadU32(rec);       // ts
      (void)ReadU32(&rec[4]);   // value
      seen |= 1u << rec[8];
      CHECK(rec[9] == 0);       // rsvd
    }
    CHECK(seen == 0xFFFF);  // series 0..15 all present
    PerfLogWriter w2;
    s2.FlushNew(w2, 9);
    CHECK(w2.size() == 4);  // watermark: nothing new
    s2.SampleNow(NowNs());
    s2.FlushNew(w2, 9);
    CHECK(w2.size() == 4 + 6 + 16 * 10);  // exactly the new sample's records
  }

  // --- PerfLogDumper integration -------------------------------------------
  {
    char path[96];
    std::snprintf(path, sizeof(path), "/tmp/c9procsampler_%d.bin",
                  static_cast<int>(getpid()));
    std::remove(path);
    ProcSampler s3;
    CHECK(s3.Start(ProcSampler::Options{}));
    PerfSampler lat;
    PerfLogDumper d;
    CHECK(d.Open(path, 0));
    s3.SampleNow(NowNs());
    lat.Note(NowNs(), 100, 3, 0);
    CHECK(d.DumpIfDue(NowNs(), lat, nullptr, 3, &s3));
    d.Close();
    FILE* f = std::fopen(path, "rb");
    CHECK(f != nullptr);
    std::vector<uint8_t> data;
    uint8_t buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
      data.insert(data.end(), buf, buf + n);
    }
    std::fclose(f);
    std::remove(path);
    CHECK(data.size() > 4);
    bool saw_sys = false;
    std::size_t p = 4;
    while (p + 6 <= data.size()) {
      const uint8_t typ = data[p];
      const uint32_t cnt = ReadU32(&data[p + 2]);
      p += 6;
      if (typ == 1) {
        p += 10 * cnt;
      } else if (typ == 2) {
        p += 20 * cnt;
      } else if (typ == 3) {
        p += 10 * cnt;
      } else if (typ == 4) {
        p += 8 * cnt;
      } else if (typ == 5) {
        CHECK(cnt == 16);  // one sample period, no probes
        p += 10 * cnt;
        saw_sys = true;
      } else if (typ == 6) {
        p += kPerfHistRecordBytes * cnt;  // OBS-09 histogram blocks
      } else if (typ == kPerfManifestBlockType) {
        p += kPerfManifestRecordBytes * cnt;
      } else if (typ == kPerfSeriesNameBlockType) {
        p += kPerfSeriesNameRecordBytes * cnt;
      } else if (typ == kPerfNodeBudgetBlockType) {
        p += kPerfNodeBudgetRecordBytes * cnt;
      } else if (typ == kPerfClockAnchorBlockType) {
        p += kPerfClockAnchorRecordBytes * cnt;
      } else {
        break;
      }
    }
    CHECK(saw_sys);
    CHECK(p == data.size());  // every byte consumed: no corrupt tail
  }

  // --- writer budget: a full ring must fit one PerfLogWriter ---------------
  {
    ProcSampler s4;
    CHECK(s4.Start(ProcSampler::Options{}));
    for (uint32_t i = 0; i < ProcSampler::kMaxProbes; ++i) {
      CHECK(s4.RegisterProbe(&ProbeFn, &depth) ==
            static_cast<int>(ProcSampler::kProbeBase + i));
    }
    CHECK(s4.RegisterProbe(&ProbeFn, &depth) == -1);  // probe table full
    // 16 proc series + 8 probes = 24 records/sample; kCap is reached at 43.
    for (int i = 0; i < 45; ++i) s4.SampleNow(NowNs());
    CHECK(s4.count() == ProcSampler::kCap);
    PerfLogWriter w;  // aborts internally if the budget is undersized
    s4.FlushNew(w, 1);
    CHECK(w.size() == 4 + 6 + ProcSampler::kCap * 10);
  }

  // --- ctx-switch series under forced contention (presence, not magnitude) -
  {
    ProcSampler s5;
    CHECK(s5.Start(ProcSampler::Options{}));
    s5.SampleNow(NowNs());  // baseline
    g_contend_stop.store(false, std::memory_order_relaxed);
    pthread_t threads[16];
    for (pthread_t& t : threads) {
      CHECK(pthread_create(&t, nullptr, &ContendMain, nullptr) == 0);
    }
    SleepNs(150000000ull);  // 150 ms of 16-way contention
    g_contend_stop.store(true, std::memory_order_relaxed);
    for (pthread_t& t : threads) pthread_join(t, nullptr);
    CHECK(s5.SampleNow(NowNs()) == 16);
    std::vector<ProcSampler::Record> r5(s5.count());
    CHECK(s5.CopyRecords(r5.data(), static_cast<uint32_t>(r5.size())) ==
          r5.size());
    // Presence only (H1 best-effort): the series must be sampled; magnitude
    // depends on the scheduler and is not asserted.
    CHECK(CountSeries(r5, ProcSampler::kVolCtx) >= 2);
    CHECK(CountSeries(r5, ProcSampler::kNonvolCtx) >= 2);
  }

  // --- hot-loop decoupling: loop iterations never read /proc ---------------
  {
    ProcSampler s6;
    CHECK(s6.Start(ProcSampler::Options{}));
    s6.SampleNow(NowNs());
    const uint64_t reads_before = s6.proc_reads();
    PerfSampler hot;  // stand-in for the loop/frame hot path
    for (int i = 0; i < 100000; ++i) {
      hot.Note(NowNs(), 1000, 3, 0);
    }
    CHECK(s6.proc_reads() == reads_before);  // 100k loops, zero /proc reads
    CHECK(s6.SampleNow(NowNs()) == 16);
    CHECK(s6.proc_reads() == reads_before + 1);
  }

  // --- ProcSampleThread: default 1Hz-style driver --------------------------
  {
    ProcSampler s7;
    ProcSampleThread t;
    CHECK(!t.Start(&s7, 1000000ull));  // sampler not Start()ed yet
    CHECK(s7.Start(ProcSampler::Options{}));
    const uint64_t reads_before = s7.proc_reads();
    CHECK(t.Start(&s7, 20000000ull));  // 20 ms interval (test speed-up)
    CHECK(t.running());
    SleepNs(250000000ull);  // ~12 periods; require >= 3 (wide margin)
    t.Stop();
    CHECK(!t.running());
    CHECK(s7.proc_reads() >= reads_before + 3);
  }

  return 0;
}
