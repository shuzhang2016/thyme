// test_trigger_center_fsm.cpp — trigger-center state machine (design 5.3/5.4)
// driven through Center::Submit with a fake recorder behind Hooks: full
// transition coverage — accept / muted reject / preempt / force preempt /
// crash override / mute window / count quota / memory quota + eviction /
// unknown source / default fallback / recorder offline / SIGHUP-style reload.
#include "thyme/check.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_config.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

using namespace thyme;
using trigger_center::SubmitResult;

namespace {

struct FakeRec {
  bool ready = true;
  bool active = false;
  uint64_t pending = 0;
  int preempt_calls = 0;
  int start_calls = 0;
  uint64_t evict_calls = 0;
  uint64_t evicted_bytes = 0;
  uint64_t last_id = 0;
  uint64_t last_t0 = 0;
  uint64_t last_before = 0;
  uint64_t last_after = 0;
  bool last_force_rt = false;
  bool last_high_priority = false;
  std::vector<std::string> last_channels;
};

FakeRec g;  // test-local fake recorder instance (this TU is single-threaded)

bool HookReady(void*) { return g.ready; }
bool HookActive(void*) { return g.active; }
bool HookStart(void*, uint64_t id, uint64_t t0, uint64_t before,
               uint64_t after, bool force_rt, bool high_priority,
               const std::vector<std::string>& channels) {
  if (!g.ready) return false;
  g.active = true;
  g.start_calls += 1;
  g.last_id = id;
  g.last_t0 = t0;
  g.last_before = before;
  g.last_after = after;
  g.last_force_rt = force_rt;
  g.last_high_priority = high_priority;
  g.last_channels = channels;
  return true;
}
bool HookPreempt(void*) {
  if (!g.active) return false;
  g.preempt_calls += 1;
  g.active = false;  // truncated capture is sealed synchronously
  return true;
}
uint64_t HookPending(void*) { return g.pending; }
uint64_t HookEvict(void*, uint64_t bytes) {
  g.evict_calls += 1;
  const uint64_t freed = g.pending < bytes ? g.pending : bytes;
  g.evicted_bytes += freed;
  g.pending -= freed;
  return freed;
}

trigger_center::Hooks MakeHooks() {
  trigger_center::Hooks h;
  h.recorder_ready = HookReady;
  h.capture_active = HookActive;
  h.start_capture = HookStart;
  h.preempt_capture = HookPreempt;
  h.pending_bytes = HookPending;
  h.evict_oldest = HookEvict;
  return h;
}

trigger_cfg::SourceRule Rule(const char* name, int32_t prio, double before_s,
                             double after_s, double mute_s,
                             const char* quota_class, bool realtime = false) {
  trigger_cfg::SourceRule r;
  r.name = name;
  r.channels = {"/h264_*"};
  r.before_ns = static_cast<uint64_t>(before_s * 1e9);
  r.after_ns = static_cast<uint64_t>(after_s * 1e9);
  r.priority = prio;
  r.mute_ns = static_cast<uint64_t>(mute_s * 1e9);
  r.quota_class = quota_class;
  r.flush_realtime = realtime;
  return r;
}

// crash(100, safety) estop(80, safety) debug(40, debug) manual(10, manual)
trigger_cfg::CenterConfig MakeCfg() {
  trigger_cfg::CenterConfig c;
  c.sources = {Rule("crash", 100, 10, 5, 0, "safety"),
               Rule("estop", 80, 10, 5, 0.05, "safety"),
               Rule("debug", 40, 10, 5, 0.05, "debug"),
               Rule("manual", 10, 10, 5, 0.05, "manual")};
  c.quota_limits = {{"safety", 3}, {"debug", 2}, {"manual", 5}};
  return c;
}

void SleepMs(int ms) {
  struct timespec ts {};
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
  nanosleep(&ts, nullptr);
}

// Finish the current capture and run the FSM clock forward past any mute.
void CompleteCapture(trigger_center::Center& c, bool wait_out_mute) {
  c.Tick();            // T2: start the already-queued trigger first
  g.active = false;
  c.Tick();
  if (wait_out_mute) {
    SleepMs(70);  // test rules use mute_s = 0.05
    c.Tick();
  }
}

using trigger_proto::TriggerCode;

void CloseFdForPath(const char* path) {
  for (int fd = 3; fd < 256; ++fd) {
    char proc_path[64];
    std::snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
    char target[256];
    const ssize_t size = readlink(proc_path, target, sizeof(target) - 1);
    if (size < 0) continue;
    target[size] = '\0';
    if (std::strcmp(target, path) == 0) {
      CHECK(close(fd) == 0);
      return;
    }
  }
  CHECK(false && "quota state fd not found");
}

void TestPersistentQuota() {
  char sock[64];
  char other_sock[64];
  char state_path[64];
  std::snprintf(sock, sizeof(sock), "/tmp/c9quota_%d.sock",
                static_cast<int>(getpid()));
  std::snprintf(other_sock, sizeof(other_sock), "/tmp/c9quota_other_%d.sock",
                static_cast<int>(getpid()));
  std::snprintf(state_path, sizeof(state_path), "/tmp/c9quota_%d.bin",
                static_cast<int>(getpid()));
  std::remove(state_path);

  trigger_cfg::CenterConfig cfg = MakeCfg();
  cfg.socket_path = sock;
  cfg.quota_state_path = state_path;
  cfg.quota_limits = {{"safety", 0}, {"debug", 0}, {"manual", 1}};

  trigger_center::Center first;
  CHECK(first.Init(cfg, MakeHooks()));
  trigger_cfg::CenterConfig other_cfg = cfg;
  other_cfg.socket_path = other_sock;
  trigger_center::Center concurrent;
  CHECK(!concurrent.Init(other_cfg, MakeHooks()));
  CHECK(first.Submit("manual", -1, 0, false, false).code ==
        TriggerCode::kAccepted);
  first.Shutdown();

  trigger_center::Center restored;
  CHECK(restored.Init(cfg, MakeHooks()));
  CHECK(restored.QuotaUsedInWindow("manual") == 1);
  CHECK(restored.Submit("manual", -1, 0, false, false).code ==
        TriggerCode::kQuotaExceeded);

  trigger_cfg::CenterConfig relaxed = cfg;
  relaxed.quota_limits = {{"safety", 0}, {"debug", 0}, {"manual", 2}};
  restored.ReloadConfig(relaxed);
  CloseFdForPath(state_path);
  CHECK(restored.Submit("manual", -1, 0, false, false).code ==
        TriggerCode::kRecorderOffline);
  CHECK(restored.stats().quota_state_errors == 1);
  CHECK(restored.QuotaUsedInWindow("manual") == 1);
  CHECK(restored.state() == trigger_center::State::kIdle);
  CHECK(restored.current_trigger_id() == 0);
  restored.Shutdown();

  std::FILE* file = std::fopen(state_path, "r+b");
  CHECK(file != nullptr);
  uint8_t bad = 0;
  CHECK(std::fseek(file, 8, SEEK_SET) == 0);
  CHECK(std::fread(&bad, 1, 1, file) == 1);
  bad ^= 0x5a;
  CHECK(std::fseek(file, 8, SEEK_SET) == 0);
  CHECK(std::fwrite(&bad, 1, 1, file) == 1);
  CHECK(std::fclose(file) == 0);
  trigger_center::Center corrupt;
  CHECK(!corrupt.Init(cfg, MakeHooks()));

  std::remove(state_path);
  const std::string lock_path = std::string(state_path) + ".lock";
  std::remove(lock_path.c_str());
}

void TestQuotaStateClocks() {
  char path[64];
  std::snprintf(path, sizeof(path), "/tmp/c9quota_clock_%d.bin",
                static_cast<int>(getpid()));
  std::remove(path);
  constexpr uint64_t kHour = 3600ull * 1000000000ull;
  constexpr uint64_t kNowRealtime = 5ull * kHour;
  constexpr uint64_t kNowMono = 2ull * kHour;

  std::map<std::string, std::deque<uint64_t>> windows;
  trigger_center::QuotaStateStore store;
  CHECK(store.Open(path, kNowRealtime, kNowMono, &windows));
  CHECK(store.Append("manual", kNowRealtime - 10000000000ull));
  CHECK(store.Append("manual", kNowRealtime + 1000000000ull));
  CHECK(store.Append("manual", kNowRealtime - kHour - 1));
  CHECK(!store.Append(std::string(64, 'x'), kNowRealtime));
  store.Close();

  std::map<std::string, std::deque<uint64_t>> restored;
  CHECK(store.Open(path, kNowRealtime, kNowMono, &restored));
  CHECK(restored["manual"].size() == 2);
  CHECK(restored["manual"][0] == kNowMono - 10000000000ull);
  CHECK(restored["manual"][1] == kNowMono);
  store.Close();
  std::remove(path);
  const std::string lock_path = std::string(path) + ".lock";
  std::remove(lock_path.c_str());
}

}  // namespace

int main() {
  trigger_cfg::CenterConfig cfg = MakeCfg();
  char sock[64];
  std::snprintf(sock, sizeof(sock), "/tmp/c9fsm_%d.sock",
                static_cast<int>(getpid()));
  cfg.socket_path = sock;
  cfg.recorder_pending_quota = 1000;

  trigger_center::Center c;
  CHECK(c.Init(cfg, MakeHooks()));
  CHECK(c.state() == trigger_center::State::kIdle);

  // 1. IDLE accept.
  SubmitResult r = c.Submit("manual", -1, 0, false, false);
  CHECK(r.code == TriggerCode::kAccepted);
  CHECK(r.trigger_id == 1);
  c.Tick();  // T2: start the queued trigger
  CHECK(r.source_used == "manual");
  CHECK(r.priority_used == 10);
  CHECK(c.state() == trigger_center::State::kCapturing);
  CHECK(g.last_before == 10ull * 1000000000ull);
  CHECK(g.last_after == 5ull * 1000000000ull);
  CHECK(!g.last_force_rt);  // manual: idle flush policy
  CHECK(!g.last_high_priority);  // low priority: normal zone
  // The rule's channel filter reaches start_capture.
  CHECK(g.last_channels.size() == 1);
  CHECK(g.last_channels[0] == "/h264_*");

  // 2. CAPTURING, same priority, no force -> muted.
  r = c.Submit("manual", -1, 0, false, false);
  CHECK(r.code == TriggerCode::kMuted);
  CHECK(c.stats().rejected_muted == 1);

  // 3. CAPTURING, equal priority + force -> preempt (manually confirmed preemption).
  r = c.Submit("manual", -1, 0, false, true);
  CHECK(r.code == TriggerCode::kAccepted);
  c.Tick();  // preemption runs in Tick
  CHECK(g.preempt_calls == 1);
  CHECK(g.last_force_rt);  // preemption forces realtime
  CHECK(c.stats().preemptions == 1);

  // 4. CAPTURING, higher priority -> preempt.
  r = c.Submit("estop", -1, 0, false, false);
  CHECK(r.code == TriggerCode::kAccepted);
  c.Tick();
  CHECK(g.preempt_calls == 2);
  CHECK(c.current_trigger_id() == 3);

  // 5. Lower priority during the estop capture -> muted.
  r = c.Submit("debug", -1, 0, false, false);
  CHECK(r.code == TriggerCode::kMuted);

  // 6. priority_hint can only lower priority (T1): manual(10) self-hinting 90 is clamped back to 10,
  // so it does not preempt the estop(80) capture.
  r = c.Submit("manual", 90, 0, false, false);
  CHECK(r.code == TriggerCode::kMuted);  // not a successful preemption
  CHECK(r.priority_used == 10);

  // 7. Capture done, mute_s = 0.05 -> MUTED, non-crash rejected, crash ok.
  //    (the capture in flight is step 6's, whose rule is manual@mute 0.05)
  g.active = false;
  c.Tick();
  CHECK(c.state() == trigger_center::State::kMuted);
  r = c.Submit("debug", -1, 0, false, false);
  CHECK(r.code == TriggerCode::kMuted);
  r = c.Submit("crash", -1, 0, false, false);
  CHECK(r.code == TriggerCode::kAccepted);  // crash level bypasses mute
  c.Tick();
  CHECK(g.last_force_rt);                   // crash forces realtime
  CHECK(g.last_high_priority);              // crash-level package goes to the high/ reserved zone

  // 8. crash capture done (mute_s=0) -> straight back to IDLE.
  CompleteCapture(c, false);
  CHECK(c.state() == trigger_center::State::kIdle);

  // 9. Mute expiry: manual capture -> done -> MUTED; after 70 ms -> IDLE.
  r = c.Submit("manual", -1, 0, false, false);
  CHECK(r.code == TriggerCode::kAccepted);
  CompleteCapture(c, false);
  CHECK(c.state() == trigger_center::State::kMuted);
  SleepMs(70);
  c.Tick();
  CHECK(c.state() == trigger_center::State::kIdle);

  // 10. Count quota: safety limit is 3/h; estop (step 4) and crash (step 7)
  // already used 2, so this crash takes the last slot WITHOUT override.
  r = c.Submit("crash", -1, 0, false, false);
  CHECK(r.code == TriggerCode::kAccepted);
  c.Tick();
  CHECK(c.stats().quota_overridden == 0);
  CompleteCapture(c, false);
  r = c.Submit("estop", -1, 0, false, false);  // safety class now at 3/3
  CHECK(r.code == TriggerCode::kQuotaExceeded);  // non-crash rejected
  CHECK(c.stats().rejected_quota == 1);

  // 11. Memory quota: over-limit pending rejects low priority; crash evicts.
  //     (manual count so far: steps 1,3,9 = 3 of 5 — passes count quota)
  trigger_cfg::CenterConfig cfg2 = cfg;
  g.pending = 2000;
  r = c.Submit("manual", -1, 0, false, false);
  CHECK(r.code == TriggerCode::kQuotaExceeded);
  r = c.Submit("crash", -1, 0, false, false);  // safety 3/3 -> crash overrides
  CHECK(r.code == TriggerCode::kAccepted);
  c.Tick();
  CHECK(c.stats().quota_overridden == 1);  // count quota overridden, audited with a warning
  CHECK(g.evict_calls == 1);               // crash level evicts the oldest pending data
  CHECK(g.evicted_bytes == 2000);
  CompleteCapture(c, false);
  g.pending = 0;

  // 12. Unknown source: rejected without default, falls back with default.
  r = c.Submit("nosuch", -1, 0, false, false);
  CHECK(r.code == TriggerCode::kUnknownSource);
  trigger_cfg::CenterConfig cfg3 = cfg2;
  cfg3.default_source = "manual";
  c.ReloadConfig(cfg3);
  r = c.Submit("nosuch", -1, 0, false, false);
  CHECK(r.code == TriggerCode::kAccepted);  // falls back to manual's parameters
  CHECK(r.source_used == "manual");
  c.Tick();
  CHECK(g.last_before == 10ull * 1000000000ull);
  CompleteCapture(c, true);

  // 13. Manual count quota (5/h): used by steps 1,3,9,12 = 4; one more ok,
  // then the 6th is rejected.
  r = c.Submit("manual", -1, 0, false, false);
  CHECK(r.code == TriggerCode::kAccepted);
  CompleteCapture(c, true);
  r = c.Submit("manual", -1, 0, false, false);
  CHECK(r.code == TriggerCode::kQuotaExceeded);

  // 14. Recorder offline.
  g.ready = false;
  r = c.Submit("crash", -1, 0, false, false);
  CHECK(r.code == TriggerCode::kRecorderOffline);
  CHECK(c.stats().rejected_offline == 1);
  g.ready = true;

  // 15. Reload keeps FSM state and trigger-id sequence, swaps rules/quotas.
  const uint64_t id_before = c.current_trigger_id();
  trigger_cfg::CenterConfig cfg4 = cfg3;
  cfg4.sources[3].priority = 95;  // manual 95: now above estop
  cfg4.quota_limits = {{"safety", 0}, {"debug", 0}, {"manual", 0}};
  cfg4.socket_path = "/tmp/should_not_apply";  // not hot-reloadable
  c.ReloadConfig(cfg4);
  CHECK(c.current_trigger_id() == id_before);
  CHECK(c.socket_path() == sock);
  CHECK(c.QuotaUsedInWindow("manual") == 5);  // window survives reload
  r = c.Submit("manual", -1, 0, false, false);  // quota now unlimited
  CHECK(r.code == TriggerCode::kAccepted);
  CHECK(r.trigger_id == id_before + 1);  // ids keep increasing
  CompleteCapture(c, true);

  c.Shutdown();
  TestPersistentQuota();
  TestQuotaStateClocks();
  return 0;
}
