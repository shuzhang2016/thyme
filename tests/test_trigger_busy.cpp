// test_trigger_busy.cpp — T2 async trigger: Accepted is answered at queue
// time, the capture starts at Tick; a second admission while the first is
// pending gets kBusy (never kRecorderOffline); a trigger during a long drain
// gets kMuted. Also: the engine drain is time-sliced per Tick so the trigger
// socket is never starved by a long settle.
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

// CPU time consumed by this thread: being preempted mid-Tick on a fully loaded machine is not an overrun —
// drain_tick_budget_ns bounds exactly "the CPU work of one Tick has an upper bound".
uint64_t ThreadCpuNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

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

}  // namespace

int main() {
  using trigger_proto::TriggerCode;

  // ---- center level: Accepted at queue time; kBusy while pending ----
  {
    trigger_cfg::CenterConfig cfg;
    char sock[64];
    std::snprintf(sock, sizeof(sock), "/tmp/c9busy_%d.sock",
                  static_cast<int>(getpid()));
    cfg.socket_path = sock;
    trigger_cfg::SourceRule manual;
    manual.name = "manual";
    manual.priority = 10;
    manual.mute_ns = 0;
    manual.quota_class = "manual";
    manual.before_ns = manual.after_ns = 1000000000ULL;
    cfg.sources = {manual};
    cfg.quota_limits = {{"manual", 0}};

    trigger_center::Hooks hooks;
    hooks.recorder_ready = HookReady;
    hooks.capture_active = HookActive;
    hooks.start_capture = HookStart;
    hooks.preempt_capture = HookPreempt;
    trigger_center::Center c;
    CHECK(c.Init(cfg, hooks));

    trigger_center::SubmitResult r = c.Submit("manual", -1, 0, false, false);
    CHECK(r.code == TriggerCode::kAccepted);
    CHECK(r.trigger_id == 1);
    CHECK(!g_active);  // queued, not started

    r = c.Submit("manual", -1, 0, false, false);  // queue occupied
    CHECK(r.code == TriggerCode::kBusy);
    CHECK(r.code != TriggerCode::kRecorderOffline);
    CHECK(c.stats().rejected_busy == 1);

    c.Tick();  // Tick is what actually starts it
    CHECK(g_active);
    r = c.Submit("manual", -1, 0, false, false);  // capturing: muted, not busy/offline
    CHECK(r.code == TriggerCode::kMuted);
    g_active = false;
    c.Tick();  // done (mute 0) -> IDLE
    CHECK(c.state() == trigger_center::State::kIdle);
    c.Shutdown();
  }

  // ---- engine level: drain is time-sliced; a huge settle does not block ----
  {
    char page_name[64], out_dir[96];
    const int me = static_cast<int>(getpid());
    std::snprintf(page_name, sizeof(page_name), "/c9busy_page_%d", me);
    std::snprintf(out_dir, sizeof(out_dir), "/tmp/c9busy_%d", me);
    ShmRegion::UnlinkName(page_name);
    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = 512;
    std::snprintf(desc.name, sizeof(desc.name), "Pod512");
    RecordMirrorRingSpec spec{512, 4096};
    record_mirror::RingClient mirror;
    CHECK(mirror.Create(page_name, &desc, &spec, 1, 1));
    std::vector<uint8_t> buf(512, 0x5A);
    for (uint64_t i = 0; i < 3000; ++i) {
      CHECK(mirror.Page()->TryWriteRing(0, buf.data(), 512, NowNs(),
                                        static_cast<uint32_t>(i)));
    }

    triggered::Config ecfg;
    ecfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name,
                     ""}};
    ecfg.out_dir = out_dir;
    ecfg.drain_tick_budget_ns = 1000000;  // 1 ms time slice
    triggered::TriggeredRecorder rec;
    CHECK(rec.Init(ecfg));
    CHECK(rec.Trigger(1, 60000000000ULL, 0));  // ~1.5 MB queue
    // Post elapses -> settle.
    const uint64_t deadline = NowNs() + 10000000000ULL;
    while (rec.phase() != triggered::Phase::kSettle) {
      rec.Tick();
      CHECK(NowNs() < deadline);
      struct timespec ts {0, 100000};
      nanosleep(&ts, nullptr);
    }
    // A single Tick must come back within its slice even with a 1.5 MB queue.
    // Judge by thread CPU: wall clock would include scheduler preemption (the root cause of existing flakes under full -j12 / coverage load),
    // while the slice budget bounds CPU work. ticks > 1 is the hard criterion that slicing took effect
    // (an unsliced Tick would drain the whole queue at once).
    uint64_t ticks = 0;
    const uint64_t t_start = NowNs();
    while (rec.phase() == triggered::Phase::kSettle) {
      const uint64_t tc = ThreadCpuNs();
      rec.Tick();
      CHECK(ThreadCpuNs() - tc < 50000000ULL);  // 50 ms CPU (1 ms budget + one row)
      ++ticks;
      CHECK(NowNs() - t_start < 30000000000ULL);
    }
    CHECK(ticks > 1);  // slicing took effect: it did not drain everything at once
    CHECK(triggered::TriggeredRecorder::PackageComplete(
        rec.last_package_dir()));
    mirror.Shutdown(true);
    rec.Shutdown();
  }
  return 0;
}
