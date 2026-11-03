#pragma once
#include <atomic>
#include <cstdint>

namespace thyme {
class Waitable {
 public:
  virtual ~Waitable() = default;
  virtual bool empty() const = 0;
  virtual void WaitUntil(uint64_t deadline_ns) = 0;
  virtual void Notify() = 0;
  // Registers an extra Waitable that implementers Notify on every successful
  // produce (NotifPipe::Push does). EventThread attaches its shared wake hub
  // to every input through this hook so a push on ANY input wakes the worker.
  // The default is a no-op: implementers without a hook keep their own Notify
  // path only, and consumers must then tolerate their idle poll bound.
  virtual void SetNotify(Waitable* target) { (void)target; }
  // Futex word that Notify() bumps and wakes, or nullptr when the target
  // wakes by another mechanism (a pipe's condvar, a counting sink). A
  // producer that also holds a shared wake word compares this against its own
  // word so one publish issues at most one bump+wake per word (PERF-14).
  // Advisory only: it is not part of the wait contract.
  virtual std::atomic<uint32_t>* NotifyWakeWord() const { return nullptr; }
  // SCH-08 queue-depth observability: instantaneous number of pending
  // entries, or kDepthUnknown when the implementer cannot report one. Read
  // only by low-frequency samplers (ProcSampler probes), never required by
  // the wait contract; SPSC queues report an approximation that may race a
  // concurrent push/pop by one entry, which is fine for a depth series.
  static constexpr uint64_t kDepthUnknown = ~0ull;
  virtual uint64_t Depth() const { return kDepthUnknown; }
};

}  // namespace thyme
