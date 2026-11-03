// SCH-08: scheduling observability — actual policy/priority/affinity with
// requested-vs-actual mismatch detection, per-callback work attribution,
// queue-depth probes (perf type-5), and overrun reason counters.
#include "thyme/check.h"
#include "thyme/runtime/event_thread.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/overrun.h"
#include "thyme/runtime/proc_sampler.h"
#include "thyme/runtime/sched_state.h"
#include "thyme/runtime/shm_spsc_ring.h"
#include "thyme/runtime/spsc_ring.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

using namespace thyme;

namespace {

uint64_t Tns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

void SleepNs(uint64_t ns) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
  nanosleep(&ts, nullptr);
}

void WaitCount(const std::atomic<uint64_t>& cnt, uint64_t want) {
  const uint64_t deadline = Tns() + 3000000000ULL;
  while (cnt.load(std::memory_order_relaxed) < want && Tns() < deadline) {
    usleep(1000);
  }
}

// Drains all queued messages, timing each message's callback work under a
// fixed edge id (per-callback attribution).
class CbWorkThread final : public EventThread {
 public:
  NotifPipe<uint32_t, 16>* q = nullptr;
  std::atomic<uint64_t> msgs{0};
  static constexpr uint8_t kEdge = 7;
  using EventThread::EventThread;
 protected:
  void Dispatch() override {
    uint32_t v = 0;
    while (q->TryPop(v)) {
      const uint64_t t = NowNs();
      NoteHandled();
      NoteCallbackWork(kEdge, NowNs() - t);
      msgs.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

// Pops exactly ONE message per Dispatch batch and sleeps 5ms per message:
// with messages still queued afterwards, an over-budget batch is a
// queue-backlog overrun; the last batch (queue drained) is a long callback.
class OnePopSlowThread final : public EventThread {
 public:
  NotifPipe<uint32_t, 8>* q = nullptr;
  uint64_t sleep_ns = 0;
  std::atomic<uint64_t> msgs{0};
  using EventThread::EventThread;
 protected:
  void Dispatch() override {
    uint32_t v = 0;
    if (q->TryPop(v)) {
      if (sleep_ns > 0) SleepNs(sleep_ns);
      NoteHandled();
      msgs.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

SchedState Fabricate(int policy, int priority, uint64_t mask_lo) {
  SchedState s{};
  s.policy = policy;
  s.priority = priority;
  s.cpu_mask_lo = mask_lo;
  uint64_t m = mask_lo;
  while (m != 0) {
    s.ncpu += static_cast<uint32_t>(m & 1);
    m >>= 1;
  }
  s.valid = true;
  return s;
}

void TestMismatchMask() {
  // Policy::Unset is the documented best-effort path: OTHER is the expected
  // outcome without privileges, never a warning.
  ThreadPlacement p{};
  CHECK(SchedMismatchMask(p, Fabricate(SCHED_OTHER, 0, 0xF)) ==
        kSchedMismatchNone);

  // Asked FIFO, got OTHER (no CAP_SYS_NICE): policy mismatch. Priority is
  // only compared when the policy actually took effect.
  p.policy = ThreadPlacement::Policy::Fifo;
  p.priority = 10;
  CHECK(SchedMismatchMask(p, Fabricate(SCHED_OTHER, 0, 0xF)) ==
        kSchedMismatchPolicy);
  CHECK(SchedMismatchMask(p, Fabricate(SCHED_FIFO, 10, 0xF)) ==
        kSchedMismatchNone);
  CHECK(SchedMismatchMask(p, Fabricate(SCHED_FIFO, 5, 0xF)) ==
        kSchedMismatchPriority);
  CHECK(SchedMismatchMask(p, Fabricate(SCHED_RR, 10, 0xF)) ==
        kSchedMismatchPolicy);

  // SCHED_OTHER pins priority to 0 in the kernel: no priority comparison.
  p.policy = ThreadPlacement::Policy::Other;
  p.priority = 10;
  CHECK(SchedMismatchMask(p, Fabricate(SCHED_OTHER, 0, 0xF)) ==
        kSchedMismatchNone);

  // Affinity: a requested CPU missing from the actual mask is flagged.
  p = ThreadPlacement{};
  p.ncpus = 1;
  p.cpus[0] = 0;
  CHECK(SchedMismatchMask(p, Fabricate(SCHED_OTHER, 0, 0x1)) ==
        kSchedMismatchNone);
  CHECK(SchedMismatchMask(p, Fabricate(SCHED_OTHER, 0, 0x2)) ==
        kSchedMismatchAffinity);

  // Unreadable actual state: nothing to compare, never a false warning.
  SchedState invalid{};
  p.policy = ThreadPlacement::Policy::Fifo;
  CHECK(SchedMismatchMask(p, invalid) == kSchedMismatchNone);
}

void TestReadSchedState() {
  SchedState st{};
  CHECK(ReadSchedState(pthread_self(), &st));
  CHECK(st.valid);
  CHECK(st.policy == SCHED_OTHER || st.policy == SCHED_FIFO ||
        st.policy == SCHED_RR || st.policy == SCHED_BATCH ||
        st.policy == SCHED_IDLE);
  CHECK(st.ncpu >= 1);
  CHECK(st.CpuSet(0) || st.ncpu > 0);
  CHECK(!st.CpuSet(-1));
  CHECK(!st.CpuSet(128));
}

// Live mismatch detection: request an (in an unprivileged test process)
// unattainable FIFO priority. The assertions are environment-independent:
// the mismatch flag must equal the pure-function verdict on the captured
// actual state, and the health counter must follow it.
void TestLiveMismatch() {
  HealthSlot health{};
  NotifPipe<uint32_t, 16> q;
  CbWorkThread th("sch08_mm", &health);
  th.q = &q;
  Waitable* srcs[] = {&q};
  th.SetWaitInputs(srcs, 1);
  ThreadPlacement pl{};
  pl.policy = ThreadPlacement::Policy::Fifo;
  pl.priority = 99;  // requires CAP_SYS_NICE; best-effort -> may degrade
  pl.fail = ThreadPlacement::Fail::BestEffort;
  th.SetPlacement(pl);
  CHECK(th.Start());
  CHECK(q.Push(1));
  WaitCount(th.msgs, 1);
  th.RequestStop();
  th.Join();
  CHECK(th.actual_sched().valid);
  const uint32_t want = SchedMismatchMask(pl, th.actual_sched());
  CHECK(th.sched_mismatch() == want);
  CHECK(health.sched_mismatch_cnt.load() == (want != 0 ? 1u : 0u));
  // A placement that matches reality never raises the flag.
  if (th.actual_sched().policy == SCHED_OTHER) {
    CHECK((th.sched_mismatch() & kSchedMismatchPolicy) != 0);
  }
}

void TestQueueDepthProbes() {
  // Waitable::Depth implementations.
  SpscRing<8> ring;
  CHECK(ring.Depth() == 0);
  ring.push_latest(1, 0);
  ring.push_latest(2, 0);
  CHECK(ring.size() == 2);
  CHECK(ring.Depth() == 2);

  NotifPipe<uint32_t, 16> q;
  CHECK(q.Depth() == 0);
  CHECK(q.Push(11));
  CHECK(q.Push(12));
  CHECK(q.Push(13));
  CHECK(q.Depth() == 3);

  // SHM ring fill (small 4-slot ring; unlinked at the end).
  char name[64];
  std::snprintf(name, sizeof(name), "/tsch08_%d", static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  struct Payload {
    uint64_t a;
  };
  ShmSpscRing<Payload, 4> shm;
  CHECK(shm.Create(name));
  CHECK(shm.Depth() == 0);
  Payload pl{1};
  CHECK(shm.Push(pl));
  CHECK(shm.Push(pl));
  CHECK(shm.Depth() == 2);

  // RegisterObservability wires every depth-reporting input plus the three
  // overrun counters into the ProcSampler probe registry.
  HealthSlot health{};
  CbWorkThread th("sch08_qd", &health);
  th.q = &q;
  Waitable* srcs[] = {&q, &shm};
  th.SetWaitInputs(srcs, 2);
  ProcSampler sampler;
  CHECK(sampler.Start(ProcSampler::Options{}));
  CHECK(th.RegisterObservability(&sampler) == 5);  // 2 depths + 3 counters
  CHECK(sampler.probe_count() == 5);
  const uint32_t nrec = sampler.SampleNow(Tns());
  CHECK(nrec == 16 + 5);  // 16 proc series + 5 probes
  ProcSampler::Record recs[32];
  const uint32_t n = sampler.CopyRecords(recs, 32);
  CHECK(n == nrec);
  uint32_t depth0 = 0, depth1 = 0;
  bool saw_ctr = false;
  for (uint32_t i = 0; i < n; ++i) {
    if (recs[i].series == ProcSampler::kProbeBase + 0) depth0 = recs[i].value;
    if (recs[i].series == ProcSampler::kProbeBase + 1) depth1 = recs[i].value;
    if (recs[i].series >= ProcSampler::kProbeBase + 2) saw_ctr = true;
  }
  CHECK(depth0 == 3);  // NotifPipe backlog
  CHECK(depth1 == 2);  // SHM ring fill
  CHECK(saw_ctr);      // overrun reason counters (instant values, 0)

  // The probe series lands in a perf type-5 block on flush.
  PerfLogWriter w;
  sampler.FlushNew(w, 9);
  bool saw_type5 = false;
  for (std::size_t i = 4; i + 6 <= w.size();) {
    const uint8_t type = w.data()[i];
    const uint32_t cnt = ReadU32(w.data() + i + 2);
    if (type == 5) saw_type5 = true;
    i += 6 + static_cast<std::size_t>(cnt) * 10;
  }
  CHECK(saw_type5);

  shm.Close();
  ShmRegion::UnlinkName(name);
}

void TestCallbackWorkAttribution() {
  HealthSlot health{};
  NotifPipe<uint32_t, 16> q;
  CbWorkThread th("sch08_cb", &health);
  th.q = &q;
  Waitable* srcs[] = {&q};
  th.SetWaitInputs(srcs, 1);
  CHECK(th.Start());
  CHECK(q.Push(1));
  CHECK(q.Push(2));
  CHECK(q.Push(3));
  WaitCount(th.msgs, 3);
  th.RequestStop();
  th.Join();
  CHECK(th.cb_work().count() == 3);  // one sample per callback invocation

  // Flush: the edge-id sample block and its percentile block both appear,
  // and the OBS-09 aggregate histogram block is labelled as callback work.
  PerfLogWriter w;
  th.cb_work().Flush(w, 9);
  bool saw_samples = false, saw_pct = false, saw_hist = false;
  for (std::size_t i = 4; i + 6 <= w.size();) {
    const uint8_t type = w.data()[i];
    const uint8_t node = w.data()[i + 1];
    const uint32_t cnt = ReadU32(w.data() + i + 2);
    if (type == 1 && node == CbWorkThread::kEdge) {
      saw_samples = true;
      CHECK(cnt == 3);
    }
    if (type == 2 && node == CbWorkThread::kEdge) saw_pct = true;
    if (type == 6) {
      CHECK(w.data()[i + 6] == kPerfHistogramRecordTag);
      CHECK(w.data()[i + 7] == PerfSampler::kHistSeriesCallbackWork);
      CHECK(node == 9);  // the flush node, not the per-edge id
      saw_hist = true;
    }
    i += 6 + static_cast<std::size_t>(cnt) *
                 (type == 2 ? 20 : (type == 6 ? kPerfHistRecordBytes : 10));
  }
  CHECK(saw_samples);
  CHECK(saw_pct);
  CHECK(saw_hist);
  CHECK(th.cb_work().histogram().count() == 3);
}

void TestOverrunReasons() {
  // Pure classifier (deterministic): one primary bucket per miss.
  CHECK(ClassifyOverrun(5000000, 0, false, 2000000) ==
        OverrunReason::kLongCallback);
  CHECK(ClassifyOverrun(5000000, 0, true, 2000000) ==
        OverrunReason::kQueueBacklog);
  CHECK(ClassifyOverrun(5000000, 1500000, false, 2000000) ==
        OverrunReason::kOsPreemption);
  // Latency must exceed HALF the budget; exactly half is not preemption.
  CHECK(ClassifyOverrun(5000000, 1000000, false, 2000000) ==
        OverrunReason::kLongCallback);
  CHECK(ClassifyOverrun(5000000, 1000001, true, 2000000) ==
        OverrunReason::kOsPreemption);  // preemption outranks backlog

  // Live: 3 queued messages, one popped per 5ms batch, 2ms deadline.
  // Batches 1-2 finish over budget with backlog pending -> queue_backlog;
  // batch 3 drains the queue -> long_callback. Sum == deadline misses.
  HealthSlot health{};
  NotifPipe<uint32_t, 8> q;
  OnePopSlowThread th("sch08_ovr", &health, 10, 2000000ULL);
  th.q = &q;
  th.sleep_ns = 5000000ULL;
  Waitable* srcs[] = {&q};
  th.SetWaitInputs(srcs, 1);
  CHECK(th.Start());
  CHECK(q.Push(1));
  CHECK(q.Push(2));
  CHECK(q.Push(3));
  WaitCount(th.msgs, 3);
  th.RequestStop();
  th.Join();
  CHECK(th.msgs.load() == 3);
  CHECK(health.deadline_miss_cnt.load() == 3);
  CHECK(th.overrun_queue_backlog_count() == 2);
  CHECK(th.overrun_long_callback_count() == 1);
  CHECK(th.overrun_os_preemption_count() == 0);
  CHECK(th.overrun_queue_backlog_count() + th.overrun_long_callback_count() +
            th.overrun_os_preemption_count() ==
        health.deadline_miss_cnt.load());
}

void TestSchedStateOnPlainThread() {
  // No explicit placement: Policy::Unset is best-effort, so no mismatch is
  // ever flagged, and the actual state is still captured.
  HealthSlot health{};
  NotifPipe<uint32_t, 16> q;
  CbWorkThread th("sch08_plain", &health);
  th.q = &q;
  Waitable* srcs[] = {&q};
  th.SetWaitInputs(srcs, 1);
  CHECK(th.Start());
  CHECK(q.Push(1));
  WaitCount(th.msgs, 1);
  th.RequestStop();
  th.Join();
  CHECK(th.actual_sched().valid);
  CHECK(th.sched_mismatch() == kSchedMismatchNone);
  CHECK(health.sched_mismatch_cnt.load() == 0);
}

}  // namespace

int main() {
  TestMismatchMask();
  TestReadSchedState();
  TestLiveMismatch();
  TestQueueDepthProbes();
  TestCallbackWorkAttribution();
  TestOverrunReasons();
  TestSchedStateOnPlainThread();
  return 0;
}
