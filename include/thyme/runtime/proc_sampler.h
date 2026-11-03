#pragma once
// proc_sampler.h — OBS-04: low-frequency process CPU / ctx-switch /
// page-fault / queue-depth sampling.
//
// Hot-path rule (roadmap §3.23): the loop/frame hot paths must not read /proc
// and must not call fprintf — every /proc read is concentrated in this
// component, driven by an independent low-frequency sampling thread
// (ProcSampleThread, 1Hz by default) or the host's periodic tick. Each sampling
// period reads /proc/self/stat + /proc/self/status once (+ best-effort
// /proc/self/io and /proc/self/fd), computes the delta against the previous
// period and writes it into a fixed ring buffer. The hot paths
// (PerfSampler::Note / EventThread::Dispatch / Loop) have zero coupling to
// this component; the proc_reads() counter proves it in regression tests.
//
// Queue depth: a subsystem registers a zero-overhead probe (a callback or an
// atomic counter pointer) through RegisterProbe / RegisterCounter, and the
// sampling thread reads the instantaneous value once per period — the
// registrant needs to add no code to its own hot path.
//
// Wire format: perf log type-5 blocks (see perf_log.h). The proc series record
// a delta (CPU already converted to ns; io/cpu_delay are per-period deltas) or
// an instantaneous value (rss/vsize/threads/fd_count/fd_limit); the probe
// series record instantaneous values.
//
// Zero allocation after Start; no exceptions. On non-Linux platforms Start()
// returns false (no /proc) and the remaining API stays safe for empty calls.
//
// Optional sources (all off by default): cgroup CPU/memory/IO (both v1 and v2),
// host PSI, host block-device/network counters, and the CPU time of up to 8
// registered threads. Each source is probed in Start(); an unsupported source
// simply registers no series, and when a requested source was not detected the
// sample stream still carries an `ext_caps` capability bit explaining "why
// there is no data". Enable with THYME_PROC_EXT= cgroup,psi,disk,net,threads
// (or all); see proc_ext.h.

#include "thyme/runtime/perf_log.h"
#include "thyme/runtime/proc_ext.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <pthread.h>
#include <time.h>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace thyme {

class ProcSampler {
 public:
  // Ring capacity == the writer's type-5 budget (perf_log.h), so a full ring
  // always fits one FlushNew.
  static constexpr std::size_t kCap = kProcSeriesMaxRecords;
  static constexpr uint32_t kMaxProbes = 8;

  // Process counter series ids (0..15 reserved). Values are per-period deltas.
  enum Series : uint8_t {
    kUtimeNs = 0,    // user CPU, ns (utime ticks * 1e9 / clk_tck)
    kStimeNs = 1,    // kernel CPU, ns
    kMinFlt = 2,     // minor page faults
    kMajFlt = 3,     // major page faults
    kVolCtx = 4,     // voluntary context switches
    kNonvolCtx = 5,  // nonvoluntary context switches
    // Runtime resource series. Deltas: io_* and cpu_delay_ms. Instant
    // values: rss_kb/vsize_kb/threads/fd_count/fd_limit.
    kRssKb = 6,            // VmRSS, KiB
    kVsizeKb = 7,          // VmSize, KiB
    kThreads = 8,          // NumThreads
    kFdCount = 9,          // open file descriptors
    kFdLimit = 10,         // RLIMIT_NOFILE soft limit
    kIoReadKb = 11,        // /proc/self/io read_bytes delta, KiB
    kIoWriteKb = 12,       // /proc/self/io write_bytes delta, KiB
    kIoReadSyscalls = 13,  // /proc/self/io syscr delta
    kIoWriteSyscalls = 14, // /proc/self/io syscw delta
    kCpuDelayMs = 15,      // delayacct_blkio_ticks delta, ms
  };
  // Registered queue-depth probes take series ids kProbeBase + slot; their
  // records carry instant values, not deltas.
  static constexpr uint8_t kProbeBase = 16;

  // Optional sources (off by default). `ext_caps` is the supported bitmask
  // recorded once per sample when anything extended is requested.
  using Options = ProcExtOptions;
  static constexpr uint8_t kExtCapsSeries = kProcExtCapsSeries;

  // Parses THYME_PROC_EXT ("cgroup,psi,disk,net,threads" or "all"; empty or
  // unset = everything off). Unknown tokens are ignored so a newer launcher
  // cannot break an older runtime.
  static Options OptionsFromEnv(const char* value = nullptr) {
    Options opt;
    const char* v = value;
    if (v == nullptr) v = std::getenv("THYME_PROC_EXT");
    if (v == nullptr) return opt;
    auto has = [v](const char* word) {
      const std::size_t n = std::strlen(word);
      for (const char* p = v; (p = std::strstr(p, word)) != nullptr; p += n) {
        const bool left = p == v || p[-1] == ',' || p[-1] == ' ';
        const bool right = p[n] == '\0' || p[n] == ',' || p[n] == ' ';
        if (left && right) return true;
      }
      return false;
    };
    if (has("all")) {
      opt.cgroup = opt.psi = opt.disk = opt.net = true;
      opt.thread_slots = kProcExtMaxThreads;
      return opt;
    }
    opt.cgroup = has("cgroup");
    opt.psi = has("psi");
    opt.disk = has("disk");
    opt.net = has("net");
    opt.thread_slots = has("threads") ? kProcExtMaxThreads : 0;
    return opt;
  }

  struct Record {
    uint32_t ts;     // PerfSampler::kTsShift units (16.384us), absolute axis
    uint32_t value;  // delta (counter series) or instant (gauge/probe)
    uint8_t series;
  };

  // Queue-depth probe: returns the current depth. Called only from the
  // sampling context, never from the registering subsystem's hot path.
  using ProbeFn = uint64_t (*)(void* ctx);

  ProcSampler() = default;
  ProcSampler(const ProcSampler&) = delete;
  ProcSampler& operator=(const ProcSampler&) = delete;

  // Takes the baseline snapshot (no records emitted). False when /proc is
  // unreadable (non-Linux, restricted procfs) — sampling is then a no-op.
  // The optional sources are probed here; unsupported ones are reported via
  // ext_supported() and simply contribute no series.
  bool Start(Options opt = OptionsFromEnv()) {
#if defined(__linux__)
    const long clk = sysconf(_SC_CLK_TCK);
    if (clk <= 0) return false;
    ns_per_tick_ = 1000000000ull / static_cast<uint64_t>(clk);
    const long page = sysconf(_SC_PAGESIZE);
    page_kb_ = page > 0 ? static_cast<uint64_t>(page) / 1024ULL : 4ULL;
    Snapshot s;
    if (!ReadSnapshot(&s)) return false;
    prev_ = s;
    ext_requested_ = ProcExtRequested(opt);
    ext_supported_ = ext_.Begin(opt, ns_per_tick_);
    started_ = true;
    return true;
#else
    (void)opt;
    return false;
#endif
  }
  bool started() const { return started_; }

  // Optional-source state (OBS-13): what was asked for vs. what the kernel
  // and cgroupfs actually provided. Threads are registered separately.
  uint8_t ext_requested() const { return ext_requested_; }
  uint8_t ext_supported() const { return ext_supported_; }
  int RegisterThread(int tid, const char* name) {
    return ext_.RegisterThread(tid, name);
  }
  uint8_t thread_count() const { return ext_.thread_count(); }

  // One sample period: reads /proc/self/{stat,status,io} once, appends one
  // record per proc series (delta for counters, instant for gauge series) and
  // one instant record per registered probe. Returns the number of records
  // appended (0 when not started or the read failed). Never allocates.
  uint32_t SampleNow(uint64_t now_ns) {
    if (!started_) return 0;
    Snapshot s;
    if (!ReadSnapshot(&s)) return 0;
    ++proc_reads_;
    const uint32_t ts =
        static_cast<uint32_t>(now_ns >> PerfSampler::kTsShift);
    uint32_t n = 0;
    Append(ts, kUtimeNs, Delta(s.utime_ticks, prev_.utime_ticks) * ns_per_tick_);
    Append(ts, kStimeNs, Delta(s.stime_ticks, prev_.stime_ticks) * ns_per_tick_);
    Append(ts, kMinFlt, Delta(s.minflt, prev_.minflt));
    Append(ts, kMajFlt, Delta(s.majflt, prev_.majflt));
    Append(ts, kVolCtx, Delta(s.vol_ctx, prev_.vol_ctx));
    Append(ts, kNonvolCtx, Delta(s.nonvol_ctx, prev_.nonvol_ctx));
    Append(ts, kRssKb, s.rss_pages * page_kb_);
    Append(ts, kVsizeKb, s.vsize_bytes / 1024ULL);
    Append(ts, kThreads, s.threads);
    Append(ts, kFdCount, s.fd_count);
    Append(ts, kFdLimit, s.fd_limit);
    Append(ts, kIoReadKb,
           Delta(s.io_read_bytes, prev_.io_read_bytes) / 1024ULL);
    Append(ts, kIoWriteKb,
           Delta(s.io_write_bytes, prev_.io_write_bytes) / 1024ULL);
    Append(ts, kIoReadSyscalls,
           Delta(s.io_read_syscalls, prev_.io_read_syscalls));
    Append(ts, kIoWriteSyscalls,
           Delta(s.io_write_syscalls, prev_.io_write_syscalls));
    Append(ts, kCpuDelayMs,
           Delta(s.cpu_delay_ticks, prev_.cpu_delay_ticks) * ns_per_tick_ /
               1000000ULL);
    n += 16;
    if (ext_requested_ != 0) {
      // Capability bitmask first: it tells an offline reader whether missing
      // extended series mean "not requested", "not supported" or "quiet".
      Append(ts, kExtCapsSeries, ext_supported_);
      ++n;
      ext_.Sample();
      const ProcExtValues& v = ext_.values();
      for (uint8_t i = 0; i < static_cast<uint8_t>(kExtCount); ++i) {
        if (!v.valid[i]) continue;
        Append(ts, ProcExtSeries(static_cast<ExtField>(i)), v.v[i]);
        ++n;
      }
      for (uint8_t i = 0; i < ext_.thread_count(); ++i) {
        if (!v.thread_valid[i]) continue;
        Append(ts, static_cast<uint8_t>(kProcExtThreadBase + i),
               v.thread_cpu_ns[i]);
        ++n;
      }
    }
    for (uint32_t i = 0; i < n_probes_; ++i) {
      Append(ts, static_cast<uint8_t>(kProbeBase + i),
             probes_[i].fn(probes_[i].ctx));
      ++n;
    }
    prev_ = s;
    return n;
  }

  // Register a queue-depth probe; returns the series id (>= kProbeBase), or -1
  // when full.
  int RegisterProbe(ProbeFn fn, void* ctx) {
    char name[kPerfSeriesNameRecordBytes];
    std::snprintf(name, sizeof(name), "probe_%u",
                  static_cast<unsigned>(n_probes_));
    return RegisterProbe(name, fn, ctx);
  }
  int RegisterProbe(const char* name, ProbeFn fn, void* ctx) {
    if (name == nullptr || name[0] == '\0' || fn == nullptr ||
        n_probes_ >= kMaxProbes) {
      return -1;
    }
    if (kProbeBase + n_probes_ > 0xFF) return -1;
    probes_[n_probes_].fn = fn;
    probes_[n_probes_].ctx = ctx;
    std::snprintf(probes_[n_probes_].name, sizeof(probes_[n_probes_].name),
                  "%s", name);
    return static_cast<int>(kProbeBase + n_probes_++);
  }
  // Convenience: probe reading an atomic counter owned by the subsystem.
  int RegisterCounter(const std::atomic<uint64_t>* counter) {
    char name[kPerfSeriesNameRecordBytes];
    std::snprintf(name, sizeof(name), "counter_%u",
                  static_cast<unsigned>(n_probes_));
    return RegisterCounter(name, counter);
  }
  int RegisterCounter(const char* name,
                      const std::atomic<uint64_t>* counter) {
    if (counter == nullptr) return -1;
    return RegisterProbe(name, &ReadAtomic,
                         const_cast<std::atomic<uint64_t>*>(counter));
  }

  // Number of successful /proc snapshots — the hot-path-decoupling regression
  // counter (loop iterations must never increment this).
  uint64_t proc_reads() const { return proc_reads_; }
  uint32_t probe_count() const { return n_probes_; }
  std::size_t count() const { return cnt_; }

  // Drops retained records and the FlushNew watermark (dump-then-reset,
  // OBS-03). The /proc delta baseline (prev_) and started_ are kept: per-
  // period deltas stay correct across a reset.
  void Reset() {
    idx_ = 0;
    cnt_ = 0;
    flushed_ = 0;
  }

  // Copies retained records oldest-first. Returns the record count.
  uint32_t CopyRecords(Record* out, uint32_t cap) const {
    const uint32_t n =
        cnt_ < cap ? static_cast<uint32_t>(cnt_) : cap;
    for (uint32_t i = 0; i < n; ++i) {
      const Rec& r = recs_[(idx_ - cnt_ + i) & (kCap - 1)];
      out[i].ts = r.ts;
      out[i].value = r.value;
      out[i].series = r.series;
    }
    return n;
  }

  // Periodic-dump variant (same watermark contract as PerfSampler::FlushNew):
  // one type-5 block with the records not covered by a previous FlushNew.
  // Records evicted from the ring before being flushed are silently gone.
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
      w.U8(0);  // rsvd
    }
    flushed_ = idx_;
  }

  void FlushMetadata(PerfLogWriter& w, uint8_t node_id) const {
    static const char* const kProcNames[] = {
        "utime_ns",     "stime_ns",           "minflt",
        "majflt",       "vol_ctx",            "nonvol_ctx",
        "rss_kb",       "vsize_kb",           "threads",
        "fd_count",     "fd_limit",           "io_read_kb",
        "io_write_kb",  "io_read_syscalls",   "io_write_syscalls",
        "cpu_delay_ms"};
    constexpr uint32_t kFixed = 16;
    const bool ext = ext_requested_ != 0;
    const uint32_t ext_fields = ext ? 1u : 0u;  // ext_caps
    const uint32_t threads = ext ? ext_.thread_count() : 0u;
    const uint32_t total = kFixed + ext_fields + ExtNameCount() + threads +
                           n_probes_;
    w.U8(kPerfSeriesNameBlockType);
    w.U8(node_id);
    w.U32(total);
    auto emit = [&](uint8_t series, const char* name) {
      w.U8(series);
      bool ended = false;
      for (std::size_t j = 0; j < kPerfSeriesNameBytes; ++j) {
        const char ch = ended ? '\0' : name[j];
        if (ch == '\0') ended = true;
        w.U8(static_cast<uint8_t>(ch));
      }
    };
    for (uint32_t i = 0; i < kFixed; ++i) {
      emit(static_cast<uint8_t>(i), kProcNames[i]);
    }
    if (ext) {
      emit(kExtCapsSeries, "ext_caps");
      for (uint8_t f = 0; f < static_cast<uint8_t>(kExtCount); ++f) {
        if (!ExtFieldNamed(f)) continue;
        emit(ProcExtSeries(static_cast<ExtField>(f)),
             kProcExtFieldNames[static_cast<std::size_t>(f)]);
      }
      for (uint8_t t = 0; t < threads; ++t) {
        emit(static_cast<uint8_t>(kProcExtThreadBase + t),
             ext_.thread_series_name(t));
      }
    }
    for (uint32_t p = 0; p < n_probes_; ++p) {
      emit(static_cast<uint8_t>(kProbeBase + p), probes_[p].name);
    }
  }

  // True when this extended field belongs to a supported source (names are
  // registered once at FlushMetadata time, so a whole source is in or out).
  bool ExtFieldNamed(uint8_t field) const {
    return (ext_supported_ &
            ProcExtFieldSource(static_cast<ExtField>(field))) != 0;
  }
  uint32_t ExtNameCount() const {
    uint32_t n = 0;
    for (uint8_t f = 0; f < static_cast<uint8_t>(kExtCount); ++f) {
      if (ExtFieldNamed(f)) ++n;
    }
    return n;
  }

 private:
  struct Snapshot {
    uint64_t utime_ticks = 0;
    uint64_t stime_ticks = 0;
    uint64_t minflt = 0;
    uint64_t majflt = 0;
    uint64_t vol_ctx = 0;
    uint64_t nonvol_ctx = 0;
    uint64_t rss_pages = 0;
    uint64_t vsize_bytes = 0;
    uint64_t threads = 0;
    uint64_t cpu_delay_ticks = 0;
    uint64_t io_read_bytes = 0;
    uint64_t io_write_bytes = 0;
    uint64_t io_read_syscalls = 0;
    uint64_t io_write_syscalls = 0;
    uint64_t fd_count = 0;
    uint64_t fd_limit = 0;
  };
  struct Rec {
    uint32_t ts;
    uint32_t value;
    uint8_t series;
    uint8_t pad[3];
  };
  struct Probe {
    ProbeFn fn = nullptr;
    void* ctx = nullptr;
    char name[kPerfSeriesNameRecordBytes]{};
  };

  static uint64_t Delta(uint64_t cur, uint64_t prev) {
    return cur >= prev ? cur - prev : 0;
  }
  static uint32_t Clamp32(uint64_t v) {
    return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(v);
  }
  static uint64_t ReadAtomic(void* ctx) {
    return static_cast<const std::atomic<uint64_t>*>(ctx)->load(
        std::memory_order_relaxed);
  }

  void Append(uint32_t ts, uint8_t series, uint64_t value) {
    Rec& r = recs_[idx_ & (kCap - 1)];
    r.ts = ts;
    r.value = Clamp32(value);
    r.series = series;
    ++idx_;
    if (cnt_ < kCap) ++cnt_;
  }

  static uint64_t ParseU64(const char* begin, const char* end) {
    uint64_t v = 0;
    for (const char* p = begin; p < end; ++p) {
      if (*p < '0' || *p > '9') break;
      v = v * 10 + static_cast<uint64_t>(*p - '0');
    }
    return v;
  }

#if defined(__linux__)
  // Reads a whole small procfs file into buf (NUL-terminated). False on any
  // I/O error.
  static bool ReadFile(const char* path, char* buf, std::size_t cap,
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
    *out_len = len;
    return true;
  }

  // /proc/self/stat: pid (comm) state ppid pgrp session tty_nr tpgid flags
  // minflt cminflt majflt cmajflt utime stime ... — comm may contain spaces
  // and parens, so fields are counted from the LAST ')': token i after it is
  // field i+3 (minflt = 7, majflt = 9, utime = 11, stime = 12).
  static bool ParseStat(const char* buf, Snapshot* s) {
    const char* p = std::strrchr(buf, ')');
    if (p == nullptr) return false;
    ++p;
    int tok = 0;
    bool got_minflt = false, got_majflt = false, got_utime = false,
         got_stime = false;
    while (tok <= 39) {
      while (*p == ' ') ++p;
      if (*p == '\0' || *p == '\n') break;
      const char* begin = p;
      while (*p != ' ' && *p != '\0' && *p != '\n') ++p;
      if (p == begin) break;
      switch (tok) {
        case 7: s->minflt = ParseU64(begin, p); got_minflt = true; break;
        case 9: s->majflt = ParseU64(begin, p); got_majflt = true; break;
        case 11: s->utime_ticks = ParseU64(begin, p); got_utime = true; break;
        case 12: s->stime_ticks = ParseU64(begin, p); got_stime = true; break;
        case 17: s->threads = ParseU64(begin, p); break;
        case 20: s->vsize_bytes = ParseU64(begin, p); break;
        case 21: s->rss_pages = ParseU64(begin, p); break;
        case 39: s->cpu_delay_ticks = ParseU64(begin, p); break;
        default: break;
      }
      ++tok;
    }
    return got_minflt && got_majflt && got_utime && got_stime;
  }

  // /proc/self/status: exact-key match at line start ("nonvoluntary_..." must
  // not match a "voluntary_..." substring search).
  static bool ParseStatus(const char* buf, Snapshot* s) {
    bool got_vol = false, got_nonvol = false;
    const char* line = buf;
    while (*line != '\0' && !(got_vol && got_nonvol)) {
      const char* end = std::strchr(line, '\n');
      if (end == nullptr) end = line + std::strlen(line);
      if (std::strncmp(line, "voluntary_ctxt_switches:", 24) == 0) {
        s->vol_ctx = ParseU64(line + 24, end);
        got_vol = true;
      } else if (std::strncmp(line, "nonvoluntary_ctxt_switches:", 27) == 0) {
        s->nonvol_ctx = ParseU64(line + 27, end);
        got_nonvol = true;
      }
      line = (*end == '\n') ? end + 1 : end;
    }
    return got_vol && got_nonvol;
  }

  static bool ParseIo(const char* buf, Snapshot* s) {
    const char* line = buf;
    while (*line != '\0') {
      const char* end = std::strchr(line, '\n');
      if (end == nullptr) end = line + std::strlen(line);
      if (std::strncmp(line, "read_bytes:", 11) == 0) {
        s->io_read_bytes = ParseU64(line + 11, end);
      } else if (std::strncmp(line, "write_bytes:", 12) == 0) {
        s->io_write_bytes = ParseU64(line + 12, end);
      } else if (std::strncmp(line, "syscr:", 6) == 0) {
        s->io_read_syscalls = ParseU64(line + 6, end);
      } else if (std::strncmp(line, "syscw:", 6) == 0) {
        s->io_write_syscalls = ParseU64(line + 6, end);
      }
      line = (*end == '\n') ? end + 1 : end;
    }
    return true;
  }

  static uint64_t FdCount() {
    DIR* dir = opendir("/proc/self/fd");
    if (dir == nullptr) return 0;
    uint64_t n = 0;
    while (struct dirent* e = readdir(dir)) {
      if (e->d_name[0] == '.' &&
          (e->d_name[1] == '\0' ||
           (e->d_name[1] == '.' && e->d_name[2] == '\0'))) {
        continue;
      }
      ++n;
    }
    closedir(dir);
    return n;
  }

  static uint64_t FdLimit() {
    struct rlimit lim {};
    if (getrlimit(RLIMIT_NOFILE, &lim) != 0) return 0;
    return lim.rlim_cur == RLIM_INFINITY ? 0xFFFFFFFFull : lim.rlim_cur;
  }

  bool ReadSnapshot(Snapshot* s) {
    char buf[4096];
    std::size_t len = 0;
    if (!ReadFile("/proc/self/stat", buf, sizeof(buf), &len)) return false;
    if (!ParseStat(buf, s)) return false;
    if (!ReadFile("/proc/self/status", buf, sizeof(buf), &len)) return false;
    if (!ParseStatus(buf, s)) return false;
    // /proc/self/io and /proc/self/fd are best-effort: restricted procfs or
    // seccomp must not disable the CPU/page-fault series.
    if (ReadFile("/proc/self/io", buf, sizeof(buf), &len)) {
      (void)ParseIo(buf, s);
    }
    s->fd_count = FdCount();
    s->fd_limit = FdLimit();
    return true;
  }
#else
  bool ReadSnapshot(Snapshot*) { return false; }
#endif

  Probe probes_[kMaxProbes]{};
  uint32_t n_probes_ = 0;
  Rec recs_[kCap]{};
  uint64_t idx_ = 0;
  std::size_t cnt_ = 0;
  uint64_t flushed_ = 0;      // FlushNew watermark (absolute record index)
  uint64_t proc_reads_ = 0;   // successful /proc snapshots
  uint64_t ns_per_tick_ = 0;
  uint64_t page_kb_ = 4;      // cached _SC_PAGESIZE/1024 (RSS is in pages)
  ProcExtSampler ext_;        // optional sources (off unless requested)
  uint8_t ext_requested_ = 0;
  uint8_t ext_supported_ = 0;
  Snapshot prev_{};
  bool started_ = false;
};

// Default OBS-04 driver: a dedicated low-frequency thread sampling `sampler`
// every interval_ns (roadmap budget: <=1 Hz, <=0.1% of one core — one base
// sample is two small /proc reads, ~10s of µs, so 1 Hz is ~0.005%). Optional
// OBS-13 sources (THYME_PROC_EXT) add a handful of cgroup/PSI/diskstats/netdev
// reads per period, still well inside the budget. The sampler itself stays
// thread-free and can equally be driven from a host tick.
class ProcSampleThread {
 public:
  ProcSampleThread() = default;
  ~ProcSampleThread() { Stop(); }
  ProcSampleThread(const ProcSampleThread&) = delete;
  ProcSampleThread& operator=(const ProcSampleThread&) = delete;

  // The sampler must be Start()ed first. False on bad args / pthread failure.
  bool Start(ProcSampler* sampler, uint64_t interval_ns) {
    if (sampler == nullptr || !sampler->started() || interval_ns == 0 ||
        started_) {
      return false;
    }
    sampler_ = sampler;
    interval_ns_ = interval_ns;
    stop_.store(false, std::memory_order_relaxed);
    if (pthread_create(&thread_, nullptr, &Trampoline, this) != 0) {
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
  }

  bool running() const { return started_; }

 private:
  static void* Trampoline(void* arg) {
    static_cast<ProcSampleThread*>(arg)->Run();
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
      struct timespec now {};
      clock_gettime(CLOCK_MONOTONIC, &now);
      sampler_->SampleNow(static_cast<uint64_t>(now.tv_sec) * 1000000000ull +
                          static_cast<uint64_t>(now.tv_nsec));
    }
  }

  ProcSampler* sampler_ = nullptr;
  uint64_t interval_ns_ = 0;
  pthread_t thread_{};
  std::atomic<bool> stop_{false};
  bool started_ = false;
};

}  // namespace thyme
