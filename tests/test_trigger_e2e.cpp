// test_trigger_e2e.cpp — end-to-end P3: forked producer (tap-wired SPMC ring
// + LatestSlot + PinnedSlotStore) -> host assembles TriggeredRecorder +
// TriggerCenter -> triggers arrive over the real Unix datagram socket ->
// packages verified on disk. Includes the preemption path: a crash-level
// trigger during a long manual capture must truncate+seal the old package
// (truncated flag) and immediately produce a complete new one.
#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/idle_detector.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/retention.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_client.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <pthread.h>
#include <string>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

struct Pod16 {
  uint64_t a;
  uint32_t b;
  uint32_t c;
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

bool PathExists(const std::string& p) {
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0;
}

void ProducerChild(const char* page_name, const char* ring_name,
                   const char* chan_name, int ready_fd) {
#if defined(__linux__)
  prctl(PR_SET_PDEATHSIG, SIGKILL);  // never outlive a failed/aborted parent
#endif
  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = sizeof(Pod16);
  std::snprintf(desc.name, sizeof(desc.name), "Pod16");
  RecordMirrorRingSpec spec{sizeof(Pod16), 512};
  record_mirror::RingClient mirror;
  CHECK(mirror.Create(page_name, &desc, &spec, 1, 1));
  ShmSpmcRing<Pod16, 16> ring;
  CHECK(ring.Create(ring_name));
  ring.SetRecordTap(mirror.Page(), 0);
  PinnedSlotWriter frames;
  // 1024 slots = ~1 s of retention at the 1 ms producer rate below. With 128
  // slots a `ctest -j12` run can delay the recorder tick past 128 ms, after
  // which every frame is overwritten and `p2.frames > 0` fails by design of
  // the test, not of the product.
  CHECK(frames.Create(chan_name, 4096, 1024, 0));
  CHECK(write(ready_fd, "R", 1) == 1);
  // Publish until the parent stops us: scenario timing must never outlive the
  // producer (retained windows only cover recent samples).
  for (uint64_t i = 0;; ++i) {
    Pod16 p{i, static_cast<uint32_t>(i * 2), static_cast<uint32_t>(i * 3)};
    ring.Push(p);
    PinnedSlotWriter::BeginView bv = frames.Begin(1000);
    CHECK(bv.payload != nullptr);
    for (uint32_t j = 0; j < 1000; ++j) {
      bv.payload[j] = static_cast<uint8_t>(i + j);
    }
    frames.Commit();
    SleepNs(1000000);
  }
}

// ---- host wiring: TriggerCenter hooks -> TriggeredRecorder ----
triggered::TriggeredRecorder* g_rec;

// P4 idle gate state (test-driven predicate standing in for /can_chassis).
struct IdleState {
  bool cond = true;  // vehicle stationary condition (production: EPB engaged && speed≈0)
  bool ok = true;    // mirror readable
};
IdleState g_idle;
bool IdlePred(void*, bool* source_ok) {
  *source_ok = g_idle.ok;
  return g_idle.cond;
}
bool g_fault = false;  // ENOSPC injection
bool FaultInject(void*) { return g_fault; }

bool HookReady(void*) { return g_rec->ready(); }
bool HookActive(void*) { return g_rec->phase() != triggered::Phase::kArmed; }
bool HookStart(void*, uint64_t id, uint64_t t0, uint64_t before,
               uint64_t after, bool force_rt, bool high_priority,
               const std::vector<std::string>& channels) {
  return g_rec->Trigger(id, before, after, t0, !force_rt, high_priority,
                        channels);
}
bool HookPreempt(void*) { return g_rec->Preempt(); }
uint64_t HookPending(void*) { return g_rec->pending_bytes(); }
uint64_t HookEvict(void*, uint64_t bytes) { return g_rec->EvictOldest(bytes); }
bool HookDiskFull(void*) { return g_rec->disk_full(); }

struct ClientJob {
  const char* socket;
  const char* source;
  trigger_client::Result res;
  std::atomic<bool> done{false};
};

void* ClientRun(void* p) {
  auto* j = static_cast<ClientJob*>(p);
  j->res = trigger_client::Trigger(j->socket, j->source);
  j->done.store(true, std::memory_order_release);
  return nullptr;
}

struct Host {
  trigger_center::Center center;
  // P4 idle gate: IdleDetector (50 ms hold in tests) drives AllowFlush.
  idle::IdleDetector idle{{50000000ULL, false, 0.8}, IdlePred, nullptr};
  uint64_t pending_quota = 64ull * 1024 * 1024;

  void Tick() {
    center.Tick();
    g_rec->Tick();
    if (g_rec->phase() == triggered::Phase::kSettle) {
      const double ratio = static_cast<double>(g_rec->pending_bytes()) /
                           static_cast<double>(pending_quota);
      if (idle.ShouldFlush(NowNs(), ratio))
        g_rec->AllowFlush();
    }
  }
};

trigger_client::Result CallWhileTicking(Host& host, const char* sock,
                                        const char* source) {
  ClientJob job{sock, source, {}, {}};
  pthread_t th{};
  CHECK(pthread_create(&th, nullptr, ClientRun, &job) == 0);
  const uint64_t deadline = NowNs() + 30000000000ULL;
  while (!job.done.load(std::memory_order_acquire)) {
    host.Tick();
    CHECK(NowNs() < deadline);
    SleepNs(100000);
  }
  CHECK(pthread_join(th, nullptr) == 0);
  return job.res;
}

void TickUntilArmed(Host& host) {
  const uint64_t deadline = NowNs() + 60000000000ULL;
  while (g_rec->phase() != triggered::Phase::kArmed) {
    host.Tick();
    CHECK(NowNs() < deadline);
    SleepNs(200000);
  }
}

struct PackageInfo {
  bool complete = false;
  bool truncated = false;
  uint64_t t0 = 0;
  uint64_t before = 0;
  uint64_t after = 0;
  uint64_t descriptors = 0;
  uint64_t samples = 0;
  uint64_t frames = 0;
  uint64_t holes = 0;
};

PackageInfo InspectPackage(const std::string& dir) {
  PackageInfo pi;
  pi.complete = triggered::TriggeredRecorder::PackageComplete(dir);
  for (const auto& e : [&] {
         ThymeBinReader rd((dir + "/meta.tbin").c_str());
         CHECK(rd.ok());
         std::vector<ThymeBinDataEntry> v;
         ThymeBinDataEntry en;
         while (rd.ReadNext(en) == thyme::ThymeBinReader::ReadResult::kOk)
           v.push_back(en);
         return v;
       }()) {
    if (e.type == ThymeBinEntryType::kTrigger) {
      pi.t0 = e.mono_ns;
      pi.before = e.pre_roll_ns;
      pi.after = e.post_roll_ns;
    }
    if (e.struct_id == triggered::kMetaStructId && e.source_sequence == 1) {
      triggered::CaptureStatsRecord csr{};
      std::memcpy(&csr, e.payload.data(), sizeof(csr));
      pi.truncated = (csr.flags & triggered::kCaptureFlagTruncated) != 0;
      pi.descriptors = csr.descriptors;
      pi.samples = csr.samples_written;
      pi.frames = csr.frames_written;
      pi.holes = csr.holes_written;
    }
  }
  return pi;
}

}  // namespace

int main() {
  alarm(120);
  const int me = static_cast<int>(getpid());
  char page_name[64], ring_name[64], chan_name[64], out_dir[96], sock[96];
  std::snprintf(page_name, sizeof(page_name), "/c9e2e_page_%d", me);
  std::snprintf(ring_name, sizeof(ring_name), "/c9e2e_ring_%d", me);
  std::snprintf(chan_name, sizeof(chan_name), "/c9e2e_h264_%d", me);
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/c9e2e_out_%d", me);
  std::snprintf(sock, sizeof(sock), "/tmp/c9e2e_%d.sock", me);
  ShmRegion::UnlinkName(page_name);
  ShmRegion::UnlinkName(ring_name);
  ShmRegion::UnlinkName(chan_name);
  {
    const std::string l = std::string(chan_name) + ".lease";
    ShmRegion::UnlinkName(l.c_str());
  }

  int ready_pipe[2];
  CHECK(pipe(ready_pipe) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(ready_pipe[0]);
    ProducerChild(page_name, ring_name, chan_name, ready_pipe[1]);
  }
  close(ready_pipe[1]);
  char b = 0;
  CHECK(read(ready_pipe[0], &b, 1) == 1 && b == 'R');
  close(ready_pipe[0]);

  // Assemble engine + center.
  triggered::Config rcfg;
  rcfg.sources = {
      {triggered::SourceConfig::Kind::kMirrorRing, page_name, ""},
      {triggered::SourceConfig::Kind::kPinnedSlot, chan_name, "h264_front"}};
  rcfg.out_dir = out_dir;
  rcfg.fault_inject_write = FaultInject;  // ENOSPC injection point (off by default)
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(rcfg));
  g_rec = &rec;

  trigger_cfg::CenterConfig ccfg;
  ccfg.socket_path = sock;
  trigger_cfg::SourceRule manual;
  manual.name = "manual";
  manual.before_ns = 50000000ULL;    // 50 ms
  manual.after_ns = 10000000000ULL;  // 10 s: stays in Post until preempted
  manual.priority = 10;
  manual.mute_ns = 50000000ULL;
  manual.quota_class = "manual";
  trigger_cfg::SourceRule crash;
  crash.name = "crash";
  crash.before_ns = 50000000ULL;
  crash.after_ns = 50000000ULL;  // 50 ms
  crash.priority = 100;
  crash.mute_ns = 0;
  crash.flush_realtime = true;
  crash.quota_class = "safety";
  // Short-window idle-policy source: used by the P4 idle-flush and ENOSPC scenarios.
  trigger_cfg::SourceRule debug_idle;
  debug_idle.name = "debug_idle";
  debug_idle.before_ns = 50000000ULL;
  debug_idle.after_ns = 50000000ULL;
  debug_idle.priority = 20;
  debug_idle.mute_ns = 50000000ULL;
  debug_idle.flush_realtime = false;  // idle policy -> defer_settle
  debug_idle.quota_class = "debug";
  ccfg.sources = {crash, manual, debug_idle};
  ccfg.quota_limits = {{"safety", 0}, {"manual", 0}, {"debug", 0}};

  trigger_center::Hooks hooks;
  hooks.recorder_ready = HookReady;
  hooks.capture_active = HookActive;
  hooks.start_capture = HookStart;
  hooks.preempt_capture = HookPreempt;
  hooks.pending_bytes = HookPending;
  hooks.evict_oldest = HookEvict;
  hooks.disk_full = HookDiskFull;

  Host host;
  CHECK(host.center.Init(ccfg, hooks));

  using trigger_proto::TriggerCode;

  // Let the producer accumulate ~30 ms of history.
  const uint64_t w0 = NowNs();
  while (NowNs() - w0 < 30000000ULL)
    host.Tick();

  // 1) manual trigger over the socket: accepted, capture starts (10 s post).
  trigger_client::Result r = CallWhileTicking(host, sock, "manual");
  CHECK(r.transport_ok && r.code == TriggerCode::kAccepted);
  CHECK(r.trigger_id == 1);
  CHECK(rec.phase() == triggered::Phase::kPost);
  const std::string pkg1 = rec.last_package_dir();

  // 2) same-priority trigger while capturing: muted.
  const uint64_t w1 = NowNs();
  while (NowNs() - w1 < 100000000ULL)
    host.Tick(); // collect ~100 ms of post
  r = CallWhileTicking(host, sock, "manual");
  CHECK(r.transport_ok && r.code == TriggerCode::kMuted);

  // 3) crash trigger: preempts — old package truncated+sealed, new capture.
  r = CallWhileTicking(host, sock, "crash");
  CHECK(r.transport_ok && r.code == TriggerCode::kAccepted);
  CHECK(r.trigger_id == 2);
  CHECK(host.center.stats().preemptions == 1);
  const std::string pkg2 = rec.last_package_dir();

  // 4) crash capture completes (50 ms post + settle); producer keeps running.
  TickUntilArmed(host);

  // 5) verify both packages.
  CHECK(pkg1 != pkg2);
  const PackageInfo p1 = InspectPackage(pkg1);
  const PackageInfo p2 = InspectPackage(pkg2);
  CHECK(p1.complete);     // preempted capture was still sealed
  CHECK(p1.truncated);    // ...and marked truncated
  CHECK(p1.descriptors > 0);
  // Under load the producer may overwrite shallow frame slots between harvest
  // and drain: holes are the correct semantics there. The exact accounting
  // (every descriptor becomes a sample, a frame, or a hole) is the invariant.
  CHECK(p1.descriptors == p1.samples + p1.frames + p1.holes);
  CHECK(p1.before == 50000000ULL);
  CHECK(p1.after == 10000000000ULL);
  CHECK(p2.complete);
  CHECK(!p2.truncated);
  CHECK(p2.samples > 0);
  CHECK(p2.frames > 0);
  CHECK(p2.before == 50000000ULL && p2.after == 50000000ULL);

  // Package 2's small.tbin: all samples inside the crash window.
  {
    ThymeBinReader rd((pkg2 + "/small.tbin").c_str());
    CHECK(rd.ok());
    ThymeBinDataEntry e;
    uint64_t n = 0, last = 0;
    while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
      CHECK(e.type == ThymeBinEntryType::kData);
      CHECK(e.mono_ns >= p2.t0 - p2.before && e.mono_ns <= p2.t0 + p2.after);
      CHECK(n == 0 || e.mono_ns > last);
      last = e.mono_ns;
      Pod16 p{};
      std::memcpy(&p, e.payload.data(), sizeof(p));
      CHECK(p.a * 2 == p.b && p.a * 3 == p.c);
      ++n;
    }
    CHECK(n == p2.samples);
  }
  // Package 2's frames: index/raw consistency + window membership.
  {
    ThymeBinReader rd((pkg2 + "/frames/h264_front.index.tbin").c_str());
    CHECK(rd.ok());
    FILE* raw = std::fopen((pkg2 + "/frames/h264_front.raw").c_str(), "rb");
    CHECK(raw != nullptr);
    ThymeBinDataEntry e;
    uint64_t n = 0;
    while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
      CHECK(e.mono_ns >= p2.t0 - p2.before && e.mono_ns <= p2.t0 + p2.after);
      triggered::FrameIndexRecord ir{};
      std::memcpy(&ir, e.payload.data(), sizeof(ir));
      CHECK(ir.len == 1000);
      std::vector<uint8_t> seg(ir.len);
      CHECK(fseeko(raw, static_cast<off_t>(ir.offset), SEEK_SET) == 0);
      CHECK(std::fread(seg.data(), 1, ir.len, raw) == ir.len);
      for (uint32_t j = 0; j < ir.len; ++j) {
        CHECK(seg[j] == static_cast<uint8_t>(e.source_sequence + j));
      }
      ++n;
    }
    CHECK(n == p2.frames);
    CHECK(n >= 20);  // ~100 ms window at 1 ms cadence, generous margin
    std::fclose(raw);
  }

  CHECK(rec.stats().captures_done == 2);
  CHECK(rec.stats().triggers_rejected == 0);  // center serializes correctly

  // ---- P4 scenario: idle flush gate (6.2) ----
  // Busy: an idle-policy capture finishes post but must NOT drain.
  g_idle.cond = false;  // vehicle in motion
  r = CallWhileTicking(host, sock, "debug_idle");
  CHECK(r.transport_ok && r.code == TriggerCode::kAccepted);
  CHECK(r.trigger_id == 3);
  const std::string pkg3 = rec.last_package_dir();
  {
    const uint64_t deadline = NowNs() + 30000000000ULL;
    while (rec.phase() != triggered::Phase::kSettle) {  // post window elapses
      host.Tick();
      CHECK(NowNs() < deadline);
      SleepNs(200000);
    }
  }
  {
    // 200 ms of busy ticking: the package stays unsealed (data stays in memory).
    const uint64_t deadline = NowNs() + 200000000ULL;
    while (NowNs() < deadline) {
      host.Tick();
      SleepNs(1000000);
    }
    CHECK(rec.phase() == triggered::Phase::kSettle);
    CHECK(!triggered::TriggeredRecorder::PackageComplete(pkg3));
  }
  g_idle.cond = true;  // stopped: released after idle_hold(50ms)
  TickUntilArmed(host);
  CHECK(triggered::TriggeredRecorder::PackageComplete(pkg3));

  // ---- P4 scenario: crash level bypasses the idle gate + writes high/ (5.3/6.3/6.4) ----
  g_idle.cond = false;  // busy again — must not matter for crash
  r = CallWhileTicking(host, sock, "crash");
  CHECK(r.transport_ok && r.code == TriggerCode::kAccepted);
  const std::string pkg4 = rec.last_package_dir();
  TickUntilArmed(host);
  CHECK(triggered::TriggeredRecorder::PackageComplete(pkg4));
  CHECK(pkg4.find("/high/") != std::string::npos);  // reserved zone
  {
    const PackageInfo p4 = InspectPackage(pkg4);
    CHECK(p4.samples > 0 && p4.frames > 0);
  }

  // ---- P4 scenario: ENOSPC (fault-injected) -> incomplete package + disk_full linkage ----
  g_idle.cond = true;  // drain must run to hit the injected write failure
  g_fault = true;
  r = CallWhileTicking(host, sock, "debug_idle");
  CHECK(r.transport_ok && r.code == TriggerCode::kAccepted);
  const std::string pkg5 = rec.last_package_dir();
  TickUntilArmed(host);  // drain write fails -> capture actively invalidated
  CHECK(rec.disk_full());
  CHECK(rec.stats().disk_errors == 1);
  CHECK(!triggered::TriggeredRecorder::PackageComplete(pkg5));
  CHECK(!PathExists(pkg5));  // engine already renamed it to invalidate it
  CHECK(PathExists(pkg5 + ".incomplete"));
  // Center linkage: low-priority triggers rejected on disk_full (quota_exceeded).
  {
    const auto rej = host.center.Submit("debug_idle", -1, 0, false, false);
    CHECK(rej.code == TriggerCode::kQuotaExceeded);
  }
  // startup recovery scan is idempotent: the engine already marked it, so retention finds no new incomplete package.
  {
    retention::Manager rm;
    retention::Config rcfg2;
    rcfg2.out_dir = out_dir;
    CHECK(rm.Init(rcfg2));
    CHECK(rm.MarkIncomplete() == 0);
    CHECK(!triggered::TriggeredRecorder::PackageComplete(pkg5));
  }
  // retry succeeds after retention + ClearDiskFull.
  g_fault = false;
  rec.ClearDiskFull();
  {
    const uint64_t deadline = NowNs() + 30000000000ULL;
    while (host.center.state() != trigger_center::State::kIdle) {
      host.Tick();
      CHECK(NowNs() < deadline);
      SleepNs(1000000);
    }
  }
  g_idle.cond = true;
  r = CallWhileTicking(host, sock, "debug_idle");
  CHECK(r.transport_ok && r.code == TriggerCode::kAccepted);
  TickUntilArmed(host);
  CHECK(triggered::TriggeredRecorder::PackageComplete(rec.last_package_dir()));
  CHECK(rec.stats().captures_done == 5);
  CHECK(rec.stats().triggers_rejected == 0);

  // An identical V4 retry returns the first decision and cannot create
  // a second recorder package, even before the pending trigger starts.
  while (host.center.state() != trigger_center::State::kIdle)
    host.Tick();
  {
    trigger_proto::TriggerRequestV4 req;
    req.magic = trigger_proto::kTriggerMagic;
    req.version = trigger_proto::kTriggerVersion;
    req.priority_hint = -1;
    req.sequence = trigger_client::NextSequence();
    req.request_id = 0x5452473139453245ull;
    std::snprintf(req.source, sizeof(req.source), "debug_idle");
    trigger_proto::StampRequestCrc(&req);
    trigger_center::PeerCred peer;
    peer.uid = geteuid();
    peer.pid = getpid();
    trigger_proto::TriggerResponseV4 first{};
    CHECK(host.center.ProcessRequest(reinterpret_cast<const uint8_t *>(&req),
                                     sizeof(req), peer, &first));
    CHECK(first.code == static_cast<uint16_t>(TriggerCode::kAccepted));
    trigger_proto::TriggerResponseV4 retry{};
    CHECK(host.center.ProcessRequest(reinterpret_cast<const uint8_t *>(&req),
                                     sizeof(req), peer, &retry));
    CHECK(retry.trigger_id == first.trigger_id);
    CHECK((retry.flags & trigger_proto::kRespFlagDeduplicated) != 0);
    host.Tick();
    const std::string retry_pkg = rec.last_package_dir();
    TickUntilArmed(host);
    CHECK(triggered::TriggeredRecorder::PackageComplete(retry_pkg));
    CHECK(rec.stats().captures_done == 6);
    CHECK(host.center.stats().dedup_hits == 1);
  }

  // Stop the producer and clean up.
  CHECK(kill(pid, SIGTERM) == 0);
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);

  // Cleanup.
  host.center.Shutdown();
  rec.Shutdown();
  ShmRegion::UnlinkName(page_name);  // producer created it; killed w/o cleanup
  ShmRegion::UnlinkName(ring_name);
  ShmRegion::UnlinkName(chan_name);
  { const std::string l = std::string(chan_name) + ".lease"; ShmRegion::UnlinkName(l.c_str()); }
  // remove package tree
  auto rm_tree = [](const std::string& path, auto&& self) -> void {
    DIR* d = opendir(path.c_str());
    if (d == nullptr) return;
    while (dirent* en = readdir(d)) {
      if (en->d_name[0] == '.') continue;
      const std::string sub = path + "/" + en->d_name;
      if (en->d_type == DT_DIR) self(sub, self);
      else std::remove(sub.c_str());
    }
    closedir(d);
    rmdir(path.c_str());
  };
  rm_tree(out_dir, rm_tree);
  return 0;
}
