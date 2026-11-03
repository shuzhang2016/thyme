// test_trigger_fsm_events.cpp — structured FSM transition event log
// ({ts, trigger_id, from, to, reason}) in TriggerCenter.
//
// Covers: every transition kind recorded in order with correct fields
// (accept / start / preempt / rollback / complete / mute-expire / cancel);
// ring overflow dropping oldest + counting; the JSONL dump path (watermark,
// no duplicates, fields match the ring); crash safety — a SIGKILLed child
// loses nothing it dumped (O_APPEND + fflush per dump).
#include "thyme/check.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_config.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace thyme;
using trigger_center::Center;
using trigger_center::FsmEvent;
using trigger_center::FsmReason;
using trigger_center::State;

namespace {

struct FakeRec {
  bool ready = true;
  bool active = false;
  int start_calls = 0;
  int preempt_calls = 0;
};

FakeRec g;

bool HookReady(void*) { return g.ready; }
bool HookActive(void*) { return g.active; }
bool HookStart(void*, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string>&) {
  if (!g.ready) return false;
  g.active = true;
  g.start_calls += 1;
  return true;
}
bool HookPreempt(void*) {
  if (!g.active) return false;
  g.preempt_calls += 1;
  g.active = false;
  return true;
}

trigger_center::Hooks MakeHooks() {
  trigger_center::Hooks h;
  h.recorder_ready = HookReady;
  h.capture_active = HookActive;
  h.start_capture = HookStart;
  h.preempt_capture = HookPreempt;
  return h;
}

trigger_cfg::SourceRule Rule(const char* name, int32_t prio, double mute_s) {
  trigger_cfg::SourceRule r;
  r.name = name;
  r.before_ns = 1000000000ull;
  r.after_ns = 1000000000ull;
  r.priority = prio;
  r.mute_ns = static_cast<uint64_t>(mute_s * 1e9);
  r.quota_class = "q";
  return r;
}

trigger_cfg::CenterConfig MakeCfg(const char* tag) {
  trigger_cfg::CenterConfig c;
  static char sock[96];
  std::snprintf(sock, sizeof(sock), "/tmp/c9fsmev_%s_%d.sock", tag,
                static_cast<int>(getpid()));
  c.socket_path = sock;
  c.sources = {Rule("low", 10, 0), Rule("mid", 50, 0.05), Rule("high", 90, 0)};
  c.quota_limits = {{"q", 0}};  // 0 = unlimited
  return c;
}

void SleepMs(int ms) {
  struct timespec ts {};
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
  nanosleep(&ts, nullptr);
}

std::vector<FsmEvent> Events(Center& c) {
  std::vector<FsmEvent> out(Center::kFsmEventRingSize);
  out.resize(c.CopyFsmEvents(out.data(), Center::kFsmEventRingSize));
  return out;
}

void CheckEvent(const FsmEvent& e, State from, State to, FsmReason reason,
                uint64_t id) {
  if (e.from != static_cast<uint8_t>(from) || e.to != static_cast<uint8_t>(to) ||
      e.reason != static_cast<uint8_t>(reason) || e.trigger_id != id) {
    std::fprintf(stderr,
                 "event mismatch: got {id=%llu from=%u to=%u why=%u} want "
                 "{id=%llu from=%u to=%u why=%u}\n",
                 static_cast<unsigned long long>(e.trigger_id), e.from, e.to,
                 e.reason, static_cast<unsigned long long>(id),
                 static_cast<uint8_t>(from),
                 static_cast<uint8_t>(to), static_cast<uint8_t>(reason));
  }
  CHECK(e.from == static_cast<uint8_t>(from));
  CHECK(e.to == static_cast<uint8_t>(to));
  CHECK(e.reason == static_cast<uint8_t>(reason));
  CHECK(e.trigger_id == id);
  CHECK(e.mono_ns > 0);
}

// Reads a JSONL dump file; tolerates a truncated last line (crash tail).
std::vector<FsmEvent> ReadDump(const char* path) {
  FILE* f = std::fopen(path, "rb");
  CHECK(f != nullptr);
  std::vector<FsmEvent> out;
  char line[256];
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    const std::size_t len = std::strlen(line);
    if (len == 0 || line[len - 1] != '\n') break;  // truncated tail: stop
    FsmEvent e;
    unsigned from = 0, to = 0, why = 0;
    unsigned long long ts = 0, id = 0;
    if (std::sscanf(line, "{\"ts\":%llu,\"id\":%llu,\"from\":%u,\"to\":%u,\"why\":%u}",
                    &ts, &id, &from, &to, &why) != 5) {
      break;
    }
    e.mono_ns = ts;
    e.trigger_id = id;
    e.from = static_cast<uint8_t>(from);
    e.to = static_cast<uint8_t>(to);
    e.reason = static_cast<uint8_t>(why);
    out.push_back(e);
  }
  std::fclose(f);
  return out;
}

}  // namespace

int main() {
  // --- transition coverage + ordering + fields ------------------------------
  {
    g = FakeRec{};
    Center c;
    const char* tag = "main";
    CHECK(c.Init(MakeCfg(tag), MakeHooks()));

    // accept + start + complete (mute=0 -> IDLE).
    CHECK(c.Submit("low", -1, 0, false, false).code ==
          trigger_proto::TriggerCode::kAccepted);
    c.Tick();  // start
    CHECK(g.active);
    g.active = false;
    c.Tick();  // complete -> IDLE
    {
      std::vector<FsmEvent> ev = Events(c);
      CHECK(ev.size() == 3);
      CheckEvent(ev[0], State::kIdle, State::kCapturing,
                 FsmReason::kSubmitAccept, 1);
      CheckEvent(ev[1], State::kCapturing, State::kCapturing,
                 FsmReason::kStarted, 1);
      CheckEvent(ev[2], State::kCapturing, State::kIdle,
                 FsmReason::kCompleted, 1);
      for (std::size_t i = 1; i < ev.size(); ++i) {
        CHECK(ev[i].mono_ns >= ev[i - 1].mono_ns);  // in order
      }
    }

    // complete with mute -> MUTED, then mute expiry -> IDLE.
    CHECK(c.Submit("mid", -1, 0, false, false).code ==
          trigger_proto::TriggerCode::kAccepted);
    c.Tick();
    g.active = false;
    c.Tick();
    CHECK(c.state() == State::kMuted);
    SleepMs(70);
    c.Tick();
    CHECK(c.state() == State::kIdle);
    {
      std::vector<FsmEvent> ev = Events(c);
      CHECK(ev.size() == 3 + 4);
      CheckEvent(ev[3], State::kIdle, State::kCapturing,
                 FsmReason::kSubmitAccept, 2);
      CheckEvent(ev[4], State::kCapturing, State::kCapturing,
                 FsmReason::kStarted, 2);
      CheckEvent(ev[5], State::kCapturing, State::kMuted,
                 FsmReason::kCompleted, 2);
      CheckEvent(ev[6], State::kMuted, State::kIdle, FsmReason::kMuteExpired,
                 2);
    }

    // preempt: low running, high accepted, Tick executes the preempt.
    CHECK(c.Submit("low", -1, 0, false, false).code ==
          trigger_proto::TriggerCode::kAccepted);
    c.Tick();
    const uint64_t old_id = c.current_trigger_id();
    CHECK(c.Submit("high", -1, 0, false, false).code ==
          trigger_proto::TriggerCode::kAccepted);
    const uint64_t new_id = c.current_trigger_id();
    c.Tick();  // preempt + start
    CHECK(g.preempt_calls == 1);
    {
      std::vector<FsmEvent> ev = Events(c);
      const std::size_t n = ev.size();
      CheckEvent(ev[n - 5], State::kIdle, State::kCapturing,
                 FsmReason::kSubmitAccept, old_id);
      CheckEvent(ev[n - 4], State::kCapturing, State::kCapturing,
                 FsmReason::kStarted, old_id);
      CheckEvent(ev[n - 3], State::kCapturing, State::kCapturing,
                 FsmReason::kSubmitAccept, new_id);
      CheckEvent(ev[n - 2], State::kCapturing, State::kCapturing,
                 FsmReason::kPreempted, old_id);
      CheckEvent(ev[n - 1], State::kCapturing, State::kCapturing,
                 FsmReason::kStarted, new_id);
    }

    // cancel the running capture (high, mute=0 -> IDLE).
    CHECK(c.Cancel(new_id) == trigger_proto::TriggerCode::kCancelled);
    {
      std::vector<FsmEvent> ev = Events(c);
      CheckEvent(ev.back(), State::kCapturing, State::kIdle,
                 FsmReason::kCancelRunning, new_id);
    }

    // cancel a pending (accepted, not yet started) trigger: FSM rolls back.
    CHECK(c.Submit("low", -1, 0, false, false).code ==
          trigger_proto::TriggerCode::kAccepted);
    const uint64_t pending_id = c.current_trigger_id();
    CHECK(c.Cancel(pending_id) == trigger_proto::TriggerCode::kCancelled);
    {
      std::vector<FsmEvent> ev = Events(c);
      CheckEvent(ev.back(), State::kCapturing, State::kIdle,
                 FsmReason::kCancelPending, pending_id);
    }

    // rollback: accepted, but the recorder drops offline before Tick.
    CHECK(c.Submit("low", -1, 0, false, false).code ==
          trigger_proto::TriggerCode::kAccepted);
    const uint64_t fail_id = c.current_trigger_id();
    g.ready = false;
    c.Tick();
    g.ready = true;
    CHECK(c.state() == State::kIdle);
    {
      std::vector<FsmEvent> ev = Events(c);
      CheckEvent(ev.back(), State::kCapturing, State::kIdle,
                 FsmReason::kStartFailed, fail_id);
    }
    c.Shutdown();
  }

  // --- overflow: drops oldest + counts --------------------------------------
  {
    g = FakeRec{};
    Center c;
    const char* tag = "overflow";
    CHECK(c.Init(MakeCfg(tag), MakeHooks()));
    // One accept+start+complete cycle = 3 events; 150 cycles = 450 > 256.
    for (int i = 0; i < 150; ++i) {
      CHECK(c.Submit("low", -1, 0, false, false).code ==
            trigger_proto::TriggerCode::kAccepted);
      c.Tick();
      g.active = false;
      c.Tick();
      CHECK(c.state() == State::kIdle);
    }
    CHECK(c.stats().fsm_events == 450);
    CHECK(c.stats().fsm_event_drops == 450 - Center::kFsmEventRingSize);
    std::vector<FsmEvent> ev = Events(c);
    CHECK(ev.size() == Center::kFsmEventRingSize);
    // Retained events are the newest 256, still chronological.
    for (std::size_t i = 1; i < ev.size(); ++i) {
      CHECK(ev[i].mono_ns >= ev[i - 1].mono_ns);
    }
    // 450 - 256 = 194 dropped; first retained is event #194 = the
    // kCompleted of trigger 65 (3 events per trigger, 0-based cycle 64).
    CheckEvent(ev[0], State::kCapturing, State::kIdle,
               FsmReason::kCompleted, 65);
    CheckEvent(ev.back(), State::kCapturing, State::kIdle,
               FsmReason::kCompleted, 150);
    c.Shutdown();
  }

  // --- dump path: JSONL, watermark, fields match the ring -------------------
  {
    g = FakeRec{};
    char sock[64];
    std::snprintf(sock, sizeof(sock), "/tmp/c9fsmev_%d.sock",
                  static_cast<int>(getpid()));
    char fsm_path[80];
    std::snprintf(fsm_path, sizeof(fsm_path), "%s.fsm", sock);
    std::remove(fsm_path);
    trigger_cfg::CenterConfig cfg = MakeCfg("dump");
    cfg.socket_path = sock;
    Center c;
    CHECK(c.Init(cfg, MakeHooks()));

    c.Submit("low", -1, 0, false, false);
    c.Tick();
    g.active = false;
    c.Tick();
    CHECK(c.DumpNewFsmEvents());
    CHECK(!c.DumpNewFsmEvents());  // watermark: nothing new
    std::vector<FsmEvent> dumped = ReadDump(fsm_path);
    std::vector<FsmEvent> ring = Events(c);
    CHECK(dumped.size() == 3);
    CHECK(dumped.size() == ring.size());
    for (std::size_t i = 0; i < dumped.size(); ++i) {
      CHECK(dumped[i].mono_ns == ring[i].mono_ns);
      CHECK(dumped[i].trigger_id == ring[i].trigger_id);
      CHECK(dumped[i].from == ring[i].from);
      CHECK(dumped[i].to == ring[i].to);
      CHECK(dumped[i].reason == ring[i].reason);
    }

    // More transitions append exactly the new events, no duplicates.
    c.Submit("low", -1, 0, false, false);
    c.Tick();
    g.active = false;
    c.Tick();
    CHECK(c.DumpNewFsmEvents());
    dumped = ReadDump(fsm_path);
    CHECK(dumped.size() == 6);
    CheckEvent(dumped[3], State::kIdle, State::kCapturing,
               FsmReason::kSubmitAccept, 2);

    // Tick-driven periodic dump: with a tiny interval, Tick dumps on its own.
    c.set_fsm_dump_interval_ns(0);
    c.Submit("low", -1, 0, false, false);
    c.Tick();  // starts the capture AND dumps (interval 0)
    dumped = ReadDump(fsm_path);
    CHECK(dumped.size() == 8);  // + accept + started
    c.Shutdown();
    std::remove(fsm_path);
    std::remove((std::string(sock) + ".audit").c_str());
  }

  // --- crash safety: a SIGKILL loses nothing that was dumped ----------------
  {
    char sock[64];
    std::snprintf(sock, sizeof(sock), "/tmp/c9fsmev_crash_%d.sock",
                  static_cast<int>(getpid()));
    char fsm_path[80];
    std::snprintf(fsm_path, sizeof(fsm_path), "%s.fsm", sock);
    std::remove(fsm_path);

    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      g = FakeRec{};
      trigger_cfg::CenterConfig cfg = MakeCfg("crash");
      cfg.socket_path = sock;
      Center c;
      CHECK(c.Init(cfg, MakeHooks()));
      c.Submit("low", -1, 0, false, false);
      c.Tick();  // start
      g.active = false;
      c.Tick();  // complete
      CHECK(c.DumpNewFsmEvents());
      // One more transition AFTER the dump: it may be lost in the crash.
      c.Submit("low", -1, 0, false, false);
      raise(SIGKILL);
      _exit(1);
    }
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    std::vector<FsmEvent> dumped = ReadDump(fsm_path);
    CHECK(dumped.size() == 3);  // exactly the pre-crash dump survived
    CheckEvent(dumped[0], State::kIdle, State::kCapturing,
               FsmReason::kSubmitAccept, 1);
    CheckEvent(dumped[1], State::kCapturing, State::kCapturing,
               FsmReason::kStarted, 1);
    CheckEvent(dumped[2], State::kCapturing, State::kIdle,
               FsmReason::kCompleted, 1);
    std::remove(fsm_path);
    std::remove((std::string(sock) + ".audit").c_str());
  }

  return 0;
}
