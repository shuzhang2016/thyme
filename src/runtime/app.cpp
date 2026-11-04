#include "thyme/runtime/app.h"
#include "thyme/limits.h"
#include "thyme/runtime/log.h"
#include "thyme/runtime/resource_check.h"

#include <atomic>
#include <csignal>

#ifdef __linux__
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/resource.h>
#endif

namespace thyme {
namespace {

// A small RLIMIT_MEMLOCK makes mlockall(MCL_FUTURE) poison every later
// mapping: thread stacks and SHM pages are charged to the same tiny budget
// and fail with EAGAIN. Treat this as "memory locking unavailable" instead.
constexpr rlim_t kMlockMinBytes = 64ULL * 1024 * 1024;

std::atomic<bool> g_prepared{false};
std::atomic<bool> g_memory_locked{false};
std::atomic<bool> g_malloc_tuned{false};
std::atomic<uint32_t> g_memory_flags{0};
std::atomic<int> g_stop{0};

void OnStopSignal(int) {
  g_stop.store(1, std::memory_order_release);
}

}  // namespace

namespace {

// THYME_NO_MLOCK is honored only for explicit truthy values
// (1/true/yes/on, case-insensitive); anything else (incl. "0") keeps the
// default mlockall behavior. The predicate lives in resource_check.h so the
// OPS-07 self-check and mlockall cannot disagree about the opt-out.
bool MlockOptOutEnabled() {
  return resource::MlockOptOut(std::getenv("THYME_NO_MLOCK"));
}

}  // namespace

bool App::Prepare() {
  if (g_prepared.load(std::memory_order_acquire)) return true;

  uint32_t flags = 0;
#ifdef __linux__
  // THYME_NO_MLOCK=1 opts out on memory-constrained targets: with
  // mlockall(MCL_CURRENT|MCL_FUTURE) every mapping is pinned — including full
  // 8 MB thread stacks (locked mappings are faulted in) — and nothing can be
  // paged out under pressure, which OOM-kills the process next to large
  // third-party workloads on memory-constrained target boards.
  const bool mlock_disabled = MlockOptOutEnabled();
  if (mlock_disabled) {
    flags |= kMemoryMlockDisabled;
    THYME_LOG_WARN("thyme: THYME_NO_MLOCK set — skipping mlockall "
                   "(RT threads may page-fault)");
  }
  struct rlimit mlock_limit {};
  const bool have_mlock_limit = getrlimit(RLIMIT_MEMLOCK, &mlock_limit) == 0;
  const bool mlock_limit_too_small =
      have_mlock_limit && mlock_limit.rlim_cur != RLIM_INFINITY &&
      mlock_limit.rlim_cur < kMlockMinBytes;
  const bool locked = !mlock_disabled && !mlock_limit_too_small &&
                      mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
  if (!locked && !mlock_disabled) {
    flags |= kMemoryMlockFailed;
    if (mlock_limit_too_small) {
      THYME_LOG_WARN("thyme: memory locking disabled: RLIMIT_MEMLOCK=%llu "
                     "bytes is below the %llu-byte minimum; RT threads may "
                     "page-fault",
                     static_cast<unsigned long long>(mlock_limit.rlim_cur),
                     static_cast<unsigned long long>(kMlockMinBytes));
    } else {
      THYME_LOG_ERROR("thyme: mlockall(MCL_CURRENT|MCL_FUTURE) failed: %s "
                      "(need CAP_IPC_LOCK or RLIMIT_MEMLOCK); RT threads may "
                      "page-fault",
                      std::strerror(errno));
    }
    // A failed/partial mlockall can leave MCL_FUTURE set. Future mappings
    // (thread stacks, SHM pages) would then be forced into the locked-memory
    // budget and fail with EAGAIN under a small RLIMIT_MEMLOCK. Clear it.
    munlockall();
  }
  bool tuned = mallopt(M_ARENA_MAX, 1) != 0;
#ifdef M_TRIM_THRESHOLD
  tuned = mallopt(M_TRIM_THRESHOLD, -1) != 0 && tuned;
#endif
  if (!tuned) {
    flags |= kMemoryMalloptFailed;
    THYME_LOG_WARN("thyme: mallopt arena/trim tuning failed; allocator may "
                   "add jitter");
  }
  g_memory_locked.store(locked, std::memory_order_release);
  g_malloc_tuned.store(tuned, std::memory_order_release);
#else
  // TODO(mac): process-wide malloc arena / trim tuning is Linux mallopt-only.
  // Lab build is a no-op; revisit if Loop() ever pages or jitter-correlates with malloc.
  flags |= kMemoryUnsupported;
  g_memory_locked.store(false, std::memory_order_release);
  g_malloc_tuned.store(false, std::memory_order_release);
#endif

  g_memory_flags.store(flags, std::memory_order_release);
  g_prepared.store(true, std::memory_order_release);
  return true;
}

bool App::memory_locked() {
  return g_memory_locked.load(std::memory_order_acquire);
}

bool App::malloc_tuned() {
  return g_malloc_tuned.load(std::memory_order_acquire);
}

uint32_t App::memory_flags() {
  return g_memory_flags.load(std::memory_order_acquire);
}

bool App::stop_requested() {
  return g_stop.load(std::memory_order_acquire) != 0;
}

void App::ResetStop() {
  g_stop.store(0, std::memory_order_release);
}

bool App::Add(CyclicThread* t) {
  if (t == nullptr || n_ + ev_n_ >= kMaxThreads) return false;
  threads_[n_++] = t;
  return true;
}

bool App::Add(EventThread* t) {
  if (t == nullptr || n_ + ev_n_ >= kMaxThreads) return false;
  ev_threads_[ev_n_++] = t;
  return true;
}

bool App::Start() {
  bool ok = Prepare();
  if (started_) return ok;
  const uint32_t memory_flags = App::memory_flags();
  for (std::size_t i = 0; i < ev_n_; ++i) {
    ev_threads_[i]->health()->SetMemoryFlags(memory_flags);
  }
  for (std::size_t i = 0; i < n_; ++i) {
    threads_[i]->health()->SetMemoryFlags(memory_flags);
  }
  for (std::size_t i = 0; i < ev_n_; ++i) {
    if (!ev_threads_[i]->Start()) {
      for (std::size_t j = 0; j <= i; ++j) {
        ev_threads_[j]->RequestStop();
      }
      for (std::size_t j = 0; j <= i; ++j) {
        ev_threads_[j]->Join();
      }
      started_ = false;
      return false;
    }
  }
  for (std::size_t i = 0; i < n_; ++i) {
    if (!threads_[i]->Start()) {
      for (std::size_t j = 0; j < ev_n_; ++j) {
        ev_threads_[j]->RequestStop();
      }
      for (std::size_t j = 0; j <= i; ++j) {
        threads_[j]->RequestStop();
      }
      for (std::size_t j = 0; j < ev_n_; ++j) {
        ev_threads_[j]->Join();
      }
      for (std::size_t j = 0; j <= i; ++j) {
        threads_[j]->Join();
      }
      started_ = false;
      return false;
    }
  }
  started_ = true;
  return ok;
}

void App::RequestStop() {
  g_stop.store(1, std::memory_order_release);
  for (std::size_t i = 0; i < n_; ++i) {
    threads_[i]->RequestStop();
  }
  for (std::size_t i = 0; i < ev_n_; ++i) {
    ev_threads_[i]->RequestStop();
  }
}

void App::Join() {
  for (std::size_t i = 0; i < n_; ++i) {
    threads_[i]->Join();
  }
  for (std::size_t i = 0; i < ev_n_; ++i) {
    ev_threads_[i]->Join();
  }
  started_ = false;
}

void App::InstallStopSignals() {
  std::signal(SIGINT, OnStopSignal);
  std::signal(SIGTERM, OnStopSignal);
}

}  // namespace thyme
