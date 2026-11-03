// test_trigger_preempt_fail.cpp — T6: a failed deferred preempt or start
// must roll the FSM back (async accept means the client already holds an
// Accepted id; the center counts the failure and keeps FSM consistent).
#include "thyme/check.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_config.h"

#include <cstdint>
#include <cstdio>
#include <unistd.h>

using namespace thyme;

namespace {

struct Fake {
  bool ready = true;
  bool active = false;
  bool preempt_ok = true;
  bool start_ok = true;
  int preempt_calls = 0;
  int start_calls = 0;
};

Fake g;

bool HookReady(void*) { return g.ready; }
bool HookActive(void*) { return g.active; }
bool HookStart(void*, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string>&) {
  ++g.start_calls;
  if (!g.start_ok) return false;
  g.active = true;
  return true;
}
bool HookPreempt(void*) {
  ++g.preempt_calls;
  if (!g.preempt_ok) return false;
  g.active = false;  // old capture truncated+sealed
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

trigger_cfg::SourceRule Rule(const char* name, int32_t prio, uint64_t mute_ns,
                             const char* cls) {
  trigger_cfg::SourceRule r;
  r.name = name;
  r.priority = prio;
  r.mute_ns = mute_ns;
  r.quota_class = cls;
  r.before_ns = r.after_ns = 1000000000ULL;
  return r;
}

}  // namespace

int main() {
  trigger_cfg::CenterConfig cfg;
  char sock[64];
  std::snprintf(sock, sizeof(sock), "/tmp/c9pfail_%d.sock",
                static_cast<int>(getpid()));
  cfg.socket_path = sock;
  cfg.sources = {Rule("crash", 100, 0, "safety"), Rule("manual", 10, 0, "manual")};
  cfg.quota_limits = {{"safety", 0}, {"manual", 0}};

  using trigger_proto::TriggerCode;
  using trigger_center::State;

  // A. preempt_capture fails at Tick: FSM rolls back to the old capture.
  {
    trigger_center::Center c;
    CHECK(c.Init(cfg, Hooks()));
    g = Fake{};
    CHECK(c.Submit("manual", -1, 0, false, false).code ==
          TriggerCode::kAccepted);  // id 1
    c.Tick();
    CHECK(g.active);
    CHECK(c.current_trigger_id() == 1);
    g.preempt_ok = false;
    CHECK(c.Submit("crash", -1, 0, false, false).code ==
          TriggerCode::kAccepted);  // id 2 accepted (queued for preempt)
    c.Tick();  // preemption fails to execute
    CHECK(c.stats().preempt_failures == 1);
    CHECK(c.stats().preemptions == 0);
    CHECK(g.start_calls == 1);  // B never started
    CHECK(c.state() == State::kCapturing);
    CHECK(c.current_trigger_id() == 1);  // rolled back to the old capture
    CHECK(g.active);                     // old capture continues
    c.Shutdown();
  }

  // B. recorder goes offline before the deferred start: rollback to Idle.
  {
    trigger_center::Center c;
    CHECK(c.Init(cfg, Hooks()));
    g = Fake{};
    CHECK(c.Submit("manual", -1, 0, false, false).code ==
          TriggerCode::kAccepted);
    g.ready = false;
    c.Tick();
    CHECK(c.stats().start_failures == 1);
    CHECK(c.state() == State::kIdle);  // nothing was started
    CHECK(!g.active);
    c.Shutdown();
  }

  // C. preempt succeeds but the new start fails: old capture was sealed ->
  // MUTED with the old rule's mute, then expires to Idle.
  {
    trigger_cfg::CenterConfig cfg2 = cfg;
    cfg2.sources[1].mute_ns = 30000000ULL;  // manual mute 30 ms
    trigger_center::Center c;
    CHECK(c.Init(cfg2, Hooks()));
    g = Fake{};
    CHECK(c.Submit("manual", -1, 0, false, false).code ==
          TriggerCode::kAccepted);
    c.Tick();
    CHECK(g.active);
    g.start_ok = false;
    CHECK(c.Submit("crash", -1, 0, false, false).code ==
          TriggerCode::kAccepted);
    c.Tick();  // preempt ok (old capture sealed), start fails
    CHECK(c.stats().preemptions == 1);
    CHECK(c.stats().start_failures == 1);
    CHECK(c.state() == State::kMuted);  // the old capture's mute is still in effect
    struct timespec ts {0, 40000000};
    nanosleep(&ts, nullptr);
    c.Tick();
    CHECK(c.state() == State::kIdle);
    c.Shutdown();
  }

  // D. Rollback while muted restores the original deadline, not mute_until=0.
  for (bool cancel_pending : {false, true}) {
    trigger_cfg::CenterConfig cfg2 = cfg;
    cfg2.sources[1].mute_ns = 120000000ULL;
    trigger_center::Center c;
    CHECK(c.Init(cfg2, Hooks()));
    g = Fake{};
    CHECK(c.Submit("manual", -1, 0, false, false).code ==
          TriggerCode::kAccepted);
    c.Tick();
    g.active = false;
    c.Tick();
    CHECK(c.state() == State::kMuted);

    const auto accepted = c.Submit("crash", -1, 0, false, false);
    CHECK(accepted.code == TriggerCode::kAccepted);
    if (cancel_pending) {
      CHECK(c.Cancel(accepted.trigger_id) == TriggerCode::kCancelled);
    } else {
      g.ready = false;
      c.Tick();
      CHECK(c.stats().start_failures == 1);
    }
    CHECK(c.state() == State::kMuted);
    c.Tick();
    CHECK(c.state() == State::kMuted);

    struct timespec ts {0, 140000000};
    nanosleep(&ts, nullptr);
    c.Tick();
    CHECK(c.state() == State::kIdle);
    c.Shutdown();
  }
  return 0;
}
