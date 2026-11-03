#pragma once
// host_sampler.h — OBS-14: machine-level resource sampling.
//
// ProcSampler answers "what is this process doing". This answers "what is the
// box doing": aggregate and per-core CPU from /proc/stat, load average,
// MemTotal/MemAvailable, and the system-wide open-fd table
// (/proc/sys/fs/file-nr). One sampler per machine, owned by the supervisor:
// the counters are host-global, so running it per role would just duplicate
// the same reads at slightly different instants.
//
// Wire format: the same type-5 sys blocks and type-8 name blocks as
// ProcSampler, written to `<prefix>_host_<pid>.bin` so `thyme_log_merge`
// merges it into the same timeline. Series ids (uint8):
//
//   0  host_cpu_user_ns        aggregate user+nice, delta
//   1  host_cpu_system_ns      aggregate system, delta
//   2  host_cpu_idle_ns        aggregate idle, delta
//   3  host_cpu_iowait_ns      aggregate iowait, delta
//   4  host_cpu_irq_ns         aggregate irq+softirq, delta
//   5  host_cpu_steal_ns       aggregate steal, delta
//   6..8   host_load{1,5,15}_milli   instant (load * 1000)
//   9  host_mem_total_kb       instant
//   10 host_mem_available_kb   instant
//   11 host_file_nr_allocated  instant (system-wide fds in use)
//   12 host_file_nr_max        instant (0xFFFFFFFF = unlimited)
//   16+i  core<i>_busy_ns      per-core busy (user+nice+system+irq+softirq+
//                              steal; idle/iowait excluded), delta
//
// Per-core series are capped at kHostMaxCpus (64). A machine with more cores
// keeps the aggregate rows and the first 64 cores; the cap is reported once
// through the name block (only names that are actually emitted appear).
//
// Container caveat: /proc/stat, /proc/loadavg and /proc/sys/fs/file-nr are
// host-global unless the runtime virtualises them; /proc/self/... remains the
// process-scoped source. Documented, not silently reinterpreted.
//
// Zero allocation in SampleNow(); the dump thread allocates one writer at
// Start. No exceptions; non-Linux Start() returns false.

#include "thyme/runtime/perf_log.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <pthread.h>
#include <time.h>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace thyme {

class HostSampler {
 public:
  static constexpr std::size_t kCap = 1024;      // records per ring
  static constexpr uint8_t kMaxCpus = 64;        // per-core series bound
  static constexpr uint8_t kCoreBase = 16;       // series id of core 0
  static constexpr uint32_t kUnlimited = 0xFFFFFFFFu;

  enum Series : uint8_t {
    kCpuUserNs = 0,
    kCpuSystemNs = 1,
    kCpuIdleNs = 2,
    kCpuIowaitNs = 3,
    kCpuIrqNs = 4,
    kCpuStealNs = 5,
    kLoad1Milli = 6,
    kLoad5Milli = 7,
    kLoad15Milli = 8,
    kMemTotalKb = 9,
    kMemAvailableKb = 10,
    kFileNrAllocated = 11,
    kFileNrMax = 12,
  };
  static constexpr uint8_t kFixedNames = 13;

  struct Record {
    uint32_t ts;     // PerfSampler::kTsShift units (16.384us), absolute
    uint32_t value;  // delta (CPU) or instant (load/mem/fd), clamped
    uint8_t series;
  };

  struct Paths {
    const char* stat = "/proc/stat";
    const char* loadavg = "/proc/loadavg";
    const char* meminfo = "/proc/meminfo";
    const char* file_nr = "/proc/sys/fs/file-nr";
  };

  HostSampler() = default;
  HostSampler(const HostSampler&) = delete;
  HostSampler& operator=(const HostSampler&) = delete;

  // Takes the baseline. False when /proc/stat is unreadable (non-Linux).
  bool Start() { return Start(Paths{}); }
  bool Start(const Paths& paths) {
#if defined(__linux__)
    paths_ = paths;
    const long clk = sysconf(_SC_CLK_TCK);
    if (clk <= 0) return false;
    ns_per_tick_ = 1000000000ull / static_cast<uint64_t>(clk);
    Snapshot s;
    if (!ReadStat(&s)) return false;
    prev_ = s;
    started_ = true;
    return true;
#else
    (void)paths;
    return false;
#endif
  }
  bool started() const { return started_; }
  uint8_t cores() const { return prev_.cores; }

  uint32_t SampleNow(uint64_t now_ns) {
    if (!started_) return 0;
    Snapshot s;
    if (!ReadStat(&s)) return 0;
    const uint32_t ts = static_cast<uint32_t>(now_ns >> PerfSampler::kTsShift);
    uint32_t n = 0;
    Append(ts, kCpuUserNs, Delta(s.user, prev_.user) * ns_per_tick_);
    Append(ts, kCpuSystemNs, Delta(s.system, prev_.system) * ns_per_tick_);
    Append(ts, kCpuIdleNs, Delta(s.idle, prev_.idle) * ns_per_tick_);
    Append(ts, kCpuIowaitNs, Delta(s.iowait, prev_.iowait) * ns_per_tick_);
    Append(ts, kCpuIrqNs, Delta(s.irq, prev_.irq) * ns_per_tick_);
    Append(ts, kCpuStealNs, Delta(s.steal, prev_.steal) * ns_per_tick_);
    n += 6;
    uint64_t loads[3] = {};
    if (ReadLoads(paths_.loadavg, loads)) {
      Append(ts, kLoad1Milli, loads[0]);
      Append(ts, kLoad5Milli, loads[1]);
      Append(ts, kLoad15Milli, loads[2]);
      n += 3;
    }
    uint64_t total = 0, avail = 0;
    if (ReadMeminfo(paths_.meminfo, &total, &avail)) {
      Append(ts, kMemTotalKb, total);
      Append(ts, kMemAvailableKb, avail);
      n += 2;
    }
    uint64_t allocated = 0, max = 0;
    if (ReadFileNr(paths_.file_nr, &allocated, &max)) {
      Append(ts, kFileNrAllocated, allocated);
      Append(ts, kFileNrMax, max >= (1ull << 60) ? kUnlimited : max);
      n += 2;
    }
    const uint8_t cores = s.cores < kMaxCpus ? s.cores : kMaxCpus;
    for (uint8_t i = 0; i < cores; ++i) {
      Append(ts, static_cast<uint8_t>(kCoreBase + i),
             Delta(s.core_busy[i], prev_.core_busy[i]) * ns_per_tick_);
      ++n;
    }
    prev_ = s;
    return n;
  }

  // Series-name block: fixed rows plus one name per observed core.
  void FlushMetadata(PerfLogWriter& w, uint8_t node_id) const {
    static const char* const kNames[] = {
        "host_cpu_user_ns",  "host_cpu_system_ns",   "host_cpu_idle_ns",
        "host_cpu_iowait_ns", "host_cpu_irq_ns",     "host_cpu_steal_ns",
        "host_load1_milli",  "host_load5_milli",     "host_load15_milli",
        "host_mem_total_kb", "host_mem_available_kb", "host_file_nr_allocated",
        "host_file_nr_max"};
    const uint8_t cores = prev_.cores < kMaxCpus ? prev_.cores : kMaxCpus;
    w.U8(kPerfSeriesNameBlockType);
    w.U8(node_id);
    w.U32(kFixedNames + cores);
    for (uint8_t i = 0; i < kFixedNames; ++i) EmitName(w, i, kNames[i]);
    for (uint8_t i = 0; i < cores; ++i) {
      char name[kPerfSeriesNameBytes + 1];
      std::snprintf(name, sizeof(name), "core%u_busy_ns",
                    static_cast<unsigned>(i));
      EmitName(w, static_cast<uint8_t>(kCoreBase + i), name);
    }
  }

  void FlushNew(PerfLogWriter& w, uint8_t node_id) {
    const uint64_t avail = idx_ - flushed_;
    if (avail == 0) return;
    const uint64_t begin = avail > kCap ? idx_ - kCap : flushed_;
    w.U8(5);
    w.U8(node_id);
    w.U32(static_cast<uint32_t>(idx_ - begin));
    for (uint64_t i = begin; i < idx_; ++i) {
      const Rec& r = recs_[i & (kCap - 1)];
      w.U32(r.ts);
      w.U32(r.value);
      w.U8(r.series);
      w.U8(0);
    }
    flushed_ = idx_;
  }

  std::size_t count() const { return cnt_; }

  // Copies retained records oldest-first (tests / hosts that render directly).
  uint32_t CopyRecords(Record* out, uint32_t cap) const {
    const uint32_t n = cnt_ < cap ? static_cast<uint32_t>(cnt_) : cap;
    for (uint32_t i = 0; i < n; ++i) {
      const Rec& r = recs_[(idx_ - cnt_ + i) & (kCap - 1)];
      out[i].ts = r.ts;
      out[i].value = r.value;
      out[i].series = r.series;
    }
    return n;
  }

 private:
  struct Snapshot {
    uint64_t user = 0, system = 0, idle = 0, iowait = 0, irq = 0, steal = 0;
    uint64_t core_busy[kMaxCpus]{};
    uint8_t cores = 0;
  };
  struct Rec {
    uint32_t ts;
    uint32_t value;
    uint8_t series;
    uint8_t pad[3];
  };

  static void EmitName(PerfLogWriter& w, uint8_t series, const char* name) {
    w.U8(series);
    bool ended = false;
    for (std::size_t j = 0; j < kPerfSeriesNameBytes; ++j) {
      const char ch = ended ? '\0' : name[j];
      if (ch == '\0') ended = true;
      w.U8(static_cast<uint8_t>(ch));
    }
  }

  static uint64_t Delta(uint64_t cur, uint64_t prev) {
    return cur >= prev ? cur - prev : 0;
  }
  static uint32_t Clamp32(uint64_t v) {
    return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(v);
  }

  void Append(uint32_t ts, uint8_t series, uint64_t value) {
    Rec& r = recs_[idx_ & (kCap - 1)];
    r.ts = ts;
    r.value = Clamp32(value);
    r.series = series;
    ++idx_;
    if (cnt_ < kCap) ++cnt_;
  }

#if defined(__linux__)
  static bool ReadWhole(const char* path, char* buf, std::size_t cap,
                        std::size_t* out_len) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    std::size_t len = 0;
    for (;;) {
      const ssize_t n = read(fd, buf + len, cap - 1 - len);
      if (n < 0) {
        if (errno == EINTR) continue;
        close(fd);
        return false;
      }
      if (n == 0) break;
      len += static_cast<std::size_t>(n);
      if (len >= cap - 1) break;
    }
    close(fd);
    buf[len] = '\0';
    if (out_len != nullptr) *out_len = len;
    return true;
  }

  static uint64_t ParseU64(const char* begin, const char* end) {
    uint64_t v = 0;
    bool seen = false;
    for (const char* p = begin; p < end; ++p) {
      if (*p < '0' || *p > '9') {
        if (seen) break;
        continue;
      }
      seen = true;
      v = v * 10 + static_cast<uint64_t>(*p - '0');
    }
    return seen ? v : 0;
  }

  // /proc/stat: "cpu  user nice system idle iowait irq softirq steal ..."
  // followed by "cpuN ..." lines. user time includes nice; irq merges
  // softirq; per-core busy excludes idle and iowait.
  bool ReadStat(Snapshot* s) {
    char buf[8192];
    std::size_t len = 0;
    if (!ReadWhole(paths_.stat, buf, sizeof(buf), &len)) return false;
    bool got_total = false;
    for (const char* line = buf; *line != '\0';) {
      const char* end = std::strchr(line, '\n');
      if (end == nullptr) end = line + std::strlen(line);
      if (std::strncmp(line, "cpu", 3) == 0) {
        uint64_t f[10] = {};
        int n = 0;
        const bool is_core = line[3] >= '0' && line[3] <= '9';
        const char* p = line + 3;
        if (is_core) {
          while (p < end && *p >= '0' && *p <= '9') ++p;  // core index digits
        }
        while (n < 10 && p < end) {
          while (p < end && (*p == ' ' || *p == '\t')) ++p;
          if (p >= end) break;
          const char* b = p;
          while (p < end && *p != ' ' && *p != '\t') ++p;
          f[n++] = ParseU64(b, p);
        }
        if (n >= 8 && line[3] == ' ') {  // aggregate row
          s->user = f[0] + f[1];
          s->system = f[2];
          s->idle = f[3];
          s->iowait = f[4];
          s->irq = f[5] + f[6];
          s->steal = n > 7 ? f[7] : 0;
          got_total = true;
        } else if (n >= 8 && line[3] >= '0' && line[3] <= '9') {
          // "cpuN ...": the core index shares the token with the name, and
          // the field list starts after its digits.
          const char* d = line + 3;
          const char* de = d;
          while (de < end && *de >= '0' && *de <= '9') ++de;
          const uint8_t core = static_cast<uint8_t>(ParseU64(d, de));
          if (core < kMaxCpus) {
            s->core_busy[core] = f[0] + f[1] + f[2] + f[5] + f[6] +
                                 (n > 7 ? f[7] : 0);
            if (static_cast<uint8_t>(core + 1) > s->cores) {
              s->cores = static_cast<uint8_t>(core + 1);
            }
          }
        }
      }
      line = (*end == '\n') ? end + 1 : end;
    }
    return got_total;
  }

  // /proc/loadavg: "1.50 0.25 0.00 2/123 4567" -> milli-loads without floats.
  static bool ReadLoads(const char* path, uint64_t out[3]) {
    char buf[128];
    if (!ReadWhole(path, buf, sizeof(buf), nullptr)) return false;
    const char* p = buf;
    for (int i = 0; i < 3; ++i) {
      while (*p == ' ' || *p == '\t') ++p;
      if (*p < '0' || *p > '9') return false;
      uint64_t whole = 0, frac = 0, scale = 1;
      while (*p >= '0' && *p <= '9') whole = whole * 10 + (*p++ - '0');
      if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9' && scale < 1000) {
          frac = frac * 10 + (*p++ - '0');
          scale *= 10;
        }
      }
      while (scale < 1000) {
        frac *= 10;
        scale *= 10;
      }
      out[i] = whole * 1000 + frac;
    }
    return true;
  }

  static bool ReadMeminfo(const char* path, uint64_t* total, uint64_t* avail) {
    char buf[4096];
    if (!ReadWhole(path, buf, sizeof(buf), nullptr)) return false;
    bool got_total = false, got_avail = false;
    for (const char* line = buf; *line != '\0';) {
      const char* end = std::strchr(line, '\n');
      if (end == nullptr) end = line + std::strlen(line);
      if (std::strncmp(line, "MemTotal:", 9) == 0) {
        *total = ParseU64(line + 9, end);
        got_total = true;
      } else if (std::strncmp(line, "MemAvailable:", 13) == 0) {
        *avail = ParseU64(line + 13, end);
        got_avail = true;
      } else if (!got_avail && std::strncmp(line, "MemFree:", 8) == 0) {
        *avail = ParseU64(line + 8, end);  // pre-3.14 fallback
        got_avail = true;
      }
      line = (*end == '\n') ? end + 1 : end;
    }
    return got_total;
  }

  static bool ReadFileNr(const char* path, uint64_t* allocated, uint64_t* max) {
    char buf[128];
    if (!ReadWhole(path, buf, sizeof(buf), nullptr)) return false;
    uint64_t f[3] = {};
    int n = 0;
    const char* p = buf;
    while (n < 3) {
      while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
      if (*p == '\0') break;
      const char* b = p;
      while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n') ++p;
      f[n++] = ParseU64(b, p);
    }
    if (n < 3) return false;
    *allocated = f[0];
    *max = f[2];
    return true;
  }
#else
  bool ReadStat(Snapshot*) { return false; }
  static bool ReadLoads(const char*, uint64_t[3]) { return false; }
  static bool ReadMeminfo(const char*, uint64_t*, uint64_t*) { return false; }
  static bool ReadFileNr(const char*, uint64_t*, uint64_t*) { return false; }
#endif

  Paths paths_{};
  Snapshot prev_{};
  Rec recs_[kCap]{};
  uint64_t idx_ = 0;
  std::size_t cnt_ = 0;
  uint64_t flushed_ = 0;
  uint64_t ns_per_tick_ = 0;
  bool started_ = false;
};

// Writes host counters to `<path>` every interval_ns (default 1 Hz) and on
// Stop(). Owned by the supervisor: one instance per machine.
class HostDumpThread {
 public:
  HostDumpThread() = default;
  ~HostDumpThread() { Stop(); }
  HostDumpThread(const HostDumpThread&) = delete;
  HostDumpThread& operator=(const HostDumpThread&) = delete;

  bool Start(const char* path, uint64_t interval_ns,
             uint64_t manifest_hash = 0, uint16_t manifest_version = 0) {
    if (path == nullptr || path[0] == '\0' || interval_ns == 0 || started_) {
      return false;
    }
    if (!sampler_.Start()) return false;
    // One name per CPU: 13 fixed + up to 64 cores.
    writer_ = PerfLogWriter(0, 0, HostSampler::kFixedNames + HostSampler::kMaxCpus);
    if (!writer_.ok()) return false;
    file_ = std::fopen(path, "wb");
    if (file_ == nullptr) return false;
    std::fwrite("T9P1", 1, 4, file_);
    if (manifest_hash != 0) {
      PerfLogWriter mw(0, 0);
      mw.U8(kPerfManifestBlockType);
      mw.U8(0);
      mw.U32(1);
      mw.U16(manifest_version);
      mw.U16(0);
      mw.U64(manifest_hash);
      std::fwrite(mw.data() + 4, 1, mw.size() - 4, file_);
    }
    interval_ns_ = interval_ns;
    stop_.store(false, std::memory_order_relaxed);
    if (pthread_create(&thread_, nullptr, &Trampoline, this) != 0) {
      std::fclose(file_);
      file_ = nullptr;
      return false;
    }
    started_ = true;
    return true;
  }

  void Stop() {
    if (!started_) return;
    stop_.store(true, std::memory_order_release);
    pthread_join(thread_, nullptr);
    started_ = false;
    Dump(NowNs());
    if (file_ != nullptr) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }

  bool running() const { return started_; }
  const HostSampler& sampler() const { return sampler_; }

 private:
  static uint64_t NowNs() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
  }
  static void* Trampoline(void* arg) {
    static_cast<HostDumpThread*>(arg)->Run();
    return nullptr;
  }
  void Run() {
    struct timespec ts {};
    ts.tv_sec = static_cast<time_t>(interval_ns_ / 1000000000ull);
    ts.tv_nsec = static_cast<long>(interval_ns_ % 1000000000ull);
    while (!stop_.load(std::memory_order_acquire)) {
      struct timespec t = ts;
      while (nanosleep(&t, &t) != 0) {
        if (stop_.load(std::memory_order_acquire)) return;
      }
      if (stop_.load(std::memory_order_acquire)) return;
      Dump(NowNs());
    }
  }
  void Dump(uint64_t now_ns) {
    if (file_ == nullptr) return;
    sampler_.SampleNow(now_ns);
    writer_.Rewind();
    // Names are static per run: write them once per file generation.
    if (!names_written_) {
      sampler_.FlushMetadata(writer_, 0);
      names_written_ = true;
    }
    sampler_.FlushNew(writer_, 0);
    std::fwrite(writer_.data() + 4, 1, writer_.size() - 4, file_);
    std::fflush(file_);
  }

  HostSampler sampler_{};
  PerfLogWriter writer_{};
  FILE* file_ = nullptr;
  uint64_t interval_ns_ = 0;
  pthread_t thread_{};
  std::atomic<bool> stop_{false};
  bool started_ = false;
  bool names_written_ = false;
};

}  // namespace thyme
