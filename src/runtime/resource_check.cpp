// resource_check.cpp — OPS-07: startup resource self-check.
//
// Read-only host probing (getrlimit/statvfs). No allocation, no exceptions, no
// file opens: this runs on the startup path before the host binds anything.

#include "thyme/runtime/resource_check.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <sys/types.h>
#else
#include <sys/resource.h>
#include <sys/statvfs.h>
#endif

namespace thyme {
namespace resource {
namespace {

constexpr uint64_t kInfinite = UINT64_MAX;

#if !defined(_WIN32)
uint64_t ToU64(rlim_t value) noexcept {
  if (value == RLIM_INFINITY) return kInfinite;
  return static_cast<uint64_t>(value);
}
#endif

char ToLower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Fixed-buffer text builder. Truncates instead of allocating; `ok` reports
// whether the whole rendering fit.
struct Buf {
  char* out;
  std::size_t cap;
  std::size_t len = 0;
  bool ok = true;

  Buf(char* buffer, std::size_t capacity) noexcept
      : out(buffer), cap(capacity) {
    if (cap > 0) out[0] = '\0';
  }

  void Append(const char* text) noexcept {
    for (const char* p = text; *p != '\0'; ++p) {
      if (len + 1 >= cap) {
        ok = false;
        break;
      }
      out[len++] = *p;
    }
    if (cap > 0) out[len < cap ? len : cap - 1] = '\0';
  }

  void Fmt(const char* fmt, ...) noexcept {
    if (len + 1 >= cap) {
      ok = false;
      return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(out + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) {
      ok = false;
      return;
    }
    const std::size_t written = static_cast<std::size_t>(n);
    if (written >= cap - len) {
      ok = false;
      len = cap - 1;
    } else {
      len += written;
    }
  }
};

uint64_t EnvU64(const char* name, uint64_t fallback) noexcept {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') return fallback;
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (errno != 0 || end == value) return fallback;
  while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') ++end;
  if (*end != '\0') return fallback;
  return static_cast<uint64_t>(parsed);
}

uint64_t EnvMbToBytes(const char* name, uint64_t fallback_mb) noexcept {
  const uint64_t mb = EnvU64(name, fallback_mb);
  if (mb > (UINT64_MAX / (1024ULL * 1024ULL))) return kInfinite;
  return mb * 1024ULL * 1024ULL;
}

void AppendU64(Buf& b, uint64_t value) noexcept {
  if (value == kInfinite) {
    b.Append("unlimited");
    return;
  }
  b.Fmt("%llu", static_cast<unsigned long long>(value));
}

}  // namespace

const char* LevelName(Level level) noexcept {
  switch (level) {
    case Level::kOk:
      return "ok";
    case Level::kWarn:
      return "warn";
    case Level::kFatal:
      return "fatal";
  }
  return "fatal";
}

Level Worst(Level a, Level b) noexcept {
  return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b;
}

bool MlockOptOut(const char* value) noexcept {
  if (value == nullptr) return false;
  const char* const kTruthy[] = {"1", "true", "yes", "on"};
  for (const char* word : kTruthy) {
    const char* a = value;
    const char* b = word;
    while (*a != '\0' && *b != '\0' && ToLower(*a) == ToLower(*b)) {
      ++a;
      ++b;
    }
    if (*a == *b) return true;
  }
  return false;
}

bool MlockConfiguredFromEnv() noexcept {
  return !MlockOptOut(std::getenv("THYME_NO_MLOCK"));
}

Plan PlanFromEnv(uint32_t channels, const char* disk_path,
                 uint64_t disk_quota_bytes, uint64_t disk_reserve_bytes,
                 const char* shm_path) noexcept {
  Plan plan;
  const char* mode = std::getenv("THYME_RESOURCE_CHECK");
  if (mode != nullptr) {
    if (std::strcmp(mode, "off") == 0 || std::strcmp(mode, "0") == 0) {
      plan.enabled = false;
    } else if (std::strcmp(mode, "warn") == 0) {
      plan.enforce = false;
    }
  }

  Limits& limits = plan.limits;
  limits.channels = channels;
  limits.files_per_channel = static_cast<uint32_t>(
      EnvU64("THYME_RESOURCE_FILES_PER_CHANNEL", limits.files_per_channel));
  limits.baseline_files = static_cast<uint32_t>(
      EnvU64("THYME_RESOURCE_BASELINE_FILES", limits.baseline_files));

  limits.mlock_configured = MlockConfiguredFromEnv();
  // The default plan is 8 MiB per recording source (a mirror-ring arena is
  // 2 MiB; PinnedSlot channels are created by producers, not by this host).
  const uint64_t default_planned_mb = 8ULL * channels;
  const uint64_t planned = EnvMbToBytes("THYME_RESOURCE_SHM_PLANNED_MB",
                                        default_planned_mb);
  limits.shm_planned_bytes = planned;
  limits.shm_reserve_bytes = EnvMbToBytes("THYME_RESOURCE_SHM_RESERVE_MB", 16);
  const uint64_t memlock_headroom =
      EnvMbToBytes("THYME_RESOURCE_MEMLOCK_HEADROOM_MB", 64);
  limits.memlock_required_bytes =
      (planned == kInfinite || memlock_headroom == kInfinite ||
       planned > kInfinite - memlock_headroom)
          ? kInfinite
          : planned + memlock_headroom;
  limits.shm_path = shm_path;
  limits.disk_path = disk_path;
  limits.disk_quota_bytes = disk_quota_bytes;
  // Disk reserve: an explicit env override wins; otherwise keep the caller's
  // byte value exactly (no MiB round trip).
  const char* reserve_env = std::getenv("THYME_RESOURCE_DISK_RESERVE_MB");
  limits.disk_reserve_bytes =
      reserve_env != nullptr
          ? EnvMbToBytes("THYME_RESOURCE_DISK_RESERVE_MB", 0)
          : disk_reserve_bytes;
  return plan;
}

Report Check(const Limits& limits) noexcept {
  Report r;
  r.nofile_required =
      static_cast<uint64_t>(limits.channels) * limits.files_per_channel +
      limits.baseline_files;

#if !defined(_WIN32)
  struct rlimit nofile {};
  if (getrlimit(RLIMIT_NOFILE, &nofile) == 0) {
    r.nofile_checked = true;
    r.nofile_soft = ToU64(nofile.rlim_cur);
    r.nofile_hard = ToU64(nofile.rlim_max);
  }
#endif
  if (!r.nofile_checked) {
    r.nofile = Level::kWarn;  // unverifiable
  } else if (r.nofile_soft != kInfinite &&
             r.nofile_soft < r.nofile_required) {
    r.nofile = Level::kFatal;
  }

  r.memlock_configured = limits.mlock_configured;
  r.memlock_required = limits.memlock_required_bytes;
  if (limits.mlock_configured) {
#if defined(RLIMIT_MEMLOCK) && !defined(_WIN32)
    struct rlimit memlock {};
    if (getrlimit(RLIMIT_MEMLOCK, &memlock) == 0) {
      r.memlock_checked = true;
      r.memlock_soft = ToU64(memlock.rlim_cur);
      r.memlock_hard = ToU64(memlock.rlim_max);
    }
#endif
    if (!r.memlock_checked ||
        (limits.memlock_required_bytes != 0 && r.memlock_soft != kInfinite &&
         r.memlock_soft < limits.memlock_required_bytes)) {
      // mlockall failure is loud but non-fatal (App::Prepare keeps running);
      // the warning is the actionable RLIMIT_MEMLOCK/CAP_IPC_LOCK hint.
      r.memlock = Level::kWarn;
    }
  }

#if !defined(_WIN32)
  if (limits.disk_path != nullptr && limits.disk_path[0] != '\0' &&
      (limits.disk_quota_bytes != 0 || limits.disk_reserve_bytes != 0)) {
    struct statvfs vfs {};
    if (statvfs(limits.disk_path, &vfs) == 0) {
      const uint64_t unit = vfs.f_frsize != 0 ? vfs.f_frsize : vfs.f_bsize;
      r.disk_checked = true;
      r.disk_free = static_cast<uint64_t>(vfs.f_bavail) * unit;
      r.disk_required = limits.disk_reserve_bytes;
      r.disk_quota = limits.disk_quota_bytes;
      if (r.disk_required != 0 && r.disk_free < r.disk_required) {
        r.disk = Level::kFatal;
      } else if (r.disk_quota != 0 && r.disk_free < r.disk_quota) {
        r.disk = Level::kWarn;
      }
    } else {
      r.disk = Level::kWarn;  // unverifiable
    }
  }

  if (limits.shm_path != nullptr && limits.shm_path[0] != '\0' &&
      (limits.shm_planned_bytes != 0 || limits.shm_reserve_bytes != 0)) {
    struct statvfs vfs {};
    if (statvfs(limits.shm_path, &vfs) == 0) {
      const uint64_t unit = vfs.f_frsize != 0 ? vfs.f_frsize : vfs.f_bsize;
      r.shm_checked = true;
      r.shm_free = static_cast<uint64_t>(vfs.f_bavail) * unit;
      r.shm_planned = limits.shm_planned_bytes;
      r.shm_required =
          (limits.shm_planned_bytes == kInfinite ||
           limits.shm_reserve_bytes == kInfinite ||
           limits.shm_planned_bytes >
               kInfinite - limits.shm_reserve_bytes)
              ? kInfinite
              : limits.shm_planned_bytes + limits.shm_reserve_bytes;
      if (r.shm_planned != 0 && r.shm_free < r.shm_planned) {
        r.shm = Level::kFatal;
      } else if (r.shm_required != 0 && r.shm_free < r.shm_required) {
        r.shm = Level::kWarn;
      }
    } else {
      r.shm = Level::kWarn;  // unverifiable
    }
  }
#endif

  const Level levels[4] = {r.nofile, r.memlock, r.disk, r.shm};
  for (const Level level : levels) {
    if (level == Level::kWarn) ++r.warnings;
    if (level == Level::kFatal) r.fatal = true;
  }
  return r;
}

bool FormatSummary(const Report& report, char* out, std::size_t cap) noexcept {
  Buf b(out, cap);
  b.Fmt("resources: %s (warnings=%u)",
        report.fatal ? "FATAL" : LevelName(report.worst()),
        static_cast<unsigned>(report.warnings));
  if (report.nofile_checked) {
    b.Fmt(" nofile=%s soft=", LevelName(report.nofile));
    AppendU64(b, report.nofile_soft);
    b.Fmt("/%llu", static_cast<unsigned long long>(report.nofile_required));
  } else {
    b.Append(" nofile=unchecked");
  }
  if (report.memlock_configured) {
    b.Fmt(" memlock=%s", LevelName(report.memlock));
    if (report.memlock_checked) {
      b.Append(" soft=");
      AppendU64(b, report.memlock_soft);
      b.Fmt("/%llu",
            static_cast<unsigned long long>(report.memlock_required));
    }
  } else {
    b.Append(" memlock=off");
  }
  if (report.disk_checked) {
    b.Fmt(" disk=%s free=", LevelName(report.disk));
    AppendU64(b, report.disk_free);
    b.Fmt("/%llu(quota=%llu)",
          static_cast<unsigned long long>(report.disk_required),
          static_cast<unsigned long long>(report.disk_quota));
  } else {
    b.Append(" disk=unchecked");
  }
  if (report.shm_checked) {
    b.Fmt(" shm=%s free=", LevelName(report.shm));
    AppendU64(b, report.shm_free);
    b.Fmt("/%llu(plan=%llu)",
          static_cast<unsigned long long>(report.shm_required),
          static_cast<unsigned long long>(report.shm_planned));
  } else {
    b.Append(" shm=unchecked");
  }
  return b.ok;
}

bool FormatJson(const Report& report, char* out, std::size_t cap) noexcept {
  Buf b(out, cap);
  b.Fmt("{\"checked\":true,\"worst\":\"%s\",\"fatal\":%s,\"warnings\":%u",
        LevelName(report.worst()), report.fatal ? "true" : "false",
        static_cast<unsigned>(report.warnings));
  b.Fmt(",\"nofile\":{\"checked\":%s,\"level\":\"%s\",\"soft\":",
        report.nofile_checked ? "true" : "false", LevelName(report.nofile));
  if (report.nofile_soft == kInfinite) {
    b.Append("null");
  } else {
    b.Fmt("%llu", static_cast<unsigned long long>(report.nofile_soft));
  }
  b.Fmt(",\"hard\":");
  if (report.nofile_hard == kInfinite) {
    b.Append("null");
  } else {
    b.Fmt("%llu", static_cast<unsigned long long>(report.nofile_hard));
  }
  b.Fmt(",\"required\":%llu}",
        static_cast<unsigned long long>(report.nofile_required));
  b.Fmt(",\"memlock\":{\"configured\":%s,\"checked\":%s,\"level\":\"%s\","
        "\"soft\":",
        report.memlock_configured ? "true" : "false",
        report.memlock_checked ? "true" : "false",
        LevelName(report.memlock));
  if (report.memlock_checked && report.memlock_soft != kInfinite) {
    b.Fmt("%llu", static_cast<unsigned long long>(report.memlock_soft));
  } else {
    b.Append("null");
  }
  b.Fmt(",\"required\":%llu}",
        static_cast<unsigned long long>(report.memlock_required));
  b.Fmt(",\"disk\":{\"checked\":%s,\"level\":\"%s\",\"free_bytes\":%llu,"
        "\"required_bytes\":%llu,\"quota_bytes\":%llu}",
        report.disk_checked ? "true" : "false", LevelName(report.disk),
        static_cast<unsigned long long>(report.disk_free),
        static_cast<unsigned long long>(report.disk_required),
        static_cast<unsigned long long>(report.disk_quota));
  b.Fmt(",\"shm\":{\"checked\":%s,\"level\":\"%s\",\"free_bytes\":%llu,"
        "\"required_bytes\":%llu,\"planned_bytes\":%llu}}",
        report.shm_checked ? "true" : "false", LevelName(report.shm),
        static_cast<unsigned long long>(report.shm_free),
        static_cast<unsigned long long>(report.shm_required),
        static_cast<unsigned long long>(report.shm_planned));
  return b.ok;
}

}  // namespace resource
}  // namespace thyme
