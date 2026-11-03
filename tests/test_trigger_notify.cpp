// test_trigger_notify.cpp — async outcome notification: sealed /
// preempted (response code 4) / start-failed / cancelled outcomes are delivered to
// deferred waiters and recorded in a bounded ring; ring and waiter-table
// overflow and waiter expiry are counted; old-protocol peers are rejected.
//
// Section order matters: the waiter table holds 8 slots, so sections that
// park waiters run before the overflow section fills the table (its stale
// entries only expire after their 30 s deadline).
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
};

Fake g;

bool HookReady(void*) { return g.ready; }
bool HookActive(void*) { return g.active; }
bool HookStart(void*, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string>&) {
  g.active = true;
  return true;
}
bool HookPreempt(void*) {
  if (!g.preempt_ok) return false;
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

trigger_proto::TriggerRequest WaitReq(uint64_t wire_filter,
                                      uint32_t timeout_ms) {
  trigger_proto::TriggerRequest req;
  req.magic = trigger_proto::kTriggerMagic;
  req.version = trigger_proto::kTriggerVersionV3;
  req.flags =
      trigger_proto::kFlagWaitOutcome | trigger_proto::kFlagHasTimestamp;
  req.timestamp_ns = wire_filter;
  req.sequence = trigger_client::NextSequence();
  req.response_timeout_ms = timeout_ms;
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
  void Send(const void* buf, std::size_t n) {
    CHECK(sendto(fd, buf, n, 0, reinterpret_cast<sockaddr*>(&server),
                 sizeof(server)) == static_cast<ssize_t>(n));
  }
  // Non-blocking: <= 0 if nothing is queued.
  ssize_t Recv(void* buf, std::size_t cap) {
    return recv(fd, buf, cap, MSG_DONTWAIT);
  }
};

bool ValidNotification(const trigger_proto::TriggerResponse &resp, ssize_t n) {
  if (n != static_cast<ssize_t>(sizeof(resp)))
    return false;
  if (resp.magic != trigger_proto::kTriggerMagic ||
      resp.version != trigger_proto::kTriggerVersionV3)
    return false;
  if (resp.crc32 != trigger_proto::ResponseCrc(resp))
    return false;
  return (resp.flags & trigger_proto::kRespFlagOutcomeNotification) != 0;
}

// Tick until a notification arrives on the raw client.
ssize_t AwaitNotification(trigger_center::Center& c, RawClient& raw,
                          trigger_proto::TriggerResponse* resp) {
  const uint64_t deadline = NowNs() + 3000000000ULL;
  ssize_t n = 0;
  while ((n = raw.Recv(resp, sizeof(*resp))) <= 0) {
    c.Tick();
    CHECK(NowNs() < deadline);
    SleepNs(100000);
  }
  return n;
}

struct WaitJob {
  const char* socket;
  uint64_t trigger_id;
  uint64_t timeout_ns;
  trigger_client::Result res;
  std::atomic<bool> done{false};
};

void* WaitRun(void* p) {
  auto* j = static_cast<WaitJob*>(p);
  j->res = trigger_client::WaitOutcome(j->socket, j->trigger_id, j->timeout_ns);
  j->done.store(true, std::memory_order_release);
  return nullptr;
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

bool HasLine(const std::vector<std::string>& lines, const char* needle) {
  for (const std::string& l : lines) {
    if (l.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

int main() {
  using trigger_center::State;
  using trigger_proto::TriggerCode;

  char base[96];
  std::snprintf(base, sizeof(base), "/tmp/c9notify_%d",
                static_cast<int>(getpid()));
  const std::string sock = std::string(base) + ".sock";
  const std::string audit = std::string(base) + ".audit";
  unlink(sock.c_str());
  unlink(audit.c_str());

  trigger_cfg::CenterConfig cfg;
  cfg.socket_path = sock;
  cfg.audit_log_path = audit;
  trigger_cfg::SourceRule manual;
  manual.name = "manual";
  manual.priority = 10;
  manual.quota_class = "manual";
  manual.before_ns = manual.after_ns = 1000000000ULL;
  trigger_cfg::SourceRule crash;
  crash.name = "crash";
  crash.priority = 100;
  crash.quota_class = "safety";
  crash.before_ns = crash.after_ns = 1000000000ULL;
  cfg.sources = {manual, crash};
  cfg.quota_limits = {{"manual", 0}, {"safety", 0}};

  trigger_center::Center center;
  g = Fake{};
  CHECK(center.Init(cfg, Hooks()));

  // ---- sealed outcome: ring hit for a late wait, notification flag set ----
  {
    CHECK(center.Submit("manual", -1, 0, false, false).code ==
          TriggerCode::kAccepted);  // id 1
    center.Tick();                  // start
    CHECK(g.active);
    g.active = false;
    center.Tick();  // completion detected -> sealed outcome pushed
    CHECK(center.state() == State::kIdle);
    CHECK(center.stats().notify_outcomes == 1);

    trigger_proto::TriggerRequest req =
        WaitReq(trigger_proto::kWaitFilterAny, 100);
    trigger_proto::TriggerResponse resp{};
    CHECK(center.ProcessRequest(reinterpret_cast<const uint8_t*>(&req),
                                sizeof(req), OwnPeer(), &resp));
    CHECK(ValidNotification(resp, sizeof(resp)));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kCaptureSealed));
    CHECK(resp.trigger_id == 1);
    // Filtered by id: a miss cannot defer in-process (no return channel) ->
    // kBusy; a hit on id 1 replies.
    req = WaitReq(42, 100);
    CHECK(center.ProcessRequest(reinterpret_cast<const uint8_t*>(&req),
                                sizeof(req), OwnPeer(), &resp));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kBusy));
    req = WaitReq(1, 100);
    CHECK(center.ProcessRequest(reinterpret_cast<const uint8_t*>(&req),
                                sizeof(req), OwnPeer(), &resp));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kCaptureSealed));
  }

  // ---- deferred waiter over the socket: delivered on completion -----------
  {
    RawClient raw(sock.c_str(), (std::string(base) + ".w1").c_str());
    trigger_proto::TriggerRequest req = WaitReq(2, 5000);
    raw.Send(&req, sizeof(req));
    center.Tick();  // parks the waiter, no reply yet
    trigger_proto::TriggerResponse resp{};
    CHECK(raw.Recv(&resp, sizeof(resp)) <= 0);

    CHECK(center.Submit("manual", -1, 0, false, false).code ==
          TriggerCode::kAccepted);  // id 2
    center.Tick();                  // start
    g.active = false;
    const ssize_t n = AwaitNotification(center, raw, &resp);  // sealed
    CHECK(ValidNotification(resp, n));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kCaptureSealed));
    CHECK(resp.trigger_id == 2);
    CHECK(center.stats().notify_delivered == 1);
    CHECK(center.state() == State::kIdle);
  }

  // ---- preempted outcome: response code 4 (kPreemptedByHigher) delivered ---------
  {
    RawClient raw(sock.c_str(), (std::string(base) + ".w2").c_str());
    CHECK(center.Submit("manual", -1, 0, false, false).code ==
          TriggerCode::kAccepted);  // id 3
    center.Tick();
    CHECK(g.active);
    trigger_proto::TriggerRequest req = WaitReq(3, 5000);
    raw.Send(&req, sizeof(req));
    center.Tick();  // waiter parked
    CHECK(center.Submit("crash", -1, 0, false, false).code ==
          TriggerCode::kAccepted);  // id 4 preempts
    trigger_proto::TriggerResponse resp{};
    const ssize_t n = AwaitNotification(center, raw, &resp);
    CHECK(ValidNotification(resp, n));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kPreemptedByHigher));
    CHECK(resp.trigger_id == 3);
    g.active = false;
    center.Tick();  // id 4 sealed
  }

  // ---- start_failed outcome -------------------------------------------------
  {
    RawClient raw(sock.c_str(), (std::string(base) + ".w3").c_str());
    trigger_proto::TriggerRequest req = WaitReq(5, 5000);
    raw.Send(&req, sizeof(req));
    center.Tick();
    CHECK(center.Submit("manual", -1, 0, false, false).code ==
          TriggerCode::kAccepted);  // id 5
    g.ready = false;                // recorder drops before the deferred start
    center.Tick();                  // start failure -> rollback + outcome
    CHECK(center.stats().start_failures == 1);
    CHECK(center.state() == State::kIdle);
    trigger_proto::TriggerResponse resp{};
    const ssize_t n = AwaitNotification(center, raw, &resp);
    CHECK(ValidNotification(resp, n));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kStartFailed));
    CHECK(resp.trigger_id == 5);
    g.ready = true;
  }

  // ---- cancelled outcome ----------------------------------------------------
  {
    RawClient raw(sock.c_str(), (std::string(base) + ".w4").c_str());
    // Filter on the specific id: an "any" wait would match older ring entries
    // immediately instead of parking.
    trigger_proto::TriggerRequest req = WaitReq(6, 5000);
    raw.Send(&req, sizeof(req));
    center.Tick();
    CHECK(center.Submit("manual", -1, 0, false, false).code ==
          TriggerCode::kAccepted);  // id 6, pending
    CHECK(center.Cancel(6) == TriggerCode::kCancelled);
    trigger_proto::TriggerResponse resp{};
    const ssize_t n = AwaitNotification(center, raw, &resp);
    CHECK(ValidNotification(resp, n));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kCancelled));
    CHECK(resp.trigger_id == 6);
  }

  // ---- waiter expiry: deadline passes, slot freed, counted, no reply --------
  {
    RawClient raw(sock.c_str(), (std::string(base) + ".we").c_str());
    trigger_proto::TriggerRequest req = WaitReq(7777, 60);  // 60 ms budget
    raw.Send(&req, sizeof(req));
    center.Tick();
    SleepNs(80000000);  // outlast the declared budget
    center.Tick();
    CHECK(center.stats().notify_waiter_expired == 1);
    trigger_proto::TriggerResponse resp{};
    CHECK(raw.Recv(&resp, sizeof(resp)) <= 0);
  }

  // ---- client WaitOutcome helper: deferred delivery -------------------------
  {
    // Submit first, then wait on the returned id: whether the capture
    // completes before or after the wait request lands, the client is
    // answered (ring hit or deferred delivery).
    const trigger_center::SubmitResult s =
        center.Submit("manual", -1, 0, false, false);
    CHECK(s.code == TriggerCode::kAccepted);
    WaitJob job{sock.c_str(), s.trigger_id, 5000000000ULL, {}, {}};
    pthread_t th{};
    CHECK(pthread_create(&th, nullptr, WaitRun, &job) == 0);
    const uint64_t deadline = NowNs() + 5000000000ULL;
    while (!job.done.load(std::memory_order_acquire)) {
      center.Tick();
      g.active = false;  // the capture completes as soon as it starts
      CHECK(NowNs() < deadline);
      SleepNs(100000);
    }
    CHECK(pthread_join(th, nullptr) == 0);
    CHECK(job.res.transport_ok);
    CHECK((job.res.response_flags &
           trigger_proto::kRespFlagOutcomeNotification) != 0);
    CHECK(job.res.code == TriggerCode::kCaptureSealed);
    CHECK(job.res.trigger_id == s.trigger_id);

    // Deadline lapse with no outcome: bounded kTransportTimeout.
    const uint64_t t0 = NowNs();
    const trigger_client::Result r =
        trigger_client::WaitOutcome(sock.c_str(), 424242, 120000000ULL);
    const uint64_t elapsed = NowNs() - t0;
    CHECK(!r.transport_ok);
    CHECK(r.code == TriggerCode::kTransportTimeout);
    CHECK(elapsed >= 100000000ULL);
    CHECK(elapsed < 3000000000ULL);
    // Nobody ticked while the client blocked, so the datagram is only
    // processed now (parking a waiter whose reply path is already gone);
    // it expires after its own declared budget.
    center.Tick();
    SleepNs(150000000);
    center.Tick();
  }

  // ---- waiter table overflow: 9th wait gets kBusy, counted -----------------
  {
    std::vector<RawClient*> waiters;
    for (int i = 0; i < 8; ++i) {
      char self[128];
      std::snprintf(self, sizeof(self), "%s.wx%d", base, i);
      waiters.push_back(new RawClient(sock.c_str(), self));
      trigger_proto::TriggerRequest req =
          WaitReq(1000 + static_cast<uint64_t>(i), 30000);
      waiters.back()->Send(&req, sizeof(req));
      center.Tick();
    }
    // The 9th wait finds no free slot: immediate kBusy, no notification flag.
    RawClient ninth(sock.c_str(), (std::string(base) + ".w9").c_str());
    trigger_proto::TriggerRequest req = WaitReq(4242, 30000);
    ninth.Send(&req, sizeof(req));
    trigger_proto::TriggerResponse resp{};
    const ssize_t n = AwaitNotification(center, ninth, &resp);
    CHECK(n == static_cast<ssize_t>(sizeof(resp)));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kBusy));
    CHECK((resp.flags & trigger_proto::kRespFlagOutcomeNotification) == 0);
    CHECK(center.stats().notify_waiter_overflows == 1);
    for (RawClient *w : waiters)
      delete w; // their entries linger, harmlessly
  }

  // ---- ring overflow: oldest outcomes overwritten, counted
  // -------------------
  {
    for (int i = 0; i < 70; ++i) {
      CHECK(center.Submit("manual", -1, 0, false, false).code ==
            TriggerCode::kAccepted);
      center.Tick();
      g.active = false;
      center.Tick();
    }
    // The ring holds the newest 64 outcomes; the rest were overwritten.
    CHECK(center.stats().notify_outcomes > 64);
    CHECK(center.stats().notify_outcomes -
              center.stats().notify_outcome_drops ==
          64);
    // The newest outcome is still matchable by a late wait.
    trigger_proto::TriggerRequest req =
        WaitReq(trigger_proto::kWaitFilterAny, 100);
    trigger_proto::TriggerResponse resp{};
    CHECK(center.ProcessRequest(reinterpret_cast<const uint8_t*>(&req),
                                sizeof(req), OwnPeer(), &resp));
    CHECK(ValidNotification(resp, sizeof(resp)));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kCaptureSealed));
  }

  // ---- old-protocol peer rejected (no cancel/wait on V1/V2) ----------------
  {
    trigger_proto::TriggerRequestV2 v2;
    v2.magic = trigger_proto::kTriggerMagic;
    v2.version = trigger_proto::kTriggerVersionV2;
    v2.sequence = trigger_client::NextSequence();
    std::snprintf(v2.source, sizeof(v2.source), "manual");
    trigger_proto::TriggerResponse resp{};
    CHECK(center.ProcessRequest(reinterpret_cast<const uint8_t*>(&v2),
                                sizeof(v2), OwnPeer(), &resp));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kUnsupportedVersion));
  }

  // ---- audit: wait registration + delivery are recorded ---------------------
  {
    const std::vector<std::string> lines = ReadLines(audit.c_str());
    CHECK(HasLine(lines, "req=wait"));
    CHECK(HasLine(lines, "reason=wait_registered"));
    CHECK(HasLine(lines, "code=capture_sealed"));
    CHECK(HasLine(lines, "code=preempted_by_higher"));
    CHECK(HasLine(lines, "code=cancelled"));
    CHECK(HasLine(lines, "reason=notify_waiter_overflow"));
  }

  center.Shutdown();
  unlink(sock.c_str());
  unlink(audit.c_str());
  return 0;
}
