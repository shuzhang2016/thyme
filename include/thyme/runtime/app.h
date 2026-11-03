#pragma once

#include "thyme/contracts/abi.h"
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/event_thread.h"

#include <cstddef>

namespace thyme {
class App {
 public:
  static constexpr std::size_t kMaxThreads = kAbiMaxThreads;

  static bool Prepare();
  static bool stop_requested();
  static void ResetStop();

  // Combined n_cyclic + n_event must stay <= kMaxThreads (SharedHealthPage).
  // Start() launches event threads before cyclic threads so in-process
  // consumers are in WaitAny before their cyclic producers Loop.
  bool Add(CyclicThread* t);
  bool Add(EventThread* t);
  bool Start();
  void RequestStop();
  void Join();

  static void InstallStopSignals();

  // Prepare() outcomes; false means the RT hardening step did not take effect
  // (on Linux a stderr warning was emitted; non-Linux steps are no-ops).
  static bool memory_locked();
  static bool malloc_tuned();
  static uint32_t memory_flags();

 private:
  CyclicThread* threads_[kMaxThreads]{};
  std::size_t n_{0};
  EventThread* ev_threads_[kMaxThreads]{};
  std::size_t ev_n_{0};
  bool started_{false};
};

}  // namespace thyme
