#include "thyme/app/replay_mode.h"

#include "thyme/runtime/event_thread.h"

namespace thyme {
namespace app {
namespace replay_mode {
namespace {

std::atomic<bool> g_active{false};
std::atomic<bool> g_full_inject{false};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_stop{false};

// Seqlock guarding g_inject_stage. SINGLE WRITER (the replay runner thread);
// readers copy under seq validation. seq==0 or an empty buffer means "no
// inject stage set".
std::atomic<uint32_t> g_stage_seq{0};
char g_inject_stage[64]{};

}  // namespace

void SetActive(bool on) { g_active.store(on, std::memory_order_release); }

bool Active() { return g_active.load(std::memory_order_acquire); }

void SetFullInject(bool on) {
  g_full_inject.store(on, std::memory_order_release);
}

bool FullInject() { return g_full_inject.load(std::memory_order_acquire); }

void SetInjectStage(const char* stage) {
  g_stage_seq.fetch_add(1, std::memory_order_relaxed);  // odd: write begins
  std::atomic_thread_fence(std::memory_order_release);
  if (stage == nullptr || stage[0] == '\0') {
    g_inject_stage[0] = '\0';
  } else {
    std::snprintf(g_inject_stage, sizeof(g_inject_stage), "%s", stage);
  }
  g_stage_seq.fetch_add(1, std::memory_order_release);  // even: write done
}

const char* InjectStage() {
  // Returning a pointer needs stable storage: copy into a per-reader
  // thread_local buffer under seqlock validation so a concurrent
  // SetInjectStage can never produce a torn stage name.
  thread_local char copy[64];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s0 = g_stage_seq.load(std::memory_order_acquire);
    if (s0 & 1u) continue;
    std::memcpy(copy, g_inject_stage, sizeof(copy));
    const uint32_t s1 = g_stage_seq.load(std::memory_order_acquire);
    if (s0 == s1) return copy[0] != '\0' ? copy : nullptr;
  }
  return nullptr;
}

void SetPaused(bool on) { g_paused.store(on, std::memory_order_release); }

bool Paused() { return g_paused.load(std::memory_order_acquire); }

void RequestStop() { g_stop.store(true, std::memory_order_release); }

void ClearStop() { g_stop.store(false, std::memory_order_release); }

bool StopRequested() { return g_stop.load(std::memory_order_acquire); }

bool PushWaitAborted(void* ctx) {
  (void)ctx;
  if (StopRequested()) return true;
  if (!Active()) return true;
  return false;
}

PushWaitGate MakePushWaitGate(uint64_t slice_ns) {
  PushWaitGate gate;
  gate.aborted = &PushWaitAborted;
  gate.ctx = nullptr;
  gate.slice_ns = slice_ns;
  return gate;
}

namespace {

bool ReplayConsumeGate() { return Active() && Paused(); }

// Register in replay_mode.o (always linked when replay APIs are used). A
// separate replay_event_gate.o in the static lib was linkable but droppable
// on some hosts, leaving EventThread pause tests without a gate (M6.2).
struct ReplayConsumeGateRegistrar {
  ReplayConsumeGateRegistrar() {
    thyme::SetReplayConsumeGate(&ReplayConsumeGate);
  }
};

const ReplayConsumeGateRegistrar kReplayConsumeGateRegistrar;

}  // namespace

}  // namespace replay_mode
}  // namespace app
}  // namespace thyme
