// test_quota_class_typo.cpp — T4: a source referencing a quota_class that is
// not listed in quotas must fail startup (a typo like "saftey" used to
// silently become "unlimited").
#include "thyme/check.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_config.h"

#include <cstdint>
#include <cstdio>
#include <unistd.h>

using namespace thyme;

namespace {

bool HookReady(void*) { return true; }
bool HookActive(void*) { return false; }
bool HookStart(void*, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string>&) {
  return true;
}
bool HookPreempt(void*) { return false; }

trigger_center::Hooks Hooks() {
  trigger_center::Hooks h;
  h.recorder_ready = HookReady;
  h.capture_active = HookActive;
  h.start_capture = HookStart;
  h.preempt_capture = HookPreempt;
  return h;
}

trigger_cfg::SourceRule Rule(const char* name, const char* cls) {
  trigger_cfg::SourceRule r;
  r.name = name;
  r.quota_class = cls;
  r.before_ns = r.after_ns = 1000000000ULL;
  return r;
}

}  // namespace

int main() {
  char sock[64];
  std::snprintf(sock, sizeof(sock), "/tmp/c9quota_%d.sock",
                static_cast<int>(getpid()));

  // Typo: "saftey" is not in quotas -> Init must refuse.
  {
    trigger_cfg::CenterConfig cfg;
    cfg.socket_path = sock;
    cfg.sources = {Rule("crash", "saftey")};
    cfg.quota_limits = {{"safety", 0}};
    trigger_center::Center c;
    CHECK(!c.Init(cfg, Hooks()));
  }

  // All classes listed -> Init succeeds.
  {
    trigger_cfg::CenterConfig cfg;
    cfg.socket_path = sock;
    cfg.sources = {Rule("crash", "safety"), Rule("manual", "manual")};
    cfg.quota_limits = {{"safety", 0}, {"manual", 30}};
    trigger_center::Center c;
    CHECK(c.Init(cfg, Hooks()));
    c.Shutdown();
  }
  return 0;
}
