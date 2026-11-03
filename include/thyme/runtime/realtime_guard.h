#pragma once
// realtime_guard.h — SCH-06: cheap "am I on a real-time worker?" context.
//
// CyclicThread::Loop() and EventThread::Dispatch() run on the node's RT
// thread. Slow-path code that can be reached from a callback (the logging
// sink is the one shared component that can) uses this flag to defer work
// that must not run there — currently rename()-based log rotation.
//
// Cost: one thread-local increment/decrement per callback batch, never per
// message. No allocation, no syscall, no lock. The flag is advisory: it does
// not make a blocking call non-blocking, it only keeps rotation/sweeps out of
// the dispatch context.

namespace thyme {
namespace rt {

namespace detail {
inline thread_local uint32_t g_depth = 0;
}  // namespace detail

inline bool InRealtimeSection() { return detail::g_depth != 0; }

class ScopedRealtimeSection {
 public:
  ScopedRealtimeSection() { ++detail::g_depth; }
  ~ScopedRealtimeSection() { --detail::g_depth; }
  ScopedRealtimeSection(const ScopedRealtimeSection&) = delete;
  ScopedRealtimeSection& operator=(const ScopedRealtimeSection&) = delete;
};

}  // namespace rt
}  // namespace thyme
