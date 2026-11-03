// test_trigger_cancel.cpp — TRG-07 Cancel(trigger_id): pending drop with FSM
// rollback, running cancel as preempt-to-idle (existing preempt semantics —
// what is written stays sealed), unknown/completed id as an explicit
// kUnknownTrigger, authorization, malformed flag combos, the wire client API
// and the audit records.
#include "thyme/check.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_client.h"
#include "thyme/record/trigger_config.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

struct Fake {
  bool ready = true;
  bool active = false;
  bool preempt_ok = true;
  int preempt_calls = 0;
  int start_calls = 0;
};

Fake g;

bool HookReady(void*) { return g.ready; }
bool HookActive(void*) { return g.active; }
bool HookStart(void*, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string>&) {
  ++g.start_calls;
  g.active = true;
  return true;
}
bool HookPreempt(void*) {
  ++g.preempt_calls;
  if (!g.preempt_ok) return false;
  g.active = false;  // truncate+seal, like a preempt
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

trigger_center::PeerCred OwnPeer() {
  trigger_center::PeerCred p;
  p.uid = geteuid();
  p.pid = getpid();
  return p;
}

trigger_proto::TriggerRequest CancelReq(uint64_t trigger_id) {
  trigger_proto::TriggerRequest req;
  req.magic = trigger_proto::kTriggerMagic;
  req.version = trigger_proto::kTriggerVersionV3;
  req.flags = trigger_proto::kFlagCancel | trigger_proto::kFlagHasTimestamp;
  req.timestamp_ns = trigger_id;
  req.sequence = trigger_client::NextSequence();
  trigger_proto::StampRequestCrc(&req);
  return req;
}

struct ClientJob {
  const char* socket;
  uint64_t trigger_id;
  bool is_cancel;
  trigger_client::Result res;
  std::atomic<bool> done{false};
};

void* ClientRun(void* p) {
  auto* j = static_cast<ClientJob*>(p);
  j->res = j->is_cancel
               ? trigger_client::Cancel(j->socket, j->trigger_id)
               : trigger_client::Result{};
  j->done.store(true, std::memory_order_release);
  return nullptr;
}

// Run trigger_client::Cancel on a thread while ticking the center.
trigger_client::Result CancelWhileTicking(trigger_center::Center& center,
                                          const char* sock, uint64_t id) {
  ClientJob job{sock, id, true, {}, {}};
  pthread_t th{};
  CHECK(pthread_create(&th, nullptr, ClientRun, &job) == 0);
  const uint64_t deadline = NowNs() + 5000000000ULL;
  while (!job.done.load(std::memory_order_acquire)) {
    center.Tick();
    CHECK(NowNs() < deadline);
    SleepNs(100000);
  }
  CHECK(pthread_join(th, nullptr) == 0);
  return job.res;
}

std::vector<std::string> ReadLines(const char* path) {
  std::vector<std::string> out;
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) return out;
  char buf[1024];
  while (std::fgets(buf, sizeof(buf), f) != nullptr) {
    std::string line(buf);
    if (!line.empty() && line.back() == '\n') line.pop_back();
    out.push_back(line);
  }
  std::fclose(f);
  return out;
}

std::string Field(const std::string& line, const char* key) {
  const std::string needle = std::string(" ") + key + "=";
  std::size_t p = line.find(needle);
  if (p == std::string::npos) {
    const std::string head = std::string(key) + "=";
    if (line.compare(0, head.size(), head) != 0)
      return {};
    p = 0;
    const std::size_t e = line.find(' ', head.size());
    return line.substr(head.size(), e - head.size());
  }
  p += needle.size();
  const std::size_t e = line.find(' ', p);
  return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

}  // namespace

int main() {
  using trigger_center::State;
  using trigger_proto::TriggerCode;

  char sock[96];
  char audit[96];
  std::snprintf(sock, sizeof(sock), "/tmp/c9cancel_%d.sock",
                static_cast<int>(getpid()));
  std::snprintf(audit, sizeof(audit), "/tmp/c9cancel_%d.audit",
                static_cast<int>(getpid()));
  unlink(sock);
  unlink(audit);

  trigger_cfg::CenterConfig cfg;
  cfg.socket_path = sock;
  cfg.audit_log_path = audit;
  cfg.sources = {Rule("manual", 10, 0, "manual"),
                 Rule("crash", 100, 0, "safety")};
  cfg.quota_limits = {{"manual", 0}, {"safety", 0}};

  trigger_center::Center center;
  g = Fake{};
  CHECK(center.Init(cfg, Hooks()));

  // ---- cancel-pending: queued trigger dropped, FSM rolls back -------------
  {
    const trigger_center::SubmitResult r =
        center.Submit("manual", -1, 0, false, false);
    CHECK(r.code == TriggerCode::kAccepted);
    CHECK(r.trigger_id == 1);
    CHECK(center.state() == State::kCapturing);
    CHECK(center.Cancel(1) == TriggerCode::kCancelled);
    CHECK(center.stats().cancels_applied == 1);
    CHECK(center.state() == State::kIdle);       // rolled back to pre-Submit
    CHECK(center.current_trigger_id() == 0);
    CHECK(g.start_calls == 0);                   // never started
    // The cancel outcome was recorded for waiters.
    trigger_proto::TriggerResponse resp{};
    trigger_proto::TriggerRequest wait;
    wait.magic = trigger_proto::kTriggerMagic;
    wait.version = trigger_proto::kTriggerVersionV3;
    wait.flags =
        trigger_proto::kFlagWaitOutcome | trigger_proto::kFlagHasTimestamp;
    wait.timestamp_ns = trigger_proto::kWaitFilterAny;
    wait.sequence = trigger_client::NextSequence();
    trigger_proto::StampRequestCrc(&wait);
    CHECK(center.ProcessRequest(reinterpret_cast<const uint8_t*>(&wait),
                                sizeof(wait), OwnPeer(), &resp));
    CHECK((resp.flags & trigger_proto::kRespFlagOutcomeNotification) != 0);
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kCancelled));
    CHECK(resp.trigger_id == 1);
  }

  // ---- cancel-pending during a queued preempt: old capture keeps running --
  {
    CHECK(center.Submit("manual", -1, 0, false, false).code ==
          TriggerCode::kAccepted);  // id 2
    center.Tick();                  // id 2 starts
    CHECK(g.active);
    CHECK(center.Submit("crash", -1, 0, false, false).code ==
          TriggerCode::kAccepted);  // id 3 queued for preempt
    CHECK(center.current_trigger_id() == 3);
    CHECK(center.Cancel(3) == TriggerCode::kCancelled);
    CHECK(center.stats().cancels_applied == 2);
    CHECK(center.state() == State::kCapturing);   // rolled back to id 2
    CHECK(center.current_trigger_id() == 2);
    CHECK(g.active);                              // old capture continues
    CHECK(g.preempt_calls == 0);                  // preempt never executed
    // The old capture is no longer cancellable by the pending id, and the
    // running one still is.
    CHECK(center.Cancel(3) == TriggerCode::kUnknownTrigger);
    CHECK(center.Cancel(2) == TriggerCode::kCancelled);
    CHECK(center.stats().cancels_applied == 3);
    CHECK(g.preempt_calls == 1);                  // running cancel = preempt
    CHECK(center.state() == State::kIdle);        // mute_ns = 0
  }

  // ---- cancel-running honours the source mute window ----------------------
  {
    trigger_cfg::CenterConfig cfg2 = cfg;
    cfg2.sources[0].mute_ns = 30000000ULL;  // manual mute 30 ms
    trigger_center::Center c2;
    g = Fake{};
    char sock2[96];
    std::snprintf(sock2, sizeof(sock2), "/tmp/c9cancel2_%d.sock",
                  static_cast<int>(getpid()));
    cfg2.socket_path = sock2;
    cfg2.audit_log_path = std::string(sock2) + ".audit";
    CHECK(c2.Init(cfg2, Hooks()));
    CHECK(c2.Submit("manual", -1, 0, false, false).code ==
          TriggerCode::kAccepted);
    c2.Tick();
    CHECK(g.active);
    CHECK(c2.Cancel(1) == TriggerCode::kCancelled);
    CHECK(c2.state() == State::kMuted);  // like a completed capture
    // Muted: a non-crash trigger is rejected while the mute window runs.
    CHECK(c2.Submit("manual", -1, 0, false, false).code == TriggerCode::kMuted);
    SleepNs(40000000);
    c2.Tick();
    CHECK(c2.state() == State::kIdle);
    // Completed id: explicit rejection, not silent success.
    CHECK(c2.Cancel(1) == TriggerCode::kUnknownTrigger);
    c2.Shutdown();
    unlink(sock2);
    unlink((std::string(sock2) + ".audit").c_str());
  }

  // ---- cancel-unknown: never issued / future ids --------------------------
  {
    const uint64_t before = center.stats().cancels_unknown;
    CHECK(center.Cancel(999) == TriggerCode::kUnknownTrigger);
    CHECK(center.Cancel(0) == TriggerCode::kUnknownTrigger);
    CHECK(center.stats().cancels_unknown == before + 2);
  }

  // ---- cancel-running with a failing rollback hook: kBusy, keeps running --
  {
    trigger_center::Center c3;
    g = Fake{};
    char sock3[96];
    std::snprintf(sock3, sizeof(sock3), "/tmp/c9cancel3_%d.sock",
                  static_cast<int>(getpid()));
    trigger_cfg::CenterConfig cfg3 = cfg;
    cfg3.socket_path = sock3;
    cfg3.audit_log_path = std::string(sock3) + ".audit";
    CHECK(c3.Init(cfg3, Hooks()));
    CHECK(c3.Submit("manual", -1, 0, false, false).code ==
          TriggerCode::kAccepted);
    c3.Tick();
    CHECK(g.active);
    g.preempt_ok = false;
    CHECK(c3.Cancel(1) == TriggerCode::kBusy);  // retryable
    CHECK(c3.stats().cancel_failures == 1);
    CHECK(c3.stats().cancels_applied == 0);
    CHECK(c3.state() == State::kCapturing);
    CHECK(g.active);  // capture keeps running
    g.preempt_ok = true;
    CHECK(c3.Cancel(1) == TriggerCode::kCancelled);  // retry succeeds
    c3.Shutdown();
    unlink(sock3);
    unlink((std::string(sock3) + ".audit").c_str());
  }

  // ---- wire cancel via the client API + audit -----------------------------
  {
    g = Fake{};
    CHECK(center.Submit("manual", -1, 0, false, false).code ==
          TriggerCode::kAccepted);  // id 4, pending
    const trigger_client::Result r = CancelWhileTicking(center, sock, 4);
    CHECK(r.transport_ok);
    CHECK(r.code == TriggerCode::kCancelled);
    CHECK(r.trigger_id == 4);
    CHECK(center.state() == State::kIdle);

    const std::vector<std::string> lines = ReadLines(audit);
    std::string line;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
      if (Field(*it, "req") == "cancel") {
        line = *it;
        break;
      }
    }
    CHECK(!line.empty());
    CHECK(Field(line, "decision") == "accepted");
    CHECK(Field(line, "code") == "cancelled");
    CHECK(Field(line, "trigger_id") == "4");
    CHECK(Field(line, "uid") == std::to_string(geteuid()));
  }

  // ---- authorization: a foreign uid's cancel is refused without a reply ---
  {
    g = Fake{};
    CHECK(center.Submit("manual", -1, 0, false, false).code ==
          TriggerCode::kAccepted);  // id 5, pending
    trigger_proto::TriggerRequest req = CancelReq(5);
    trigger_proto::TriggerResponse resp{};
    trigger_center::PeerCred foreign;
    foreign.uid = static_cast<uid_t>(geteuid() + 1);
    foreign.pid = getpid();
    const uint64_t auth_before = center.stats().auth_rejects;
    CHECK(!center.ProcessRequest(reinterpret_cast<const uint8_t*>(&req),
                                 sizeof(req), foreign, &resp));
    CHECK(center.stats().auth_rejects == auth_before + 1);
    CHECK(center.state() == State::kCapturing);  // untouched
    // Own uid cancels it; the reject was audited too.
    CHECK(center.ProcessRequest(reinterpret_cast<const uint8_t*>(&req),
                                sizeof(req), OwnPeer(), &resp));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kCancelled));
    const std::vector<std::string> lines = ReadLines(audit);
    // Auth rejects are audited before the datagram is parsed (SEC-02), so the
    // record carries the reject reason, not the cancel opcode.
    bool found_reject = false;
    for (const std::string& l : lines) {
      if (Field(l, "decision") == "rejected" &&
          Field(l, "reason") == "auth_uid_not_allowed") {
        found_reject = true;
      }
    }
    CHECK(found_reject);
  }

  // ---- malformed flag combos: no reply, counted ---------------------------
  {
    const uint64_t mal_before = center.stats().malformed_datagrams;
    // cancel + wait together
    trigger_proto::TriggerRequest req = CancelReq(1);
    req.flags |= trigger_proto::kFlagWaitOutcome;
    trigger_proto::StampRequestCrc(&req);
    trigger_proto::TriggerResponse resp{};
    CHECK(!center.ProcessRequest(reinterpret_cast<const uint8_t*>(&req),
                                 sizeof(req), OwnPeer(), &resp));
    // cancel without the timestamp pairing
    req = CancelReq(1);
    req.flags = trigger_proto::kFlagCancel;
    trigger_proto::StampRequestCrc(&req);
    CHECK(!center.ProcessRequest(reinterpret_cast<const uint8_t*>(&req),
                                 sizeof(req), OwnPeer(), &resp));
    CHECK(center.stats().malformed_datagrams == mal_before + 2);
  }

  // ---- legacy peers: TRG-07 flag bits on V1/V2 layouts fail closed --------
  {
    const uint64_t mal_before = center.stats().malformed_datagrams;
    trigger_proto::TriggerRequestV2 v2;
    v2.magic = trigger_proto::kTriggerMagic;
    v2.version = trigger_proto::kTriggerVersionV2;
    v2.flags = trigger_proto::kFlagCancel | trigger_proto::kFlagHasTimestamp;
    v2.timestamp_ns = 1;
    v2.sequence = trigger_client::NextSequence();
    trigger_proto::TriggerResponse resp{};
    CHECK(!center.ProcessRequest(reinterpret_cast<const uint8_t*>(&v2),
                                 sizeof(v2), OwnPeer(), &resp));
    trigger_proto::TriggerRequestV1 v1;
    v1.magic = trigger_proto::kTriggerMagic;
    v1.version = trigger_proto::kTriggerVersionLegacy;
    v1.flags = trigger_proto::kFlagCancel | trigger_proto::kFlagHasTimestamp;
    v1.timestamp_ns = 1;
    CHECK(!center.ProcessRequest(reinterpret_cast<const uint8_t*>(&v1),
                                 sizeof(v1), OwnPeer(), &resp));
    CHECK(center.stats().malformed_datagrams == mal_before + 2);

    // A plain V2 peer is still rejected with the explicit code + reply.
    v2.flags = 0;
    v2.timestamp_ns = 0;
    v2.sequence = trigger_client::NextSequence();
    CHECK(center.ProcessRequest(reinterpret_cast<const uint8_t*>(&v2),
                                sizeof(v2), OwnPeer(), &resp));
    CHECK(resp.version == trigger_proto::kTriggerVersionV2);
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kUnsupportedVersion));
  }

  center.Shutdown();
  unlink(sock);
  unlink(audit);
  return 0;
}
