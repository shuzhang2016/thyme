// test_trigger_security.cpp — SEC-02 control-surface security, exercised
// through a real UNIX datagram socket (kernel SCM_CREDENTIALS, the datagram
// equivalent of SO_PEERCRED) with forked clients, plus direct ProcessRequest
// checks and the audit-log implementation.
//
// Covered:
//   * allowed uid accepted, kernel-supplied peer pid recorded;
//   * non-allowlisted uid rejected with no reply (config allowlist change);
//   * priority_hint clamped by [security].max_priority_hint and echoed in the
//     V2 response + audit;
//   * duplicate/old sequence rejected (per-peer sliding window);
//   * malformed / oversized datagram rejected without a crash or reply;
//   * V1 requests fail closed unless allow_legacy_unsequenced = true;
//   * audit records parseable for accept and reject, rotation bounded;
//   * [security] config keys parse/dump/round-trip and reject bad values.
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "thyme/check.h"
#include "thyme/record/trigger_audit.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_client.h"
#include "thyme/record/trigger_config.h"

using namespace thyme;

namespace {

std::atomic<bool> g_active{false};

bool HookReady(void *) { return true; }
bool HookActive(void *) { return g_active.load(); }
bool HookStart(void *, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string> &) {
  g_active.store(true);
  return true;
}
bool HookPreempt(void *) {
  g_active.store(false);
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

trigger_cfg::CenterConfig MakeCfg(const char *sock, const char *audit) {
  trigger_cfg::CenterConfig c;
  c.socket_path = sock;
  c.audit_log_path = audit;
  c.audit_max_bytes = 1 << 20;
  c.audit_rotate_keep = 2;
  c.max_priority_hint = 50;
  trigger_cfg::SourceRule manual;
  manual.name = "manual";
  manual.before_ns = 1000000000ULL;
  manual.after_ns = 1000000000ULL;
  manual.priority = 10;
  manual.quota_class = "manual";
  trigger_cfg::SourceRule high;
  high.name = "high";
  high.priority = 100;  // crash class; only the request hint is clamped
  high.quota_class = "safety";
  c.sources = {manual, high};
  c.quota_limits = {{"manual", 0}, {"safety", 0}};
  return c;
}

// One accepted capture is left running by a trigger test; finish it so the
// FSM is IDLE for the next case (mute_ns is 0 in this test config).
void FinishCapture(trigger_center::Center &c) {
  c.Tick();  // start the deferred capture
  g_active.store(false);
  c.Tick();  // detect completion -> IDLE
}

struct ClientOutcome {
  bool transport_ok = false;
  trigger_proto::TriggerCode code =
      trigger_proto::TriggerCode::kRecorderOffline;
  uint64_t trigger_id = 0;
  int32_t effective_priority = 0;
  bool priority_clamped = false;
};

// Fork a real client process; the parent ticks the center until the child
// exits. The child's pid is the peer pid the kernel reports.
ClientOutcome RunForkedClient(trigger_center::Center &c, const char *sock,
                              const char *source, int32_t hint = -1,
                              uint64_t timeout_ns = 2000000000ULL) {
  int fds[2];
  CHECK(pipe(fds) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(fds[0]);
    const trigger_client::Result r =
        trigger_client::Trigger(sock, source, 0, hint, false, timeout_ns);
    const ClientOutcome o{r.transport_ok, r.code, r.trigger_id,
                          r.effective_priority, r.priority_clamped};
    (void)!write(fds[1], &o, sizeof(o));
    _exit(0);
  }
  close(fds[1]);
  int status = 0;
  const uint64_t deadline = NowNs() + timeout_ns + 5000000000ULL;
  while (waitpid(pid, &status, WNOHANG) == 0) {
    c.Tick();
    CHECK(NowNs() < deadline);
    SleepNs(100000);
  }
  ClientOutcome out{};
  const ssize_t got = read(fds[0], &out, sizeof(out));
  close(fds[0]);
  CHECK(got == static_cast<ssize_t>(sizeof(out)));
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  return out;
}

struct RawClient {
  int fd = -1;
  sockaddr_un self{};
  sockaddr_un server{};

  RawClient(const char *sock, const char *self_path) {
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    unlink(self_path);
    self.sun_family = AF_UNIX;
    std::snprintf(self.sun_path, sizeof(self.sun_path), "%s", self_path);
    CHECK(bind(fd, reinterpret_cast<sockaddr *>(&self), sizeof(self)) == 0);
    server.sun_family = AF_UNIX;
    std::snprintf(server.sun_path, sizeof(server.sun_path), "%s", sock);
  }
  ~RawClient() {
    if (fd >= 0) close(fd);
    unlink(self.sun_path);
  }
  void Send(const void *buf, std::size_t n) {
    CHECK(sendto(fd, buf, n, 0, reinterpret_cast<sockaddr *>(&server),
                 sizeof(server)) == static_cast<ssize_t>(n));
  }
  // Non-blocking: returns 0 if nothing is queued.
  ssize_t Recv(void *buf, std::size_t cap) {
    return recv(fd, buf, cap, MSG_DONTWAIT);
  }
};

// Send one datagram and tick until a reply arrives (or the deadline passes).
bool Exchange(trigger_center::Center &c, RawClient &raw, const void *req,
              std::size_t req_size, void *resp, std::size_t resp_cap,
              uint64_t timeout_ns = 1000000000ULL) {
  raw.Send(req, req_size);
  const uint64_t deadline = NowNs() + timeout_ns;
  while (NowNs() < deadline) {
    c.Tick();
    const ssize_t n = raw.Recv(resp, resp_cap);
    if (n > 0)
      return true;
    SleepNs(100000);
  }
  return false;
}

// Tick for a while and assert no reply arrives (auth rejects are silent).
bool NoReplyWhileTicking(trigger_center::Center &c, RawClient &raw, void *buf,
                         std::size_t cap, uint64_t duration_ns = 150000000ULL) {
  const uint64_t deadline = NowNs() + duration_ns;
  while (NowNs() < deadline) {
    c.Tick();
    if (raw.Recv(buf, cap) > 0)
      return false;
    SleepNs(100000);
  }
  return true;
}

std::vector<std::string> ReadLines(const char *path) {
  std::vector<std::string> out;
  std::FILE *f = std::fopen(path, "rb");
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

std::string Field(const std::string &line, const char *key) {
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

// Find the last audit line that satisfies `pred`.
template <typename Pred>
std::string FindLine(const std::vector<std::string> &lines, Pred pred) {
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    if (pred(*it)) return *it;
  }
  return {};
}

void TestConfigSurface() {
  const std::string text =
      "[trigger_center]\nsocket = /tmp/sec_cfg.sock\n"
      "[source.manual]\nbefore_s = 1\nafter_s = 1\npriority = 10\n"
      "quota_class = manual\n"
      "[quota.manual]\nmax_triggers_per_hour = 0\n"
      "[security]\n"
      "allowed_uids = 1000,1001\n"
      "allow_root = true\n"
      "require_peer_pid = false\n"
      "max_priority_hint = 42\n"
      "allow_legacy_unsequenced = true\n"
      "allow_legacy_v2 = true\n"
      "allow_legacy_control = true\n"
      "signing_key = /tmp/sec_cfg.key\n"
      "audit_log = /tmp/sec_cfg.audit\n"
      "control_audit_log = /tmp/sec_cfg.control.audit\n"
      "audit_max_bytes = 8192\n"
      "audit_rotate_keep = 3\n"
      "audit_fsync_interval_s = 0.5\n";
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(trigger_cfg::ParseCenterConfig(text, &cfg, &err));
  CHECK(cfg.allowed_uids.size() == 2);
  CHECK(cfg.allowed_uids[0] == 1000u);
  CHECK(cfg.allowed_uids[1] == 1001u);
  CHECK(cfg.allow_root);
  CHECK(!cfg.require_peer_pid);
  CHECK(cfg.max_priority_hint == 42);
  CHECK(cfg.allow_legacy_unsequenced);
  CHECK(cfg.allow_legacy_v2);
  CHECK(cfg.allow_legacy_control);
  CHECK(cfg.signing_key_path == "/tmp/sec_cfg.key");
  CHECK(cfg.audit_log_path == "/tmp/sec_cfg.audit");
  CHECK(cfg.control_audit_log_path == "/tmp/sec_cfg.control.audit");
  CHECK(cfg.audit_max_bytes == 8192u);
  CHECK(cfg.audit_rotate_keep == 3u);
  CHECK(cfg.audit_fsync_interval_ns == 500000000ULL);

  // Dump materialises every default and parses back to the same values.
  const std::string dump = trigger_cfg::DumpConfig(cfg);
  CHECK(dump.find("[security]") != std::string::npos);
  CHECK(dump.find("max_priority_hint=42") != std::string::npos);
  CHECK(dump.find("allow_legacy_v2=true") != std::string::npos);
  CHECK(dump.find("allow_legacy_control=true") != std::string::npos);
  CHECK(dump.find("control_audit_log=/tmp/sec_cfg.control.audit") !=
        std::string::npos);
  CHECK(dump.find("signing_key=/tmp/sec_cfg.key") != std::string::npos);
  CHECK(dump.find("audit_max_bytes=8192") != std::string::npos);
  CHECK(dump.find("audit_log=/tmp/sec_cfg.audit") != std::string::npos);
  trigger_cfg::CenterConfig round;
  CHECK(trigger_cfg::ParseCenterConfig(dump, &round, &err));
  CHECK(round.allowed_uids == cfg.allowed_uids);
  CHECK(round.allow_root == cfg.allow_root);
  CHECK(round.require_peer_pid == cfg.require_peer_pid);
  CHECK(round.max_priority_hint == cfg.max_priority_hint);
  CHECK(round.allow_legacy_v2 == cfg.allow_legacy_v2);
  CHECK(round.allow_legacy_control == cfg.allow_legacy_control);
  CHECK(round.signing_key_path == cfg.signing_key_path);
  CHECK(round.audit_log_path == cfg.audit_log_path);
  CHECK(round.control_audit_log_path == cfg.control_audit_log_path);
  CHECK(round.audit_max_bytes == cfg.audit_max_bytes);
  CHECK(round.audit_rotate_keep == cfg.audit_rotate_keep);
  CHECK(round.audit_fsync_interval_ns == cfg.audit_fsync_interval_ns);

  // Default-derived audit path is explicit in the dump.
  trigger_cfg::CenterConfig def;
  CHECK(trigger_cfg::ParseCenterConfig(
      "[trigger_center]\nsocket = /tmp/x.sock\n"
      "[source.a]\nchannels = /c\nquota_class = q\n"
      "[quota.q]\nmax_triggers_per_hour = 0\n"
      "[channel./c]\nkind = pinned_slot\n",
      &def, &err));
  const std::string d2 = trigger_cfg::DumpConfig(def);
  CHECK(d2.find("audit_log=/tmp/x.sock.audit") != std::string::npos);
  CHECK(d2.find("control_audit_log=/tmp/thyme_recorder.sock.audit") !=
        std::string::npos);

  // Every key is effective-or-error: typos, bad values, clobber path.
  trigger_cfg::CenterConfig bad;
  CHECK(!trigger_cfg::ParseCenterConfig("[security]\nmax_priority_hint = zz\n",
                                        &bad, &err));
  CHECK(!trigger_cfg::ParseCenterConfig("[security]\nmax_priority_hint = -1\n",
                                        &bad, &err));
  CHECK(!trigger_cfg::ParseCenterConfig("[security]\naudit_max_bytes = 0\n",
                                        &bad, &err));
  CHECK(!trigger_cfg::ParseCenterConfig("[security]\naudit_rotate_keep = 0\n",
                                        &bad, &err));
  CHECK(!trigger_cfg::ParseCenterConfig("[security]\naudit_rotate_keep = 33\n",
                                        &bad, &err));
  CHECK(!trigger_cfg::ParseCenterConfig("[security]\nrequire_peir_pid = true\n",
                                        &bad, &err));
  CHECK(!trigger_cfg::ParseCenterConfig("[security]\nallow_legacy_v2 = maybe\n",
                                        &bad, &err));
  CHECK(!trigger_cfg::ParseCenterConfig(
      "[security]\nallow_legacy_control = maybe\n", &bad, &err));
  CHECK(
      !trigger_cfg::ParseCenterConfig("[trigger_center]\nsocket = /tmp/s.sock\n"
                                      "[security]\naudit_log = /tmp/s.sock\n",
                                      &bad, &err));
  CHECK(!trigger_cfg::ParseCenterConfig(
      "[recorder]\nsocket = /tmp/r.sock\n"
      "[security]\ncontrol_audit_log = /tmp/r.sock\n",
      &bad, &err));
  CHECK(!trigger_cfg::ParseCenterConfig(
      "[trigger_center]\nsocket = /tmp/same.sock\n"
      "[recorder]\nsocket = /tmp/same.sock\n",
      &bad, &err));
}

void TestAuditRotation() {
  char base[96];
  std::snprintf(base, sizeof(base), "/tmp/c9sec_rot_%d",
                static_cast<int>(getpid()));
  const std::string path = std::string(base) + ".log";
  unlink(path.c_str());
  unlink((path + ".1").c_str());
  unlink((path + ".2").c_str());

  trigger_audit::Config ac;
  ac.path = path;
  ac.max_bytes = 600;
  ac.rotate_keep = 2;
  trigger_audit::Log log;
  CHECK(log.Open(ac));
  trigger_audit::Record rec;
  rec.source = "manual";
  rec.peer_uid = geteuid();
  rec.peer_pid = getpid();
  rec.proto_version = trigger_proto::kTriggerVersionV3;
  rec.sequence = 1;
  rec.hint_raw = 80;
  rec.hint_effective = 50;
  rec.priority_clamped = true;
  rec.accepted = true;
  rec.reason = "ok";
  for (int i = 0; i < 40; ++i) {
    rec.sequence = static_cast<uint64_t>(i + 1);
    CHECK(log.Append(rec));
  }
  CHECK(log.records() == 40);
  CHECK(log.drops() == 0);
  CHECK(log.rotations() >= 1);
  log.Close();

  struct stat st {};
  CHECK(stat(path.c_str(), &st) == 0);
  CHECK(static_cast<uint64_t>(st.st_size) <= 600u);
  CHECK(stat((path + ".1").c_str(), &st) == 0);
  const std::vector<std::string> lines = ReadLines(path.c_str());
  CHECK(!lines.empty());
  CHECK(Field(lines.back(), "decision") == "accepted");
  unlink(path.c_str());
  unlink((path + ".1").c_str());
  unlink((path + ".2").c_str());
}

}  // namespace

int main() {
  TestConfigSurface();
  TestAuditRotation();

  using trigger_proto::TriggerCode;

  char sock[96];
  char audit[96];
  char self[96];
  std::snprintf(sock, sizeof(sock), "/tmp/c9sec_%d.sock",
                static_cast<int>(getpid()));
  std::snprintf(audit, sizeof(audit), "/tmp/c9sec_%d.audit",
                static_cast<int>(getpid()));
  std::snprintf(self, sizeof(self), "/tmp/c9sec_%d.cli",
                static_cast<int>(getpid()));
  unlink(sock);
  unlink(audit);
  unlink((std::string(audit) + ".1").c_str());

  trigger_cfg::CenterConfig cfg = MakeCfg(sock, audit);
  trigger_center::Center center;
  CHECK(center.Init(cfg, Hooks()));
  RawClient raw(sock, self);

  // ---- allowed uid: real fork + kernel credentials ----------------------
  {
    const ClientOutcome out = RunForkedClient(center, sock, "manual");
    CHECK(out.transport_ok);
    CHECK(out.code == TriggerCode::kAccepted);
    CHECK(out.trigger_id == 1);
    CHECK(out.effective_priority == 10);
    CHECK(!out.priority_clamped);
    FinishCapture(center);
  }

  // ---- priority_hint clamp visible in response + audit ------------------
  {
    const ClientOutcome out = RunForkedClient(center, sock, "high", 80);
    CHECK(out.transport_ok);
    CHECK(out.code == TriggerCode::kAccepted);
    CHECK(out.effective_priority == 50);  // max_priority_hint, not 80/100
    CHECK(out.priority_clamped);
    CHECK(center.stats().priority_clamps >= 1);
    const std::vector<std::string> lines = ReadLines(audit);
    const std::string line = FindLine(lines, [](const std::string &l) {
      return Field(l, "src") == "high" && Field(l, "clamp") == "1";
    });
    CHECK(!line.empty());
    CHECK(Field(line, "hint_raw") == "80");
    CHECK(Field(line, "hint_eff") == "50");
    CHECK(Field(line, "decision") == "accepted");
    FinishCapture(center);
  }

  // ---- duplicate / old sequence rejected --------------------------------
  {
    trigger_proto::TriggerRequest req;
    req.magic = trigger_proto::kTriggerMagic;
    req.version = trigger_proto::kTriggerVersionV3;
    req.priority_hint = -1;
    req.sequence = trigger_client::NextSequence();
    std::snprintf(req.source, sizeof(req.source), "manual");
    const uint64_t first_seq = req.sequence;
    trigger_proto::StampRequestCrc(&req);

    trigger_proto::TriggerResponse resp{};
    CHECK(Exchange(center, raw, &req, sizeof(req), &resp, sizeof(resp)));
    CHECK(resp.version == trigger_proto::kTriggerVersionV3);
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kAccepted));
    FinishCapture(center);

    const uint64_t before = center.stats().replay_rejects;
    CHECK(Exchange(center, raw, &req, sizeof(req), &resp, sizeof(resp)));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kReplayRejected));
    CHECK(center.stats().replay_rejects == before + 1);

    // Advance the window, then replay the old sequence: too old.
    trigger_proto::TriggerRequest far = req;
    far.sequence = first_seq + 1000;
    far.source[0] = 'm';
    std::snprintf(far.source, sizeof(far.source), "manual");
    trigger_proto::StampRequestCrc(&far);
    CHECK(Exchange(center, raw, &far, sizeof(far), &resp, sizeof(resp)));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kAccepted));
    FinishCapture(center);
    CHECK(Exchange(center, raw, &req, sizeof(req), &resp, sizeof(resp)));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kReplayRejected));

    const std::vector<std::string> lines = ReadLines(audit);
    const std::string line = FindLine(lines, [](const std::string &l) {
      return Field(l, "reason") == "replay_duplicate";
    });
    CHECK(!line.empty());
    CHECK(Field(line, "decision") == "rejected");
  }

  // ---- malformed / oversized datagram: counted, no reply, no crash ------
  {
    const uint64_t bad_before = center.stats().malformed_datagrams;
    uint8_t garbage[512];
    std::memset(garbage, 0xA5, sizeof(garbage));
    raw.Send(garbage, sizeof(garbage));  // oversized
    CHECK(NoReplyWhileTicking(center, raw, garbage, sizeof(garbage)));
    CHECK(center.stats().malformed_datagrams == bad_before + 1);

    trigger_proto::TriggerRequest bad{};
    bad.magic = 0xDEADBEEF;
    bad.version = trigger_proto::kTriggerVersionV3;
    bad.sequence = 1;
    std::snprintf(bad.source, sizeof(bad.source), "manual");
    trigger_proto::StampRequestCrc(&bad);
    raw.Send(&bad, sizeof(bad));
    CHECK(NoReplyWhileTicking(center, raw, garbage, sizeof(garbage)));
    CHECK(center.stats().malformed_datagrams == bad_before + 2);

    const std::vector<std::string> lines = ReadLines(audit);
    const std::string line = FindLine(lines, [](const std::string &l) {
      return Field(l, "reason") == "malformed_size";
    });
    CHECK(!line.empty());
    CHECK(Field(line, "decision") == "rejected");
    // The center is still alive and serving.
    CHECK(center.state() == trigger_center::State::kIdle);
  }

  // ---- SEC-04: V3 CRC32 (good accepted, bad rejected before sequence) ---
  {
    trigger_proto::TriggerRequest req;
    req.magic = trigger_proto::kTriggerMagic;
    req.version = trigger_proto::kTriggerVersionV3;
    req.priority_hint = -1;
    // The duplicate-window test above deliberately advanced the peer's max
    // sequence by +1000; start well above it so this request is not "too old".
    req.sequence = trigger_client::NextSequence() + 5000;
    req.response_timeout_ms = 100;
    std::snprintf(req.source, sizeof(req.source), "manual");
    trigger_proto::StampRequestCrc(&req);

    trigger_proto::TriggerResponse resp{};
    CHECK(Exchange(center, raw, &req, sizeof(req), &resp, sizeof(resp)));
    CHECK(resp.version == trigger_proto::kTriggerVersionV3);
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kAccepted));
    CHECK(resp.crc32 == trigger_proto::ResponseCrc(resp));
    FinishCapture(center);

    // A corrupted CRC is rejected as malformed (no reply) and — crucially —
    // never reaches the sequence/replay window: re-sending the same sequence
    // with a valid CRC is accepted afterwards.
    const uint64_t crc_before = center.stats().crc_rejects;
    const uint64_t mal_before = center.stats().malformed_datagrams;
    trigger_proto::TriggerRequest bad = req;
    bad.sequence = req.sequence + 1;
    bad.crc32 ^= 0x00000001u;
    raw.Send(&bad, sizeof(bad));
    uint8_t sink[256];
    CHECK(NoReplyWhileTicking(center, raw, sink, sizeof(sink)));
    CHECK(center.stats().crc_rejects == crc_before + 1);
    CHECK(center.stats().malformed_datagrams == mal_before + 1);
    trigger_proto::StampRequestCrc(&bad);
    CHECK(Exchange(center, raw, &bad, sizeof(bad), &resp, sizeof(resp)));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kAccepted));
    FinishCapture(center);

    const std::vector<std::string> lines = ReadLines(audit);
    const std::string line = FindLine(lines, [](const std::string &l) {
      return Field(l, "reason") == "bad_crc";
    });
    CHECK(!line.empty());
    CHECK(Field(line, "decision") == "rejected");
    CHECK(Field(line, "code") == "malformed_request");
  }

  // ---- SEC-04: V2 without CRC rejected unless allow_legacy_v2 -----------
  {
    trigger_proto::TriggerRequestV2 v2;
    v2.magic = trigger_proto::kTriggerMagic;
    v2.version = trigger_proto::kTriggerVersionV2;
    v2.priority_hint = -1;
    v2.sequence = trigger_client::NextSequence() + 6000;
    std::snprintf(v2.source, sizeof(v2.source), "manual");
    trigger_proto::TriggerResponse resp{};
    CHECK(Exchange(center, raw, &v2, sizeof(v2), &resp, sizeof(resp)));
    CHECK(resp.version == trigger_proto::kTriggerVersionV2);
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kUnsupportedVersion));

    trigger_cfg::CenterConfig compat = cfg;
    compat.allow_legacy_v2 = true;
    center.ReloadConfig(compat);
    const uint64_t accepted_before = center.stats().legacy_v2_requests;
    v2.sequence += 1;
    CHECK(Exchange(center, raw, &v2, sizeof(v2), &resp, sizeof(resp)));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kAccepted));
    CHECK(center.stats().legacy_v2_requests == accepted_before + 1);
    FinishCapture(center);
    center.ReloadConfig(cfg);
  }

  // ---- V1 legacy request: fail closed, then explicit opt-in -------------
  {
    trigger_proto::TriggerRequestV1 v1;
    v1.magic = trigger_proto::kTriggerMagic;
    v1.version = trigger_proto::kTriggerVersionLegacy;
    v1.priority_hint = -1;
    std::snprintf(v1.source, sizeof(v1.source), "manual");
    trigger_proto::TriggerResponse resp{};
    CHECK(Exchange(center, raw, &v1, sizeof(v1), &resp, sizeof(resp)));
    CHECK(resp.version == trigger_proto::kTriggerVersionLegacy);
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kUnsupportedVersion));

    trigger_cfg::CenterConfig legacy_cfg = cfg;
    legacy_cfg.allow_legacy_unsequenced = true;
    center.ReloadConfig(legacy_cfg);
    CHECK(Exchange(center, raw, &v1, sizeof(v1), &resp, sizeof(resp)));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kAccepted));
    CHECK(center.stats().legacy_requests == 1);
    FinishCapture(center);
    center.ReloadConfig(cfg);
  }

  // ---- non-allowlisted uid rejected: config allowlist + forked client ---
  {
    trigger_cfg::CenterConfig deny = cfg;
    deny.allowed_uids = {static_cast<uint32_t>(geteuid()) + 1u};
    center.ReloadConfig(deny);  // allowlist is hot-reloadable

    const uint64_t before = center.stats().auth_rejects;
    const ClientOutcome out =
        RunForkedClient(center, sock, "manual", -1, 150000000ULL);
    CHECK(!out.transport_ok);  // silent rejection: no reply at all
    CHECK(center.stats().auth_rejects == before + trigger_client::kMaxAttempts);

    const std::vector<std::string> lines = ReadLines(audit);
    const std::string line = FindLine(lines, [](const std::string &l) {
      return Field(l, "reason") == "auth_uid_not_allowed";
    });
    CHECK(!line.empty());
    CHECK(Field(line, "decision") == "rejected");
    CHECK(Field(line, "uid") == std::to_string(geteuid()));

    // Restore the default and confirm the same path works again.
    center.ReloadConfig(cfg);
    const ClientOutcome ok = RunForkedClient(center, sock, "manual");
    CHECK(ok.transport_ok);
    CHECK(ok.code == TriggerCode::kAccepted);
    FinishCapture(center);
  }

  // ---- TRG-19: V4 CRC + bounded request_id dedup -------------------------
  {
    trigger_proto::TriggerRequestV4 req;
    req.magic = trigger_proto::kTriggerMagic;
    req.version = trigger_proto::kTriggerVersion;
    req.priority_hint = -1;
    req.sequence = trigger_client::NextSequence() + 10000;
    req.request_id = 0x5452473139000000ull + static_cast<uint64_t>(getpid());
    req.response_timeout_ms = 100;
    std::snprintf(req.source, sizeof(req.source), "manual");
    trigger_proto::StampRequestCrc(&req);

    trigger_proto::TriggerResponseV4 first{};
    CHECK(Exchange(center, raw, &req, sizeof(req), &first, sizeof(first)));
    CHECK(first.version == trigger_proto::kTriggerVersion);
    CHECK(first.code == static_cast<uint16_t>(TriggerCode::kAccepted));
    CHECK(first.request_id == req.request_id);
    CHECK(first.crc32 == trigger_proto::ResponseCrcV4(first));
    CHECK((first.flags & trigger_proto::kRespFlagDeduplicated) == 0);
    const uint64_t accepted = center.stats().accepted;

    trigger_proto::TriggerResponseV4 duplicate{};
    CHECK(Exchange(center, raw, &req, sizeof(req), &duplicate,
                   sizeof(duplicate)));
    CHECK(duplicate.code == first.code);
    CHECK(duplicate.trigger_id == first.trigger_id);
    CHECK((duplicate.flags & trigger_proto::kRespFlagDeduplicated) != 0);
    CHECK(center.stats().accepted == accepted);
    CHECK(center.stats().dedup_hits == 1);
    CHECK(center.stats().retry_requests == 1);
    FinishCapture(center);

    // A corrupted retry never touches the dedup/replay windows; the valid
    // retry still returns the original result and does not execute again.
    trigger_proto::TriggerRequestV4 corrupt = req;
    corrupt.crc32 ^= 1u;
    raw.Send(&corrupt, sizeof(corrupt));
    uint8_t sink[256];
    CHECK(NoReplyWhileTicking(center, raw, sink, sizeof(sink)));
    CHECK(Exchange(center, raw, &req, sizeof(req), &duplicate,
                   sizeof(duplicate)));
    CHECK((duplicate.flags & trigger_proto::kRespFlagDeduplicated) != 0);
    CHECK(center.stats().accepted == accepted);

    // Reusing an identity for different bytes is a conflict, never a cache hit.
    trigger_proto::TriggerRequestV4 conflict = req;
    conflict.priority_hint = 1;
    trigger_proto::StampRequestCrc(&conflict);
    CHECK(Exchange(center, raw, &conflict, sizeof(conflict), &duplicate,
                   sizeof(duplicate)));
    CHECK(duplicate.code ==
          static_cast<uint16_t>(TriggerCode::kReplayRejected));
    CHECK(center.stats().dedup_conflicts == 1);
    CHECK(center.stats().accepted == accepted);

    const std::vector<std::string> lines = ReadLines(audit);
    const std::string line = FindLine(lines, [](const std::string &l) {
      return Field(l, "reason") == "dedup_hit";
    });
    CHECK(!line.empty());
    CHECK(Field(line, "decision") == "deduplicated");
    CHECK(Field(line, "proto") == "4");
    CHECK(Field(line, "request_id") == std::to_string(req.request_id));
    CHECK(Field(line, "retry") == "1");
    CHECK(Field(line, "dedup") == "1");
  }

  // ---- SEC-04: client deadline against a bound but unserviced socket ----
  {
    char dead[96];
    std::snprintf(dead, sizeof(dead), "/tmp/c9sec_dead_%d.sock",
                  static_cast<int>(getpid()));
    unlink(dead);
    const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", dead);
    CHECK(bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
    const uint64_t t0 = NowNs();
    const trigger_client::Result tr =
        trigger_client::Trigger(dead, "manual", 0, -1, false, 80000000ULL);
    const uint64_t elapsed = NowNs() - t0;
    CHECK(!tr.transport_ok);
    CHECK(tr.code == TriggerCode::kTransportTimeout);
    CHECK(elapsed >= 60000000ULL);   // it actually waited
    CHECK(elapsed < 2000000000ULL);  // but stayed bounded
    close(fd);
    unlink(dead);
  }

  // ---- require_peer_pid: direct handler path ----------------------------
  {
    trigger_proto::TriggerRequest req;
    req.magic = trigger_proto::kTriggerMagic;
    req.version = trigger_proto::kTriggerVersionV3;
    req.sequence = trigger_client::NextSequence();
    std::snprintf(req.source, sizeof(req.source), "manual");
    trigger_proto::StampRequestCrc(&req);
    trigger_proto::TriggerResponse resp{};
    trigger_center::PeerCred no_pid;
    no_pid.uid = geteuid();
    no_pid.pid = 0;
    CHECK(!center.ProcessRequest(reinterpret_cast<const uint8_t *>(&req),
                                 sizeof(req), no_pid, &resp));
    CHECK(center.stats().audit_drops == 0);
  }

  // ---- audit records: accepted line has every documented field ----------
  {
    const std::vector<std::string> lines = ReadLines(audit);
    const std::string line = FindLine(lines, [](const std::string &l) {
      return Field(l, "decision") == "accepted" && Field(l, "src") == "manual" &&
             Field(l, "proto") == "3";
    });
    CHECK(!line.empty());
    CHECK(!Field(line, "ts").empty());
    CHECK(!Field(line, "ts_ns").empty());
    CHECK(!Field(line, "mono_ns").empty());
    CHECK(!Field(line, "pid").empty());
    CHECK(Field(line, "uid") == std::to_string(geteuid()));
    CHECK(Field(line, "req") == "trigger");
    CHECK(Field(line, "proto") == "3");
    CHECK(!Field(line, "seq").empty());
    CHECK(Field(line, "hint_eff") == "10");
    CHECK(Field(line, "code") == "accepted");
    CHECK(!Field(line, "trigger_id").empty());
  }

  center.Shutdown();

  // Count and time bounds are fail-safe: once an entry leaves the window the
  // old anti-replay window still prevents a second execution.
  {
    trigger_cfg::CenterConfig bounded_cfg = cfg;
    bounded_cfg.dedup_window_entries = 1;
    bounded_cfg.dedup_window_ns = 1000000ull;
    trigger_center::Center bounded;
    CHECK(bounded.Init(bounded_cfg, Hooks()));
    trigger_center::PeerCred peer;
    peer.uid = geteuid();
    peer.pid = getpid();
    uint64_t sequence = trigger_client::NextSequence() + 20000;
    const auto make_request = [&sequence](uint64_t request_id) {
      trigger_proto::TriggerRequestV4 req;
      req.magic = trigger_proto::kTriggerMagic;
      req.version = trigger_proto::kTriggerVersion;
      req.priority_hint = -1;
      req.sequence = ++sequence;
      req.request_id = request_id;
      std::snprintf(req.source, sizeof(req.source), "manual");
      trigger_proto::StampRequestCrc(&req);
      return req;
    };
    const trigger_proto::TriggerRequestV4 first = make_request(101);
    const trigger_proto::TriggerRequestV4 second = make_request(102);
    trigger_proto::TriggerResponseV4 resp{};
    CHECK(bounded.ProcessRequest(reinterpret_cast<const uint8_t *>(&first),
                                 sizeof(first), peer, &resp));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kAccepted));
    FinishCapture(bounded);
    CHECK(bounded.ProcessRequest(reinterpret_cast<const uint8_t *>(&second),
                                 sizeof(second), peer, &resp));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kAccepted));
    FinishCapture(bounded);
    const uint64_t accepted = bounded.stats().accepted;
    CHECK(bounded.ProcessRequest(reinterpret_cast<const uint8_t *>(&first),
                                 sizeof(first), peer, &resp));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kReplayRejected));
    SleepNs(2000000ull);
    CHECK(bounded.ProcessRequest(reinterpret_cast<const uint8_t *>(&second),
                                 sizeof(second), peer, &resp));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kReplayRejected));
    CHECK(bounded.stats().accepted == accepted);
    CHECK(bounded.stats().dedup_hits == 0);
    bounded.Shutdown();
  }
  unlink(sock);
  unlink(audit);
  unlink((std::string(audit) + ".1").c_str());
  return 0;
}
