#include "thyme/runtime/rotating_file.h"

#include "thyme/runtime/realtime_guard.h"

#include <atomic>
#include <cerrno>
#include <cstdio>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace thyme {
namespace logging {
namespace {

std::atomic<uint64_t> g_limit{0};
std::atomic<uint64_t> g_used{0};
std::atomic<uint64_t> g_drops{0};
std::atomic<uint64_t> g_rotations{0};
std::atomic<uint64_t> g_deferred_rotations{0};

uint64_t FileSizeAt(const std::string& path) {
#if defined(_WIN32)
  (void)path;
  return 0;
#else
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) return 0;
  if (!S_ISREG(st.st_mode)) return 0;
  return static_cast<uint64_t>(st.st_size);
#endif
}

}  // namespace

void LogBudget::Configure(uint64_t limit_bytes) {
  g_limit.store(limit_bytes, std::memory_order_relaxed);
}

uint64_t LogBudget::limit_bytes() {
  return g_limit.load(std::memory_order_relaxed);
}

uint64_t LogBudget::used_bytes() {
  return g_used.load(std::memory_order_relaxed);
}

uint64_t LogBudget::drops() {
  return g_drops.load(std::memory_order_relaxed);
}

uint64_t LogBudget::rotations() {
  return g_rotations.load(std::memory_order_relaxed);
}

uint64_t LogBudget::deferred_rotations() {
  return g_deferred_rotations.load(std::memory_order_relaxed);
}

bool LogBudget::WouldExceed(uint64_t bytes) {
  const uint64_t limit = g_limit.load(std::memory_order_relaxed);
  if (limit == 0) return false;
  const uint64_t used = g_used.load(std::memory_order_relaxed);
  return used > limit || bytes > limit - used;
}

void LogBudget::Add(uint64_t bytes) {
  g_used.fetch_add(bytes, std::memory_order_relaxed);
}

void LogBudget::Subtract(uint64_t bytes) {
  uint64_t used = g_used.load(std::memory_order_relaxed);
  for (;;) {
    const uint64_t next = bytes >= used ? 0 : used - bytes;
    if (g_used.compare_exchange_weak(used, next, std::memory_order_relaxed)) {
      return;
    }
  }
}

void LogBudget::NoteDrop() {
  g_drops.fetch_add(1, std::memory_order_relaxed);
}

void LogBudget::NoteRotation() {
  g_rotations.fetch_add(1, std::memory_order_relaxed);
}

void LogBudget::NoteDeferredRotation() {
  g_deferred_rotations.fetch_add(1, std::memory_order_relaxed);
}

void LogBudget::ResetForTest() {
  g_limit.store(0, std::memory_order_relaxed);
  g_used.store(0, std::memory_order_relaxed);
  g_drops.store(0, std::memory_order_relaxed);
  g_rotations.store(0, std::memory_order_relaxed);
  g_deferred_rotations.store(0, std::memory_order_relaxed);
}

RotatingFile::~RotatingFile() { Close(); }

bool RotatingFile::Open(const RotateConfig& cfg) {
  Close();
  if (cfg.path.empty()) return false;
  path_ = cfg.path;
  max_bytes_ = cfg.max_bytes;
  keep_ = cfg.keep;
  mandatory_ = cfg.mandatory;
  account_ = cfg.account;
  bytes_written_ = 0;
  records_ = 0;
  drops_ = 0;
  rotations_ = 0;
  rotate_errors_ = 0;
#if defined(_WIN32)
  path_.clear();
  return false;
#else
  if (!Reopen()) {
    path_.clear();
    return false;
  }
  // Adopt the on-disk generations BEFORE rotating: the rotation path subtracts
  // the evicted generation from the budget, so it must already be counted.
  if (account_) {
    accounted_ = DiskBytes();
    LogBudget::Add(accounted_);
  }
  // A file left over from a previous run may already be at the bound.
  (void)RotateIfNeeded();
  if (fd_ < 0) {
    ForgetAccounting();
    path_.clear();
    return false;
  }
  return true;
#endif
}

bool RotatingFile::Reopen() {
#if defined(_WIN32)
  return false;
#else
  fd_ = open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd_ < 0) {
    size_bytes_ = 0;
    return false;
  }
  size_bytes_ = FileSizeAt(path_);
  return true;
#endif
}

uint64_t RotatingFile::GenerationSize(uint32_t gen) const {
  return FileSizeAt(path_ + "." + std::to_string(gen));
}

bool RotatingFile::Rotate() {
#if defined(_WIN32)
  return false;
#else
  if (keep_ == 0 || path_.empty()) return false;
  const int saved = fd_;
  fd_ = -1;
  if (saved >= 0) close(saved);

  // The oldest generation is dropped first: its bytes leave the budget, the
  // renames move the rest without changing their size. ARCH-05: only release
  // the bytes when the unlink actually succeeded (or the file was already
  // gone). On failure the generation may still be on disk, so keeping it in
  // the budget over-counts (never under-counts) and rotation_errors makes the
  // condition visible.
  const std::string oldest = path_ + "." + std::to_string(keep_);
  const uint64_t evicted = FileSizeAt(oldest);
  const int unlink_rc = unlink(oldest.c_str());
  if (unlink_rc != 0 && errno != ENOENT) {
    ++rotate_errors_;
  } else if (account_ && evicted != 0) {
    LogBudget::Subtract(evicted);
    accounted_ = evicted >= accounted_ ? 0 : accounted_ - evicted;
  }
  for (uint32_t i = keep_; i > 1; --i) {
    const std::string from = path_ + "." + std::to_string(i - 1);
    const std::string to = path_ + "." + std::to_string(i);
    if (rename(from.c_str(), to.c_str()) != 0 && errno != ENOENT) {
      ++rotate_errors_;  // ENOENT: fewer rotations exist
    }
  }
  const std::string rotated = path_ + ".1";
  const bool moved = rename(path_.c_str(), rotated.c_str()) == 0;
  if (moved) {
    ++rotations_;
    LogBudget::NoteRotation();
  }
  if (!Reopen()) return false;
  return moved;
#endif
}

bool RotatingFile::RotateIfNeeded() {
  if (fd_ < 0 || max_bytes_ == 0 || keep_ == 0) return false;
  if (size_bytes_ < max_bytes_) return false;
  // SCH-06: rename()/unlink()/reopen() must not run in the RT dispatch/Loop
  // context. Keep appending to the live file and let the next non-RT caller
  // rotate it; the cap is soft (the header already allows one-record
  // overshoot), and RT logging is rate-limited by contract (CR-03).
  if (rt::InRealtimeSection()) {
    LogBudget::NoteDeferredRotation();
    return false;
  }
  return Rotate();
}

std::ptrdiff_t RotatingFile::Append(const void* data, std::size_t len) {
#if defined(_WIN32)
  (void)data;
  (void)len;
  return -1;
#else
  if (fd_ < 0) {
    ++drops_;
    LogBudget::NoteDrop();
    return -1;
  }
  // Rotation check: one comparison; a failed rotation keeps the live file
  // growing (losing a record would be worse than a soft cap overshoot).
  (void)RotateIfNeeded();
  if (!mandatory_ && account_ &&
      LogBudget::WouldExceed(static_cast<uint64_t>(len))) {
    ++drops_;
    LogBudget::NoteDrop();
    return 0;
  }
  const ssize_t wrote = write(fd_, data, len);
  if (wrote > 0) {
    const uint64_t n = static_cast<uint64_t>(wrote);
    size_bytes_ += n;
    bytes_written_ += n;
    if (account_) {
      accounted_ += n;
      LogBudget::Add(n);
    }
    if (wrote == static_cast<ssize_t>(len)) {
      ++records_;
      return static_cast<std::ptrdiff_t>(len);
    }
  }
  ++drops_;
  LogBudget::NoteDrop();
  return wrote < 0 ? -1 : static_cast<std::ptrdiff_t>(wrote);
#endif
}

bool RotatingFile::Sync() {
#if defined(_WIN32)
  return false;
#else
  if (fd_ < 0) return false;
  return fdatasync(fd_) == 0;
#endif
}

void RotatingFile::ForgetAccounting() {
  if (account_ && accounted_ != 0) {
    LogBudget::Subtract(accounted_);
    accounted_ = 0;
  }
}

void RotatingFile::Close() {
#if !defined(_WIN32)
  if (fd_ >= 0) close(fd_);
#endif
  fd_ = -1;
  ForgetAccounting();
  path_.clear();
  size_bytes_ = 0;
}

uint64_t RotatingFile::DiskBytes() const {
  uint64_t total = FileSizeAt(path_);
  for (uint32_t i = 1; i <= keep_; ++i) total += GenerationSize(i);
  return total;
}

}  // namespace logging
}  // namespace thyme
