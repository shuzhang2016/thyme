#pragma once
// proc_ext.h — optional (off-by-default) runtime sources for ProcSampler:
// cgroup CPU/memory/IO, host PSI, host block-device and network counters, and
// per-thread CPU time.
//
// Why a separate header: the base ProcSampler only reads /proc/self/stat,
// status, io and fd — files that exist on every Linux. Everything here is
// kernel-config and deployment dependent, so each source is probed at
// Start() and either sampled or reported unsupported; nothing pretends a
// missing file is a zero reading.
//
//   cgroup   cgroup v2 (unified: cpu.stat / memory.current / memory.events /
//            io.stat) or cgroup v1 (cpuacct.usage, cpu.stat nr_throttled +
//            throttled_time, memory.usage_in_bytes / limit_in_bytes /
//            oom_control, blkio.throttle.io_service_bytes). Both may be
//            absent (no cgroupfs mounted, no permission, kernel without the
//            controllers) — then the source is simply unsupported.
//   psi      /proc/pressure/{cpu,memory,io}: needs CONFIG_PSI and kernel
//            >= 4.20. Host-wide, not per-cgroup (a cgroup-lifetime signal is
//            deliberately out of scope for a 1 Hz process sampler).
//   disk     /proc/diskstats, whole block devices only (those with
//            /sys/block/<name>). Namespaced containers see the host digits.
//   net      /proc/net/dev, every interface except lo. Same namespace caveat.
//   threads  /proc/self/task/<tid>/stat utime+stime for up to 8 threads
//            registered by the application (bounded by design: no dynamic
//            per-thread series).
//
// All values are u32 on the wire: gauges are KiB / counts, counter sources
// are per-sample-period deltas. A 1 s period cannot overflow any of them at
// plausible rates (PSI total and thread CPU are microseconds/ns; 4.29 s of
// CPU per period would be needed).

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace thyme {

// One bit per source; also the value of the `ext_caps` series.
enum ProcExtSource : uint8_t {
  kExtSrcCgroup = 1u << 0,
  kExtSrcPsi = 1u << 1,
  kExtSrcDisk = 1u << 2,
  kExtSrcNet = 1u << 3,
  kExtSrcThreads = 1u << 4,
};

// Extended series ids live above the probe range (16..23) with room to grow:
// 63 carries the capability bitmask, 64..80 the fixed fields, 96..103 the
// registered threads.
inline constexpr uint8_t kProcExtCapsSeries = 63;
inline constexpr uint8_t kProcExtBase = 64;
inline constexpr uint8_t kProcExtMaxThreads = 8;
inline constexpr uint8_t kProcExtThreadBase = 96;

enum ExtField : uint8_t {
  kExtCgroupCpuUsage = 0,   // delta, us
  kExtCgroupCpuThrottled,   // delta, us
  kExtCgroupCpuNrThrottled, // delta, periods
  kExtCgroupMemCurrent,     // instant, KiB
  kExtCgroupMemMax,         // instant, KiB (0xFFFFFFFF = unlimited)
  kExtCgroupOomEvents,      // delta, events
  kExtCgroupIoRead,         // delta, KiB
  kExtCgroupIoWrite,        // delta, KiB
  kExtPsiCpuSome,           // delta, us
  kExtPsiMemSome,           // delta, us
  kExtPsiMemFull,           // delta, us
  kExtPsiIoSome,            // delta, us
  kExtPsiIoFull,            // delta, us
  kExtDiskRead,             // delta, KiB
  kExtDiskWrite,            // delta, KiB
  kExtNetRx,                // delta, KiB
  kExtNetTx,                // delta, KiB
  kExtCount
};

struct ProcExtPaths {
  const char* cgroup_file = "/proc/self/cgroup";
  const char* cgroup_root = "/sys/fs/cgroup";
  const char* pressure_dir = "/proc/pressure";
  const char* diskstats = "/proc/diskstats";
  const char* netdev = "/proc/net/dev";
  const char* sys_block = "/sys/block";  // whole-device filter for diskstats
};

struct ProcExtOptions {
  bool cgroup = false;
  bool psi = false;
  bool disk = false;
  bool net = false;
  uint8_t thread_slots = 0;  // 0..kProcExtMaxThreads
  // Skip partitions / virtual devices (only those with /sys/block/<name>).
  // Disable in tests that use a synthetic /proc/diskstats.
  bool whole_devices_only = true;
  // Test seam / deployment override: defaults are the standard Linux paths.
  ProcExtPaths paths{};
};

// Bitmask of sources requested by `opt`.
inline uint8_t ProcExtRequested(const ProcExtOptions& opt) {
  uint8_t bits = 0;
  if (opt.cgroup) bits |= kExtSrcCgroup;
  if (opt.psi) bits |= kExtSrcPsi;
  if (opt.disk) bits |= kExtSrcDisk;
  if (opt.net) bits |= kExtSrcNet;
  if (opt.thread_slots > 0) bits |= kExtSrcThreads;
  return bits;
}

inline const char* const kProcExtFieldNames[kExtCount] = {
    "cgroup_cpu_usage_us",     "cgroup_cpu_throttle_us",
    "cgroup_cpu_nr_throttled", "cgroup_mem_current_kb",
    "cgroup_mem_max_kb",       "cgroup_oom_events",
    "cgroup_io_read_kb",       "cgroup_io_write_kb",
    "host_psi_cpu_some_us",    "host_psi_mem_some_us",
    "host_psi_mem_full_us",    "host_psi_io_some_us",
    "host_psi_io_full_us",     "host_disk_read_kb",
    "host_disk_write_kb",      "host_net_rx_kb",
    "host_net_tx_kb",
};

inline uint8_t ProcExtSeries(ExtField f) {
  return static_cast<uint8_t>(kProcExtBase + static_cast<uint8_t>(f));
}

// Source of one extended field; unsupported fields are never emitted.
inline ProcExtSource ProcExtFieldSource(ExtField f) {
  switch (f) {
    case kExtPsiCpuSome:
    case kExtPsiMemSome:
    case kExtPsiMemFull:
    case kExtPsiIoSome:
    case kExtPsiIoFull:
      return kExtSrcPsi;
    case kExtDiskRead:
    case kExtDiskWrite:
      return kExtSrcDisk;
    case kExtNetRx:
    case kExtNetTx:
      return kExtSrcNet;
    default:
      return kExtSrcCgroup;
  }
}

struct ProcExtValues {
  uint64_t v[kExtCount]{};                    // delta or instant per field
  bool valid[kExtCount]{};                    // filled this period
  uint8_t supported = 0;                      // detected capabilities
  uint64_t thread_cpu_ns[kProcExtMaxThreads]{};
  bool thread_valid[kProcExtMaxThreads]{};
};

// Parser + state for the optional sources. Zero allocation after Begin();
// never throws. All file reads are bounded and best-effort: an unreadable
// file marks that field invalid for the period instead of failing sampling.
class ProcExtSampler {
 public:
  ProcExtSampler() = default;
  ProcExtSampler(const ProcExtSampler&) = delete;
  ProcExtSampler& operator=(const ProcExtSampler&) = delete;

  // Probes every requested source, caches paths and takes the delta baseline.
  // `ns_per_tick` converts /proc utime/stime clock ticks to ns (ProcSampler
  // caches it from _SC_CLK_TCK). Returns the supported bitmask (0 when
  // nothing requested is available).
  uint8_t Begin(const ProcExtOptions& opt, uint64_t ns_per_tick = 0) {
    opt_ = opt;
    ns_per_tick_ = ns_per_tick != 0 ? ns_per_tick : 1;
    requested_ = ProcExtRequested(opt);
    supported_ = 0;
    cgroup_mode_ = kCgroupNone;
#if defined(__linux__)
    if (opt.cgroup) {
      // Detection alone is not enough: a container can see a cgroupfs where
      // this process's own group is not readable (host-global controller
      // trees). Only claim the source when one real file opens.
      if (DetectCgroupV2()) {
        if (CgroupV2Readable()) supported_ |= kExtSrcCgroup;
      } else if (DetectCgroupV1() && CgroupV1Readable()) {
        supported_ |= kExtSrcCgroup;
      }
      if ((supported_ & kExtSrcCgroup) == 0) cgroup_mode_ = kCgroupNone;
    }
    if (opt.psi) {
      char p[kPathCap];
      if (PressurePath(p, sizeof(p), "cpu")) supported_ |= kExtSrcPsi;
    }
    if (opt.disk) {
      if (Access(opt.paths.diskstats)) supported_ |= kExtSrcDisk;
    }
    if (opt.net) {
      if (Access(opt.paths.netdev)) supported_ |= kExtSrcNet;
    }
    if (opt.thread_slots > 0) supported_ |= kExtSrcThreads;
    // Baseline: one silent read of every supported field.
    ProcExtValues base;
    Read(&base, /*emit=*/false);
#endif
    values_ = ProcExtValues{};
    values_.supported = supported_;
    return supported_;
  }

  // Samples one period. `values()` afterwards holds the deltas/instants of
  // fields whose source is supported, plus the registered thread CPUs.
  void Sample() {
    values_ = ProcExtValues{};
    values_.supported = supported_;
#if defined(__linux__)
    Read(&values_, /*emit=*/true);
#endif
  }

  const ProcExtValues& values() const { return values_; }
  uint8_t requested() const { return requested_; }
  uint8_t supported() const { return supported_; }

  // Registers a thread (tid, e.g. gettid()) for per-thread CPU sampling.
  // Returns the slot, or -1 when full / unsupported / already registered.
  int RegisterThread(int tid, const char* name) {
    if (tid <= 0 || !(supported_ & kExtSrcThreads) ||
        n_threads_ >= opt_.thread_slots || n_threads_ >= kProcExtMaxThreads) {
      return -1;
    }
    for (uint8_t i = 0; i < n_threads_; ++i) {
      if (tid_[i] == tid) return -1;
    }
    const uint8_t slot = n_threads_;
    tid_[slot] = tid;
    std::snprintf(tname_[slot], sizeof(tname_[slot]), "thr_%s_cpu_ns",
                  (name != nullptr && name[0] != '\0') ? name : "thread");
    uint64_t u = 0, s = 0;
    char p[kPathCap];
    if (ThreadStatPath(p, sizeof(p), tid) && ReadTicks(p, &u, &s)) {
      tprev_[slot] = u + s;
      tslot_valid_[slot] = true;
    }
    ++n_threads_;
    return static_cast<int>(slot);
  }
  uint8_t thread_count() const { return n_threads_; }
  int thread_tid(uint8_t slot) const {
    return slot < n_threads_ ? tid_[slot] : 0;
  }
  // "thr_<name>_cpu_ns", truncated to the 31-byte series-name field.
  const char* thread_series_name(uint8_t slot) const {
    return slot < n_threads_ ? tname_[slot] : "";
  }

 private:
  enum CgroupMode : uint8_t { kCgroupNone = 0, kCgroupV2 = 1, kCgroupV1 = 2 };
  static constexpr std::size_t kPathCap = 192;

  static bool Access(const char* path) {
#if defined(__linux__)
    return path != nullptr && access(path, R_OK) == 0;
#else
    (void)path;
    return false;
#endif
  }

  static bool ReadWhole(const char* path, char* buf, std::size_t cap,
                        std::size_t* out_len) {
#if defined(__linux__)
    if (path == nullptr || cap == 0) return false;
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
#else
    (void)path;
    (void)buf;
    (void)cap;
    (void)out_len;
    return false;
#endif
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

  // Exact-key lookup at line start: "oom=" must not match "oom_kill=".
  static bool KeyU64(const char* buf, const char* key, uint64_t* out) {
    const std::size_t klen = std::strlen(key);
    for (const char* line = buf; *line != '\0';) {
      const char* end = std::strchr(line, '\n');
      if (end == nullptr) end = line + std::strlen(line);
      if (std::strncmp(line, key, klen) == 0 &&
          (line + klen == end || line[klen] == '=' || line[klen] == ' ')) {
        const char* v = line + klen;
        while (v < end && (*v == '=' || *v == ' ' || *v == '\t')) ++v;
        *out = ParseU64(v, end);
        return true;
      }
      line = (*end == '\n') ? end + 1 : end;
    }
    return false;
  }

  static bool ReadU64File(const char* path, uint64_t* out) {
    char buf[64];
    if (!ReadWhole(path, buf, sizeof(buf), nullptr)) return false;
    if (buf[0] == 'm' || buf[0] == 'M') {  // cgroup v2 "max" = unlimited
      *out = ~0ull;
      return true;
    }
    *out = ParseU64(buf, buf + std::strlen(buf));
    return true;
  }

  static bool ReadKeyFile(const char* path, const char* key, uint64_t* out) {
    char buf[1024];
    if (!ReadWhole(path, buf, sizeof(buf), nullptr)) return false;
    return KeyU64(buf, key, out);
  }

  static void JoinPath(char* out, std::size_t cap, const char* a,
                       const char* b, const char* c = nullptr) {
    if (a == nullptr) a = "";
    if (b == nullptr) b = "";
    if (c == nullptr) c = "";
    std::snprintf(out, cap, "%s%s%s", a, b, c);
  }

  // --- cgroup detection -----------------------------------------------------
  bool DetectCgroupV2() {
    char buf[4096];
    if (!ReadWhole(opt_.paths.cgroup_file, buf, sizeof(buf), nullptr)) {
      return false;
    }
    const char* rel = nullptr;
    for (const char* line = buf; *line != '\0';) {
      const char* end = std::strchr(line, '\n');
      if (end == nullptr) end = line + std::strlen(line);
      if (line[0] == '0' && line[1] == ':' && line[2] == ':') {
        rel = line + 3;
        char path[kPathCap];
        std::size_t n = static_cast<std::size_t>(end - rel);
        if (n >= sizeof(path)) n = sizeof(path) - 1;
        std::memcpy(path, rel, n);
        path[n] = '\0';
        JoinPath(cgroup_dir_, sizeof(cgroup_dir_), opt_.paths.cgroup_root,
                 std::strcmp(path, "/") == 0 ? "" : path);
        char probe[kPathCap + 32];
        JoinPath(probe, sizeof(probe), cgroup_dir_, "/cgroup.controllers");
        if (Access(probe)) {
          cgroup_mode_ = kCgroupV2;
          return true;
        }
        return false;
      }
      line = (*end == '\n') ? end + 1 : end;
    }
    (void)rel;
    return false;
  }

  bool DetectCgroupV1() {
    char buf[4096];
    if (!ReadWhole(opt_.paths.cgroup_file, buf, sizeof(buf), nullptr)) {
      return false;
    }
    bool found = false;
    for (const char* line = buf; *line != '\0';) {
      const char* end = std::strchr(line, '\n');
      if (end == nullptr) end = line + std::strlen(line);
      const char* c1 = std::strchr(line, ':');
      const char* c2 = c1 != nullptr ? std::strchr(c1 + 1, ':') : nullptr;
      if (c2 != nullptr && c2 < end) {
        const std::size_t n = static_cast<std::size_t>(c2 - c1 - 1);
        char ctrls[64];
        if (n < sizeof(ctrls)) {
          std::memcpy(ctrls, c1 + 1, n);
          ctrls[n] = '\0';
          char rel[kPathCap];
          std::size_t rn = static_cast<std::size_t>(end - c2 - 1);
          if (rn < sizeof(rel)) {
            std::memcpy(rel, c2 + 1, rn);
            rel[rn] = '\0';
            if (std::strstr(ctrls, "cpuacct") != nullptr ||
                std::strstr(ctrls, "cpu") != nullptr) {
              if (MountDir(cpu_dir_, sizeof(cpu_dir_), "cpu,cpuacct", rel) ||
                  MountDir(cpu_dir_, sizeof(cpu_dir_), "cpuacct", rel)) {
                found = true;
              }
            }
            if (std::strstr(ctrls, "memory") != nullptr) {
              if (MountDir(mem_dir_, sizeof(mem_dir_), "memory", rel)) {
                found = true;
              }
            }
            if (std::strstr(ctrls, "blkio") != nullptr) {
              if (MountDir(io_dir_, sizeof(io_dir_), "blkio", rel)) {
                found = true;
              }
            }
          }
        }
      }
      line = (*end == '\n') ? end + 1 : end;
    }
    if (found) cgroup_mode_ = kCgroupV1;
    return found;
  }

  bool MountDir(char* out, std::size_t cap, const char* controller,
                const char* rel) {
    char base[kPathCap];
    JoinPath(base, sizeof(base), opt_.paths.cgroup_root, "/", controller);
    if (!Access(base)) return false;
    char full[kPathCap * 2];
    std::snprintf(full, sizeof(full), "%s%s%s", base,
                  (rel[0] == '/' && rel[1] == '\0') ? "" : rel,
                  (rel[0] == '/' && rel[1] == '\0') ? "" : "/");
    // Keep a trailing slash off: callers append "<file>".
    const std::size_t n = std::strlen(full);
    if (n > 0 && full[n - 1] == '/') full[n - 1] = '\0';
    const std::size_t len = std::strlen(full);
    if (len >= cap) return false;  // path longer than this build supports
    std::memcpy(out, full, len + 1);
    return true;
  }

  // A cgroupfs can be visible while this process's own group is not (host
  // controller trees inside a container). Require one stat file to open.
  bool CgroupV2Readable() {
    char p[kPathCap * 2];
    const char* const files[] = {"/cpu.stat", "/memory.current", "/io.stat"};
    for (const char* f : files) {
      JoinPath(p, sizeof(p), cgroup_dir_, f);
      if (Access(p)) return true;
    }
    return false;
  }

  bool CgroupV1Readable() {
    char p[kPathCap * 2];
    if (cpu_dir_[0] != '\0') {
      JoinPath(p, sizeof(p), cpu_dir_, "/cpuacct.usage");
      if (Access(p)) return true;
    }
    if (mem_dir_[0] != '\0') {
      JoinPath(p, sizeof(p), mem_dir_, "/memory.usage_in_bytes");
      if (Access(p)) return true;
    }
    if (io_dir_[0] != '\0') {
      JoinPath(p, sizeof(p), io_dir_, "/blkio.throttle.io_service_bytes");
      if (Access(p)) return true;
    }
    return false;
  }

  // --- sampling -------------------------------------------------------------
  void Read(ProcExtValues* out, bool emit) {
    if ((supported_ & kExtSrcCgroup) != 0) ReadCgroup(out, emit);
    if ((supported_ & kExtSrcPsi) != 0) ReadPsi(out, emit);
    if ((supported_ & kExtSrcDisk) != 0) ReadDisk(out, emit);
    if ((supported_ & kExtSrcNet) != 0) ReadNet(out, emit);
    if ((supported_ & kExtSrcThreads) != 0) ReadThreads(out, emit);
  }

  // Delta for counter sources, instant for gauges. `emit=false` only refreshes
  // the baseline.
  void Put(ProcExtValues* out, ExtField f, uint64_t cur, bool is_delta,
           bool emit) {
    uint64_t& prev = prev_[static_cast<uint8_t>(f)];
    const uint64_t v = is_delta ? (cur >= prev ? cur - prev : 0) : cur;
    prev = cur;
    if (emit) {
      out->v[static_cast<uint8_t>(f)] = v;
      out->valid[static_cast<uint8_t>(f)] = true;
    }
  }

  void ReadCgroup(ProcExtValues* out, bool emit) {
    if (cgroup_mode_ == kCgroupV2) {
      ReadCgroupV2(out, emit);
    } else if (cgroup_mode_ == kCgroupV1) {
      ReadCgroupV1(out, emit);
    }
  }

  void ReadCgroupV2(ProcExtValues* out, bool emit) {
    char p[kPathCap * 2];
    uint64_t v = 0;
    JoinPath(p, sizeof(p), cgroup_dir_, "/cpu.stat");
    if (ReadKeyFile(p, "usage_usec", &v)) {
      Put(out, kExtCgroupCpuUsage, v, true, emit);
    }
    if (ReadKeyFile(p, "throttled_usec", &v)) {
      Put(out, kExtCgroupCpuThrottled, v, true, emit);
    }
    if (ReadKeyFile(p, "nr_throttled", &v)) {
      Put(out, kExtCgroupCpuNrThrottled, v, true, emit);
    }
    JoinPath(p, sizeof(p), cgroup_dir_, "/memory.current");
    if (ReadU64File(p, &v)) {
      Put(out, kExtCgroupMemCurrent, v / 1024ULL, false, emit);
    }
    JoinPath(p, sizeof(p), cgroup_dir_, "/memory.max");
    if (ReadU64File(p, &v)) {
      Put(out, kExtCgroupMemMax, UnlimitedKb(v), false, emit);
    }
    JoinPath(p, sizeof(p), cgroup_dir_, "/memory.events");
    if (ReadKeyFile(p, "oom", &v)) {
      Put(out, kExtCgroupOomEvents, v, true, emit);
    }
    JoinPath(p, sizeof(p), cgroup_dir_, "/io.stat");
    if (ReadIoStat(p, &v, true)) {
      Put(out, kExtCgroupIoRead, v / 1024ULL, true, emit);
    }
    if (ReadIoStat(p, &v, false)) {
      Put(out, kExtCgroupIoWrite, v / 1024ULL, true, emit);
    }
  }

  static uint64_t UnlimitedKb(uint64_t bytes) {
    // v1 reports a 2^63-ish sentinel; v2 reports "max" (handled by the
    // caller). Normalise anything beyond 2^60 bytes to "unlimited".
    if (bytes >= (1ull << 60)) return 0xFFFFFFFFull;
    return bytes / 1024ULL;
  }

  void ReadCgroupV1(ProcExtValues* out, bool emit) {
    char p[kPathCap * 2];
    uint64_t v = 0;
    if (cpu_dir_[0] != '\0') {
      JoinPath(p, sizeof(p), cpu_dir_, "/cpuacct.usage");  // ns
      if (ReadU64File(p, &v)) {
        Put(out, kExtCgroupCpuUsage, v / 1000ULL, true, emit);
      }
      JoinPath(p, sizeof(p), cpu_dir_, "/cpu.stat");
      if (ReadKeyFile(p, "throttled_time", &v)) {  // ns
        Put(out, kExtCgroupCpuThrottled, v / 1000ULL, true, emit);
      }
      if (ReadKeyFile(p, "nr_throttled", &v)) {
        Put(out, kExtCgroupCpuNrThrottled, v, true, emit);
      }
    }
    if (mem_dir_[0] != '\0') {
      JoinPath(p, sizeof(p), mem_dir_, "/memory.usage_in_bytes");
      if (ReadU64File(p, &v)) {
        Put(out, kExtCgroupMemCurrent, v / 1024ULL, false, emit);
      }
      JoinPath(p, sizeof(p), mem_dir_, "/memory.limit_in_bytes");
      if (ReadU64File(p, &v)) {
        Put(out, kExtCgroupMemMax, UnlimitedKb(v), false, emit);
      }
      JoinPath(p, sizeof(p), mem_dir_, "/memory.oom_control");
      if (ReadKeyFile(p, "oom_kill", &v)) {
        Put(out, kExtCgroupOomEvents, v, true, emit);
      }
    }
    if (io_dir_[0] != '\0') {
      if (ReadBlkioBytes(io_dir_, &v, true)) {
        Put(out, kExtCgroupIoRead, v / 1024ULL, true, emit);
      }
      if (ReadBlkioBytes(io_dir_, &v, false)) {
        Put(out, kExtCgroupIoWrite, v / 1024ULL, true, emit);
      }
    }
  }

  // v2 io.stat: "8:0 rbytes=N wbytes=M ..." per device; sum over devices.
  static bool ReadIoStat(const char* path, uint64_t* out, bool read_side) {
    char buf[4096];
    if (!ReadWhole(path, buf, sizeof(buf), nullptr)) return false;
    const char* key = read_side ? "rbytes=" : "wbytes=";
    const std::size_t klen = std::strlen(key);
    uint64_t sum = 0;
    for (const char* p = buf; *p != '\0';) {
      const char* hit = std::strstr(p, key);
      if (hit == nullptr) break;
      const char* line_end = std::strchr(hit, '\n');
      const char* v = hit + klen;
      sum += ParseU64(v, line_end != nullptr ? line_end : v + 32);
      p = line_end != nullptr ? line_end + 1 : v + std::strlen(v);
    }
    *out = sum;
    return true;
  }

  // v1 blkio files: lines "8:0 Read 1234" / "8:0 Write 567" (+ "Total ...").
  // Two names exist across kernel versions / elevator configs; try both.
  static bool ReadBlkioBytes(const char* dir, uint64_t* out, bool read_side) {
    char path[kPathCap * 2];
    JoinPath(path, sizeof(path), dir, "/blkio.throttle.io_service_bytes");
    if (!ReadBlkioBytesFile(path, out, read_side)) {
      JoinPath(path, sizeof(path), dir, "/blkio.io_service_bytes");
      return ReadBlkioBytesFile(path, out, read_side);
    }
    return true;
  }

  static bool ReadBlkioBytesFile(const char* path, uint64_t* out,
                                 bool read_side) {
    char buf[4096];
    if (!ReadWhole(path, buf, sizeof(buf), nullptr)) return false;
    const char* key = read_side ? "Read " : "Write ";
    const std::size_t klen = std::strlen(key);
    uint64_t sum = 0;
    for (const char* line = buf; *line != '\0';) {
      const char* end = std::strchr(line, '\n');
      if (end == nullptr) end = line + std::strlen(line);
      const char* hit = std::strstr(line, key);
      if (hit != nullptr && hit < end) {
        sum += ParseU64(hit + klen, end);
      }
      line = (*end == '\n') ? end + 1 : end;
    }
    *out = sum;
    return true;
  }

  bool PressurePath(char* out, std::size_t cap, const char* name) {
    JoinPath(out, cap, opt_.paths.pressure_dir, "/", name);
    return Access(out);
  }

  // PSI files: "some avg10=0.00 avg60=0.01 avg300=0.08 total=123456".
  // We keep the monotonic `total` (cumulative stall time, us) and delta it;
  // the avg* columns are kernel-side EWMA, not what a 1 Hz sampler needs.
  static bool ParsePressure(const char* buf, const char* kind, uint64_t* out) {
    const std::size_t klen = std::strlen(kind);
    for (const char* line = buf; *line != '\0';) {
      const char* end = std::strchr(line, '\n');
      if (end == nullptr) end = line + std::strlen(line);
      if (std::strncmp(line, kind, klen) == 0 &&
          (line[klen] == ' ' || line[klen] == '\t')) {
        for (const char* p = line + klen; p < end;) {
          const char* hit = std::strstr(p, "total=");
          if (hit == nullptr || hit >= end) return false;
          *out = ParseU64(hit + 6, end);
          return true;
        }
      }
      line = (*end == '\n') ? end + 1 : end;
    }
    return false;
  }

  bool ReadPressureFile(const char* name, uint64_t* some, bool* has_some,
                        uint64_t* full, bool* has_full) {
    char p[kPathCap];
    if (!PressurePath(p, sizeof(p), name)) return false;
    char buf[512];
    if (!ReadWhole(p, buf, sizeof(buf), nullptr)) return false;
    if (has_some != nullptr) *has_some = false;
    if (has_full != nullptr) *has_full = false;
    if (some != nullptr) *has_some = ParsePressure(buf, "some", some);
    if (full != nullptr) *has_full = ParsePressure(buf, "full", full);
    return true;
  }

  void ReadPsi(ProcExtValues* out, bool emit) {
    uint64_t some = 0, full = 0;
    bool has_some = false, has_full = false;
    if (ReadPressureFile("cpu", &some, &has_some, nullptr, nullptr) &&
        has_some) {
      Put(out, kExtPsiCpuSome, some, true, emit);
    }
    if (ReadPressureFile("memory", &some, &has_some, &full, &has_full)) {
      if (has_some) Put(out, kExtPsiMemSome, some, true, emit);
      if (has_full) Put(out, kExtPsiMemFull, full, true, emit);
    }
    if (ReadPressureFile("io", &some, &has_some, &full, &has_full)) {
      if (has_some) Put(out, kExtPsiIoSome, some, true, emit);
      if (has_full) Put(out, kExtPsiIoFull, full, true, emit);
    }
  }

  void ReadDisk(ProcExtValues* out, bool emit) {
    char buf[8192];
    if (!ReadWhole(opt_.paths.diskstats, buf, sizeof(buf), nullptr)) return;
    uint64_t read_sectors = 0, write_sectors = 0;
    for (const char* line = buf; *line != '\0';) {
      const char* end = std::strchr(line, '\n');
      if (end == nullptr) end = line + std::strlen(line);
      // major minor name reads merged sectors ms writes merged wsec ...
      char name[64];
      unsigned long long t[7] = {};
      int n = std::sscanf(line, "%*u %*u %63s %llu %llu %llu %llu %llu %llu "
                                "%llu",
                          name, &t[0], &t[1], &t[2], &t[3], &t[4], &t[5],
                          &t[6]);
      if (n >= 8 && (!opt_.whole_devices_only || WholeDevice(name))) {
        read_sectors += t[2];
        write_sectors += t[6];
      }
      line = (*end == '\n') ? end + 1 : end;
    }
    // 512-byte sectors -> KiB
    Put(out, kExtDiskRead, read_sectors / 2ULL, true, emit);
    Put(out, kExtDiskWrite, write_sectors / 2ULL, true, emit);
  }

  bool WholeDevice(const char* name) {
    // Partitions (sda1, nvme0n1p2) and virtual devices without a
    // /sys/block entry are skipped so a disk is not counted twice.
    char p[kPathCap];
    JoinPath(p, sizeof(p), opt_.paths.sys_block, "/", name);
    return Access(p);
  }

  void ReadNet(ProcExtValues* out, bool emit) {
    char buf[8192];
    if (!ReadWhole(opt_.paths.netdev, buf, sizeof(buf), nullptr)) return;
    uint64_t rx = 0, tx = 0;
    int line_no = 0;
    for (const char* line = buf; *line != '\0';) {
      const char* end = std::strchr(line, '\n');
      if (end == nullptr) end = line + std::strlen(line);
      if (line_no >= 2) {  // skip the two header lines
        const char* colon = std::strchr(line, ':');
        if (colon != nullptr) {
          char ifname[32];
          const std::size_t i = static_cast<std::size_t>(
              colon - line);
          std::size_t n = i < sizeof(ifname) ? i : sizeof(ifname) - 1;
          std::memcpy(ifname, line, n);
          ifname[n] = '\0';
          // Trim leading spaces: the iface column is right-aligned.
          char* nm = ifname;
          while (*nm == ' ' || *nm == '\t') ++nm;
          if (std::strcmp(nm, "lo") != 0) {
            unsigned long long v[16] = {};
            const int got = std::sscanf(colon + 1,
                                        "%llu %llu %llu %llu %llu %llu %llu "
                                        "%llu %llu",
                                        &v[0], &v[1], &v[2], &v[3], &v[4],
                                        &v[5], &v[6], &v[7], &v[8]);
            if (got >= 9) {
              rx += v[0];  // rx bytes
              tx += v[8];  // tx bytes
            }
          }
        }
      }
      ++line_no;
      line = (*end == '\n') ? end + 1 : end;
    }
    Put(out, kExtNetRx, rx / 1024ULL, true, emit);
    Put(out, kExtNetTx, tx / 1024ULL, true, emit);
  }

  static bool ThreadStatPath(char* out, std::size_t cap, int tid) {
    std::snprintf(out, cap, "/proc/self/task/%d/stat", tid);
    return true;
  }

  static bool ReadTicks(const char* path, uint64_t* utime, uint64_t* stime) {
    char buf[1024];
    if (!ReadWhole(path, buf, sizeof(buf), nullptr)) return false;
    const char* close = std::strrchr(buf, ')');
    if (close == nullptr) return false;
    const char* p = close + 1;
    int tok = 0;
    bool got_u = false, got_s = false;
    while (tok <= 12) {
      while (*p == ' ') ++p;
      if (*p == '\0' || *p == '\n') break;
      const char* begin = p;
      while (*p != ' ' && *p != '\0' && *p != '\n') ++p;
      if (p == begin) break;
      if (tok == 11) {
        *utime = ParseU64(begin, p);
        got_u = true;
      } else if (tok == 12) {
        *stime = ParseU64(begin, p);
        got_s = true;
      }
      ++tok;
    }
    return got_u && got_s;
  }

  void ReadThreads(ProcExtValues* out, bool emit) {
    for (uint8_t i = 0; i < n_threads_; ++i) {
      char p[kPathCap];
      if (!ThreadStatPath(p, sizeof(p), tid_[i])) continue;
      uint64_t u = 0, s = 0;
      if (!ReadTicks(p, &u, &s)) continue;
      const uint64_t cur = u + s;
      const uint64_t prev = tprev_[i];
      tprev_[i] = cur;
      if (emit) {
        const uint64_t delta_ticks = cur >= prev ? cur - prev : 0;
        out->thread_cpu_ns[i] = delta_ticks * ns_per_tick_;
        out->thread_valid[i] = true;
      }
    }
  }

  ProcExtOptions opt_{};
  ProcExtValues values_{};
  uint64_t prev_[kExtCount]{};
  uint64_t ns_per_tick_ = 1;  // clock ticks -> ns for per-thread CPU
  uint8_t requested_ = 0;
  uint8_t supported_ = 0;
  uint8_t cgroup_mode_ = kCgroupNone;
  char cgroup_dir_[kPathCap]{};
  char cpu_dir_[kPathCap]{};
  char mem_dir_[kPathCap]{};
  char io_dir_[kPathCap]{};
  char tname_[kProcExtMaxThreads][24]{};
  int tid_[kProcExtMaxThreads]{};
  uint64_t tprev_[kProcExtMaxThreads]{};
  bool tslot_valid_[kProcExtMaxThreads]{};
  uint8_t n_threads_ = 0;
};

}  // namespace thyme
