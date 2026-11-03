// test_trigger_storm.cpp — TRG-09 trigger storm.
//
// Default mode (mr_gate, deterministic): scripted bursts over the real
// trigger socket — accept, muted probes, busy (queue-depth-1), crash
// preemption, mute window + expiry, pending/running cancel — with exact
// accounting: every socket datagram is answered, accepted == start-hook
// calls == terminal outcomes, preemptions == preempt-hook calls, and the FSM
// event ring drop counter balances (drops == events - retained).
//
// "soak" mode (nightly, label "storm", not gated): paces the socket at
// ~10k requests/s for ~2 s (20k requests, bursts of 8 per 800 us) with
// time-based auto-completing captures and a crash-priority request every
// 64th, then asserts the same balance invariants and that the FSM drains
// back to IDLE (no stuck state).
#include "thyme/check.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_client.h"
#include "thyme/record/trigger_config.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

struct Fake {
  bool active = false;        // gated mode: manual completion control
  bool auto_complete = false; // soak mode: time-based completion
  uint64_t active_ns = 1000000;  // auto-complete after 1 ms
  uint64_t start_ns = 0;
  uint64_t starts = 0;
  uint64_t preempts = 0;
};

Fake g;

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

void SleepNs(uint64_t ns) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
  nanosleep(&ts, nullptr);
}

bool HookReady(void*) { return true; }
bool HookActive(void*) {
  if (g.auto_complete) return NowNs() < g.start_ns + g.active_ns;
  return g.active;
}
bool HookStart(void*, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string>&) {
  g.starts += 1;
  g.start_ns = NowNs();
  g.active = true;
  return true;
}
bool HookPreempt(void*) {
  g.preempts += 1;
  g.active = false;
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

trigger_proto::TriggerRequest ValidReq(const char* source) {
  trigger_proto::TriggerRequest req;
  req.magic = trigger_proto::kTriggerMagic;
  req.version = trigger_proto::kTriggerVersionV3;
  req.priority_hint = -1;
  std::snprintf(req.source, sizeof(req.source), "%s", source);
  req.sequence = trigger_client::NextSequence();
  req.response_timeout_ms = trigger_proto::kDefaultResponseTimeoutMs;
  trigger_proto::StampRequestCrc(&req);
  return req;
}

struct RawClient {
  int fd = -1;
  sockaddr_un self{};
  sockaddr_un server{};

  RawClient(const char* sock, const char* self_path) {
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    unlink(self_path);
    self.sun_family = AF_UNIX;
    std::snprintf(self.sun_path, sizeof(self.sun_path), "%s", self_path);
    CHECK(bind(fd, reinterpret_cast<sockaddr*>(&self), sizeof(self)) == 0);
    server.sun_family = AF_UNIX;
    std::snprintf(server.sun_path, sizeof(server.sun_path), "%s", sock);
  }
  ~RawClient() {
    if (fd >= 0) close(fd);
    unlink(self.sun_path);
  }
  bool SendNb(const void* buf, std::size_t n) {
    return sendto(fd, buf, n, 0, reinterpret_cast<sockaddr*>(&server),
                  sizeof(server)) == static_cast<ssize_t>(n);
  }
  ssize_t Recv(void* buf, std::size_t cap) {
    return recv(fd, buf, cap, MSG_DONTWAIT);
  }
};

trigger_cfg::CenterConfig MakeCfg(const std::string& sock,
                                  const std::string& audit,
                                  uint64_t manual_mute_ns,
                                  uint64_t poll_budget_ns = 200000ull) {
  trigger_cfg::CenterConfig cfg;
  cfg.socket_path = sock;
  cfg.socket_poll_budget_ns = poll_budget_ns;
  cfg.audit_log_path = audit;
  cfg.audit_max_bytes = 4ull * 1024 * 1024;
  trigger_cfg::SourceRule manual;
  manual.name = "manual";
  manual.priority = 10;
  manual.mute_ns = manual_mute_ns;
  manual.quota_class = "manual";
  manual.before_ns = manual.after_ns = 1000000000ULL;
  trigger_cfg::SourceRule crash;
  crash.name = "crash";
  crash.priority = 100;  // crash class (>= default crash_priority)
  crash.quota_class = "safety";
  crash.before_ns = crash.after_ns = 1000000000ULL;
  cfg.sources = {manual, crash};
  cfg.quota_limits = {{"manual", 0}, {"safety", 0}};
  return cfg;
}

void UnlinkAll(const std::string& sock, const std::string& audit) {
  unlink(sock.c_str());
  unlink((sock + ".fsm").c_str());
  unlink(audit.c_str());
  unlink((audit + ".1").c_str());
  unlink((audit + ".2").c_str());
}

// Send one trigger and tick once; the reply is produced in that same Tick.
uint16_t RoundTrip(trigger_center::Center& c, RawClient& raw,
                   const char* source, uint64_t* trigger_id) {
  const trigger_proto::TriggerRequest req = ValidReq(source);
  CHECK(raw.SendNb(&req, sizeof(req)));
  c.Tick();
  trigger_proto::TriggerResponse resp{};
  const ssize_t n = raw.Recv(&resp, sizeof(resp));
  CHECK(n == static_cast<ssize_t>(sizeof(resp)));
  CHECK(resp.magic == trigger_proto::kTriggerMagic);
  CHECK(resp.version == trigger_proto::kTriggerVersionV3);
  CHECK(resp.crc32 == trigger_proto::ResponseCrc(resp));
  if (trigger_id != nullptr)
    *trigger_id = resp.trigger_id;
  return resp.code;
}

// Balance invariants shared by both modes. `socket_sent` counts datagrams
// that went through the socket; `direct_accepts` counts accepts that came in
// through the direct Submit path (they bypass socket_datagrams but land in
// accepted). `pending_cancels` / `running_cancels` adjust the hook-call
// expectations: a pending cancel is accepted but never reaches the start
// hook, and a running cancel drives the preempt hook without counting as an
// FSM preemption.
void CheckBalance(const trigger_center::Center& c, uint64_t socket_sent,
                  uint64_t direct_accepts, uint64_t pending_cancels,
                  uint64_t running_cancels) {
  const trigger_center::CenterStats& s = c.stats();
  CHECK(s.socket_datagrams == socket_sent);
  // Only plain trigger requests were sent: every processed datagram lands in
  // exactly one of these counters.
  CHECK(s.accepted - direct_accepts + s.rejected_muted + s.rejected_busy ==
        socket_sent);
  CHECK(s.malformed_datagrams == 0);
  CHECK(s.auth_rejects == 0);
  CHECK(s.replay_rejects == 0);
  CHECK(s.rejected_quota == 0);
  CHECK(s.rejected_offline == 0);
  CHECK(s.rejected_unknown == 0);
  CHECK(s.start_failures == 0);
  CHECK(s.preempt_failures == 0);
  CHECK(s.audit_drops == 0);
  // Every accepted trigger either started exactly once or was cancelled while
  // still pending; every accepted trigger produced exactly one terminal
  // outcome (sealed / preempted / cancelled).
  CHECK(g.starts == s.accepted - pending_cancels);
  CHECK(g.preempts == s.preemptions + running_cancels);
  CHECK(s.notify_outcomes == s.accepted);
  // OBS-05 ring accounting: drops == events beyond the retained 256.
  const uint64_t expect_drops =
      s.fsm_events > trigger_center::Center::kFsmEventRingSize
          ? s.fsm_events - trigger_center::Center::kFsmEventRingSize
          : 0;
  CHECK(s.fsm_event_drops == expect_drops);
}

void RunDeterministicBursts(const std::string &sock, const std::string &audit) {
  using trigger_center::State;
  using trigger_proto::TriggerCode;

  UnlinkAll(sock, audit);
  trigger_cfg::CenterConfig cfg = MakeCfg(sock, audit, 0, 10000000ull);
  cfg.tick_ns = 2000000ull;
  cfg.max_trigger_rate_hz = 20000;
  trigger_center::Center center;
  g = Fake{};
  CHECK(center.Init(cfg, Hooks()));
  CHECK(center.SocketReceiveBufferBytes() >=
        static_cast<int>(40 * sizeof(trigger_proto::TriggerRequestV4)));
  RawClient raw(sock.c_str(), (sock + ".cli").c_str());

  uint64_t sent = 0;

  // Phase A: accept + muted probes while capturing.
  for (int i = 0; i < 30; ++i) {
    CHECK(RoundTrip(center, raw, "manual", nullptr) ==
          static_cast<uint16_t>(TriggerCode::kAccepted));
    ++sent;
    CHECK(center.state() == State::kCapturing);
    CHECK(g.active);  // started in the same Tick
    CHECK(RoundTrip(center, raw, "manual", nullptr) ==
          static_cast<uint16_t>(TriggerCode::kMuted));
    ++sent;
    CHECK(RoundTrip(center, raw, "manual", nullptr) ==
          static_cast<uint16_t>(TriggerCode::kMuted));
    ++sent;
    g.active = false;
    center.Tick();  // sealed -> idle (mute_ns = 0)
    CHECK(center.state() == State::kIdle);
  }

  // Phase B: queue depth 1 — a second trigger in the same Tick is kBusy.
  for (int i = 0; i < 10; ++i) {
    const trigger_proto::TriggerRequest r1 = ValidReq("manual");
    const trigger_proto::TriggerRequest r2 = ValidReq("manual");
    CHECK(raw.SendNb(&r1, sizeof(r1)));
    CHECK(raw.SendNb(&r2, sizeof(r2)));
    sent += 2;
    center.Tick();  // both processed in one PollSocket pass
    trigger_proto::TriggerResponse resp{};
    CHECK(raw.Recv(&resp, sizeof(resp)) == static_cast<ssize_t>(sizeof(resp)));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kAccepted));
    CHECK(raw.Recv(&resp, sizeof(resp)) == static_cast<ssize_t>(sizeof(resp)));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kBusy));
    CHECK(center.state() == State::kCapturing);
    g.active = false;
    center.Tick();
    CHECK(center.state() == State::kIdle);
  }

  // Phase C: crash-priority storm preempts the running capture.
  for (int i = 0; i < 10; ++i) {
    CHECK(RoundTrip(center, raw, "manual", nullptr) ==
          static_cast<uint16_t>(TriggerCode::kAccepted));
    ++sent;
    CHECK(RoundTrip(center, raw, "crash", nullptr) ==
          static_cast<uint16_t>(TriggerCode::kAccepted));
    ++sent;
    CHECK(center.state() == State::kCapturing);  // preempt executed, still live
    g.active = false;
    center.Tick();
    CHECK(center.state() == State::kIdle);
  }

  // Phase D: mute window holds for non-crash, crash overrides, expiry works.
  trigger_cfg::CenterConfig muted_cfg = MakeCfg(sock, audit, 300000000ULL);
  center.ReloadConfig(muted_cfg);
  {
    CHECK(RoundTrip(center, raw, "manual", nullptr) ==
          static_cast<uint16_t>(TriggerCode::kAccepted));
    ++sent;
    g.active = false;
    center.Tick();
    CHECK(center.state() == State::kMuted);
    CHECK(RoundTrip(center, raw, "manual", nullptr) ==
          static_cast<uint16_t>(TriggerCode::kMuted));
    ++sent;
    CHECK(RoundTrip(center, raw, "crash", nullptr) ==
          static_cast<uint16_t>(TriggerCode::kAccepted));  // crash over mute
    ++sent;
    g.active = false;
    center.Tick();
    CHECK(center.state() == State::kIdle);  // crash rule has mute_ns = 0

    // Mute expiry: MUTED -> IDLE once the window passes.
    CHECK(RoundTrip(center, raw, "manual", nullptr) ==
          static_cast<uint16_t>(TriggerCode::kAccepted));
    ++sent;
    g.active = false;
    center.Tick();
    CHECK(center.state() == State::kMuted);
    SleepNs(350000000ULL);
    center.Tick();
    CHECK(center.state() == State::kIdle);
  }
  center.ReloadConfig(cfg);

  // Phase E: cancel accounting — running capture over the socket, pending
  // (accepted, not yet started) via the direct path.
  for (int i = 0; i < 6; ++i) {
    uint64_t id = 0;
    if (i % 2 == 0) {
      CHECK(RoundTrip(center, raw, "manual", &id) ==
            static_cast<uint16_t>(TriggerCode::kAccepted));
      ++sent;
      CHECK(center.state() == State::kCapturing);
      CHECK(center.Cancel(id) == TriggerCode::kCancelled);
      CHECK(center.state() == State::kIdle);  // mute_ns = 0
    } else {
      const trigger_center::SubmitResult r =
          center.Submit("manual", -1, 0, false, false);
      CHECK(r.code == TriggerCode::kAccepted);
      CHECK(center.Cancel(r.trigger_id) == TriggerCode::kCancelled);
      CHECK(center.state() == State::kIdle);  // rolled back to pre-submit
    }
  }
  CHECK(center.stats().cancels_applied == 6);
  CHECK(center.stats().cancels_unknown == 0);

  // Trigger ids stay sequential across the whole storm: the next accept gets
  // accepted_so_far + 1 (cancelled ids are not reused).
  uint64_t probe_id = 0;
  CHECK(RoundTrip(center, raw, "manual", &probe_id) ==
        static_cast<uint16_t>(TriggerCode::kAccepted));
  ++sent;
  CHECK(probe_id == center.stats().accepted);
  g.active = false;
  center.Tick();
  CHECK(center.state() == State::kIdle);

  // Exactly 16 must not be mistaken for backlog. A 100-datagram single-Tick
  // burst must also drain to EAGAIN under the enlarged deterministic budget.
  const auto burst = [&](uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
      const trigger_proto::TriggerRequest req = ValidReq("manual");
      CHECK(raw.SendNb(&req, sizeof(req)));
    }
    sent += count;
    center.Tick();
    CHECK(center.stats().socket_datagrams == sent);
    for (uint32_t i = 0; i < count; ++i) {
      trigger_proto::TriggerResponse resp{};
      CHECK(raw.Recv(&resp, sizeof(resp)) ==
            static_cast<ssize_t>(sizeof(resp)));
    }
    CHECK(center.stats().socket_backlog_events == 0);
    g.active = false;
    center.Tick();
    CHECK(center.state() == State::kIdle);
  };
  burst(16);
  burst(100);

  CheckBalance(center, sent, 3, 3, 3);
  CHECK(center.state() == State::kIdle);
  CHECK(center.stats().rejected_muted == 30 * 2 + 1);
  CHECK(center.stats().rejected_busy == 10 + 15 + 99);
  CHECK(center.stats().preemptions == 10);
  CHECK(center.stats().socket_backlog_events == 0);

  center.Shutdown();
  UnlinkAll(sock, audit);
}

void RunSoak(const std::string& sock, const std::string& audit) {
  using trigger_center::State;

  UnlinkAll(sock, audit);
  trigger_cfg::CenterConfig cfg = MakeCfg(sock, audit, 5000000ULL);  // 5 ms mute
  trigger_center::Center center;
  g = Fake{};
  g.auto_complete = true;
  CHECK(center.Init(cfg, Hooks()));
  RawClient raw(sock.c_str(), (sock + ".cli").c_str());
  // A full peer queue must not block the storm loop.
  const int flags = fcntl(raw.fd, F_GETFL, 0);
  CHECK(fcntl(raw.fd, F_SETFL, flags | O_NONBLOCK) == 0);

  constexpr uint64_t kTotal = 20000;       // 2 s at the 10k/s pace
  constexpr uint64_t kBurst = 8;           // requests per slot
  constexpr uint64_t kSlotNs = 800000ULL;  // 800 us -> 10k requests/s
  uint64_t sent = 0;
  uint64_t replies = 0;
  uint64_t max_inflight = 0;
  uint64_t first_half_max_inflight = 0;
  uint64_t second_half_max_inflight = 0;
  trigger_proto::TriggerResponse resp{};

  // Deterministic preemption probe (the paced storm below is wall-clock
  // phased, so preemption there is likely but not guaranteed): park the FSM
  // in IDLE — it recurs every few ms because captures auto-complete in 1 ms
  // and the manual mute is 5 ms — then accept a manual capture and preempt
  // it with a crash request inside the 1 ms auto-completion window.
  const auto force_preemption = [&]() {
    const uint64_t dl = NowNs() + 2000000000ULL;
    while (center.state() != State::kIdle) {
      center.Tick();
      while (raw.Recv(&resp, sizeof(resp)) > 0)
        ++replies;
      CHECK(NowNs() < dl);
      SleepNs(200000ULL);
    }
    const uint64_t preempts_before = center.stats().preemptions;
    const trigger_proto::TriggerRequest m = ValidReq("manual");
    CHECK(raw.SendNb(&m, sizeof(m)));
    ++sent;
    center.Tick();  // accept + start
    CHECK(center.state() == State::kCapturing);
    const trigger_proto::TriggerRequest c = ValidReq("crash");
    CHECK(raw.SendNb(&c, sizeof(c)));
    ++sent;
    center.Tick();  // accepted; preempt executed in the same Tick
    CHECK(center.stats().preemptions == preempts_before + 1);
    while (raw.Recv(&resp, sizeof(resp)) > 0)
      ++replies;
  };

  force_preemption();
  const uint64_t t0 = NowNs();
  uint64_t next_slot = t0;
  while (sent < kTotal + 2) {  // +2: the opening probe already sent two
    const uint64_t now = NowNs();
    CHECK(now < t0 + 15000000000ULL);  // runaway guard
    if (now < next_slot)
      SleepNs(next_slot - now);
    for (uint64_t i = 0; i < kBurst && sent < kTotal + 2; ++i) {
      // Every 64th request is crash priority: exercises preemption under load.
      const trigger_proto::TriggerRequest req =
          ValidReq(sent % 64 == 63 ? "crash" : "manual");
      if (!raw.SendNb(&req, sizeof(req)))
        break; // peer queue full: retry
      ++sent;
    }
    center.Tick();
    while (raw.Recv(&resp, sizeof(resp)) > 0)
      ++replies;
    const uint64_t inflight = sent - center.stats().socket_datagrams;
    if (inflight > max_inflight) max_inflight = inflight;
    if (sent < kTotal / 2) {
      if (inflight > first_half_max_inflight) {
        first_half_max_inflight = inflight;
      }
    } else if (inflight > second_half_max_inflight) {
      second_half_max_inflight = inflight;
    }
    next_slot += kSlotNs;
  }
  const uint64_t t_sent = NowNs();
  force_preemption();

  // Drain: auto-completion seals every accepted capture; mute windows expire.
  const uint64_t deadline = NowNs() + 10000000000ULL;
  for (;;) {
    center.Tick();
    while (raw.Recv(&resp, sizeof(resp)) > 0)
      ++replies;
    if (center.state() == State::kIdle && g.starts == center.stats().accepted &&
        center.stats().notify_outcomes == center.stats().accepted) {
      break;
    }
    CHECK(NowNs() < deadline);
    SleepNs(1000000ULL);
  }
  const uint64_t t_done = NowNs();

  // Sequential ids survived the storm, and the FSM still takes new work.
  uint64_t probe_id = 0;
  CHECK(RoundTrip(center, raw, "manual", &probe_id) ==
        static_cast<uint16_t>(trigger_proto::TriggerCode::kAccepted));
  ++sent;
  ++replies;
  CHECK(probe_id == center.stats().accepted);
  SleepNs(2000000ULL);  // past the 1 ms auto-completion
  center.Tick();        // sealed -> MUTED (manual mute is 5 ms)
  SleepNs(6000000ULL);  // past the mute window
  center.Tick();        // MUTED -> IDLE

  CheckBalance(center, sent, 0, 0, 0);
  CHECK(replies == sent);  // every processed datagram was answered
  CHECK(center.stats().rejected_muted > 0);   // mute logic engaged
  CHECK(center.stats().preemptions >= 2);     // probe preemptions landed
  CHECK(center.state() == State::kIdle);

  const double send_s = static_cast<double>(t_sent - t0) * 1e-9;
  const double rate = static_cast<double>(sent) / send_s;
  std::printf(
      "test_trigger_storm soak: sent=%llu in %.2fs (%.0f req/s, "
      "target 10k/s), drained to idle in %.2fs, backlog_events=%llu, "
      "inflight_max=%llu (first_half=%llu second_half=%llu), final=%llu\n",
      static_cast<unsigned long long>(sent), send_s, rate,
      static_cast<double>(t_done - t0) * 1e-9,
      static_cast<unsigned long long>(center.stats().socket_backlog_events),
      static_cast<unsigned long long>(max_inflight),
      static_cast<unsigned long long>(first_half_max_inflight),
      static_cast<unsigned long long>(second_half_max_inflight),
      static_cast<unsigned long long>(sent - center.stats().socket_datagrams));
  // Sanity floor, not a timing gate: an in-process AF_UNIX hop must sustain
  // well above the 10k/s storm target even on a loaded box.
  CHECK(rate >= 5000.0);

  center.Shutdown();
  UnlinkAll(sock, audit);
}

}  // namespace

int main(int argc, char** argv) {
  char base[96];
  std::snprintf(base, sizeof(base), "/tmp/c9storm_%d",
                static_cast<int>(getpid()));
  const std::string sock = std::string(base) + ".sock";
  const std::string audit = std::string(base) + ".audit";
  if (argc > 1 && std::strcmp(argv[1], "soak") == 0) {
    RunSoak(sock, audit);
    return 0;
  }
  RunDeterministicBursts(sock, audit);
  return 0;
}
