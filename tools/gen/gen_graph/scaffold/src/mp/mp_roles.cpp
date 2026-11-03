#include "mp/mp_entry.h"
#include "mp/mp_layout.h"
#include "mp/mp_tables.h"

#include "support/record_trigger.h"
#include "support/record_mirror_ctx.h"
#include "support/scheduling_setup.h"
#include "generated/record_manifest.h"
#include "generated/scheduling_rings.h"
#include "thyme/app/mp_runtime.h"
#include "thyme/record/record_schema.h"
#include "thyme/runtime/app.h"
#include "thyme/runtime/event_thread.h"
#include "thyme/runtime/proc_sampler.h"
#include "generated/mp_roles.h"
#include "generated/mp_role_locals.h"
#include "generated/mp_unknown.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif

namespace {

// ABI-10: pin this build's GEN-12 record manifest at the top of every T9P1
// perf file. RecordStructId is a row index into the sorted recorded-channel
// list, so an analyser that was built against a different manifest must not
// join these samples (thyme_log_merge refuses, see --expect-hash).
void WritePerfManifest(FILE* f) {
  thyme::PerfLogWriter bw(0, 0);
  bw.U8(thyme::kPerfManifestBlockType);
  bw.U8(0);  // build-wide, not attributed to a node
  bw.U32(1);
  bw.U16(thyme::schema::kManifestVersion);
  bw.U16(0);  // reserved, must be 0
  bw.U64(kRecordManifestHash);
  std::fwrite(bw.data() + 4, 1, bw.size() - 4, f);  // skip T9P1 magic
}

}  // namespace

int RunRole(const char* prefix, const char* role, const char* record_shm,
            int proc_idx) {
  // Name the process t9_<role> so /proc comm (and ps) can tell role children
  // apart for per-role CPU/RSS accounting by eval tooling.
  {
    char nm[16];
    std::snprintf(nm, sizeof(nm), "t9_%s", role);
#ifdef __linux__
    prctl(PR_SET_NAME, nm, 0, 0, 0);
#elif defined(__APPLE__)
    pthread_setname_np(nm);  // macOS names the current thread only
#endif
  }
  thyme::app::MpRoleSession session;
  if (!session.Open(prefix, record_shm, kMpHealthN, thyme::app::kMpOpenTimeoutNs,
                    0, /*record_ring=*/true, kGraphHash)) {
    return 2;
  }
  MpControl* ctrl = session.ctrl();
  thyme::HealthSlot* health = session.slots();
  record_trigger::SetControl(ctrl);

  const auto shm_channels = MpShmChannels();
  const auto slot_specs = MpSlotSpecs();
  thyme::app::MpRoleChannels channels;
  if (!channels.Open(prefix, role, shm_channels,
                     {slot_specs.data(), slot_specs.size()}))
    return 2;

#ifdef THYME_GRAPH_HAS_EVENT_NODES
  EventTransports ev;
#ifdef THYME_GRAPH_HAS_LOCAL_HUB
  if (!BindLocalFrameHubOutputs(ev, health)) return 2;
#endif
#ifdef THYME_GRAPH_HAS_EVENT_SHM
  if (!OpenEventShmChannels(ev, prefix, role, thyme::app::kMpOpenTimeoutNs))
    return 2;
#endif
  SetEventRecordTaps(ev);
#endif

#ifdef THYME_GRAPH_HAS_BROADCAST
  BroadcastTransports bc;
  if (!OpenBroadcastShmChannels(bc, prefix, role, thyme::app::kMpOpenTimeoutNs))
    return 2;
  SetBroadcastRecordTaps(bc);
#endif

#ifdef THYME_GRAPH_HAS_BIGFRAME
  PinnedSlotTransports bf;
#ifdef THYME_GRAPH_HAS_EVENT_NODES
  if (!OpenPinnedSlotShmChannels(bf, prefix, role, thyme::app::kMpOpenTimeoutNs,
                               &ev)) {
    return 2;
  }
#else
  if (!OpenPinnedSlotShmChannels(bf, prefix, role, thyme::app::kMpOpenTimeoutNs)) {
    return 2;
  }
#endif
#endif

  auto locals = RoleLocals::Create();
  if (!AssembleRoleLocals(*locals, role, channels, ctrl)) return 2;

  const SchedulingRings sched_rings = BuildSchedulingRings(*locals);

  thyme::App app;
  thyme::App::InstallStopSignals();

  constexpr std::size_t kMaxThreads = thyme::App::kMaxThreads;
  thyme::CyclicThread* threads[kMaxThreads]{};
  std::size_t thread_count = 0;
  thyme::EventThread* ev_threads[kMaxThreads]{};
  std::size_t ev_thread_count = 0;
  uint8_t thread_node[kMaxThreads]{};   // graph node index per cyclic thread
  uint8_t ev_node[kMaxThreads]{};       // ... and per event thread
#ifdef THYME_GRAPH_HAS_EVENT_NODES
  // One sampler per event node: ProcSampler's eight slots must not be shared
  // across nodes with independent queue/overrun probes.
  thyme::ProcSampler ev_proc[kMaxThreads];
  thyme::ProcSampleThread ev_proc_threads[kMaxThreads];
#endif

#ifdef THYME_GRAPH_HAS_LOCAL_HUB
  thyme::HealthSlot local_hub_health{};
  auto* local_hub_thread = new LocalHubDispatcher(role, &ev, &local_hub_health);
  if (local_hub_thread->active()) {
    if (!app.Add(local_hub_thread)) {
      std::fprintf(stderr, "RunRole: app.Add failed for local hubs\n");
      return 2;
    }
  } else {
    delete local_hub_thread;
    local_hub_thread = nullptr;
  }
#endif

  for (std::size_t i = 0; i < std::size(kGraphNodes); ++i) {
    const auto& n = kGraphNodes[i];
    if (std::strcmp(n.process, role) != 0) continue;
    if (i >= kMpHealthN) {
      std::fprintf(stderr,
                   "RunRole: node index %zu >= health_n %zu (graph IR)\n", i,
                   kMpHealthN);
      return 2;
    }
    thyme::HealthSlot* h = &health[i];

#ifdef THYME_GRAPH_HAS_EVENT_NODES
    if (kGraphNodeIsEvent[i]) {
      thyme::EventThread* eth = MakeEventThread(i, ev, *locals,
#ifdef THYME_GRAPH_HAS_BROADCAST
                                                bc,
#endif
#ifdef THYME_GRAPH_HAS_BIGFRAME
                                                bf,
#endif
                                                h);
      if (eth == nullptr) {
        std::fprintf(stderr,
                     "RunRole: event node '%s' (id='%s') not wired yet\n",
                     n.type, n.id);
        return 2;
      }
      if (ev_thread_count >= kMaxThreads) {
        std::fprintf(stderr, "RunRole: too many event threads for role '%s'\n",
                     role);
        return 2;
      }
      ev_node[ev_thread_count] = static_cast<uint8_t>(i);
      ev_threads[ev_thread_count] = eth;
      if (ev_proc[ev_thread_count].Start()) {
        eth->RegisterObservability(&ev_proc[ev_thread_count]);
        ev_proc_threads[ev_thread_count].Start(&ev_proc[ev_thread_count],
                                               1000000000ULL);
      }
      ++ev_thread_count;
      if (!app.Add(eth)) {
        std::fprintf(stderr, "RunRole: app.Add failed for event node '%s'\n",
                     n.id);
        return 2;
      }
      continue;
    }
#endif

    thyme::CyclicThread* t = MakeKnownThread(
        i, *locals,
#ifdef THYME_GRAPH_HAS_EVENT_NODES
        ev,
#endif
#ifdef THYME_GRAPH_HAS_BROADCAST
        bc,
#endif
#ifdef THYME_GRAPH_HAS_BIGFRAME
        bf,
#endif
        health);
    if (t == nullptr) {
      t = MakeUnknownThread(i, *locals,
#ifdef THYME_GRAPH_HAS_EVENT_NODES
                            ev,
#endif
#ifdef THYME_GRAPH_HAS_BROADCAST
                            bc,
#endif
#ifdef THYME_GRAPH_HAS_BIGFRAME
                            bf,
#endif
                            h);
      if (t == nullptr) {
        std::fprintf(stderr,
                     "RunRole: unknown node type '%s' (id='%s') not wired yet\n",
                     n.type, n.id);
        return 2;
      }
    }
    if (thread_count >= kMaxThreads) {
      std::fprintf(stderr, "RunRole: too many threads for role '%s'\n", role);
      return 2;
    }
    thread_node[thread_count] = static_cast<uint8_t>(i);
    threads[thread_count++] = t;
    ApplyNodeScheduling(n.id, t, sched_rings);
    if (!app.Add(t)) {
      std::fprintf(stderr, "RunRole: app.Add failed for node '%s'\n", n.id);
      return 2;
    }
  }

  if (thread_count == 0 && ev_thread_count == 0) {
    std::fprintf(stderr, "RunRole: role '%s' has no threads\n", role);
    return 2;
  }

  if (!app.Start()) return 2;

  session.WaitUntilStop();

  app.RequestStop();
  app.Join();
#ifdef THYME_GRAPH_HAS_EVENT_NODES
  for (std::size_t i = 0; i < ev_thread_count; ++i) {
    ev_proc_threads[i].Stop();
  }
#endif

  uint64_t min_loops = ~0ull;

  // Per-sample perf log (10B records), gated by T9_PERF_LOG=<path-prefix>:
  // node latency/work, edge latency, and event callback work use separate files
  // so their percentile series never share a RecordStructId key.
  if (const char* perf_prefix = std::getenv("T9_PERF_LOG")) {
    char path[192];
    auto dump_sampler = [&](FILE* pf, const thyme::PerfSampler& sampler,
                            uint8_t id) {
      // OBS-03: rings are rate-sized at thread construction; size the writer
      // for the actual ring capacity.
      thyme::PerfLogWriter bw(sampler.capacity(), 0);
      sampler.Flush(bw, id);
      std::fwrite(bw.data() + 4, 1, bw.size() - 4, pf);  // skip magic
    };
    std::snprintf(path, sizeof(path), "%s_node_%s_%d.bin", perf_prefix, role,
                  static_cast<int>(getpid()));
    if (FILE* pf = std::fopen(path, "wb")) {
      std::fwrite("T9P1", 1, 4, pf);
      WritePerfManifest(pf);
      auto dump_work = [&](FILE* f, const thyme::PerfWorkLog& wl, uint8_t id) {
        thyme::PerfLogWriter bw(0, wl.capacity());
        wl.Flush(bw, id);
        std::fwrite(bw.data() + 4, 1, bw.size() - 4, f);
      };
      for (std::size_t i = 0; i < thread_count; ++i) {
        thyme::PerfLogWriter bw(0, 0);
        thyme::WritePerfNodeBudget(bw, thread_node[i], threads[i]->period_ns(),
                                   threads[i]->deadline_ns());
        std::fwrite(bw.data() + 4, 1, bw.size() - 4, pf);
        dump_sampler(pf, threads[i]->perf(), thread_node[i]);
        dump_work(pf, threads[i]->work_log(), thread_node[i]);
      }
      for (std::size_t i = 0; i < ev_thread_count; ++i) {
        thyme::PerfLogWriter bw(0, 0);
        thyme::WritePerfNodeBudget(bw, ev_node[i], ev_threads[i]->period_ns(),
                                   ev_threads[i]->deadline_ns());
        std::fwrite(bw.data() + 4, 1, bw.size() - 4, pf);
        dump_sampler(pf, ev_threads[i]->perf(), ev_node[i]);
        dump_work(pf, ev_threads[i]->work_log(), ev_node[i]);
      }
#ifdef THYME_GRAPH_HAS_EVENT_NODES
      for (std::size_t i = 0; i < ev_thread_count; ++i) {
        thyme::PerfLogWriter proc_bw(0, 0);
        ev_proc[i].FlushNew(proc_bw, ev_node[i]);
        std::fwrite(proc_bw.data() + 4, 1, proc_bw.size() - 4, pf);
      }
#endif
      std::fclose(pf);
    }
    std::snprintf(path, sizeof(path), "%s_edge_%s_%d.bin", perf_prefix, role,
                  static_cast<int>(getpid()));
    if (FILE* pf = std::fopen(path, "wb")) {
      std::fwrite("T9P1", 1, 4, pf);
      WritePerfManifest(pf);
      for (std::size_t i = 0; i < thread_count; ++i) {
        dump_sampler(pf, threads[i]->perf_edge(), 0);
      }
      for (std::size_t i = 0; i < ev_thread_count; ++i) {
        dump_sampler(pf, ev_threads[i]->perf_edge(), 0);
      }
      std::fclose(pf);
    }
#ifdef THYME_GRAPH_HAS_EVENT_NODES
    std::snprintf(path, sizeof(path), "%s_cbwork_%s_%d.bin", perf_prefix, role,
                  static_cast<int>(getpid()));
    if (FILE* pf = std::fopen(path, "wb")) {
      std::fwrite("T9P1", 1, 4, pf);
      WritePerfManifest(pf);
      for (std::size_t i = 0; i < ev_thread_count; ++i) {
        dump_sampler(pf, ev_threads[i]->cb_work(), 0);
      }
      std::fclose(pf);
    }
#endif
  }
  for (std::size_t i = 0; i < thread_count; ++i) {
    uint64_t lc = threads[i]->loop_count();
    if (lc < min_loops) min_loops = lc;
    delete threads[i];
  }
  // Event nodes are passive (notification sinks): a role with zero handled
  // events is alive and wired, just idle, so they do not drive min_loops.
  for (std::size_t i = 0; i < ev_thread_count; ++i) {
    delete ev_threads[i];
  }

  if (proc_idx < 0 ||
      static_cast<std::size_t>(proc_idx) >= MpControl::kMaxProcSlots) {
    std::fprintf(stderr, "RunRole: invalid proc_idx %d\n", proc_idx);
    return 2;
  }
  ctrl->proc_loops[proc_idx].store(min_loops, std::memory_order_release);

#ifdef THYME_GRAPH_HAS_EVENT_NODES
  ClearEventRecordTaps(ev);
#endif
#ifdef THYME_GRAPH_HAS_BROADCAST
  ClearBroadcastRecordTaps(bc);
#endif
  if (record_shm != nullptr && record_shm[0] != '\0') {
    record_mirror::Shutdown(false);
  }
  record_trigger::SetControl(nullptr);
  if (const char* e = std::getenv("T9_MP_ROLE_EXIT_CODE")) {
    const int code = std::atoi(e);
    if (code > 0 && code < 255) return code;
  }
  return min_loops >= 1 ? 0 : 1;
}
