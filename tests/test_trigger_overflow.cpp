// test_trigger_overflow.cpp — TRG-09 overflow counters on the trigger socket
// path:
//   * socket_backlog_events / socket_datagrams: PollSocket has a 16-datagram
//     cap and a time budget, so a 17-datagram burst may need several Ticks.
//     Drain to EAGAIN within a bounded loop, then require no new backlog event;
//     no datagram is lost or reordered (first Tick: accepted + kBusy, later
//     Ticks: kMuted against the running capture).
//   * notify_outcome_drops: the 64-entry outcome ring overwrites the oldest
//     entry once full; the drop count and the retained window are exact.
// Waiter-table overflow (notify_waiter_overflows) is covered by
// test_trigger_notify and not repeated here.
#include "thyme/check.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_client.h"
#include "thyme/record/trigger_config.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

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

trigger_center::PeerCred OwnPeer() {
  trigger_center::PeerCred p;
  p.uid = geteuid();
  p.pid = getpid();
  return p;
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
  void Send(const void* buf, std::size_t n) {
    CHECK(sendto(fd, buf, n, 0, reinterpret_cast<sockaddr*>(&server),
                 sizeof(server)) == static_cast<ssize_t>(n));
  }
  ssize_t Recv(void* buf, std::size_t cap) {
    return recv(fd, buf, cap, MSG_DONTWAIT);
  }
};

trigger_cfg::CenterConfig MakeCfg(const std::string& sock,
                                  const std::string& audit) {
  trigger_cfg::CenterConfig cfg;
  cfg.socket_path = sock;
  cfg.audit_log_path = audit;
  cfg.audit_max_bytes = 1 << 20;
  trigger_cfg::SourceRule manual;
  manual.name = "manual";
  manual.priority = 10;
  manual.quota_class = "manual";
  manual.before_ns = manual.after_ns = 1000000000ULL;
  cfg.sources = {manual};
  cfg.quota_limits = {{"manual", 0}};
  return cfg;
}

void UnlinkAll(const std::string& sock, const std::string& audit) {
  unlink(sock.c_str());
  unlink((sock + ".fsm").c_str());
  unlink(audit.c_str());
  unlink((audit + ".1").c_str());
  unlink((audit + ".2").c_str());
}

}  // namespace

int main() {
  using trigger_center::State;
  using trigger_proto::TriggerCode;

  char base[96];
  std::snprintf(base, sizeof(base), "/tmp/c9ovf_%d",
                static_cast<int>(getpid()));

  // ---- socket drain: 17 datagrams reach EAGAIN without a false backlog ----
  {
    const std::string sock = std::string(base) + ".sock";
    const std::string audit = std::string(base) + ".audit";
    UnlinkAll(sock, audit);
    trigger_center::Center center;
    g_active = false;
    CHECK(center.Init(MakeCfg(sock, audit), Hooks()));
    RawClient raw(sock.c_str(), (std::string(base) + ".cli").c_str());

    for (int i = 0; i < 17; ++i) {
      const trigger_proto::TriggerRequest req = ValidReq("manual");
      raw.Send(&req, sizeof(req));
    }
    CHECK(center.stats().socket_datagrams == 0);
    center.Tick();
    const uint64_t first_pass = center.stats().socket_datagrams;
    CHECK(first_pass > 0);
    CHECK(first_pass <= 17);
    for (uint32_t drain = 0;
         drain < 64 && center.stats().socket_datagrams < 17; ++drain) {
      center.Tick();
    }
    CHECK(center.stats().socket_datagrams == 17);
    const uint64_t drained_backlog = center.stats().socket_backlog_events;
    center.Tick();
    CHECK(center.stats().socket_backlog_events == drained_backlog);
    CHECK(center.stats().socket_datagrams == 17);

    // All 17 answered in order: accepted, busy while pending, then muted.
    uint64_t n_accepted = 0;
    uint64_t n_busy = 0;
    uint64_t n_muted = 0;
    for (int i = 0; i < 17; ++i) {
      trigger_proto::TriggerResponse resp{};
      const ssize_t n = raw.Recv(&resp, sizeof(resp));
      CHECK(n == static_cast<ssize_t>(sizeof(resp)));
      CHECK(resp.magic == trigger_proto::kTriggerMagic);
      CHECK(resp.crc32 == trigger_proto::ResponseCrc(resp));
      const uint16_t code = resp.code;
      if (code == static_cast<uint16_t>(TriggerCode::kAccepted))
        ++n_accepted;
      if (code == static_cast<uint16_t>(TriggerCode::kBusy))
        ++n_busy;
      if (code == static_cast<uint16_t>(TriggerCode::kMuted))
        ++n_muted;
    }
    CHECK(n_accepted == 1);
    CHECK(n_busy == first_pass - 1);
    CHECK(n_muted == 17 - first_pass);
    CHECK(center.stats().accepted == 1);
    CHECK(center.stats().rejected_busy == first_pass - 1);
    CHECK(center.stats().rejected_muted == 17 - first_pass);
    CHECK(center.stats().malformed_datagrams == 0);
    CHECK(center.state() == State::kCapturing);
    g_active = false;
    center.Tick();  // sealed -> idle (mute_ns = 0)
    CHECK(center.state() == State::kIdle);
    center.Shutdown();
    UnlinkAll(sock, audit);
  }

  // ---- outcome ring overflow: 65 outcomes, ring holds the newest 64 --------
  {
    const std::string sock = std::string(base) + "r.sock";
    const std::string audit = std::string(base) + "r.audit";
    UnlinkAll(sock, audit);
    trigger_center::Center center;
    g_active = false;
    CHECK(center.Init(MakeCfg(sock, audit), Hooks()));

    for (int i = 0; i < 65; ++i) {
      CHECK(center.Submit("manual", -1, 0, false, false).code ==
            TriggerCode::kAccepted);
      center.Tick();  // deferred start
      g_active = false;
      center.Tick();  // completion -> sealed outcome
    }
    CHECK(center.stats().notify_outcomes == 65);
    CHECK(center.stats().notify_outcome_drops == 1);  // id 1 overwritten

    // The overwritten id is gone (in-process wait has no return channel, so a
    // ring miss is answered kBusy); the oldest retained and newest ids match.
    trigger_proto::TriggerResponse resp{};
    trigger_proto::TriggerRequest req = ValidReq("manual");
    req.flags = trigger_proto::kFlagWaitOutcome |
                trigger_proto::kFlagHasTimestamp;
    req.timestamp_ns = 1;  // overwritten
    trigger_proto::StampRequestCrc(&req);
    CHECK(center.ProcessRequest(reinterpret_cast<const uint8_t*>(&req),
                                sizeof(req), OwnPeer(), &resp));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kBusy));

    req = ValidReq("manual");
    req.flags = trigger_proto::kFlagWaitOutcome |
                trigger_proto::kFlagHasTimestamp;
    req.timestamp_ns = 2;  // oldest retained
    trigger_proto::StampRequestCrc(&req);
    CHECK(center.ProcessRequest(reinterpret_cast<const uint8_t*>(&req),
                                sizeof(req), OwnPeer(), &resp));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kCaptureSealed));
    CHECK(resp.trigger_id == 2);

    req = ValidReq("manual");
    req.flags = trigger_proto::kFlagWaitOutcome |
                trigger_proto::kFlagHasTimestamp;
    req.timestamp_ns = 65;  // newest
    trigger_proto::StampRequestCrc(&req);
    CHECK(center.ProcessRequest(reinterpret_cast<const uint8_t*>(&req),
                                sizeof(req), OwnPeer(), &resp));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kCaptureSealed));
    CHECK(resp.trigger_id == 65);
    CHECK(center.state() == State::kIdle);
    center.Shutdown();
    UnlinkAll(sock, audit);
  }

  return 0;
}
