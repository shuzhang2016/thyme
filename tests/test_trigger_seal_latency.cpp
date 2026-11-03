// test_trigger_seal_latency.cpp — TRG-14 trigger->seal latency measurement
// plumbing (deterministic; the latency NUMBERS live in bench_trigger_seal and
// are never asserted here).
//
// Metric (docs/guides/trigger-config.md "trigger->seal latency SLO"):
//   start anchor — CLOCK_MONOTONIC of the center's OBS-05 FSM kSubmitAccept
//                  event for the trigger id;
//   end anchor   — CLOCK_MONOTONIC of full seal completion in the recorder
//                  (TriggeredRecorder::last_seal_mono_ns(), stamped after
//                  every durability step: file fsyncs, wal seal row, REC-18
//                  manifest, package-dir sync).
//   durable record — the package's wal.log seal row carries the CLOCK_MONOTONIC
//                  journal-commit time, paired with the header's trigger_id.
//
// Asserted here (all deterministic):
//   * accept/seal timestamps exist, are non-zero, and order correctly
//     (accept <= wal journal commit <= full seal <= now);
//   * accept->seal pairing by trigger_id, including the preempt path where the
//     truncated old capture seals synchronously with ITS id;
//   * the FSM kCompleted event (center-observed completion) never precedes
//     the accept stamp;
//   * aborted captures never touch the seal accessors (no false endpoint).
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <string>
#include <unistd.h>
#include <utility>
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

void RmTree(const std::string& path) {
  DIR* d = opendir(path.c_str());
  if (d == nullptr) return;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const std::string sub = path + "/" + e->d_name;
    if (e->d_type == DT_DIR) RmTree(sub);
    else std::remove(sub.c_str());
  }
  closedir(d);
  rmdir(path.c_str());
}

// wal.log: header + fixed 24-byte rows; the seal row is the LAST row of a
// completed capture and (TRG-14) carries the journal-commit mono stamp.
struct WalInfo {
  uint64_t trigger_id = 0;
  bool has_seal = false;
  uint64_t seal_mono_ns = 0;
};

WalInfo ReadWal(const std::string& pkg_dir) {
  WalInfo w;
  FILE* f = std::fopen((pkg_dir + "/wal.log").c_str(), "rb");
  CHECK(f != nullptr);
  triggered::WalHeader wh{};
  CHECK(std::fread(&wh, sizeof(wh), 1, f) == 1);
  CHECK(std::memcmp(wh.magic, triggered::kWalMagic, 8) == 0);
  w.trigger_id = wh.trigger_id;
  triggered::WalRow last{};
  bool any = false;
  for (;;) {
    triggered::WalRow row{};
    if (std::fread(&row, sizeof(row), 1, f) != 1) break;
    last = row;
    any = true;
  }
  std::fclose(f);
  if (any && last.op == triggered::kWalOpSeal) {
    w.has_seal = true;
    w.seal_mono_ns = last.mono_ns;
  }
  return w;
}

// ---- host wiring: TriggerCenter hooks -> TriggeredRecorder (e2e pattern) --
triggered::TriggeredRecorder* g_rec;

bool HookReady(void*) { return g_rec->ready(); }
bool HookActive(void*) { return g_rec->phase() != triggered::Phase::kArmed; }
bool HookStart(void*, uint64_t id, uint64_t t0, uint64_t before,
               uint64_t after, bool force_rt, bool high_priority,
               const std::vector<std::string>& channels) {
  return g_rec->Trigger(id, before, after, t0, !force_rt, high_priority,
                        channels);
}
bool HookPreempt(void*) { return g_rec->Preempt(); }

// Last FSM event for (trigger_id, reason); false when absent.
bool FindFsmEvent(trigger_center::Center& center, uint64_t id,
                  trigger_center::FsmReason reason, uint64_t* mono_ns) {
  trigger_center::FsmEvent ev[trigger_center::Center::kFsmEventRingSize];
  const uint32_t n =
      center.CopyFsmEvents(ev, trigger_center::Center::kFsmEventRingSize);
  bool found = false;
  for (uint32_t i = 0; i < n; ++i) {
    if (ev[i].trigger_id == id &&
        ev[i].reason == static_cast<uint8_t>(reason)) {
      *mono_ns = ev[i].mono_ns;
      found = true;
    }
  }
  return found;
}

}  // namespace

int main() {
  alarm(60);
  const int me = static_cast<int>(getpid());
  char page_name[64], ring_name[64], out_dir[96], sock[96];
  std::snprintf(page_name, sizeof(page_name), "/tseal_page_%d", me);
  std::snprintf(ring_name, sizeof(ring_name), "/tseal_ring_%d", me);
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/tseal_out_%d", me);
  std::snprintf(sock, sizeof(sock), "/tmp/tseal_%d.sock", me);
  RmTree(out_dir);
  ShmRegion::UnlinkName(page_name);
  ShmRegion::UnlinkName(ring_name);

  // In-process producer: tap-wired SPMC ring -> mirror ring page.
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

  triggered::Config rcfg;
  rcfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name, ""}};
  rcfg.out_dir = out_dir;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(rcfg));
  g_rec = &rec;
  // No capture has sealed yet: the endpoint accessors start at zero.
  CHECK(rec.last_seal_mono_ns() == 0);
  CHECK(rec.last_seal_trigger_id() == 0);

  trigger_cfg::CenterConfig ccfg;
  ccfg.socket_path = sock;
  trigger_cfg::SourceRule normal;
  normal.name = "normal";
  normal.before_ns = 10000000ULL;   // 10 ms
  normal.after_ns = 20000000ULL;    // 20 ms
  normal.priority = 10;
  normal.mute_ns = 0;
  normal.flush_realtime = true;     // no idle gate in this test
  normal.quota_class = "q";
  trigger_cfg::SourceRule crash;
  crash.name = "crash";
  crash.before_ns = 10000000ULL;
  crash.after_ns = 20000000ULL;
  crash.priority = 100;
  crash.mute_ns = 0;
  crash.flush_realtime = true;
  crash.quota_class = "q";
  trigger_cfg::SourceRule slow;
  slow.name = "slow";               // long post window: stays up until preempted
  slow.before_ns = 10000000ULL;
  slow.after_ns = 10000000000ULL;   // 10 s
  slow.priority = 20;
  slow.mute_ns = 0;
  slow.flush_realtime = true;
  slow.quota_class = "q";
  ccfg.sources = {normal, crash, slow};
  ccfg.quota_limits = {{"q", 0}};

  trigger_center::Hooks hooks;
  hooks.recorder_ready = HookReady;
  hooks.capture_active = HookActive;
  hooks.start_capture = HookStart;
  hooks.preempt_capture = HookPreempt;
  trigger_center::Center center;
  CHECK(center.Init(ccfg, hooks));

  uint64_t pub = 0;
  auto tick_publish = [&] {
    center.Tick();
    rec.Tick();
    Pod16 p{pub, static_cast<uint32_t>(pub * 2), static_cast<uint32_t>(pub * 3)};
    ring.Push(p);
    ++pub;
  };

  // ~30 ms of producer history.
  {
    const uint64_t w0 = NowNs();
    while (NowNs() - w0 < 30000000ULL) {
      tick_publish();
      SleepNs(200000);
    }
  }

  // Fire one trigger and wait for its seal; returns {accept_mono, seal_mono}.
  auto run_trigger = [&](const char* source) {
    const uint64_t t_before = NowNs();
    const trigger_center::SubmitResult r = center.Submit(source, -1, 0, false,
                                                         false);
    const uint64_t t_after = NowNs();
    CHECK(r.code == trigger_proto::TriggerCode::kAccepted);
    const uint64_t id = r.trigger_id;

    // Start anchor: the accept stamp sits inside the Submit call.
    uint64_t accept_mono = 0;
    CHECK(FindFsmEvent(center, id, trigger_center::FsmReason::kSubmitAccept,
                       &accept_mono));
    CHECK(accept_mono >= t_before && accept_mono <= t_after);

    const uint64_t deadline = NowNs() + 10000000000ULL;
    while (rec.last_seal_trigger_id() != id) {
      tick_publish();
      SleepNs(200000);
      CHECK(NowNs() < deadline);
    }
    return std::pair<uint64_t, uint64_t>{accept_mono,
                                         rec.last_seal_mono_ns()};
  };

  // Tick until the center has observed the capture end (kCompleted is
  // emitted one center Tick after the recorder's seal, mute_ns == 0 here).
  auto drain_idle = [&] {
    const uint64_t deadline = NowNs() + 5000000000ULL;
    while (center.state() != trigger_center::State::kIdle) {
      tick_publish();
      SleepNs(200000);
      CHECK(NowNs() < deadline);
    }
  };

  // Verify one sealed package's timestamps end to end. end_reason is the
  // capture's terminal FSM event: kCompleted for a capture that ran out its
  // post window, kPreempted for one truncated by a higher-priority trigger.
  auto verify_sealed = [&](uint64_t id, uint64_t accept_mono,
                           uint64_t seal_mono, const std::string& pkg_dir,
                           trigger_center::FsmReason end_reason) {
    CHECK(triggered::TriggeredRecorder::PackageComplete(pkg_dir));
    const WalInfo wal = ReadWal(pkg_dir);
    CHECK(wal.trigger_id == id);         // durable pairing by trigger_id
    CHECK(wal.has_seal);
    CHECK(wal.seal_mono_ns != 0);        // TRG-14 journal-commit stamp
    CHECK(accept_mono <= wal.seal_mono_ns);
    CHECK(wal.seal_mono_ns <= seal_mono);
    CHECK(seal_mono <= NowNs());
    // The center-observed capture end never precedes the accept stamp.
    uint64_t end_mono = 0;
    CHECK(FindFsmEvent(center, id, end_reason, &end_mono));
    CHECK(end_mono >= accept_mono);
  };

  // ---- scenario A: two sequential captures, per-id pairing ----
  uint64_t accept1 = 0, seal1 = 0;
  {
    const auto a = run_trigger("normal");
    accept1 = a.first;
    seal1 = a.second;
    CHECK(rec.last_seal_trigger_id() == 1);
    drain_idle();
    verify_sealed(1, accept1, seal1, rec.last_package_dir(),
                  trigger_center::FsmReason::kCompleted);
  }
  {
    const auto a = run_trigger("normal");
    CHECK(rec.last_seal_trigger_id() == 2);
    CHECK(a.first >= accept1 && a.second >= seal1);  // monotone across ids
    drain_idle();
    verify_sealed(2, a.first, a.second, rec.last_package_dir(),
                  trigger_center::FsmReason::kCompleted);
  }

  // ---- scenario B: preempt — the truncated OLD capture seals with ITS id --
  {
    const uint64_t t_before = NowNs();
    const trigger_center::SubmitResult ra = center.Submit("slow", -1, 0, false,
                                                          false);
    CHECK(ra.code == trigger_proto::TriggerCode::kAccepted);
    const uint64_t slow_id = ra.trigger_id;
    const uint64_t t_after = NowNs();
    uint64_t slow_accept = 0;
    CHECK(FindFsmEvent(center, slow_id,
                       trigger_center::FsmReason::kSubmitAccept, &slow_accept));
    CHECK(slow_accept >= t_before && slow_accept <= t_after);

    // Let it start and collect a little post data.
    {
      const uint64_t deadline = NowNs() + 5000000000ULL;
      while (rec.phase() != triggered::Phase::kPost) {
        tick_publish();
        SleepNs(200000);
        CHECK(NowNs() < deadline);
      }
      const uint64_t w0 = NowNs();
      while (NowNs() - w0 < 30000000ULL) {
        tick_publish();
        SleepNs(200000);
      }
    }
    const std::string slow_pkg = rec.last_package_dir();

    const trigger_center::SubmitResult rb = center.Submit("crash", -1, 0,
                                                          false, false);
    CHECK(rb.code == trigger_proto::TriggerCode::kAccepted);
    const uint64_t crash_id = rb.trigger_id;
    uint64_t crash_accept = 0;
    CHECK(FindFsmEvent(center, crash_id,
                       trigger_center::FsmReason::kSubmitAccept,
                       &crash_accept));

    // The preempting Tick seals the truncated slow capture synchronously,
    // then the crash capture runs to its own seal.
    const uint64_t deadline = NowNs() + 10000000000ULL;
    while (rec.last_seal_trigger_id() != crash_id) {
      tick_publish();
      SleepNs(200000);
      CHECK(NowNs() < deadline);
    }
    // Both captures sealed, each stamped under its own trigger id: the slow
    // package's wal pairs its seal with slow_id even though the seal was
    // driven by the crash trigger's Tick.
    drain_idle();
    verify_sealed(slow_id, slow_accept, /*seal_mono=*/NowNs(), slow_pkg,
                  trigger_center::FsmReason::kPreempted);
    verify_sealed(crash_id, crash_accept, rec.last_seal_mono_ns(),
                  rec.last_package_dir(),
                  trigger_center::FsmReason::kCompleted);
  }

  // ---- scenario C: aborted capture leaves no seal endpoint ----
  {
    // Fault-inject a drain write failure: the capture aborts unsealed and the
    // seal accessors must keep pointing at the previous successful seal.
    const uint64_t sealed_id = rec.last_seal_trigger_id();
    const uint64_t sealed_mono = rec.last_seal_mono_ns();
    rec.Shutdown();  // re-init with fault injection for a clean abort
    triggered::Config fcfg;
    fcfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name,
                     ""}};
    fcfg.out_dir = out_dir;
    static bool g_fault = true;
    fcfg.fault_inject_write = [](void*) { return g_fault; };
    CHECK(rec.Init(fcfg));
    const uint64_t deadline = NowNs() + 5000000000ULL;
    while (!rec.ready()) {  // re-attach
      rec.Tick();
      SleepNs(1000000);
      CHECK(NowNs() < deadline);
    }
    // The crash capture from scenario B must drain out of the FSM first,
    // otherwise this submit is muted by its higher priority.
    while (center.state() != trigger_center::State::kIdle) {
      tick_publish();
      SleepNs(200000);
      CHECK(NowNs() < deadline);
    }
    const trigger_center::SubmitResult r = center.Submit("normal", -1, 0,
                                                         false, false);
    CHECK(r.code == trigger_proto::TriggerCode::kAccepted);
    const uint64_t abort_deadline = NowNs() + 10000000000ULL;
    // Wait for the deferred start first — the phase is still kArmed until the
    // center's next Tick runs StartPendingTrigger.
    while (rec.phase() == triggered::Phase::kArmed) {
      tick_publish();
      SleepNs(200000);
      CHECK(NowNs() < abort_deadline);
    }
    while (rec.phase() != triggered::Phase::kArmed) {
      tick_publish();
      SleepNs(200000);
      CHECK(NowNs() < abort_deadline);
    }
    CHECK(rec.disk_full());
    CHECK(rec.last_seal_trigger_id() == sealed_id);  // no false endpoint
    CHECK(rec.last_seal_mono_ns() == sealed_mono);
    rec.ClearDiskFull();
  }
  center.Shutdown();
  rec.Shutdown();
  RmTree(out_dir);
  ShmRegion::UnlinkName(page_name);
  ShmRegion::UnlinkName(ring_name);
  std::printf("test_trigger_seal_latency: OK\n");
  return 0;
}
