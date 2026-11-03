#include "thyme/runtime/shm_region.h"

#include "thyme/runtime/shm_lifecycle.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ctime>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <vector>

namespace thyme {
namespace {

// Test-only ownership tracking (THYME_SHM_TRACK=1, set by ctest). Names this
// process *created* are unlinked at normal exit, including on a CHECK failure
// (check.h exits instead of aborting). Production never sets the variable, so
// segments keep their documented lifetime there.
bool TrackEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("THYME_SHM_TRACK");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  }();
  return on;
}
// Intentionally leaked: atexit cleanup runs before (or interleaved with)
// static destructors, so these must outlive every exit handler. The owning
// pointers are never deleted by design, including during process teardown.
std::mutex& TrackMu() {
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): process-lifetime owner.
  static std::mutex* const m = new std::mutex();
  return *m;
}
std::vector<std::string>& Tracked() {
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): process-lifetime owner.
  static std::vector<std::string>* const v = new std::vector<std::string>();
  return *v;
}
void CleanupTracked() {
  std::lock_guard<std::mutex> lk(TrackMu());
  for (const auto& n : Tracked()) {
    shm_unlink(n.c_str());
    const std::string lease = n + ".lease";
    shm_unlink(lease.c_str());
  }
  Tracked().clear();
  // Forked children in the test suite inherit the tracked list, but a child
  // that `_exit`s skips atexit. Only names bounded by *this* process's pid are
  // reclaimed: sweeping arbitrary dead pids would let one role unlink a
  // sibling role's channel (the fault-matrix roles share a test pid prefix).
  const std::string suffix = "_" + std::to_string(static_cast<long>(getpid()));
  DIR* d = opendir("/dev/shm");
  if (d == nullptr) return;
  while (dirent* e = readdir(d)) {
    const char* n = e->d_name;
    if (n[0] == '.') continue;
    const std::string name = n;
    const std::size_t at = name.find(suffix);
    if (at == std::string::npos) continue;
    const std::size_t end = at + suffix.size();
    if (end < name.size() && name[end] != '.' && name[end] != '_' &&
        name[end] != '-') {
      continue;
    }
    std::string path = "/dev/shm/" + name;
    shm_unlink(path.c_str());
  }
  closedir(d);
}
void Track(const char* name) {
  if (!TrackEnabled()) return;
  std::lock_guard<std::mutex> lk(TrackMu());
  Tracked().emplace_back(name);
}

// Install the handler even when this process only forks owners (the children
// inherit the environment but may `_exit`); the pid-based sweep then reclaims
// what they created.
struct TrackInit {
  TrackInit() {
    if (TrackEnabled()) std::atexit(CleanupTracked);
  }
};
const TrackInit kTrackInit;

}  // namespace

ShmRegion::~ShmRegion() { Unmap(); }

ShmRegion::ShmRegion(ShmRegion&& o) noexcept
    : addr_(o.addr_), size_(o.size_), fd_(o.fd_) {
  o.Reset();
}

ShmRegion& ShmRegion::operator=(ShmRegion&& o) noexcept {
  if (this != &o) {
    Unmap();
    addr_ = o.addr_;
    size_ = o.size_;
    fd_ = o.fd_;
    o.Reset();
  }
  return *this;
}

void ShmRegion::Reset() {
  addr_ = nullptr;
  size_ = 0;
  fd_ = -1;
}

void ShmRegion::Unmap() {
  if (addr_ != nullptr) {
    munmap(addr_, size_);
    addr_ = nullptr;
  }
  if (fd_ != -1) {
    close(fd_);
    fd_ = -1;
  }
  size_ = 0;
}

void ShmRegion::UnlinkName(const char* name) { shm_unlink(name); }

std::size_t ShmRegion::PageSize() {
  long ps = sysconf(_SC_PAGESIZE);
  if (ps <= 0) ps = 4096;
  return static_cast<std::size_t>(ps);
}

std::size_t ShmRegion::RoundUpPage(std::size_t bytes) {
  const std::size_t page = PageSize();
  if (bytes == 0) return page;
  return ((bytes + page - 1) / page) * page;
}

bool ShmRegion::MapFd(int fd, std::size_t bytes, bool read_only) {
  const int prot = read_only ? PROT_READ : (PROT_READ | PROT_WRITE);
  void* p = mmap(nullptr, bytes, prot, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    return false;
  }
  fd_ = fd;
  addr_ = p;
  size_ = bytes;
  return true;
}

bool ShmRegion::Create(const char* name, std::size_t bytes) {
  Unmap();
  int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd == -1 && errno == EEXIST) {
    // A crashed session can leave its pid-named segment behind. Reclaim it
    // only when liveness is proved absent; otherwise keep the old behaviour
    // (Create fails, no segment is touched).
    if (SweepStaleShmSegment(name) == ShmSweepResult::kSwept) {
      fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    }
  }
  if (fd == -1) return false;
  const std::size_t need = RoundUpPage(bytes);
  if (ftruncate(fd, static_cast<off_t>(need)) != 0) {
    close(fd);
    shm_unlink(name);  // do not leave the half-created object behind
    return false;
  }
  if (!MapFd(fd, need, false)) {
    close(fd);
    shm_unlink(name);
    return false;
  }
  Track(name);
  return true;
}

bool ShmRegion::CreateDescribed(const char* name, const ShmPageSpec& spec) {
  if (!IsShmPageSpecValid(spec) || !Create(name, spec.page_bytes)) return false;
  // The region owns the mapped object lifetime, as in Create<T>.
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): object lives in SHM.
  auto* header = new (data()) ShmSuperHeader;
  InitShmSuperHeader(header, spec);
  return true;
}

bool ShmRegion::OpenDescribed(const char* name, const ShmPageSpec& spec,
                              uint64_t timeout_ns, bool read_only,
                              ShmPageError* error) {
  if (!IsShmPageSpecValid(spec)) {
    if (error != nullptr) *error = ShmPageError{ShmPageErrc::kInvalidSpec, 0, 0};
    return false;
  }
  struct timespec start {};
  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return false;
  const auto elapsed_ns = [&start]() -> uint64_t {
    struct timespec now {};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return UINT64_MAX;
    const int64_t total =
        (static_cast<int64_t>(now.tv_sec) - start.tv_sec) * 1000000000LL +
        (static_cast<int64_t>(now.tv_nsec) - start.tv_nsec);
    return total > 0 ? static_cast<uint64_t>(total) : 0;
  };
  // Phase 1: map only the fixed super-header. This makes every field
  // mismatch diagnosable, including an expected page_bytes that is larger
  // than the producer actually created (mapping spec.page_bytes up front
  // would just time out as "not ready"). kNotReady (magic still 0) is the
  // only transient result and is retried until the timeout.
  for (;;) {
    const uint64_t used = elapsed_ns();
    const uint64_t remaining = used < timeout_ns ? timeout_ns - used : 0;
    if (!OpenRetry(name, sizeof(ShmSuperHeader), remaining, read_only)) {
      if (error != nullptr) {
        *error = ShmPageError{ShmPageErrc::kNotReady, spec.magic, 0};
      }
      return false;
    }
    const ShmPageErrc result =
        ValidateShmSuperHeader(data(), size(), spec, error);
    if (result == ShmPageErrc::kNotReady) {
      Unmap();
      if (elapsed_ns() >= timeout_ns) break;
      struct timespec delay {};
      delay.tv_nsec = 10000000L;
      nanosleep(&delay, nullptr);
      continue;
    }
    if (result != ShmPageErrc::kOk) {
      Unmap();
      return false;
    }
    break;
  }
  if (data() == nullptr) {
    if (error != nullptr) {
      *error = ShmPageError{ShmPageErrc::kNotReady, spec.magic, 0};
    }
    return false;
  }
  // Phase 2: remap the full page the header announced and revalidate. A
  // producer that recreated the object between the two mappings is caught
  // here rather than by reading a truncated/foreign page.
  const std::size_t mapped_header = size();
  if (!OpenRetry(name, spec.page_bytes, 0, read_only)) {
    if (error != nullptr) {
      *error = ShmPageError{ShmPageErrc::kTooSmall, spec.page_bytes,
                            mapped_header};
    }
    return false;
  }
  if (size() < spec.page_bytes) {
    if (error != nullptr) {
      *error = ShmPageError{ShmPageErrc::kTooSmall, spec.page_bytes, size()};
    }
    Unmap();
    return false;
  }
  const ShmPageErrc result =
      ValidateShmSuperHeader(data(), size(), spec, error);
  if (result != ShmPageErrc::kOk) {
    Unmap();
    return false;
  }
  return true;
}

bool ShmRegion::OpenRetry(const char* name, std::size_t bytes,
                          uint64_t timeout_ns, bool read_only) {
  Unmap();
  const uint64_t step_ns = 10000000ULL;  // 10ms
  uint64_t elapsed_ns = 0;
  const std::size_t need = RoundUpPage(bytes);
  const int oflag = read_only ? O_RDONLY : O_RDWR;
  while (true) {
    int fd = shm_open(name, oflag, 0600);
    if (fd != -1) {
      struct stat st {};
      if (fstat(fd, &st) != 0) {
        close(fd);
        return false;
      }
      // mmap length is the page-rounded `need`; a mapping longer than the
      // backing file would SIGBUS past EOF, so wait until the producer's
      // ftruncate has grown the object to at least `need`.
      if (static_cast<uint64_t>(st.st_size) < need) {
        close(fd);
        if (elapsed_ns >= timeout_ns) return false;
        struct timespec ts {};
        ts.tv_sec = 0;
        ts.tv_nsec = static_cast<long>(step_ns);
        nanosleep(&ts, nullptr);
        elapsed_ns += step_ns;
        continue;
      }
      if (!MapFd(fd, need, read_only)) {
        close(fd);
        return false;
      }
      return true;
    }
    if (elapsed_ns >= timeout_ns) return false;
    struct timespec ts {};
    ts.tv_sec = 0;
    ts.tv_nsec = static_cast<long>(step_ns);
    nanosleep(&ts, nullptr);
    elapsed_ns += step_ns;
  }
}

}  // namespace thyme
