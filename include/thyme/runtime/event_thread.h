#pragma once
#include "thyme/contracts/abi.h"
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/overrun.h"
#include "thyme/runtime/sched_state.h"
#include "thyme/runtime/waitable.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <pthread.h>

namespace thyme {

class ProcSampler;

// Optional hook for replay backpressure (M6.2): when true, EventThread
// skips Dispatch so input queues stay frozen. Registered by thyme_app.
using ReplayConsumeGateFn = bool (*)();
void SetReplayConsumeGate(ReplayConsumeGateFn fn);
ReplayConsumeGateFn GetReplayConsumeGate();

// Event-driven counterpart of CyclicThread: instead of a periodic deadline,
// the worker blocks until any registered Waitable becomes non-empty, then
// runs one Dispatch() batch. The subclass drains its inputs inside Dispatch
// and must finish the current batch before returning. Health is marked alive
// after every batch and on every idle-poll tick, so a message-starved but
// live thread is not flagged stalled; work duration is timed like
// CyclicThread's Loop. Optional dispatch_deadline_ns (M4.3) marks a
// deadline miss when Dispatch() work exceeds the budget.
/** \brief Event-driven worker thread: block until any registered Waitable is
 *  non-empty, then run one Dispatch() batch.
 *  \note Waitable inputs and the HealthSlot are borrowed, not owned; they
 *        must outlive Start()..Join(). Dispatch() must drain the current
 *        batch before returning and never block indefinitely. */
class EventThread {
 public:
  // expected_hz is the node's declared message rate: the stall watchdog
  // window is 3/hz, so the idle alive tick runs at half that interval
  // (capped by kIdlePollNs) instead of always 1s — a high-hz node starved
  // of input is not falsely flagged stalled.
  EventThread(const char* name, HealthSlot* health, uint32_t expected_hz = 1,
              uint64_t dispatch_deadline_ns = 0);
  virtual ~EventThread();
  EventThread(const EventThread&) = delete;
  EventThread& operator=(const EventThread&) = delete;
  bool Start();
  // Same ThreadPlacement contract as CyclicThread (affinity + sched policy,
  // best-effort unless Fail::Require); set before Start.
  void SetPlacement(const ThreadPlacement& p) { placement_ = p; }
  // Redirects the wake hub's futex word (Linux) to a shared-memory word so a
  // producer in another process can wake this thread (ShmSpscRing bumping the
  // same word on Push). In-process pipes notify the hub, which bumps the word
  // either way. nullptr restores the private in-process word.
  void SetWakeWord(std::atomic<uint32_t>* w) {
    wake_word_ = (w != nullptr) ? w : &wake_word_own_;
  }
  // Fixed-size input registry: no allocation after construction. Ceiling
  // covers fan-in well past App::kMaxThreads same-process producers times
  // channels; gen_graph rejects exec=event nodes with more in-edges at
  // validate time (see kAbiMaxEventWaitInputs).
  static constexpr std::size_t kMaxWaitInputs = kAbiMaxEventWaitInputs;
  void SetWaitInputs(Waitable* const* srcs, std::size_t n);
  void RequestStop();
  void Join();
  uint64_t dispatch_count() const {
    return dispatch_count_.load(std::memory_order_relaxed);
  }
  // Per-message loops accounting: generated Dispatch implementations call
  // NoteHandled() per consumed message, so the supervisor loops gate (hz =
  // expected message-rate lower bound) can read a message-granular count.
  uint64_t handled_count() const {
    return handled_count_.load(std::memory_order_relaxed);
  }
  uint64_t last_work_ns() const { return last_work_ns_.load(std::memory_order_relaxed); }
  const char* name() const { return name_; }
  HealthSlot* health() const { return health_; }
  uint64_t period_ns() const { return expected_period_ns_; }
  uint64_t deadline_ns() const { return dispatch_deadline_ns_; }
  // Wake-path observability (PERF-14): 0 means the hub has no blocked
  // sleeper and Notify() skips the futex syscall. Used by the wake-budget
  // test to time a push around a proven registration.
  uint32_t wake_waiter_count() const { return hub_.waiter_count(); }

  // Same contract as CyclicThread::CopyWorkNs; call only after Join.
  std::size_t CopyWorkNs(uint32_t* out, std::size_t cap) const {
    std::size_t n = (work_ns_cnt_ < cap) ? work_ns_cnt_ : cap;
    for (std::size_t i = 0; i < n; ++i) {
      out[i] = work_ns_hist_[(work_ns_idx_ - 1 - i) & kHistMask];
    }
    return n;
  }
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
  const PerfSampler& perf() const { return perf_; }
  // Edge-granular sampler: generated Dispatch taps NoteLatencyEdge with the
  // channel's RecordStructId, giving per-edge (not just per-node) latency.
  const PerfSampler& perf_edge() const { return perf_edge_; }
  // SCH-08 per-callback work sampler: NoteCallbackWork taps land here keyed
  // by edge id, so Flush yields per-callback sample + percentile blocks.
  const PerfSampler& cb_work() const { return cb_work_; }
  const PerfWorkLog& work_log() const { return work_log_; }

  // SCH-08: actual scheduling state captured once at Start (best-effort
  // placement can silently degrade, e.g. FIFO without CAP_SYS_NICE).
  // sched_mismatch() is the SchedMismatch bitmask against the requested
  // placement (0 = as requested, unreadable, or nothing explicit requested).
  const SchedState& actual_sched() const { return sched_actual_; }
  uint32_t sched_mismatch() const { return sched_mismatch_; }

  // SCH-08 overrun reason counters (ClassifyOverrun; sum == HealthSlot
  // deadline_miss_cnt when a deadline is configured).
  uint64_t overrun_long_callback_count() const {
    return overrun_long_callback_.load(std::memory_order_relaxed);
  }
  uint64_t overrun_queue_backlog_count() const {
    return overrun_queue_backlog_.load(std::memory_order_relaxed);
  }
  uint64_t overrun_os_preemption_count() const {
    return overrun_os_preemption_.load(std::memory_order_relaxed);
  }
  // Wake-to-Dispatch-start latency of the last batch (os-preemption proxy).
  uint64_t last_dispatch_latency_ns() const {
    return last_dispatch_latency_ns_.load(std::memory_order_relaxed);
  }

  // SCH-04 overrun policy. Set before Start(); kSkip (default) preserves the
  // pre-SCH-04 behaviour. Policy counters count ACTIONS (one per miss) and
  // are independent of the SCH-08 reason counters above.
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
  // kDegrade: true while a cooldown equal to dispatch_deadline_ns_ is applied
  // before each batch. Recovers after kDegradeRecoverCycles on-time batches.
  bool degraded() const { return degraded_.load(std::memory_order_relaxed); }
  static constexpr uint32_t kDegradeRecoverCycles = 8;

  // SCH-03 input scheduling: bound each in-edge to at most
  // `max_per_input_per_dispatch` messages per Dispatch cycle (0 = unlimited,
  // the default). Generated Dispatch calls InputBudgetAvailable()/Consume()
  // around TryPop; a hand-written Dispatch does the same. Call before Start,
  // ideally after SetWaitInputs. `n_inputs` is the number of in-edge slots the
  // Dispatch loop indexes (the generated graph passes the in-edge count).
  void SetInputBudget(uint32_t max_per_input_per_dispatch,
                      uint32_t n_inputs = 0);
  uint32_t input_budget() const { return input_budget_; }
  uint32_t input_budget_slots() const { return budget_inputs_; }
  // Per-input count of dispatch cycles that stopped at the cap (one increment
  // per cycle, not per skipped message). Sum over inputs is available via the
  // total below.
  uint64_t input_budget_exhausted_count(std::size_t input) const {
    if (input >= budget_inputs_) return 0;
    return input_budget_exhausted_[input].load(std::memory_order_relaxed);
  }
  uint64_t input_budget_exhausted_total() const {
    return input_budget_exhausted_total_.load(std::memory_order_relaxed);
  }

  // SCH-08: register queue-depth probes (one per wait input that reports a
  // Depth — NotifPipe backlog, SHM ring fill) and the three overrun reason
  // counters with a ProcSampler, so they land in perf type-5 blocks on the
  // next sample tick. Call after SetWaitInputs; returns the number of
  // probes/counters registered (0 when sampler is nullptr).
  uint32_t RegisterObservability(ProcSampler* sampler);

 protected:
  virtual void Dispatch() = 0;
  // SCH-04 restart hook (kRestart only), on the worker thread immediately
  // after an over-budget batch. Default no-op. Must be non-blocking and
  // allocation-free; input rings are NOT cleared by the framework.
  virtual void OnOverrunRestart() {}
  // SCH-03 per-input budget protocol: check before TryPop, consume only after
  // a successful pop. With no budget (the default) both are single
  // predictable branches, so an unconfigured node keeps the old drain loop.
  bool InputBudgetAvailable(std::size_t input) {
    if (input_budget_ == 0 || input >= budget_inputs_) return true;
    if (cycle_input_count_[input] < input_budget_) return true;
    cycle_input_hit_[input] = 1;
    return false;
  }
  void ConsumeInputBudget(std::size_t input) {
    if (input_budget_ != 0 && input < budget_inputs_) {
      ++cycle_input_count_[input];
    }
  }
  void NoteHandled(uint64_t n = 1) {
    handled_count_.fetch_add(n, std::memory_order_relaxed);
  }
  // Monotonic clock for callback-side timing taps (NoteCallbackWork).
  static uint64_t NowNs() { return MonoNs(); }
  // SCH-08 per-callback work attribution: record one callback's execution
  // duration under its edge id (RecordStructId). Generated/hand Dispatch
  // wraps each input callback: const uint64_t t = NowNs(); OnX(...);
  // NoteCallbackWork(edge, NowNs() - t); Samples feed the percentile
  // machinery via cb_work().Flush (block types 1/2, per edge id).
  void NoteCallbackWork(uint8_t edge_id, uint64_t work_ns) {
    cb_work_.Note(MonoNs(), work_ns, edge_id);
  }
  // Records one consumer-side latency sample (clamped to u32). Generated
  // Dispatch taps this per consumed message: now - frame hdr.mono_ns.
  void NoteLatency(uint64_t ns) {
    const uint32_t v = ns > 0xFFFFFFFFULL ? 0xFFFFFFFFu : static_cast<uint32_t>(ns);
    lat_hist_[lat_idx_ & kHistMask] = v;
    ++lat_idx_;
    if (lat_cnt_ < kHistCap) ++lat_cnt_;
    perf_.Note(MonoNs(), ns);
  }
  // Same sample additionally recorded under an edge id (perf edge file).
  // io carries the consumed frame's sequence nibble (frame-join key).
  void NoteLatencyEdge(uint8_t edge_id, uint64_t ns, uint8_t io = 0) {
    perf_edge_.Note(MonoNs(), ns, edge_id, io);
  }
  // Batch frame attribution: first-wins + span_multi (ROS2-style message
  // batch). Unlike CyclicThread's last-wins overwrite (one Loop → one
  // in-frame), the first NoteInFrame in a Dispatch batch fixes the input
  // nibble; a later call with a DIFFERENT nibble sets span_multi (work-log
  // flag bit0 — batch not splittable to one frame). Same API name as
  // CyclicThread, different exec model — see ARCHITECTURE.md §4. The nibble
  // (mod-16 sequence) is the frame key: side channels of one frame share it
  // (lockstep counters) even when their publish timestamps differ, while
  // consecutive frames always differ.
  void NoteInFrame(uint8_t nib) {
    nib &= 0xF;
    if (!in_valid_) {
      in_nib_ = nib;
      in_valid_ = true;
    } else if (nib != in_nib_) {
      span_multi_ = true;
    }
  }
  void NoteOutFrame(uint8_t nib) {
    out_nib_ = nib & 0xF;
    out_valid_ = true;
  }

 private:
  // Shared wakeup hub: SetWaitInputs attaches every input pipe to it through
  // Waitable::SetNotify, so a push on ANY input — not just the first one —
  // wakes WaitAny. empty() re-checks all inputs, keeping the
  // register-then-check protocol (mirrors SpscRing::WaitUntil/Notify)
  // race-free against a push landing between AnyReady and the wait.
  class WakeHub final : public Waitable {
   public:
    explicit WakeHub(const EventThread* owner);
    ~WakeHub() override;
    WakeHub(const WakeHub&) = delete;
    WakeHub& operator=(const WakeHub&) = delete;
    bool empty() const override;
    void WaitUntil(uint64_t deadline_ns) override;
    void Notify() override;
    // Lets a producer whose ring shares this hub's wake word skip its own
    // redundant bump/wake (ShmSpscRing::Push, PERF-14).
    std::atomic<uint32_t>* NotifyWakeWord() const override;
    // Number of sleepers registered with this hub (see EventThread).
    uint32_t waiter_count() const {
      return waiters_.load(std::memory_order_seq_cst);
    }

   private:
    const EventThread* owner_;
    std::atomic<uint32_t> waiters_{0};
    pthread_mutex_t mu_{};
    pthread_cond_t cv_{};
  };

  static void* Trampoline(void* arg);
  void ThreadMain();
  static uint64_t MonoNs();
  static uint64_t DepthProbe(void* ctx);
  bool AnyReady() const;
  void WaitAny();

  void RecordWorkNs(uint32_t v) {
    work_ns_hist_[work_ns_idx_ & kHistMask] = v;
    ++work_ns_idx_;
    if (work_ns_cnt_ < kHistCap) ++work_ns_cnt_;
  }

  static constexpr std::size_t kHistCap = 4096;
  static constexpr uint32_t kHistMask = static_cast<uint32_t>(kHistCap - 1);
  // Idle poll bound: guards against a missed wakeup and bounds stop latency
  // when no input ever fires. Notify wakes the wait long before this.
  static constexpr uint64_t kIdlePollNs = 1000000000ULL;
  // Idle alive tick: half the stall watchdog window (3/hz), capped at the
  // default idle poll. hz=1 -> 1s (the cap); hz=10 -> 150ms; hz=100 -> 15ms.
  static uint64_t IdlePollNs(uint32_t expected_hz) {
    if (expected_hz == 0) return kIdlePollNs;
    const uint64_t half_window = (3000000000ULL / expected_hz) / 2;
    return half_window < kIdlePollNs ? half_window : kIdlePollNs;
  }

  const char* name_;
  HealthSlot* health_;
  uint64_t expected_period_ns_;
  uint64_t dispatch_deadline_ns_;
  uint64_t idle_poll_ns_;
  ThreadPlacement placement_{};
  // Futex word the hub waits on (Linux); points at wake_word_own_ unless
  // SetWakeWord redirected it to shared memory for cross-process producers.
  std::atomic<uint32_t> wake_word_own_{0};
  std::atomic<uint32_t>* wake_word_{&wake_word_own_};
  WakeHub hub_;
  Waitable* waits_[kMaxWaitInputs]{};
  std::size_t nwait_{0};
  pthread_t thread_{};
  std::atomic<bool> started_{false};
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> dispatch_count_{0};
  std::atomic<uint64_t> handled_count_{0};
  std::atomic<uint64_t> last_work_ns_{0};

  uint32_t work_ns_hist_[kHistCap]{};
  uint64_t work_ns_idx_{0};
  std::size_t work_ns_cnt_{0};

  uint32_t lat_hist_[kHistCap]{};
  uint64_t lat_idx_{0};
  std::size_t lat_cnt_{0};
  PerfSampler perf_;
  PerfSampler perf_edge_;
  PerfSampler cb_work_;
  PerfWorkLog work_log_;
  SchedState sched_actual_{};
  uint32_t sched_mismatch_ = 0;
  std::atomic<uint64_t> overrun_long_callback_{0};
  std::atomic<uint64_t> overrun_queue_backlog_{0};
  std::atomic<uint64_t> overrun_os_preemption_{0};
  std::atomic<uint64_t> last_dispatch_latency_ns_{0};
  OverrunPolicy overrun_policy_ = OverrunPolicy::kSkip;
  std::atomic<uint64_t> overrun_skip_actions_{0};
  std::atomic<uint64_t> overrun_degrade_actions_{0};
  std::atomic<uint64_t> overrun_restart_actions_{0};
  std::atomic<bool> degraded_{false};
  uint32_t degrade_remaining_ = 0;
  // SCH-03 budget state. Written only by the worker thread (counts/hits);
  // the exhaustion counters are atomics for external readers.
  uint32_t input_budget_ = 0;
  uint32_t budget_inputs_ = 0;
  uint32_t cycle_input_count_[kMaxWaitInputs]{};
  uint8_t cycle_input_hit_[kMaxWaitInputs]{};
  std::atomic<uint64_t> input_budget_exhausted_[kMaxWaitInputs]{};
  std::atomic<uint64_t> input_budget_exhausted_total_{0};
  uint8_t in_nib_{0};
  uint8_t out_nib_{0};
  bool in_valid_{false};
  bool out_valid_{false};
  bool span_multi_{false};
};

}  // namespace thyme
