#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/log.h"
#include "thyme/runtime/placement.h"
#include "thyme/runtime/realtime_guard.h"

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <sched.h>

namespace thyme {
CyclicThread::CyclicThread(const char* name, uint32_t period_hz, HealthSlot* health)
    : name_(name),
      period_hz_(period_hz),
      health_(health),
      // OBS-03: size the perf rings from the declared loop rate so a hot
      // node keeps a full dump window of history (default dump window:
      // kPerfRingDefaultWindowNs; low-rate nodes stay at the 4096 default).
      perf_(PerfSampler::CapacityForRate(period_hz, kPerfRingDefaultWindowNs)),
      // OBS-09: label the edge sampler's histogram series for the analyzer.
      perf_edge_(PerfSampler::CapacityForRate(period_hz,
                                              kPerfRingDefaultWindowNs),
                 PerfSampler::kHistSeriesEdgeLatency),
      work_log_(PerfWorkLog::CapacityForRate(period_hz,
                                             kPerfRingDefaultWindowNs)) {
  period_ns_ = (period_hz_ == 0) ? 0 : (1000000000ULL / period_hz_);
}

CyclicThread::~CyclicThread() {
  if (started_.load(std::memory_order_relaxed)) {
    RequestStop();
    Join();
  }
}

uint64_t CyclicThread::MonoNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

void CyclicThread::NoteLatencyEdge(uint8_t edge_id, uint64_t ns, uint8_t io) {
  perf_edge_.Note(MonoNs(), ns, edge_id, io);
}

void CyclicThread::SleepUntil(uint64_t target_ns) {
#ifdef __linux__
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(target_ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(target_ns % 1000000000ULL);
  // clock_nanosleep returns EINTR on signal delivery (TIMER_ABSTIME keeps the
  // absolute deadline across retries, matching the nanosleep loop below).
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    if (stop_.load(std::memory_order_relaxed)) return;
  }
#else
  while (!stop_.load(std::memory_order_relaxed)) {
    uint64_t now = MonoNs();
    if (now >= target_ns) break;
    uint64_t remaining = target_ns - now;
    struct timespec req;
    req.tv_sec = remaining / 1000000000ULL;
    req.tv_nsec = static_cast<long>(remaining % 1000000000ULL);
    struct timespec rem;
    if (nanosleep(&req, &rem) == 0) break;
    if (errno == EINTR) {
      continue;
    }
    break;
  }
#endif
}

void* CyclicThread::Trampoline(void* arg) {
  static_cast<CyclicThread*>(arg)->ThreadMain();
  return nullptr;
}

void CyclicThread::SetPlacement(const ThreadPlacement& p) {
  placement_ = p;
}

void CyclicThread::SetWaitInputs(Waitable* const* srcs, std::size_t n) {
  if (n > kMaxWaitInputs) n = kMaxWaitInputs;
  nwait_ = n;
  for (std::size_t i = 0; i < n; ++i) {
    waits_[i] = srcs[i];
  }
  for (std::size_t i = n; i < kMaxWaitInputs; ++i) {
    waits_[i] = nullptr;
  }
}

bool CyclicThread::AllReady() const {
  for (std::size_t i = 0; i < nwait_; ++i) {
    if (waits_[i] != nullptr && waits_[i]->empty()) return false;
  }
  return true;
}

static void CpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
#else
  sched_yield();
#endif
}

void CyclicThread::WaitAllUntil(uint64_t next) {
  const uint64_t spin_end = MonoNs() + 100000ULL;
  while (MonoNs() < spin_end) {
    if (stop_.load(std::memory_order_relaxed) || AllReady()) return;
    CpuRelax();
  }
  for (;;) {
    if (stop_.load(std::memory_order_relaxed) || MonoNs() >= next || AllReady()) {
      return;
    }
    Waitable* first_empty = nullptr;
    for (std::size_t i = 0; i < nwait_; ++i) {
      if (waits_[i] != nullptr && waits_[i]->empty()) {
        first_empty = waits_[i];
        break;
      }
    }
    if (first_empty == nullptr) return;
    first_empty->WaitUntil(next);
  }
}

bool CyclicThread::FailCreatedThread(const char* what) {
  THYME_LOG_ERROR("cyclic_thread: %s %s failed", name_, what);
  RequestStop();
  pthread_join(thread_, nullptr);
  return false;
}

bool CyclicThread::Start() {
  if (started_.load(std::memory_order_relaxed)) return true;
  if (period_hz_ == 0) {
    THYME_LOG_ERROR("cyclic_thread: %s period_hz must be > 0", name_);
    return false;
  }

  stop_.store(false, std::memory_order_relaxed);
  // SCH-13: publish the process epoch before the worker can read it.
  // Explicit start_epoch_ns wins for this thread; otherwise the first Start
  // in the process stamps CLOCK_MONOTONIC now and later Starts keep it.
  if (placement_.phase_mode != ThreadPlacement::PhaseMode::Off) {
    const uint64_t want = placement_.start_epoch_ns != 0
                              ? placement_.start_epoch_ns
                              : MonoNs();
    TryPublishSchedEpochNs(want);
  }
  int rc = pthread_create(&thread_, nullptr, &CyclicThread::Trampoline, this);
  if (rc != 0) return false;

  switch (ApplyThreadPlacement(thread_, placement_)) {
    case PlacementResult::kAffinity:
      return FailCreatedThread("affinity");
    case PlacementResult::kSched:
      return FailCreatedThread("sched");
    case PlacementResult::kOk:
      break;
  }

  // SCH-08: capture the ACTUAL scheduling state once (startup path only).
  // Best-effort placement can silently degrade (FIFO without CAP_SYS_NICE
  // stays OTHER); an explicit request that did not take effect is surfaced
  // as a mismatch flag + health counter, not a Start failure.
  if (ReadSchedState(thread_, &sched_actual_)) {
    sched_mismatch_ = SchedMismatchMask(placement_, sched_actual_);
    if (sched_mismatch_ != kSchedMismatchNone) {
      THYME_LOG_WARN_FIELDS(
          "cyclic_thread: requested scheduling not in effect",
          log::Fields()
              .Str("thread", name_)
              .Hex("mismatch", sched_mismatch_)
              .I64("policy", sched_actual_.policy)
              .I64("prio", sched_actual_.priority));
      health_->MarkSchedMismatch();
    }
  }

  started_.store(true, std::memory_order_relaxed);
  return true;
}

void CyclicThread::RequestStop() {
  stop_.store(true, std::memory_order_relaxed);
  for (std::size_t i = 0; i < nwait_; ++i) {
    if (waits_[i] != nullptr) waits_[i]->Notify();
  }
}

void CyclicThread::Join() {
  if (!started_.load(std::memory_order_relaxed)) return;
  pthread_join(thread_, nullptr);
  started_.store(false, std::memory_order_relaxed);
}

void CyclicThread::ThreadMain() {
  uint64_t next;
  if (placement_.phase_mode == ThreadPlacement::PhaseMode::Off) {
    next = MonoNs() + period_ns_;
  } else {
    uint64_t epoch = placement_.start_epoch_ns;
    if (epoch == 0) epoch = GetSchedEpochNs();
    if (epoch == 0) epoch = MonoNs();
    next = AlignCyclicDeadline(epoch, placement_.phase_ns, period_ns_,
                               MonoNs());
  }
  while (!stop_.load(std::memory_order_relaxed)) {
    if (nwait_ > 0) {
      WaitAllUntil(next);
    } else {
      SleepUntil(next);
    }
    uint64_t now = MonoNs();
    uint64_t wake_err = (now > next) ? (now - next) : 0;
    last_wake_error_ns_.store(wake_err, std::memory_order_relaxed);
    uint32_t we_clamped = (wake_err > 0xFFFFFFFFULL) ? 0xFFFFFFFFu
                                                     : static_cast<uint32_t>(wake_err);
    RecordWakeErr(we_clamped);

    uint64_t t0 = MonoNs();
    {
      rt::ScopedRealtimeSection rt_scope;
      Loop();
    }
    uint64_t t1 = MonoNs();

    uint64_t work_ns = t1 - t0;
    // flags bit1 = produced output, bit2 = consumed input: the frame join
    // reads validity from flags because nibble 0 is a legitimate frame
    // (io == 0 does not mean "no attribution").
    work_log_.Note(t0, work_ns, in_nib_, out_nib_,
                   static_cast<uint8_t>((out_valid_ ? 0x2 : 0) | (in_valid_ ? 0x4 : 0)));
    in_nib_ = 0;
    out_nib_ = 0;
    in_valid_ = false;
    out_valid_ = false;
    last_work_ns_.store(work_ns, std::memory_order_relaxed);
    uint32_t wk_clamped = (work_ns > 0xFFFFFFFFULL) ? 0xFFFFFFFFu
                                                    : static_cast<uint32_t>(work_ns);
    RecordWorkNs(wk_clamped);
    bool restarted = false;
    if (work_ns > period_ns_) {
      health_->MarkDeadlineMiss();
      // SCH-08: attribute the miss to one primary reason (overrun.h). The
      // wake error stands in for the dispatch latency; input backlog applies
      // only to cyclic threads with wait inputs.
      bool backlog = false;
      for (std::size_t i = 0; i < nwait_; ++i) {
        if (waits_[i] != nullptr && !waits_[i]->empty()) {
          backlog = true;
          break;
        }
      }
      switch (ClassifyOverrun(work_ns, wake_err, backlog, period_ns_)) {
        case OverrunReason::kOsPreemption:
          overrun_os_preemption_.fetch_add(1, std::memory_order_relaxed);
          break;
        case OverrunReason::kQueueBacklog:
          overrun_queue_backlog_.fetch_add(1, std::memory_order_relaxed);
          break;
        case OverrunReason::kLongCallback:
          overrun_long_callback_.fetch_add(1, std::memory_order_relaxed);
          break;
      }
      // SCH-04: apply the configured action. Policy counters are separate
      // from the reason counters (one action + one reason per miss).
      overrun_session::Note(overrun_policy_);
      switch (overrun_policy_) {
        case OverrunPolicy::kSkip:
          overrun_skip_actions_.fetch_add(1, std::memory_order_relaxed);
          break;
        case OverrunPolicy::kDegrade:
          overrun_degrade_actions_.fetch_add(1, std::memory_order_relaxed);
          degrade_remaining_ = kDegradeRecoverCycles;
          degraded_.store(true, std::memory_order_relaxed);
          break;
        case OverrunPolicy::kRestart:
          overrun_restart_actions_.fetch_add(1, std::memory_order_relaxed);
          degraded_.store(false, std::memory_order_relaxed);
          degrade_remaining_ = 0;
          OnOverrunRestart();
          restarted = true;
          break;
      }
    } else if (degraded_.load(std::memory_order_relaxed)) {
      // On-time cycle while degraded: recover after a bounded number of
      // clean cycles so a transient miss cannot halve the rate forever.
      if (degrade_remaining_ > 0) --degrade_remaining_;
      if (degrade_remaining_ == 0) {
        degraded_.store(false, std::memory_order_relaxed);
      }
    }
    health_->MarkAlive(t1);
    loop_count_.fetch_add(1, std::memory_order_relaxed);

    const uint64_t step_ns =
        degraded_.load(std::memory_order_relaxed) ? period_ns_ * 2 : period_ns_;
    if (restarted) {
      // kRestart re-arms from completion time: drop any catch-up backlog and
      // let the restart hook decide what application state survives.
      next = t1 + step_ns;
    } else {
      next += step_ns;
    }
    /* Spiral-of-death guard: when the wake is more than a full step overdue
       (t1 > next + step), snap forward instead of bursting catch-up
       iterations. Reuses t1 to avoid extra clock_gettime calls. */
    if (t1 > next + step_ns) {
      next = t1 + step_ns;
    }
  }
}

}  // namespace thyme
