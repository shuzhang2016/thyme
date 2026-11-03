#include "thyme/runtime/shm_lifecycle.h"

#include "thyme/record/file_ptr.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace thyme {
namespace {

#if defined(__linux__)

constexpr std::size_t kPathCap = 512;

bool IsDecimal(const char* s) {
  if (s == nullptr || *s == '\0') return false;
  for (; *s != '\0'; ++s) {
    if (*s < '0' || *s > '9') return false;
  }
  return true;
}

// Exact target match, tolerating the kernel's " (deleted)" suffix (an
// already-unlinked mapping is not a name we could sweep anyway).
bool SameShmPath(const char* link, const char* target) {
  const std::size_t n = std::strlen(target);
  if (std::strncmp(link, target, n) != 0) return false;
  return link[n] == '\0' || std::strcmp(link + n, " (deleted)") == 0;
}

struct ProcScan {
  bool holder = false;
  bool complete = true;
};

bool ScanFdDir(const char* fd_dir, const char* target, bool* complete) {
  DIR* d = opendir(fd_dir);
  if (d == nullptr) {
    // An fd directory we cannot read could hide a holder. Root processes are
    // normally unreadable to non-root callers; the lease check below makes
    // the verdict conclusive anyway when the object really is unheld.
    if (errno == EACCES || errno == EPERM) *complete = false;
    return false;
  }
  bool found = false;
  while (struct dirent* e = readdir(d)) {
    if (!IsDecimal(e->d_name)) continue;
    char link_path[kPathCap];
    const int n = std::snprintf(link_path, sizeof(link_path), "%s/%s", fd_dir,
                                e->d_name);
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(link_path)) continue;
    char link[kPathCap];
    const ssize_t len = readlink(link_path, link, sizeof(link) - 1);
    if (len < 0) {
      if (errno == EACCES || errno == EPERM) *complete = false;
      continue;
    }
    link[len] = '\0';
    if (SameShmPath(link, target)) {
      found = true;
      break;
    }
  }
  closedir(d);
  return found;
}

bool ScanMaps(const char* maps_path, const char* target, bool* complete) {
  FilePtr f(std::fopen(maps_path, "re"));
  if (f == nullptr) {
    if (errno == EACCES || errno == EPERM) *complete = false;
    return false;
  }
  const std::size_t target_len = std::strlen(target);
  char line[1024];
  bool found = false;
  while (std::fgets(line, sizeof(line), f.get()) != nullptr) {
    const char* p = std::strstr(line, target);
    if (p == nullptr) continue;
    const char* after = p + target_len;
    if (*after == '\0' || *after == '\n' ||
        std::strncmp(after, " (deleted)", 10) == 0) {
      found = true;
      break;
    }
  }
  return found;
}

// Any process holding the object open (fd) or mapped (maps) is a live
// holder. `complete` is false when some process could not be inspected.
ProcScan ScanProcsForHolder(const char* target) {
  ProcScan out;
  DIR* proc = opendir("/proc");
  if (proc == nullptr) {
    out.complete = false;
    return out;
  }
  while (struct dirent* e = readdir(proc)) {
    if (!IsDecimal(e->d_name)) continue;
    char sub[kPathCap];
    int n = std::snprintf(sub, sizeof(sub), "/proc/%s/fd", e->d_name);
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(sub) &&
        ScanFdDir(sub, target, &out.complete)) {
      out.holder = true;
      break;
    }
    n = std::snprintf(sub, sizeof(sub), "/proc/%s/maps", e->d_name);
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(sub) &&
        ScanMaps(sub, target, &out.complete)) {
      out.holder = true;
      break;
    }
  }
  closedir(proc);
  return out;
}

bool ReleaseLease(int fd) {
  if (fd < 0) return true;
  bool ok = true;
#if defined(F_SETLEASE) && defined(F_WRLCK)
  ok = fcntl(fd, F_SETLEASE, F_UNLCK) == 0;
#endif
  if (close(fd) != 0) ok = false;
  return ok;
}

// Classify `name`. On kStale, *lease_fd_out is either an fd holding an
// exclusive lease (the caller must unlink while holding it, then release),
// or -1 when leases are unavailable and the complete /proc scan was the
// proof. On every other verdict *lease_fd_out is -1.
ShmLiveness Classify(const char* name, bool* scan_complete, int* lease_fd_out) {
  *scan_complete = true;
  *lease_fd_out = -1;
  if (name == nullptr || name[0] != '/' ||
      std::strchr(name + 1, '/') != nullptr) {
    return ShmLiveness::kUnknown;
  }
  char path[kPathCap];
  const int n = std::snprintf(path, sizeof(path), "/dev/shm%s", name);
  if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(path)) {
    return ShmLiveness::kUnknown;
  }
  struct stat st {};
  if (stat(path, &st) != 0) {
    return errno == ENOENT ? ShmLiveness::kAbsent : ShmLiveness::kUnknown;
  }
  if (!S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
    return ShmLiveness::kUnknown;
  }
  const ProcScan scan = ScanProcsForHolder(path);
  *scan_complete = scan.complete;
  if (scan.holder) return ShmLiveness::kLive;

  const int fd = shm_open(name, O_RDWR, 0600);
  if (fd < 0) {
    if (errno == ENOENT) return ShmLiveness::kAbsent;
    return scan.complete ? ShmLiveness::kStale : ShmLiveness::kUnknown;
  }
  struct stat fst {};
  if (fstat(fd, &fst) != 0 || fst.st_dev != st.st_dev ||
      fst.st_ino != st.st_ino) {
    close(fd);
    return ShmLiveness::kUnknown;
  }
#if defined(F_SETLEASE) && defined(F_WRLCK)
  if (fcntl(fd, F_SETLEASE, F_WRLCK) == 0) {
    *lease_fd_out = fd;
    return ShmLiveness::kStale;
  }
  const int lease_errno = errno;
  close(fd);
  if (lease_errno == EAGAIN || lease_errno == EWOULDBLOCK) {
    return ShmLiveness::kLive;
  }
#else
  close(fd);
#endif
  return scan.complete ? ShmLiveness::kStale : ShmLiveness::kUnknown;
}

#endif  // __linux__

}  // namespace

ShmSweepCounters& ShmSweepGlobalCounters() {
  static ShmSweepCounters counters;
  return counters;
}

const char* ShmLivenessName(ShmLiveness v) {
  switch (v) {
    case ShmLiveness::kAbsent:
      return "absent";
    case ShmLiveness::kStale:
      return "stale";
    case ShmLiveness::kLive:
      return "live";
    case ShmLiveness::kUnknown:
      return "unknown";
  }
  return "?";
}

const char* ShmSweepResultName(ShmSweepResult r) {
  switch (r) {
    case ShmSweepResult::kSwept:
      return "swept";
    case ShmSweepResult::kNotPresent:
      return "not-present";
    case ShmSweepResult::kLive:
      return "live";
    case ShmSweepResult::kInconclusive:
      return "inconclusive";
  }
  return "?";
}

ShmLiveness ProbeShmSegmentLiveness(const char* name) {
#if defined(__linux__)
  bool complete = true;
  int lease_fd = -1;
  const ShmLiveness verdict = Classify(name, &complete, &lease_fd);
  (void)complete;
  ReleaseLease(lease_fd);
  return verdict;
#else
  (void)name;
  return ShmLiveness::kUnknown;
#endif
}

ShmSweepResult SweepStaleShmSegment(const char* name, ShmSweepStats* stats) {
  ShmSweepStats local;
  ShmSweepStats& s = stats != nullptr ? *stats : local;
  ShmSweepCounters& global = ShmSweepGlobalCounters();
  ++s.examined;
  global.examined.fetch_add(1, std::memory_order_relaxed);

  ShmLiveness verdict = ShmLiveness::kUnknown;
#if defined(__linux__)
  bool complete = true;
  int lease_fd = -1;
  verdict = Classify(name, &complete, &lease_fd);
  (void)complete;
  if (verdict == ShmLiveness::kStale) {
    // Hold the lease across shm_unlink: any process opening the object now
    // blocks on the lease until we release it, so it cannot start using an
    // inode we are about to detach (the lease is released after the unlink).
    const int unlink_rc = shm_unlink(name);
    const int unlink_errno = errno;
    ReleaseLease(lease_fd);
    if (unlink_rc == 0 || unlink_errno == ENOENT) {
      ++s.swept;
      global.swept.fetch_add(1, std::memory_order_relaxed);
      return ShmSweepResult::kSwept;
    }
    ++s.inconclusive;
    global.inconclusive.fetch_add(1, std::memory_order_relaxed);
    return ShmSweepResult::kInconclusive;
  }
#else
  (void)name;
#endif

  switch (verdict) {
    case ShmLiveness::kAbsent:
      return ShmSweepResult::kNotPresent;
    case ShmLiveness::kLive:
      ++s.skipped_live;
      global.skipped_live.fetch_add(1, std::memory_order_relaxed);
      return ShmSweepResult::kLive;
    case ShmLiveness::kStale:
      // Non-Linux: Classify never returns kStale, so this is unreachable.
    case ShmLiveness::kUnknown:
      break;
  }
  ++s.inconclusive;
  global.inconclusive.fetch_add(1, std::memory_order_relaxed);
  return ShmSweepResult::kInconclusive;
}

}  // namespace thyme
