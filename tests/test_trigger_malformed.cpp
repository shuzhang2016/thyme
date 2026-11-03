// test_trigger_malformed.cpp — TRG-09 malformed-datagram counter coverage:
// every rejected-at-parse shape (bad length, truncated header, bad magic,
// bad version, bad CRC, bad timeout, unknown/contradictory opcode flags on
// V3/V2/V1) bumps CenterStats::malformed_datagrams exactly once, is audited,
// gets no reply, and leaves the FSM untouched. Exercised through the direct
// ProcessRequest path and through the real socket (PollSocket wiring).
#include "thyme/check.h"
#include "thyme/record/trigger_audit.h"
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

// Direct-path malformed probe: must be counted and answered with nothing.
void ExpectMalformed(trigger_center::Center& c, const void* buf, std::size_t n,
                     uint64_t expect) {
  trigger_proto::TriggerResponse resp{};
  CHECK(!c.ProcessRequest(reinterpret_cast<const uint8_t*>(buf), n, OwnPeer(),
                          &resp));
  CHECK(c.stats().malformed_datagrams == expect);
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

bool FileHas(const char* path, const char* needle) {
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) return false;
  char buf[1024];
  bool found = false;
  while (std::fgets(buf, sizeof(buf), f) != nullptr) {
    if (std::strstr(buf, needle) != nullptr) {
      found = true;
      break;
    }
  }
  std::fclose(f);
  return found;
}

}  // namespace

int main() {
  using trigger_center::State;
  using trigger_proto::TriggerCode;

  char base[96];
  std::snprintf(base, sizeof(base), "/tmp/c9mal_%d",
                static_cast<int>(getpid()));
  const std::string sock = std::string(base) + ".sock";
  const std::string audit = std::string(base) + ".audit";
  unlink(sock.c_str());
  unlink(audit.c_str());

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

  trigger_center::Center center;
  CHECK(center.Init(cfg, Hooks()));

  uint64_t mal = 0;  // expected malformed_datagrams

  // ---- bad length / truncated header --------------------------------------
  {
    const trigger_proto::TriggerRequest req = ValidReq("manual");
    ExpectMalformed(center, &req, 8, ++mal);    // truncated header
    ExpectMalformed(center, &req, 50, ++mal);   // between known sizes
    uint8_t garbage[512];
    std::memset(garbage, 0xA5, sizeof(garbage));
    ExpectMalformed(center, garbage, sizeof(garbage), ++mal);  // oversized
  }

  // ---- bad magic / bad version (right size) --------------------------------
  {
    trigger_proto::TriggerRequest req = ValidReq("manual");
    req.magic = 0xDEADBEEF;
    trigger_proto::StampRequestCrc(&req);
    ExpectMalformed(center, &req, sizeof(req), ++mal);

    req = ValidReq("manual");
    req.version = 99;
    trigger_proto::StampRequestCrc(&req);
    ExpectMalformed(center, &req, sizeof(req), ++mal);
  }

  // ---- bad CRC (SEC-04): malformed + crc_rejects ---------------------------
  {
    trigger_proto::TriggerRequest req = ValidReq("manual");
    req.crc32 ^= 0x00000001u;
    const uint64_t crc_before = center.stats().crc_rejects;
    ExpectMalformed(center, &req, sizeof(req), ++mal);
    CHECK(center.stats().crc_rejects == crc_before + 1);
  }

  // ---- bad response_timeout_ms ---------------------------------------------
  {
    trigger_proto::TriggerRequest req = ValidReq("manual");
    req.response_timeout_ms = 0;  // < kMinResponseTimeoutMs
    trigger_proto::StampRequestCrc(&req);
    ExpectMalformed(center, &req, sizeof(req), ++mal);

    req = ValidReq("manual");
    req.response_timeout_ms = trigger_proto::kMaxResponseTimeoutMs + 1;
    trigger_proto::StampRequestCrc(&req);
    ExpectMalformed(center, &req, sizeof(req), ++mal);
  }

  // ---- unknown / contradictory opcodes (TRG-07 flags) -----------------------
  {
    // cancel + wait simultaneously is contradictory.
    trigger_proto::TriggerRequest req = ValidReq("manual");
    req.flags = trigger_proto::kFlagHasTimestamp | trigger_proto::kFlagCancel |
                trigger_proto::kFlagWaitOutcome;
    req.timestamp_ns = 1;
    trigger_proto::StampRequestCrc(&req);
    ExpectMalformed(center, &req, sizeof(req), ++mal);

    // cancel without kFlagHasTimestamp carries no target id.
    req = ValidReq("manual");
    req.flags = trigger_proto::kFlagCancel;
    trigger_proto::StampRequestCrc(&req);
    ExpectMalformed(center, &req, sizeof(req), ++mal);

    // V2 datagram carrying a V3-only opcode bit.
    trigger_proto::TriggerRequestV2 v2;
    v2.magic = trigger_proto::kTriggerMagic;
    v2.version = trigger_proto::kTriggerVersionV2;
    v2.flags = trigger_proto::kFlagCancel;
    v2.priority_hint = -1;
    std::snprintf(v2.source, sizeof(v2.source), "manual");
    v2.sequence = trigger_client::NextSequence();
    ExpectMalformed(center, &v2, sizeof(v2), ++mal);

    // V1 datagram carrying a V3-only opcode bit.
    trigger_proto::TriggerRequestV1 v1;
    v1.magic = trigger_proto::kTriggerMagic;
    v1.version = trigger_proto::kTriggerVersionLegacy;
    v1.flags = trigger_proto::kFlagWaitOutcome;
    v1.priority_hint = -1;
    std::snprintf(v1.source, sizeof(v1.source), "manual");
    ExpectMalformed(center, &v1, sizeof(v1), ++mal);
  }

  // ---- the FSM never saw any of it ------------------------------------------
  CHECK(center.state() == State::kIdle);
  CHECK(center.stats().accepted == 0);
  CHECK(center.stats().malformed_datagrams == mal);

  // ---- socket path: PollSocket counts the same way, no reply ----------------
  RawClient raw(sock.c_str(), (std::string(base) + ".cli").c_str());
  {
    uint8_t garbage[300];
    std::memset(garbage, 0x5A, sizeof(garbage));
    raw.Send(garbage, sizeof(garbage));  // unknown size
    trigger_proto::TriggerRequest bad = ValidReq("manual");
    bad.version = 7;
    trigger_proto::StampRequestCrc(&bad);
    raw.Send(&bad, sizeof(bad));  // bad version

    center.Tick();  // single-threaded: both datagrams processed now
    CHECK(center.stats().malformed_datagrams == mal + 2);
    mal += 2;
    CHECK(center.stats().socket_datagrams == 2);
    uint8_t sink[256];
    CHECK(raw.Recv(sink, sizeof(sink)) <= 0);  // malformed: never answered
  }

  // ---- a valid datagram right after still works, counter untouched ----------
  {
    trigger_proto::TriggerRequest req = ValidReq("manual");
    raw.Send(&req, sizeof(req));
    center.Tick();
    trigger_proto::TriggerResponse resp{};
    const ssize_t n = raw.Recv(&resp, sizeof(resp));
    CHECK(n == static_cast<ssize_t>(sizeof(resp)));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kAccepted));
    CHECK(resp.crc32 == trigger_proto::ResponseCrc(resp));
    CHECK(center.stats().malformed_datagrams == mal);
    g_active = false;
    center.Tick();  // complete -> idle
    CHECK(center.state() == State::kIdle);
  }

  // ---- every malformed shape is audited with its reason ---------------------
  CHECK(FileHas(audit.c_str(), "reason=malformed_size"));
  CHECK(FileHas(audit.c_str(), "reason=malformed_magic_version"));
  CHECK(FileHas(audit.c_str(), "reason=bad_crc"));
  CHECK(FileHas(audit.c_str(), "reason=bad_timeout"));
  CHECK(FileHas(audit.c_str(), "reason=bad_flags"));
  CHECK(center.stats().audit_drops == 0);

  center.Shutdown();
  unlink(sock.c_str());
  unlink(audit.c_str());
  unlink((audit + ".1").c_str());
  unlink((audit + ".2").c_str());
  unlink((sock + ".fsm").c_str());
  return 0;
}
