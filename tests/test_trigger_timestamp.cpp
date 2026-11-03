// test_trigger_timestamp.cpp — T7: a caller-supplied timestamp_ns must lie in
// [now - before_ns, now + tolerance]; out-of-range triggers are rejected with
// kInvalidTimestamp instead of producing all-hole packages or pushing the
// post deadline far into the future.
#include "thyme/check.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_config.h"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <unistd.h>

using namespace thyme;

namespace {

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

bool HookReady(void*) { return true; }
bool HookActive(void*) { return false; }
bool HookStart(void*, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string>&) {
  return true;
}
bool HookPreempt(void*) { return false; }

}  // namespace

int main() {
  trigger_cfg::CenterConfig cfg;
  char sock[64];
  std::snprintf(sock, sizeof(sock), "/tmp/c9ts_%d.sock",
                static_cast<int>(getpid()));
  cfg.socket_path = sock;
  trigger_cfg::SourceRule manual;
  manual.name = "manual";
  manual.priority = 10;
  manual.before_ns = 1000000000ULL;  // 1 s
  manual.after_ns = 1000000000ULL;
  manual.quota_class = "manual";
  cfg.sources = {manual};
  cfg.quota_limits = {{"manual", 0}};
  cfg.timestamp_future_tolerance_ns = 5000000000ULL;  // 5 s

  trigger_center::Hooks hooks;
  hooks.recorder_ready = HookReady;
  hooks.capture_active = HookActive;
  hooks.start_capture = HookStart;
  hooks.preempt_capture = HookPreempt;
  trigger_center::Center c;
  CHECK(c.Init(cfg, hooks));
  using trigger_proto::TriggerCode;

  const uint64_t now = NowNs();
  // In-window: accepted.
  trigger_center::SubmitResult r =
      c.Submit("manual", -1, now - 500000000ULL, true, false);  // 500 ms ago
  CHECK(r.code == TriggerCode::kAccepted);

  // Too old (pre window would be all holes): 10 s ago with before = 1 s.
  r = c.Submit("manual", -1, now - 10000000000ULL, true, false);
  CHECK(r.code == TriggerCode::kInvalidTimestamp);
  CHECK(c.stats().rejected_bad_timestamp == 1);

  // Too far in the future: beyond the 5 s tolerance.
  r = c.Submit("manual", -1, now + 60000000000ULL, true, false);
  CHECK(r.code == TriggerCode::kInvalidTimestamp);
  CHECK(c.stats().rejected_bad_timestamp == 2);

  // Just inside tolerance: accepted.
  c.Tick();  // drain the first pending trigger
  r = c.Submit("manual", -1, now + 4000000000ULL, true, false);
  CHECK(r.code == TriggerCode::kAccepted);

  c.Shutdown();
  return 0;
}
