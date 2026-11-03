// test_trigger_audit.cpp — TRG-12: structured audit records for every
// accept / reject (with reason) / preempt / quota-override decision.
//
// Covers:
//   * accepted and rejected trigger records carry every documented field,
//     including kernel-style attribution (pid/uid) and the source name;
//   * a crash-level accept that overrode the count quota is marked
//     quota_override=1 (and the matching CenterStats counter moved);
//   * an executed preemption emits a `req=preempt decision=preempted` record
//     naming the preempted trigger_id, the preempting trigger (`by=`), and
//     the preempting request's source/uid/pid;
//   * crash safety: a SIGKILLed child loses nothing it appended (O_APPEND +
//     one unbuffered write() per record, no userspace buffer);
//   * drop accounting: an unwritable/failing sink counts drops() instead of
//     blocking or dying, and the center exposes them via stats().audit_drops.
#include "thyme/check.h"
#include "thyme/record/trigger_audit.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_config.h"
#include "thyme/record/trigger_protocol.h"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace thyme;
using trigger_center::Center;

namespace {

struct FakeRec {
  bool ready = true;
  bool active = false;
};

FakeRec g;

bool HookReady(void*) { return g.ready; }
bool HookActive(void*) { return g.active; }
bool HookStart(void*, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string>&) {
  g.active = true;
  return true;
}
bool HookPreempt(void*) {
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

trigger_cfg::SourceRule Rule(const char* name, int32_t prio,
                             const char* quota_class) {
  trigger_cfg::SourceRule r;
  r.name = name;
  r.before_ns = 1000000ull;
  r.after_ns = 1000000ull;
  r.priority = prio;
  r.quota_class = quota_class;
  return r;
}

trigger_cfg::CenterConfig MakeCfg(const char* tag, uint32_t manual_quota) {
  trigger_cfg::CenterConfig c;
  static char sock[96];
  std::snprintf(sock, sizeof(sock), "/tmp/c9audit_%s_%d.sock", tag,
                static_cast<int>(getpid()));
  c.socket_path = sock;  // audit path derives to <socket>.audit
  // Both sources share the "man" quota class so the crash-level source can
  // exercise the count-quota override path (limit > 0 is required).
  c.sources = {Rule("manual", 10, "man"), Rule("high", 100, "man")};
  c.quota_limits = {{"man", manual_quota}};
  return c;
}

std::vector<std::string> ReadLines(const char* path) {
  std::vector<std::string> out;
  std::FILE* f = std::fopen(path, "rb");
  CHECK(f != nullptr);
  char buf[1024];
  while (std::fgets(buf, sizeof(buf), f) != nullptr) {
    std::string line(buf);
    if (!line.empty() && line.back() == '\n')
      line.pop_back();
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

// Find the last audit line that satisfies `pred`.
template <typename Pred>
std::string FindLine(const std::vector<std::string>& lines, Pred pred) {
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    if (pred(*it)) return *it;
  }
  return {};
}

uint64_t g_seq = 1000;

// One V3 request through the direct ProcessRequest path with explicit peer
// credentials (the same values the kernel would stamp via SCM_CREDENTIALS).
trigger_proto::TriggerCode Send(Center& c, const char* source,
                                int32_t hint = -1) {
  trigger_proto::TriggerRequest req;
  req.magic = trigger_proto::kTriggerMagic;
  req.version = trigger_proto::kTriggerVersionV3;
  req.priority_hint = hint;
  req.sequence = ++g_seq;
  std::snprintf(req.source, sizeof(req.source), "%s", source);
  trigger_proto::StampRequestCrc(&req);
  trigger_proto::TriggerResponse resp{};
  trigger_center::PeerCred peer;
  peer.uid = geteuid();
  peer.pid = getpid();
  CHECK(c.ProcessRequest(reinterpret_cast<const uint8_t*>(&req), sizeof(req),
                         peer, &resp));
  return static_cast<trigger_proto::TriggerCode>(resp.code);
}

void FinishCapture(Center& c) {
  c.Tick();  // start the deferred capture
  g.active = false;
  c.Tick();  // detect completion -> IDLE (mute_ns = 0 here)
}

void CheckCommonFields(const std::string& line) {
  CHECK(!Field(line, "ts").empty());
  CHECK(Field(line, "ts").size() == 30);  // ISO-8601 with 9 fractional digits
  CHECK(!Field(line, "ts_ns").empty());
  CHECK(!Field(line, "mono_ns").empty());
  CHECK(Field(line, "pid") == std::to_string(getpid()));
  CHECK(Field(line, "uid") == std::to_string(geteuid()));
  CHECK(Field(line, "proto") == "3");
  CHECK(!Field(line, "seq").empty());
  CHECK(!Field(line, "hint_raw").empty());
  CHECK(!Field(line, "hint_eff").empty());
  CHECK(!Field(line, "clamp").empty());
  CHECK(!Field(line, "trigger_id").empty());
  CHECK(!Field(line, "quota_override").empty());
  CHECK(!Field(line, "by").empty());
}

// Accept, reject-with-reason and quota-override records.
void TestAcceptRejectQuotaOverride() {
  g = FakeRec{};
  trigger_cfg::CenterConfig cfg = MakeCfg("quota", 1);
  const std::string audit = cfg.socket_path + ".audit";
  unlink(audit.c_str());
  Center c;
  CHECK(c.Init(cfg, MakeHooks()));

  using trigger_proto::TriggerCode;
  CHECK(Send(c, "manual") == TriggerCode::kAccepted);
  CHECK(c.current_trigger_id() == 1);

  // Accepted record: every documented field present, correct attribution.
  {
    const std::vector<std::string> lines = ReadLines(audit.c_str());
    CHECK(lines.size() == 1);
    const std::string& line = lines.back();
    CheckCommonFields(line);
    CHECK(Field(line, "req") == "trigger");
    CHECK(Field(line, "src") == "manual");
    CHECK(Field(line, "decision") == "accepted");
    CHECK(Field(line, "reason") == "ok");
    CHECK(Field(line, "code") == "accepted");
    CHECK(Field(line, "trigger_id") == "1");
    CHECK(Field(line, "hint_eff") == "10");
    CHECK(Field(line, "quota_override") == "0");
    CHECK(Field(line, "by") == "0");
  }
  FinishCapture(c);

  // Quota window holds one admission: the same class is rejected with reason.
  CHECK(Send(c, "manual") == TriggerCode::kQuotaExceeded);
  {
    const std::vector<std::string> lines = ReadLines(audit.c_str());
    CHECK(lines.size() == 2);
    const std::string& line = lines.back();
    CheckCommonFields(line);
    CHECK(Field(line, "src") == "manual");
    CHECK(Field(line, "decision") == "rejected");
    CHECK(Field(line, "reason") == "quota_exceeded");
    CHECK(Field(line, "code") == "quota_exceeded");
    CHECK(Field(line, "quota_override") == "0");
  }

  // Crash level (high priority 100 >= crash_priority 100) overrides the
  // quota: accepted, counter moved, audit line marked.
  CHECK(Send(c, "high") == TriggerCode::kAccepted);
  CHECK(c.stats().quota_overridden == 1);
  {
    const std::vector<std::string> lines = ReadLines(audit.c_str());
    CHECK(lines.size() == 3);
    const std::string& line = lines.back();
    CheckCommonFields(line);
    CHECK(Field(line, "src") == "high");
    CHECK(Field(line, "decision") == "accepted");
    CHECK(Field(line, "reason") == "ok");
    CHECK(Field(line, "quota_override") == "1");
    CHECK(Field(line, "trigger_id") == "2");
  }
  FinishCapture(c);

  c.Shutdown();
  unlink(cfg.socket_path.c_str());
  unlink(audit.c_str());
}

// An executed preemption is persisted as its own audit record.
void TestPreemptRecord() {
  g = FakeRec{};
  trigger_cfg::CenterConfig cfg = MakeCfg("preempt", 0);
  const std::string audit = cfg.socket_path + ".audit";
  unlink(audit.c_str());
  Center c;
  CHECK(c.Init(cfg, MakeHooks()));

  using trigger_proto::TriggerCode;
  CHECK(Send(c, "manual") == TriggerCode::kAccepted);
  c.Tick();  // start; g.active stays true (capture running)
  CHECK(g.active);

  // Higher priority preempts: accepted immediately, executed at the next
  // Tick (T2 async).
  CHECK(Send(c, "high") == TriggerCode::kAccepted);
  CHECK(c.current_trigger_id() == 2);
  c.Tick();  // preempt executes: old capture truncated+sealed
  CHECK(c.stats().preemptions == 1);
  CHECK(g.active);  // the new capture is running

  const std::vector<std::string> lines = ReadLines(audit.c_str());
  CHECK(lines.size() == 3);  // accept(1), accept(2), preempt
  const std::string& line = lines.back();
  CheckCommonFields(line);
  CHECK(Field(line, "req") == "preempt");
  CHECK(Field(line, "decision") == "preempted");
  CHECK(Field(line, "reason") == "preempted_by_higher");
  CHECK(Field(line, "code") == "preempted_by_higher");
  CHECK(Field(line, "trigger_id") == "1");   // the preempted capture
  CHECK(Field(line, "by") == "2");           // the preempting trigger
  CHECK(Field(line, "src") == "high");       // preempting request's source
  CHECK(Field(line, "hint_eff") == "100");   // preempting priority
  CHECK(Field(line, "quota_override") == "0");

  FinishCapture(c);
  c.Shutdown();
  unlink(cfg.socket_path.c_str());
  unlink(audit.c_str());
}

void TestManifestDowngradeAudit() {
  g = FakeRec{};
  trigger_cfg::CenterConfig cfg = MakeCfg("manifest", 0);
  cfg.manifest_mode = integrity::ManifestMode::kMetadata;
  triggered::SourceConfig bulk;
  bulk.kind = triggered::SourceConfig::Kind::kPinnedSlot;
  bulk.shm_name = "/cam";
  bulk.channel_name = "cam";
  bulk.manifest = triggered::SourceConfig::ManifestPolicy::kOff;
  cfg.recorder_sources = {bulk};
  const std::string audit = cfg.socket_path + ".audit";
  unlink(audit.c_str());
  Center center;
  CHECK(center.Init(cfg, MakeHooks()));
  const std::vector<std::string> lines = ReadLines(audit.c_str());
  CHECK(lines.size() == 2);
  CHECK(Field(lines[0], "req") == "config");
  CHECK(Field(lines[0], "src") == "security");
  CHECK(Field(lines[0], "decision") == "configured");
  CHECK(Field(lines[0], "reason") == "integrity_downgrade");
  CHECK(Field(lines[0], "integrity_mode") == "metadata");
  CHECK(Field(lines[1], "req") == "config");
  CHECK(Field(lines[1], "src") == "cam");
  CHECK(Field(lines[1], "integrity_mode") == "off");
  center.Shutdown();
  unlink(cfg.socket_path.c_str());
  unlink(audit.c_str());
}

// Crash safety: a SIGKILLed process loses nothing it appended — every record
// is one unbuffered write() to an O_APPEND file (no userspace buffer, no
// watermark), so the loss window is at most the in-flight record itself.
void TestCrashSafety() {
  char path[96];
  std::snprintf(path, sizeof(path), "/tmp/c9audit_crash_%d.log",
                static_cast<int>(getpid()));
  unlink(path);

  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    trigger_audit::Config ac;
    ac.path = path;
    trigger_audit::Log log;
    CHECK(log.Open(ac));
    for (uint64_t i = 1; i <= 5; ++i) {
      trigger_audit::Record rec;
      rec.peer_uid = geteuid();
      rec.peer_pid = getpid();
      rec.proto_version = trigger_proto::kTriggerVersionV3;
      rec.sequence = i;
      rec.source = "manual";
      rec.accepted = true;
      rec.reason = "ok";
      rec.trigger_id = i;
      CHECK(log.Append(rec));
    }
    // No Close, no fdatasync: the crash must not lose what Append returned.
    raise(SIGKILL);
    _exit(1);
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);

  const std::vector<std::string> lines = ReadLines(path);
  CHECK(lines.size() == 5);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    CHECK(Field(lines[i], "decision") == "accepted");
    CHECK(Field(lines[i], "trigger_id") == std::to_string(i + 1));
    CHECK(Field(lines[i], "quota_override") == "0");
    CHECK(Field(lines[i], "by") == "0");
  }
  unlink(path);
}

// Drop accounting: a failed Append returns false and counts, never blocks or
// crashes; the sink going away must not lose the count.
void TestDropCounter() {
  // Open failure (parent directory does not exist): the log stays disabled
  // and Append counts drops.
  {
    trigger_audit::Config ac;
    ac.path = "/tmp/c9audit_no_such_dir_xyz/x.log";
    trigger_audit::Log log;
    CHECK(!log.Open(ac));
    trigger_audit::Record rec;
    CHECK(!log.Append(rec));
    CHECK(log.drops() == 1);
    CHECK(log.records() == 0);
  }
  // Write failure on an opened sink (/dev/full always fails with ENOSPC).
  if (access("/dev/full", W_OK) == 0) {
    trigger_audit::Config ac;
    ac.path = "/dev/full";
    trigger_audit::Log log;
    CHECK(log.Open(ac));
    trigger_audit::Record rec;
    rec.source = "manual";
    rec.accepted = true;
    CHECK(!log.Append(rec));
    CHECK(log.drops() == 1);
    CHECK(log.records() == 0);
    log.Close();
  }
}

}  // namespace

int main() {
  TestAcceptRejectQuotaOverride();
  TestPreemptRecord();
  TestManifestDowngradeAudit();
  TestCrashSafety();
  TestDropCounter();
  return 0;
}
