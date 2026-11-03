// test_trigger_priority_clamp.cpp — T1: priority_hint can only LOWER a
// source's configured priority; a same-uid client must not self-promote into
// crash class (which would pierce mute windows, quotas, and preemption).
#include "thyme/check.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_config.h"

#include <cstdint>
#include <cstdio>
#include <unistd.h>

using namespace thyme;

namespace {

bool g_active = false;
bool HookReady(void*) { return true; }
bool HookActive(void*) { return g_active; }
bool HookStart(void*, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string>&) {
  g_active = true;
  return true;
}
bool HookPreempt(void*) {
  g_active = false;
  return true;
}

trigger_center::Hooks Hooks() {
  trigger_center::Hooks h;
  h.recorder_ready = HookReady;
  h.capture_active = HookActive;
  h.start_capture = HookStart;
  h.preempt_capture = HookPreempt;
  return h;
}

}  // namespace

int main() {
  trigger_cfg::CenterConfig cfg;
  char sock[64];
  std::snprintf(sock, sizeof(sock), "/tmp/c9clamp_%d.sock",
                static_cast<int>(getpid()));
  cfg.socket_path = sock;
  trigger_cfg::SourceRule manual;
  manual.name = "manual";
  manual.priority = 10;
  manual.mute_ns = 50000000ULL;
  manual.quota_class = "manual";
  manual.before_ns = manual.after_ns = 1000000000ULL;
  cfg.sources = {manual};
  cfg.crash_priority = 100;
  cfg.quota_limits = {{"manual", 0}};

  trigger_center::Center c;
  CHECK(c.Init(cfg, Hooks()));
  using trigger_proto::TriggerCode;

  // hint = 100 (crash) on a priority-10 source: clamped to 10.
  trigger_center::SubmitResult r = c.Submit("manual", 100, 0, false, false);
  CHECK(r.code == TriggerCode::kAccepted);
  CHECK(r.priority_used == 10);  // clamped
  c.Tick();  // T2: start the queued trigger

  // If the clamp had not applied, the capture would be crash-class and could
  // preempt; with the clamp a same-hint retry during capture is just muted
  // (same priority, no force).
  r = c.Submit("manual", 100, 0, false, false);
  CHECK(r.code == TriggerCode::kMuted);  // NOT a crash-class preemption
  CHECK(c.stats().preemptions == 0);

  // hint lower than the rule: applies.
  g_active = false;
  c.Tick();  // capture done -> MUTED (mute 50 ms)
  r = c.Submit("manual", 5, 0, false, false);
  CHECK(r.code == TriggerCode::kMuted);  // still muted, but...
  CHECK(r.priority_used == 5);           // ...the hint was honored
  c.Shutdown();
  return 0;
}
