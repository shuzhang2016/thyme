#pragma once
#include "thyme/contracts/abi.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/overrun.h"
#include "thyme/runtime/perf_log.h"
#include "thyme/runtime/sched_state.h"
#include "thyme/runtime/thread_placement.h"
#include "thyme/runtime/waitable.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <pthread.h>

namespace thyme {

/** \brief Fixed-rate worker thread: sleep until the next deadline, then call
 *  the subclass Loop().
 *  \note The thread object owns itself only; HealthSlot and Waitable inputs
 *        are borrowed and must outlive Start()..Join(). Loop() runs on the
 *        worker thread, never on the caller's. */
class CyclicThread {
 public:
  CyclicThread(const char* name, uint32_t period_hz, HealthSlot* health);
  virtual ~CyclicThread();
  CyclicThread(const CyclicThread&) = delete;
  CyclicThread& operator=(const CyclicThread&) = delete;
  bool Start();
  void SetPlacement(const ThreadPlacement& p);
  // Optional wait_inputs: when n>0, ThreadMain uses WaitAllUntil instead of
  // SleepUntil so the tick waits for AND-all readiness (period still bounds
  // the wait). Product graphs are often pure-timer (no wait_inputs).
  static constexpr std::size_t kMaxWaitInputs = kAbiMaxCyclicWaitInputs;
  void SetWaitInputs(Waitable* const* srcs, std::size_t n);
  void RequestStop();
  void Join();
  uint64_t loop_count() const { return loop_count_.load(std::memory_order_relaxed); }
  uint64_t last_work_ns() const { return last_work_ns_.load(std::memory_order_relaxed); }
  uint64_t last_wake_error_ns() const {
    return last_wake_error_ns_.load(std::memory_order_relaxed);
  }
  const char* name() const { return name_; }
  HealthSlot* health() const { return health_; }
  uint32_t period_hz() const { return period_hz_; }
  uint64_t period_ns() const { return period_ns_; }
  uint64_t deadline_ns() const { return period_ns_; }

  // Copy the last min(count,cap) samples (newest-first ordering is irrelevant
  // for percentile computation). Returns count copied. Call only after Join.
  std::size_t CopyWakeErrs(uint32_t* out, std::size_t cap) const {
    std::size_t n = (wake_err_cnt_ < cap) ? wake_err_cnt_ : cap;
    for (std::size_t i = 0; i < n; ++i) {
      out[i] = wake_err_hist_[(wake_err_idx_ - 1 - i) & kHistMask];
    }
    return n;
  }
  std::size_t CopyWorkNs(uint32_t* out, std::size_t cap) const {
    std::size_t n = (work_ns_cnt_ < cap) ? work_ns_cnt_ : cap;
    for (std::size_t i = 0; i < n; ++i) {
      out[i] = work_ns_hist_[(work_ns_idx_ - 1 - i) & kHistMask];
    }
    return n;
  }
  std::size_t wake_err_count() const { return wake_err_cnt_; }
  std::size_t work_ns_count() const { return work_ns_cnt_; }

  // Consumer-side latency samples (frame ts -> consume time), same ring
  // contract as CopyWorkNs; call only after Join.
  std::size_t CopyLatenciesNs(uint32_t* out, std::size_t cap) const {
    std::size_t n = (lat_cnt_ < cap) ? lat_cnt_ : cap;
    for (std::size_t i = 0; i < n; ++i) {
      out[i] = lat_hist_[(lat_idx_ - 1 - i) & kHistMask];
    }
    return n;
  }
  std::size_t latency_count() const { return lat_cnt_; }

  // Per-sample perf log (10B wire records): the same NoteLatency tap feeds a
  // quantized sampler with the consume timestamp; flush after Join.
  const PerfSampler& perf() const { return perf_; }
  const PerfWorkLog& work_log() const { return work_log_; }
  const PerfSampler& perf_edge() const { return perf_edge_; }

  // SCH-08: actual scheduling state captured once at Start (best-effort
  // placement can silently degrade, e.g. FIFO without CAP_SYS_NICE).
  // sched_mismatch() is the SchedMismatch bitmask against the requested
  // placement (0 = as requested, unreadable, or nothing explicit requested).
  const SchedState& actual_sched() const { return sched_actual_; }
  uint32_t sched_mismatch() const { return sched_mismatch_; }

  // SCH-08 overrun reason counters (ClassifyOverrun with latency = the
  // wake error; sum == HealthSlot deadline_miss_cnt).
  uint64_t overrun_long_callback_count() const {
    return overrun_long_callback_.load(std::memory_order_relaxed);
  }
  uint64_t overrun_queue_backlog_count() const {
    return overrun_queue_backlog_.load(std::memory_order_relaxed);
  }
  uint64_t overrun_os_preemption_count() const {
    return overrun_os_preemption_.load(std::memory_order_relaxed);
  }

  // SCH-04 overrun policy. Set before Start(); the default kSkip preserves
  // the pre-SCH-04 behaviour. Policy counters count ACTIONS (one per miss)
  // and are independent of the reason counters above, so a miss increments
  // both exactly once (no double counting of the health miss itself).
  void SetOverrunPolicy(OverrunPolicy p) { overrun_policy_ = p; }
  OverrunPolicy overrun_policy() const { return overrun_policy_; }
  uint64_t overrun_skip_actions() const {
    return overrun_skip_actions_.load(std::memory_order_relaxed);
  }
  uint64_t overrun_degrade_actions() const {
    return overrun_degrade_actions_.load(std::memory_order_relaxed);
  }
  uint64_t overrun_restart_actions() const {
    return overrun_restart_actions_.load(std::memory_order_relaxed);
  }
  // Degrade state: true while the effective period is doubled. Recovery is
  // automatic after kDegradeRecoverCycles consecutive on-time loops.
  bool degraded() const { return degraded_.load(std::memory_order_relaxed); }
  uint64_t effective_period_ns() const {
    const uint64_t base = period_ns_;
    return degraded() ? base * 2 : base;
  }
  static constexpr uint32_t kDegradeRecoverCycles = 8;

 protected:
  virtual void Loop() = 0;
  // SCH-04 restart hook (kRestart only): runs on the worker thread after a
  // miss, before the cadence is re-armed. Default is a no-op. Implementations
  // must be non-blocking and allocation-free and must not assume input rings
  // were cleared by the framework (they were not). See docs/reference/scheduling-model.md.
  virtual void OnOverrunRestart() {}
  // Records one consumer-side latency sample (clamped to u32): now - the
  // consumed frame's hdr.mono_ns. Hand nodes call this per consumed frame.
  // Frame attribution for the work log (last-wins): each NoteInFrame overwrites
  // the input nibble — one Loop() → at most one in-frame key. Matches cyclic
  // DrainKeepLast / rate+latest consume (openpilot-style). EventThread uses
  // the same API name with first-wins + span_multi — do not assume symmetry;
  // see ARCHITECTURE.md §4. Hand nodes pass the consumed frame's sequence low
  // nibble; generated RecordTap calls NoteOutFrame with the produced one.
  void NoteInFrame(uint8_t nib) {
    in_nib_ = nib & 0xF;
    in_valid_ = true;
  }
  // Edge-granular latency sample (perf edge file, RecordStructId-keyed).
  // Hand nodes call this per consumed channel: now - that frame's hdr ts.
  // io carries the consumed frame's sequence nibble (frame-join key).
  void NoteLatencyEdge(uint8_t edge_id, uint64_t ns, uint8_t io = 0);
  void NoteOutFrame(uint8_t nib) {
    out_nib_ = nib & 0xF;
    out_valid_ = true;
  }
  void NoteLatency(uint64_t ns) {
    const uint32_t v = ns > 0xFFFFFFFFULL ? 0xFFFFFFFFu
                                          : static_cast<uint32_t>(ns);
    lat_hist_[lat_idx_ & kHistMask] = v;
    ++lat_idx_;
    if (lat_cnt_ < kHistCap) ++lat_cnt_;
    perf_.Note(MonoNs(), ns);
  }

 private:
  static void* Trampoline(void* arg);
  void ThreadMain();
  static uint64_t MonoNs();
  void SleepUntil(uint64_t target_ns);
  // Period-bounded AND-all wait for SetWaitInputs. After a short spin, may
  // WaitUntil on the first empty Waitable only — AllReady() is re-checked on
  // wake/timeout; not a true multi-waiter barrier.
  void WaitAllUntil(uint64_t next);
  bool AllReady() const;
  bool FailCreatedThread(const char* what);

  // Wake-error / work-duration ring histograms (preallocated; no malloc after
  // Start). Written only from ThreadMain (single producer); read via Copy*
  // only after Join.
  void RecordWakeErr(uint32_t v) {
    wake_err_hist_[wake_err_idx_ & kHistMask] = v;
    ++wake_err_idx_;
    if (wake_err_cnt_ < kHistCap) ++wake_err_cnt_;
  }
  void RecordWorkNs(uint32_t v) {
    work_ns_hist_[work_ns_idx_ & kHistMask] = v;
    ++work_ns_idx_;
    if (work_ns_cnt_ < kHistCap) ++work_ns_cnt_;
  }

  static constexpr std::size_t kHistCap = 4096;
  static constexpr uint32_t kHistMask = static_cast<uint32_t>(kHistCap - 1);

  const char* name_;
  uint32_t period_hz_;
  uint64_t period_ns_;
  HealthSlot* health_;
  ThreadPlacement placement_{};
  Waitable* waits_[kMaxWaitInputs]{};
  std::size_t nwait_{0};
  pthread_t thread_{};
  std::atomic<bool> started_{false};
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> loop_count_{0};
  std::atomic<uint64_t> last_work_ns_{0};
  std::atomic<uint64_t> last_wake_error_ns_{0};

  uint32_t wake_err_hist_[kHistCap]{};
  uint32_t work_ns_hist_[kHistCap]{};
  uint32_t lat_hist_[kHistCap]{};
  uint64_t wake_err_idx_{0};
  uint64_t work_ns_idx_{0};
  uint64_t lat_idx_{0};
  std::size_t wake_err_cnt_{0};
  std::size_t work_ns_cnt_{0};
  std::size_t lat_cnt_{0};
  PerfSampler perf_;
  PerfSampler perf_edge_;
  PerfWorkLog work_log_;
  SchedState sched_actual_{};
  uint32_t sched_mismatch_ = 0;
  std::atomic<uint64_t> overrun_long_callback_{0};
  std::atomic<uint64_t> overrun_queue_backlog_{0};
  std::atomic<uint64_t> overrun_os_preemption_{0};
  OverrunPolicy overrun_policy_ = OverrunPolicy::kSkip;
  std::atomic<uint64_t> overrun_skip_actions_{0};
  std::atomic<uint64_t> overrun_degrade_actions_{0};
  std::atomic<uint64_t> overrun_restart_actions_{0};
  std::atomic<bool> degraded_{false};
  uint32_t degrade_remaining_ = 0;
  uint8_t in_nib_{0};
  uint8_t out_nib_{0};
  bool in_valid_{false};
  bool out_valid_{false};
};

}  // namespace thyme
