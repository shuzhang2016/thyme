// test_trigger_protocol.cpp — trigger socket protocol (design 5.1): real
// client<->server datagram roundtrips, auth rejection, malformed datagrams,
// client-side timeout, plus the INI config parser (5.2/7 mapping).
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

using namespace thyme;

namespace {

// Fake recorder hooks.
std::atomic<bool> g_active{false};
bool HookReady(void*) { return true; }
bool HookActive(void*) { return g_active.load(); }
bool HookStart(void*, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string>&) {
  g_active.store(true);
  return true;
}
bool HookPreempt(void*) {
  g_active.store(false);
  return true;
}

struct ClientJob {
  const char* socket;
  const char* source;
  int32_t prio;
  bool force;
  trigger_client::Result res;
  std::atomic<bool> done{false};
};

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

void* ClientRun(void* p) {
  auto* j = static_cast<ClientJob*>(p);
  j->res = trigger_client::Trigger(j->socket, j->source, 0, j->prio, j->force);
  j->done.store(true, std::memory_order_release);
  return nullptr;
}

// Run a client call on a thread while ticking the center until it completes.
trigger_client::Result CallWhileTicking(trigger_center::Center& center,
                                        const char* sock, const char* source,
                                        int32_t prio = -1, bool force = false) {
  ClientJob job{sock, source, prio, force, {}, {}};
  pthread_t th{};
  CHECK(pthread_create(&th, nullptr, ClientRun, &job) == 0);
  const uint64_t deadline = NowNs() + 5000000000ULL;
  while (!job.done.load(std::memory_order_acquire)) {
    center.Tick();
    CHECK(NowNs() < deadline);
    struct timespec ts {
      0, 100000
    };
    nanosleep(&ts, nullptr);
  }
  CHECK(pthread_join(th, nullptr) == 0);
  return job.res;
}

trigger_cfg::CenterConfig ProtoCfg(const char* sock) {
  trigger_cfg::CenterConfig c;
  c.socket_path = sock;
  trigger_cfg::SourceRule crash;
  crash.name = "crash";
  crash.before_ns = 10000000000ULL;
  crash.after_ns = 5000000000ULL;
  crash.priority = 100;
  crash.quota_class = "safety";
  crash.flush_realtime = true;
  trigger_cfg::SourceRule manual;
  manual.name = "manual";
  manual.before_ns = 10000000000ULL;
  manual.after_ns = 5000000000ULL;
  manual.priority = 10;
  manual.mute_ns = 50000000ULL;  // 50 ms
  manual.quota_class = "manual";
  c.sources = {crash, manual};
  c.quota_limits = {{"safety", 0}, {"manual", 0}};
  return c;
}

void TestConfigParser() {
  const std::string text = R"(
# trigger_center config (matches the chapter 7 design yaml schema)
[trigger_center]
socket = /tmp/thyme_trigger.sock
max_concurrent_captures = 1
default_source = manual
crash_priority = 100

[recorder]
output_dir = /opt/usr/triggered_data
pending_bytes_quota = 67108864

[channel./h264_front]
kind = pinned_slot
name = h264_front

[channel./thyme_recr_100]
kind = mirror_ring

[source.crash]
channels = /h264_*, /thyme_recr_*
before_s = 10
after_s = 5
priority = 100
mute_s = 0
flush_policy = realtime
quota_class = safety

[source.manual]
channels = /h264_*
before_s = 2.5
after_s = 0.5
priority = 10
mute_s = 30
quota_class = manual

[quota.safety]
max_triggers_per_hour = 0
[quota.manual]
max_triggers_per_hour = 30
)";
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(trigger_cfg::ParseCenterConfig(text, &cfg, &err));
  CHECK(cfg.socket_path == "/tmp/thyme_trigger.sock");
  CHECK(cfg.max_concurrent_captures == 1);
  CHECK(cfg.default_source == "manual");
  CHECK(cfg.crash_priority == 100);
  CHECK(cfg.recorder_out_dir == "/opt/usr/triggered_data");
  CHECK(cfg.recorder_pending_quota == 67108864ull);
  CHECK(cfg.recorder_sources.size() == 2);
  CHECK(cfg.recorder_sources[0].kind ==
        triggered::SourceConfig::Kind::kPinnedSlot);
  CHECK(cfg.recorder_sources[0].shm_name == "/h264_front");
  CHECK(cfg.recorder_sources[0].channel_name == "h264_front");
  CHECK(cfg.recorder_sources[1].kind ==
        triggered::SourceConfig::Kind::kMirrorRing);
  CHECK(cfg.sources.size() == 2);
  CHECK(cfg.sources[0].name == "crash");
  CHECK(cfg.sources[0].channels.size() == 2);
  CHECK(cfg.sources[0].channels[1] == "/thyme_recr_*");
  CHECK(cfg.sources[0].before_ns == 10000000000ULL);
  CHECK(cfg.sources[0].after_ns == 5000000000ULL);
  CHECK(cfg.sources[0].priority == 100);
  CHECK(cfg.sources[0].mute_ns == 0);
  CHECK(cfg.sources[0].flush_realtime);
  CHECK(cfg.sources[0].quota_class == "safety");
  CHECK(cfg.sources[1].before_ns == 2500000000ULL);  // 2.5 s
  CHECK(cfg.sources[1].after_ns == 500000000ULL);    // 0.5 s
  CHECK(!cfg.sources[1].flush_realtime);             // idle default
  CHECK(cfg.quota_limits.size() == 2);
  CHECK(cfg.quota_limits[0].second == 0);   // safety unlimited
  CHECK(cfg.quota_limits[1].second == 30);  // manual 30/h

  // Error paths: line numbers reported.
  trigger_cfg::CenterConfig bad;
  CHECK(!trigger_cfg::ParseCenterConfig("[source.x]\npriority = zz\n", &bad,
                                        &err));
  CHECK(err.find("line 2") != std::string::npos);
  CHECK(!trigger_cfg::ParseCenterConfig("priority = 1\n", &bad, &err));
  CHECK(!trigger_cfg::ParseCenterConfig("[bogus]\nkey = 1\n", &bad, &err));
  CHECK(!trigger_cfg::ParseCenterConfig(
      "[trigger_center]\ndefault_source = ghost\n"
      "[source.x]\nchannels = /x\nquota_class = q\n",
      &bad, &err));
  CHECK(err.find("default_source") != std::string::npos);
  // Empty topology must be explicit; unknown value/keys fail fast.
  CHECK(!trigger_cfg::ParseCenterConfig("", &bad, &err));
  CHECK(err.find("allow_empty_sources") != std::string::npos);
  CHECK(trigger_cfg::ParseCenterConfig(
      "[trigger_center]\nallow_empty_sources = true\n", &bad, &err));
  CHECK(!trigger_cfg::ParseCenterConfig(
      "[trigger_center]\nallow_empty_sources = maybe\n", &bad, &err));
  CHECK(!trigger_cfg::ParseCenterConfig(
      "[trigger_center]\nallow_empty_sources = true\nno_such_key = 1\n", &bad,
      &err));
  CHECK(!trigger_cfg::ParseCenterConfig(
      "[continuous]\nenable = true\nslice_mb = 0\n", &bad, &err));
  CHECK(err.find("slice_mb") != std::string::npos);
  CHECK(!trigger_cfg::ParseCenterConfig(
      "[recorder]\ndisk_quota_mb = 100\n"
      "[continuous]\nenable = true\nslice_mb = 200\n",
      &bad, &err));
  CHECK(err.find("slice_mb") != std::string::npos);  // parse-time
  trigger_cfg::ContinuousPlanReport rep;
  std::string warn;
  // Duplicate section headers are config errors, not last-wins.
  CHECK(!trigger_cfg::ParseCenterConfig(
      "[source.a]\nchannels = /x\nquota_class = q\n"
      "[source.a]\nchannels = /y\nquota_class = q\n",
      &bad, &err));
  CHECK(err.find("duplicate source") != std::string::npos);
  CHECK(
      !trigger_cfg::ParseCenterConfig("[quota.q]\nmax_triggers_per_hour = 1\n"
      "[quota.q]\nmax_triggers_per_hour = 2\n",
      &bad, &err));
  CHECK(err.find("duplicate quota") != std::string::npos);
  CHECK(!trigger_cfg::ParseCenterConfig("[channel./x]\nkind = pinned_slot\n"
      "[channel./x]\nkind = pinned_slot\n",
      &bad, &err));
  CHECK(err.find("duplicate channel") != std::string::npos);
  CHECK(trigger_cfg::ParseCenterConfig(
      "[channel./h264_front]\nkind = h264_stream\nname = h264_front\n"
      "bitrate_kbps = 4000\n"
      "[continuous]\nenable = true\nchannels = pinned_slot:nope\n",
      &bad, &err));
  CHECK(!trigger_cfg::ValidateContinuousPlan(bad, &rep, &warn, &err));
  CHECK(err.find("matches no configured source") != std::string::npos);
  CHECK(!trigger_cfg::ParseCenterConfig(
      "[continuous]\nenable = true\nwait_idle = true\n", &bad, &err));
  CHECK(err.find("demo_idle") != std::string::npos);
  CHECK(trigger_cfg::ParseCenterConfig(
      "[continuous]\nenable = true\nwait_idle = true\ndemo_idle = true\n", &bad,
      &err));
  // continuous geometry, [recorder] policy, channel layout/params and
  // [chassis] need a restart; source/quota policy can hot-reload.
  trigger_cfg::CenterConfig a;
  trigger_cfg::CenterConfig b;
  CHECK(!trigger_cfg::ReloadRequiresRestart(a, b));
  b.cont_slice_bytes += 1024;
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  b = a;
  triggered::SourceConfig sc;
  sc.shm_name = "/new_channel";
  sc.kind = triggered::SourceConfig::Kind::kPinnedSlot;
  b.recorder_sources.push_back(sc);
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  // [recorder] is bound into the recorder/retention/idle objects at startup.
  b = a;
  b.recorder_out_dir = "/tmp/other_out";
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  b = a;
  b.recorder_pending_quota = 1;
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  b = a;
  b.recorder_io_rate_limit_bps = 1;
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  b = a;
  b.recorder_disk_quota_bytes = 1;
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  // retention .incomplete policy is bound into the Manager at startup.
  b = a;
  b.recorder_incomplete_policy = "delete_immediately";
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  b = a;
  b.recorder_idle_hold_ns = 1;
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  b = a;
  b.recorder_idle_fallback_idle = !a.recorder_idle_fallback_idle;
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  // Channel recording params were previously compared only by name/kind.
  b = a;
  triggered::SourceConfig sp;
  sp.shm_name = "/h264_front";
  sp.kind = triggered::SourceConfig::Kind::kPinnedSlot;
  sp.rate_hz = 10.0;
  a.recorder_sources.push_back(sp);
  b.recorder_sources.push_back(sp);
  CHECK(!trigger_cfg::ReloadRequiresRestart(a, b));
  b.recorder_sources[0].bitrate_kbps = 8000;
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  b = a;
  b.recorder_sources[0].rate_hz = 20.0;
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  b = a;
  b.recorder_sources[0].h264_stream = true;
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  // [chassis] binds the idle predicate at startup.
  b = a;
  b.chassis_present = true;
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  b = a;
  b.chassis.speed_threshold = 0.5;
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  // socket geometry and dedup allocation are startup-only.
  b = a;
  b.socket_poll_budget_ns += 1000;
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  b = a;
  b.dedup_window_entries += 1;
  CHECK(trigger_cfg::ReloadRequiresRestart(a, b));
  // Source policy stays hot-reloadable.
  b = a;
  trigger_cfg::SourceRule rule;
  rule.name = "manual";
  rule.before_ns = 1000000000ull;
  a.sources.push_back(rule);
  b.sources.push_back(rule);
  CHECK(!trigger_cfg::ReloadRequiresRestart(a, b));
  b.sources[0].before_ns = 2000000000ull;
  CHECK(!trigger_cfg::ReloadRequiresRestart(a, b));
  b.sources[0].channels.push_back("/h264_front");
  CHECK(!trigger_cfg::ReloadRequiresRestart(a, b));
  b = a;
  // Minimal deployable template: one trigger source, one recorder channel,
  // one continuous selector, and a quota. It must parse and pass the startup
  // rate plan (minimal template).
  {
    const std::string minimal =
        "[trigger_center]\ndefault_source = manual\n"
        "[source.manual]\nchannels = /h264_*\nbefore_s = 1\nafter_s = 0.5\n"
        "quota_class = manual\n"
        "[quota.manual]\nmax_triggers_per_hour = 10\n"
        "[recorder]\noutput_dir = /tmp/thyme_min\npending_bytes_quota = "
        "67108864\n"
        "disk_quota_mb = 100\n"
        "[channel./h264_front]\nkind = h264_stream\nname = h264_front\n"
        "bitrate_kbps = 4000\n"
        "[continuous]\nenable = true\nchannels = pinned_slot:h264_front\n"
        "slice_mb = 50\nwait_idle = false\n";
    trigger_cfg::CenterConfig m;
    CHECK(trigger_cfg::ParseCenterConfig(minimal, &m, &err));
    trigger_cfg::ContinuousPlanReport mr;
    std::string mw;
    CHECK(trigger_cfg::ValidateContinuousPlan(m, &mr, &mw, &err));
  }
}

}  // namespace

int main() {
  TestConfigParser();

  char sock[64];
  std::snprintf(sock, sizeof(sock), "/tmp/c9tproto_%d.sock",
                static_cast<int>(getpid()));
  trigger_cfg::CenterConfig cfg = ProtoCfg(sock);
  trigger_center::Hooks hooks;
  hooks.recorder_ready = HookReady;
  hooks.capture_active = HookActive;
  hooks.start_capture = HookStart;
  hooks.preempt_capture = HookPreempt;

  trigger_center::Center center;
  CHECK(center.Init(cfg, hooks));

  using trigger_proto::TriggerCode;

  // Real roundtrip: accepted.
  trigger_client::Result r = CallWhileTicking(center, sock, "manual");
  CHECK(r.transport_ok);
  CHECK(r.code == TriggerCode::kAccepted);
  CHECK(r.trigger_id == 1);
  CHECK(g_active.load());

  // Same priority while capturing -> muted (over the wire).
  r = CallWhileTicking(center, sock, "manual");
  CHECK(r.transport_ok);
  CHECK(r.code == TriggerCode::kMuted);

  // Higher priority preempts (over the wire).
  r = CallWhileTicking(center, sock, "crash");
  CHECK(r.transport_ok);
  CHECK(r.code == TriggerCode::kAccepted);
  CHECK(r.trigger_id == 2);

  // Unknown source (over the wire).
  g_active.store(false);
  center.Tick();  // capture done; crash mute_s=0 -> IDLE
  r = CallWhileTicking(center, sock, "nosuch");
  CHECK(r.transport_ok);
  CHECK(r.code == TriggerCode::kUnknownSource);
  CHECK(r.trigger_id == 0);

  // Malformed datagram: counted, no reply (sender just fire-and-forgets).
  {
    const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
    const char garbage[] = "definitely not a trigger request";
    CHECK(sendto(fd, garbage, sizeof(garbage), 0,
                 reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) > 0);
    close(fd);
    center.Tick();
    CHECK(center.stats().malformed_datagrams == 1);
  }

  // Auth: ProcessRequest with a foreign uid is refused without a reply.
  {
    trigger_proto::TriggerRequest req;
    req.magic = trigger_proto::kTriggerMagic;
    req.version = trigger_proto::kTriggerVersionV3;
    req.sequence = trigger_client::NextSequence();
    std::snprintf(req.source, sizeof(req.source), "manual");
    trigger_proto::StampRequestCrc(&req);
    trigger_proto::TriggerResponse resp{};
    const uid_t foreign = static_cast<uid_t>(geteuid() + 1);
    trigger_center::PeerCred foreign_peer;
    foreign_peer.uid = foreign;
    foreign_peer.pid = getpid();
    CHECK(!center.ProcessRequest(reinterpret_cast<const uint8_t *>(&req),
                                 sizeof(req), foreign_peer, &resp));
    CHECK(center.stats().auth_rejects == 1);
    // Own uid passes.
    trigger_center::PeerCred own_peer;
    own_peer.uid = geteuid();
    own_peer.pid = getpid();
    CHECK(center.ProcessRequest(reinterpret_cast<const uint8_t*>(&req),
                                sizeof(req), own_peer, &resp));
    CHECK(resp.code == static_cast<uint16_t>(TriggerCode::kAccepted));
    g_active.store(false);
    center.Tick();
    SleepNs(60000000);  // outlast manual's 50 ms mute
    center.Tick();
  }

  // Bad version via the public handler -> malformed, no reply.
  {
    trigger_proto::TriggerRequest req;
    req.magic = trigger_proto::kTriggerMagic;
    req.version = 99;
    req.sequence = 501;
    std::snprintf(req.source, sizeof(req.source), "manual");
    trigger_proto::StampRequestCrc(&req);
    trigger_proto::TriggerResponse resp{};
    trigger_center::PeerCred own_peer;
    own_peer.uid = geteuid();
    own_peer.pid = getpid();
    CHECK(!center.ProcessRequest(reinterpret_cast<const uint8_t*>(&req),
                                 sizeof(req), own_peer, &resp));
    CHECK(center.stats().malformed_datagrams == 2);
  }

  // Client timeout: a bound socket that never answers -> bounded failure.
  {
    char dead[64];
    std::snprintf(dead, sizeof(dead), "/tmp/c9tproto_dead_%d.sock",
                  static_cast<int>(getpid()));
    unlink(dead);
    const int dead_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(dead_fd >= 0);
    sockaddr_un dead_addr{};
    dead_addr.sun_family = AF_UNIX;
    std::snprintf(dead_addr.sun_path, sizeof(dead_addr.sun_path), "%s", dead);
    CHECK(bind(dead_fd, reinterpret_cast<sockaddr*>(&dead_addr),
               sizeof(dead_addr)) == 0);
    const uint64_t t0 = NowNs();
    const trigger_client::Result r2 =
        trigger_client::Trigger(dead, "manual", 0, -1, false, 100000000ULL);
    const uint64_t elapsed = NowNs() - t0;
    CHECK(!r2.transport_ok);
    CHECK(r2.code == TriggerCode::kTransportTimeout);
    CHECK(elapsed >= 50000000ULL);   // the deadline was actually waited
    CHECK(elapsed < 2000000000ULL);  // bounded, ~100 ms
    close(dead_fd);
    unlink(dead);
  }

  center.Shutdown();
  return 0;
}
