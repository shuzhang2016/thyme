// test_perf_dump_on_crash.cpp — review P1: PerfLogDumper appends new samples
// to an O_APPEND file on an interval, so a SIGKILL loses at most one
// interval. The child writes + dumps, gets killed mid-run, and the parent
// must find a readable file with the dumped samples; a watermark flush writes
// each sample exactly once.
#include "thyme/check.h"
#include "thyme/runtime/perf_dump.h"
#include "thyme/runtime/perf_log.h"

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
  uint8_t buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out.insert(out.end(), buf, buf + n);
  }
  std::fclose(f);
  return out;
}

}  // namespace

int main() {
  char path[96];
  std::snprintf(path, sizeof(path), "/tmp/c9perf_%d.bin",
                static_cast<int>(getpid()));
  std::remove(path);

  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    PerfSampler sampler;
    PerfWorkLog work;
    PerfLogDumper dumper;
    CHECK(dumper.Open(path, 50000000ULL));  // 50 ms interval
    const uint64_t base = NowNs();
    for (uint64_t i = 0; i < 10; ++i) {
      sampler.Note(base + i * 1000000ULL, 1000 + i, 3, 0);
      work.Note(base + i * 1000000ULL, 500, 0, 0, 0);
      dumper.DumpIfDue(base + i * 1000000ULL, sampler, &work, 3);
      // note: DumpIfDue uses the caller's clock; interval 50 ms vs 1 ms
      // steps -> dumps at i=0 only. Force two more real-time dumps:
      struct timespec ts {0, 60000000};
      nanosleep(&ts, nullptr);
      dumper.DumpIfDue(NowNs(), sampler, &work, 3);
    }
    raise(SIGKILL);  // crash without any exit-path flush
    _exit(1);
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);

  // The file must be readable and contain the samples dumped before death.
  const std::vector<uint8_t> data = ReadFile(path);
  CHECK(data.size() > 4);
  CHECK(std::memcmp(data.data(), "T9P1", 4) == 0);
  // Walk blocks: count sample records (type 1) for node 3.
  std::size_t p = 4;
  uint32_t sample_rows = 0;
  uint32_t work_rows = 0;
  while (p + 6 <= data.size()) {
    const uint8_t typ = data[p];
    const uint32_t cnt = ReadU32(&data[p + 2]);
    p += 6;
    if (typ == 1) {
      for (uint32_t i = 0; i < cnt && p + 10 <= data.size(); ++i, p += 10) {
        ++sample_rows;
      }
    } else if (typ == 2) {
      p += 20 * cnt;
    } else if (typ == 3) {
      for (uint32_t i = 0; i < cnt && p + 10 <= data.size(); ++i, p += 10) {
        ++work_rows;
      }
    } else if (typ == 4) {
      p += 8 * cnt;  // epoch blocks (P2)
    } else if (typ == 6) {
      p += kPerfHistRecordBytes * cnt;  // histogram blocks
    } else if (typ == kPerfClockAnchorBlockType) {
      p += kPerfClockAnchorRecordBytes * cnt;
    } else {
      break;  // unknown/partial tail after a crash: stop, not corrupt
    }
  }
  CHECK(sample_rows == 10);  // all 10 samples made it despite the SIGKILL
  CHECK(work_rows == 10);

  // FlushNew watermark: dumping again adds nothing; a second dumper pass over
  // the same sampler writes each sample exactly once.
  {
    std::remove(path);
    PerfSampler s;
    s.Note(1000, 10, 1, 0);
    s.Note(2000, 20, 1, 0);
    PerfLogDumper d;
    CHECK(d.Open(path, 0));
    CHECK(d.DumpIfDue(0, s, nullptr, 1));
    CHECK(!d.DumpIfDue(1, s, nullptr, 1));  // nothing new
    s.Note(3000, 30, 1, 0);
    CHECK(d.DumpIfDue(2, s, nullptr, 1));  // one new sample
    d.Close();
    const std::vector<uint8_t> d2 = ReadFile(path);
    std::size_t q = 4;
    uint32_t rows = 0;
    while (q + 6 <= d2.size()) {
      const uint8_t typ = d2[q];
      const uint32_t cnt = ReadU32(&d2[q + 2]);
      q += 6;
      if (typ == 1) {
        rows += cnt;
        q += 10 * cnt;
      } else if (typ == 2) {
        q += 20 * cnt;
      } else if (typ == 4) {
        q += 8 * cnt;  // epoch blocks (P2)
      } else if (typ == 6) {
        q += kPerfHistRecordBytes * cnt;  // histogram blocks
      } else if (typ == kPerfClockAnchorBlockType) {
        q += kPerfClockAnchorRecordBytes * cnt;
      } else {
        break;
      }
    }
    CHECK(rows == 3);  // 2 + 1, no duplicates
  }

  // P2: epoch blocks let a wrapped u32 ts expand back to a monotonic u64
  // axis. Note two samples straddling the 2^32-unit wrap, one dump apart.
  {
    std::remove(path);
    PerfSampler s;
    PerfLogDumper d;
    CHECK(d.Open(path, 0));
    const uint64_t wrap_units = 1ull << 32;
    const uint64_t before_ns = (wrap_units - 2) << 14;  // ts low = FFFFFFFE
    const uint64_t after_ns = (wrap_units + 3) << 14;   // ts low = 3 (wrapped)
    s.Note(before_ns, 10, 1, 0);
    CHECK(d.DumpIfDue(before_ns, s, nullptr, 1));
    s.Note(after_ns, 20, 1, 0);
    CHECK(d.DumpIfDue(after_ns, s, nullptr, 1));
    d.Close();

    const std::vector<uint8_t> d3 = ReadFile(path);
    // Walk: expect type-4 epoch + type-1 sample block, twice; expand lows
    // with the parser's rule (bump epoch when the low regresses).
    std::size_t q = 4;
    uint64_t epoch = 0, last_low = 0;
    uint64_t expanded[2] = {0, 0};
    int n_samples = 0;
    while (q + 6 <= d3.size()) {
      const uint8_t typ = d3[q];
      const uint32_t cnt = ReadU32(&d3[q + 2]);
      q += 6;
      if (typ == 4) {
        for (uint32_t i = 0; i < cnt; ++i, q += 8) {
          epoch = 0;
          for (int b = 0; b < 8; ++b) {
            epoch |= static_cast<uint64_t>(d3[q + b]) << (8 * b);
          }
          last_low = 0;
        }
      } else if (typ == 1) {
        for (uint32_t i = 0; i < cnt; ++i, q += 10) {
          const uint32_t low = ReadU32(&d3[q]);
          if (epoch != 0 && last_low != 0 && low < last_low) {
            epoch += 1ull << 32;
          }
          last_low = low;
          CHECK(n_samples < 2);
          expanded[n_samples++] = epoch + low;
        }
      } else if (typ == 2) {
        q += 20 * cnt;
      } else if (typ == 6) {
        q += kPerfHistRecordBytes * cnt;  // histogram blocks
      } else if (typ == kPerfClockAnchorBlockType) {
        q += kPerfClockAnchorRecordBytes * cnt;
      } else {
        break;
      }
    }
    CHECK(n_samples == 2);
    CHECK(expanded[0] == wrap_units - 2);
    CHECK(expanded[1] == wrap_units + 3);  // still monotonic after wrap expansion
    CHECK(expanded[1] > expanded[0]);
  }

  std::remove(path);
  return 0;
}
