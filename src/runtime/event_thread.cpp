#include "thyme/runtime/event_thread.h"

#include "thyme/runtime/futex.h"
#include "thyme/runtime/log.h"
#include "thyme/runtime/placement.h"
#include "thyme/runtime/proc_sampler.h"
#include "thyme/runtime/realtime_guard.h"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace thyme {

namespace {
ReplayConsumeGateFn g_replay_consume_gate = nullptr;
}  // namespace

void SetReplayConsumeGate(ReplayConsumeGateFn fn) {
  g_replay_consume_gate = fn;
}

ReplayConsumeGateFn GetReplayConsumeGate() { return g_replay_consume_gate; }

static bool ConsumePaused() {
  ReplayConsumeGateFn fn = g_replay_consume_gate;
  return fn != nullptr && fn();
}

EventThread::EventThread(const char* name, HealthSlot* health,
                         uint32_t expected_hz, uint64_t dispatch_deadline_ns)
    : name_(name),
      health_(health),
      expected_period_ns_(expected_hz == 0 ? 0 : 1000000000ULL / expected_hz),
      dispatch_deadline_ns_(dispatch_deadline_ns),
      idle_poll_ns_(IdlePollNs(expected_hz)),
      hub_(this),
      // OBS-03: size the perf rings from the declared message rate (same
      // dump-window contract as CyclicThread).
      perf_(PerfSampler::CapacityForRate(expected_hz,
                                         kPerfRingDefaultWindowNs)),
      // OBS-09: histogram series labels (latency = 0 default, edge = 1,
      // SCH-08 callback work = 2) so the analyzer can tell them apart.
      perf_edge_(PerfSampler::CapacityForRate(expected_hz,
                                              kPerfRingDefaultWindowNs),
                 PerfSampler::kHistSeriesEdgeLatency),
      cb_work_(PerfSampler::CapacityForRate(expected_hz,
                                            kPerfRingDefaultWindowNs),
               PerfSampler::kHistSeriesCallbackWork),
      work_log_(PerfWorkLog::CapacityForRate(expected_hz,
                                             kPerfRingDefaultWindowNs)) {}

EventThread::~EventThread() {
  if (started_.load(std::memory_order_relaxed)) {
    RequestStop();
    Join();
  }
  // Pipes may outlive the thread; they must not notify a dead hub.
  for (std::size_t i = 0; i < nwait_; ++i) {
    if (waits_[i] != nullptr) waits_[i]->SetNotify(nullptr);
  }
}

EventThread::WakeHub::WakeHub(const EventThread* owner) : owner_(owner) {
  pthread_mutex_init(&mu_, nullptr);
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
#ifdef __linux__
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
  pthread_cond_init(&cv_, &attr);
  pthread_condattr_destroy(&attr);
}

EventThread::WakeHub::~WakeHub() {
  pthread_cond_destroy(&cv_);
  pthread_mutex_destroy(&mu_);
}

bool EventThread::WakeHub::empty() const { return !owner_->AnyReady(); }

std::atomic<uint32_t>* EventThread::WakeHub::NotifyWakeWord() const {
  return owner_->wake_word_;
}

void EventThread::WakeHub::Notify() {
  // Bump the futex word first: a waiter snapshotting it afterwards either
  // sees the bump (and skips the wait) or is already inside FUTEX_WAIT and
  // gets woken. The owner re-checks AnyReady before sleeping, so a push
  // landing between the check and the wait is not lost.
  owner_->wake_word_->fetch_add(1, std::memory_order_seq_cst);
  // A private word has exactly this hub as its waiter, so its local count is
  // authoritative and a zero count can skip the syscall. A word redirected to
  // shared memory can be shared by several hubs; this hub's local count says
  // nothing about the others, so it must not gate the wake (PERF-14).
  const bool private_word = owner_->wake_word_ == &owner_->wake_word_own_;
  if (private_word &&
      waiters_.load(std::memory_order_seq_cst) == 0) {
    return;
  }
#ifdef __APPLE__
  pthread_mutex_lock(&mu_);
  pthread_cond_broadcast(&cv_);
  pthread_mutex_unlock(&mu_);
#else
  if (private_word) {
    FutexWakeOne(owner_->wake_word_);
  } else {
    FutexWakeAll(owner_->wake_word_);
  }
#endif
}

void EventThread::WakeHub::WaitUntil(uint64_t deadline_ns) {
  // Registration is the syscall gate for Notify. It happens before the
  // readiness re-check, and Notify bumps the word before it reads the count,
  // so the pair is race-free in both directions (PERF-14).
  waiters_.fetch_add(1, std::memory_order_seq_cst);
#ifdef __APPLE__
  std::atomic_thread_fence(std::memory_order_seq_cst);
  pthread_mutex_lock(&mu_);
  if (empty() && !owner_->stop_.load(std::memory_order_acquire)) {
    struct timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t now_ns =
        static_cast<uint64_t>(now.tv_sec) * 1000000000ULL +
        static_cast<uint64_t>(now.tv_nsec);
    if (now_ns < deadline_ns) {
      const uint64_t rem = deadline_ns - now_ns;
      struct timespec rel {};
      rel.tv_sec = static_cast<time_t>(rem / 1000000000ULL);
      rel.tv_nsec = static_cast<long>(rem % 1000000000ULL);
      pthread_cond_timedwait_relative_np(&cv_, &mu_, &rel);
    }
  }
  pthread_mutex_unlock(&mu_);
  waiters_.fetch_sub(1, std::memory_order_release);
#else
  for (;;) {
    // stop_ is part of the wait condition, not just the outer loop: a stop
    // that lands after the registration but before FUTEX_WAIT must not be
    // ridden out to the idle-poll deadline.
    if (!empty() || owner_->stop_.load(std::memory_order_acquire)) break;
    const uint64_t now = MonoNs();
    if (now >= deadline_ns) break;
    const uint32_t snap = owner_->wake_word_->load(std::memory_order_seq_cst);
    if (!empty() || owner_->stop_.load(std::memory_order_acquire)) break;
    const uint64_t rem = deadline_ns - now;
    struct timespec rel {};
    rel.tv_sec = static_cast<time_t>(rem / 1000000000ULL);
    rel.tv_nsec = static_cast<long>(rem % 1000000000ULL);
    FutexWait(owner_->wake_word_, snap, &rel);
  }
  waiters_.fetch_sub(1, std::memory_order_release);
#endif
}

uint64_t EventThread::MonoNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

void* EventThread::Trampoline(void* arg) {
  static_cast<EventThread*>(arg)->ThreadMain();
  return nullptr;
}

void EventThread::SetWaitInputs(Waitable* const* srcs, std::size_t n) {
  if (n > kMaxWaitInputs) n = kMaxWaitInputs;
  nwait_ = n;
  for (std::size_t i = 0; i < n; ++i) {
    waits_[i] = srcs[i];
    // Attach the shared wake hub: a push on any input wakes WaitAny.
    // Waitables without the hook (default no-op SetNotify) still drain via
    // the AnyReady scan at each idle-poll tick.
    if (srcs[i] != nullptr) srcs[i]->SetNotify(&hub_);
  }
  for (std::size_t i = n; i < kMaxWaitInputs; ++i) {
    waits_[i] = nullptr;
  }
}

void EventThread::SetInputBudget(uint32_t max_per_input_per_dispatch,
                                 uint32_t n_inputs) {
  input_budget_ = max_per_input_per_dispatch;
  uint32_t slots = n_inputs != 0 ? n_inputs : static_cast<uint32_t>(nwait_);
  if (slots > kMaxWaitInputs) slots = kMaxWaitInputs;
  budget_inputs_ = (input_budget_ == 0) ? 0 : slots;
  for (std::size_t i = 0; i < kMaxWaitInputs; ++i) {
    cycle_input_count_[i] = 0;
    cycle_input_hit_[i] = 0;
  }
}

bool EventThread::AnyReady() const {
  for (std::size_t i = 0; i < nwait_; ++i) {
    if (waits_[i] != nullptr && !waits_[i]->empty()) return true;
  }
  return false;
}

void EventThread::WaitAny() {
  for (;;) {
    if (stop_.load(std::memory_order_relaxed) || AnyReady()) return;
    // Block on the shared wake hub only. WakeHub::empty() re-checks
    // AnyReady under the register-then-check protocol, so a push landing
    // between the AnyReady above and the wait is not lost. With no inputs
    // registered the hub simply rides out the idle poll (or the stop kick).
    hub_.WaitUntil(MonoNs() + idle_poll_ns_);
    if (!stop_.load(std::memory_order_relaxed) && !AnyReady()) {
      // Idle-poll tick: the wait timed out with no input. Keep the alive
      // counter advancing so the stall watchdog only flags a truly wedged
      // thread, not a message-starved one. Idle ticks are not dispatches.
      health_->MarkAlive(MonoNs());
    }
  }
}

bool EventThread::Start() {
  if (started_.load(std::memory_order_relaxed)) return true;
  stop_.store(false, std::memory_order_relaxed);
  int rc = pthread_create(&thread_, nullptr, &EventThread::Trampoline, this);
  if (rc != 0) {
    THYME_LOG_ERROR("event_thread: %s pthread_create failed: %s", name_,
                    std::strerror(rc));
    return false;
  }
  const PlacementResult pr = ApplyThreadPlacement(thread_, placement_);
  if (pr != PlacementResult::kOk) {
    THYME_LOG_ERROR("event_thread: %s %s failed", name_,
                    pr == PlacementResult::kAffinity ? "affinity" : "sched");
    RequestStop();
    pthread_join(thread_, nullptr);
    return false;
  }
  // SCH-08: capture the ACTUAL scheduling state once (startup path only).
  // Best-effort placement can silently degrade (FIFO without CAP_SYS_NICE
  // stays OTHER); an explicit request that did not take effect is surfaced
  // as a mismatch flag + health counter, not a Start failure.
  if (ReadSchedState(thread_, &sched_actual_)) {
    sched_mismatch_ = SchedMismatchMask(placement_, sched_actual_);
    if (sched_mismatch_ != kSchedMismatchNone) {
      THYME_LOG_WARN_FIELDS(
          "event_thread: requested scheduling not in effect",
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

uint32_t EventThread::RegisterObservability(ProcSampler* sampler) {
  if (sampler == nullptr) return 0;
  uint32_t n = 0;
  for (std::size_t i = 0; i < nwait_; ++i) {
    if (waits_[i] == nullptr ||
        waits_[i]->Depth() == Waitable::kDepthUnknown) {
      continue;
    }
    char name[kPerfSeriesNameRecordBytes];
    std::snprintf(name, sizeof(name), "queue_depth_%zu", i);
    if (sampler->RegisterProbe(name, &DepthProbe, waits_[i]) >= 0) ++n;
  }
  if (sampler->RegisterCounter("overrun_long_callback",
                               &overrun_long_callback_) >= 0) {
    ++n;
  }
  if (sampler->RegisterCounter("overrun_queue_backlog",
                               &overrun_queue_backlog_) >= 0) {
    ++n;
  }
  if (sampler->RegisterCounter("overrun_os_preemption",
                               &overrun_os_preemption_) >= 0) {
    ++n;
  }
  // SCH-03: budget exhaustion is only registered when a budget is configured,
  // so an unbudgeted node keeps the pre-SCH-03 probe count (5 with 2 inputs).
  if (input_budget_ != 0) {
    for (uint32_t i = 0; i < budget_inputs_; ++i) {
      char name[kPerfSeriesNameRecordBytes];
      std::snprintf(name, sizeof(name), "input_budget_exhausted_%u",
                    static_cast<unsigned>(i));
      if (sampler->RegisterCounter(name, &input_budget_exhausted_[i]) >= 0) {
        ++n;
      }
    }
  }
  return n;
}

uint64_t EventThread::DepthProbe(void* ctx) {
  return static_cast<Waitable*>(ctx)->Depth();
}

void EventThread::RequestStop() {
  // Release: the hub's wait condition reads stop_ with acquire, and Notify's
  // seq_cst word bump publishes it to a waiter that only observes the bump.
  stop_.store(true, std::memory_order_release);
  // The worker blocks on the hub only; one kick wakes it.
  hub_.Notify();
}

void EventThread::Join() {
  if (!started_.load(std::memory_order_relaxed)) return;
  pthread_join(thread_, nullptr);
  started_.store(false, std::memory_order_relaxed);
}

void EventThread::ThreadMain() {
  while (!stop_.load(std::memory_order_relaxed)) {
    WaitAny();
    if (stop_.load(std::memory_order_relaxed)) break;
    if (ConsumePaused()) {
      // Replay pause: do not drain inputs; bounded sleep avoids busy-spin
      // when pipes are non-empty (M6.2).
      struct timespec pause_ts {0, 1000000};
      nanosleep(&pause_ts, nullptr);
      health_->MarkAlive(MonoNs());
      continue;
    }
    // SCH-04 kDegrade: insert a cooldown equal to the dispatch budget before
    // each batch while degraded. This is a deliberate rate reduction, not an
    // idle poll, so the wake-to-start latency below excludes it.
    if (degraded_.load(std::memory_order_relaxed) &&
        dispatch_deadline_ns_ > 0) {
      struct timespec cool_ts {};
      cool_ts.tv_sec =
          static_cast<time_t>(dispatch_deadline_ns_ / 1000000000ULL);
      cool_ts.tv_nsec =
          static_cast<long>(dispatch_deadline_ns_ % 1000000000ULL);
      nanosleep(&cool_ts, nullptr);
      if (stop_.load(std::memory_order_relaxed)) break;
    }
    const uint64_t t_ready = MonoNs();  // wake: inputs ready, pre-dispatch
    if (input_budget_ != 0) {
      for (uint32_t i = 0; i < budget_inputs_; ++i) {
        cycle_input_count_[i] = 0;
        cycle_input_hit_[i] = 0;
      }
    }
    const uint64_t t0 = MonoNs();
    in_valid_ = false;
    span_multi_ = false;
    {
      rt::ScopedRealtimeSection rt_scope;
      Dispatch();
    }
    const uint64_t t1 = MonoNs();

    const uint64_t work_ns = t1 - t0;
    const uint64_t dispatch_latency_ns = t0 - t_ready;
    last_dispatch_latency_ns_.store(dispatch_latency_ns,
                                    std::memory_order_relaxed);
    work_log_.Note(t0, work_ns, in_nib_, out_nib_,
                   static_cast<uint8_t>((span_multi_ ? 0x1 : 0x0) | (out_valid_ ? 0x2 : 0x0) |
                       (in_valid_ ? 0x4 : 0x0)));
    in_nib_ = 0;
    out_nib_ = 0;
    out_valid_ = false;
    last_work_ns_.store(work_ns, std::memory_order_relaxed);
    const uint32_t wk_clamped = (work_ns > 0xFFFFFFFFULL)
                                    ? 0xFFFFFFFFu
                                    : static_cast<uint32_t>(work_ns);
    RecordWorkNs(wk_clamped);
    // SCH-03: one exhaustion increment per capped input per cycle (the flag
    // is set by InputBudgetAvailable when a capped input is revisited).
    if (input_budget_ != 0) {
      for (uint32_t i = 0; i < budget_inputs_; ++i) {
        if (cycle_input_hit_[i] != 0) {
          input_budget_exhausted_[i].fetch_add(1, std::memory_order_relaxed);
          input_budget_exhausted_total_.fetch_add(
              1, std::memory_order_relaxed);
          cycle_input_hit_[i] = 0;
        }
      }
    }
    if (dispatch_deadline_ns_ > 0 && work_ns > dispatch_deadline_ns_) {
      health_->MarkDeadlineMiss();
      // SCH-08: attribute the miss to one primary reason (overrun.h).
      switch (ClassifyOverrun(work_ns, dispatch_latency_ns, AnyReady(),
                              dispatch_deadline_ns_)) {
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
      // SCH-04 action (independent of the reason counter above).
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
          break;
      }
    } else if (degraded_.load(std::memory_order_relaxed)) {
      if (degrade_remaining_ > 0) --degrade_remaining_;
      if (degrade_remaining_ == 0) {
        degraded_.store(false, std::memory_order_relaxed);
      }
    }
    health_->MarkAlive(t1);
    dispatch_count_.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace thyme
