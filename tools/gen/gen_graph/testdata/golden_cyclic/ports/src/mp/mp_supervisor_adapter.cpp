// App-side supervisor playbook. The thyme::app::MpSupervisorSession skeleton
// owns the SHM set + mirror create/attach, role fork/exec, cooperative stop,
// child reap, and counter/health collection (the "how"); this file composes it
// with the app policy (the "what"): run duration, trigger-relay polling,
// success-gate composition, the printf report, and the exit code.
#include "mp/mp_supervisor_adapter.h"

#include "generated/graph_ir.h"
#include "generated/shm_slots.h"
#include "generated/mp_unknown.h"
#ifdef THYME_GRAPH_HAS_RECORD
#include "generated/record_table.h"
#endif
#include "mp/mp_layout.h"
#include "mp/mp_tables.h"
#include "support/trigger_relay.h"
#include "thyme/app/mp_supervisor.h"
#include "thyme/app/record_mirror.h"
#include "thyme/runtime/app.h"
#include "thyme/runtime/host_sampler.h"
#include "thyme/record/record_schema.h"
#include "generated/record_manifest.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

const char* RoleExecutable(void*, const char* role, const char* self_exe) {
  static std::string path;
  const std::string env_name = std::string("THYME_ROLE_EXECUTABLE_") + role;
  if (const char* override_path = std::getenv(env_name.c_str())) {
    if (override_path[0] != '\0') return override_path;
  }
  path = self_exe;
  const auto slash = path.find_last_of('/');
  const std::string executable = std::string("app_") + role;
  path = slash == std::string::npos ? executable : path.substr(0, slash + 1) + executable;
  return access(path.c_str(), X_OK) == 0 ? path.c_str() : self_exe;
}

// Generated rows -> thyme::app descriptor rows, built once. The generated
// structs carry extra fields (e.g. channel endpoints by id) that the
// app-side descriptors do not, so the mapping is explicit. MpSupervisorConfig
// only borrows from these arrays, hence they must outlive the Run call. The
// SHM channel rows and per-kind slot sizes are shared with the role side via
// mp_tables.h.
struct SupervisorTables {
  std::array<thyme::app::MpProcess, std::size(kGraphProcesses)> processes{};
  std::array<thyme::app::MpNode, std::size(kGraphNodes)> nodes{};
  std::array<thyme::app::MpRecordGate, std::size(kGraphRecordGates)> record_gates{};
  // Cross-process frame gates from the graph's mp_gate edge annotations
  // (S-D4). kGraphCrossGateCount is authoritative; kGraphCrossGates carries
  // a placeholder row when the graph gates nothing.
  std::array<thyme::app::MpCrossGate, std::size(kGraphCrossGates)> cross_gates{};
  // The supervisor creates the record mirror from this table in record mode.
#ifdef THYME_GRAPH_HAS_RECORD
  thyme::ThymeRecordTable record_table = GetThymeRecordTable();
#endif
};

const SupervisorTables& Tables() {
  static const SupervisorTables tables = []() {
    SupervisorTables t;
    for (std::size_t i = 0; i < std::size(kGraphProcesses); ++i) {
      t.processes[i] = {kGraphProcesses[i].id, kGraphProcesses[i].role};
    }
    for (std::size_t i = 0; i < std::size(kGraphNodes); ++i) {
      t.nodes[i] = {kGraphNodes[i].id, kGraphNodes[i].type,
                    kGraphNodes[i].process, kGraphNodes[i].hz};
    }
    for (std::size_t i = 0; i < std::size(kGraphRecordGates); ++i) {
      t.record_gates[i] = {kGraphRecordGates[i].process,
                           kGraphRecordGates[i].slot};
    }
    for (std::size_t i = 0; i < kGraphCrossGateCount; ++i) {
      t.cross_gates[i] = {static_cast<int>(kGraphCrossGates[i].kind),
                          kGraphCrossGates[i].counter_slot};
    }
    return t;
  }();
  return tables;
}

}  // namespace

int RunSupervisor(const char* self_exe, int seconds, bool record,
                  const char* record_shm, const char* relay_recorder_socket) {
  const SupervisorTables& tables = Tables();
  const auto shm_channels = MpShmChannels();
  const auto slot_specs = MpSlotSpecs();
  thyme::app::MpSupervisorConfig cfg;
  cfg.processes = {tables.processes.data(), tables.processes.size()};
  cfg.nodes = {tables.nodes.data(), tables.nodes.size()};
  cfg.shm_channels = shm_channels;
  cfg.record_gates = {tables.record_gates.data(), tables.record_gates.size()};
  cfg.slot_specs = {slot_specs.data(), slot_specs.size()};
  cfg.health_n = kMpHealthN;
  cfg.shm_prefix_fmt = kGraphShmPrefixFmt;
  cfg.graph_hash = kGraphHash;
  cfg.role_executable = RoleExecutable;
  // Spec §6: every gated cross-process channel must see >= 1 frame. The gate
  // table comes from graph.yaml mp_gate edge annotations (S-D4), not from any
  // product-specific kind mapping.
  cfg.cross_gates = {tables.cross_gates.data(),
                     static_cast<std::size_t>(kGraphCrossGateCount)};
#ifdef THYME_GRAPH_HAS_RECORD
  cfg.record_table = &tables.record_table;
  cfg.record_ring_specs = kRecordMirrorRingSpecs;
#endif

  // Queue + wake regions match LatestSlot ownership: supervisor Prepare
  // (Unlink+Init) before fork; roles only Open. Unlink after Run so leftovers
  // from a prior crash are cleared by the next Prepare as well. Emitted only
  // for graphs with queue-delivery event edges (THYME_GRAPH_HAS_EVENT_SHM).
  // Broadcast SPMC regions (THYME_GRAPH_HAS_BROADCAST) follow the same
  // supervisor-Prepare / role-Open split; they are not LatestSlot channels.
#if defined(THYME_GRAPH_HAS_EVENT_SHM) || defined(THYME_GRAPH_HAS_BROADCAST) || \
    defined(THYME_GRAPH_HAS_BIGFRAME)
  char prefix[24];
  std::snprintf(prefix, sizeof(prefix), kGraphShmPrefixFmt,
                static_cast<int>(getpid()));
#endif
#ifdef THYME_GRAPH_HAS_EVENT_SHM
  if (!PrepareEventShmChannels(prefix)) {
    std::fprintf(stderr, "supervisor: PrepareEventShmChannels failed for %s\n",
                 prefix);
    UnlinkEventShmChannels(prefix);
#ifdef THYME_GRAPH_HAS_BROADCAST
    UnlinkBroadcastShmChannels(prefix);
#endif
#ifdef THYME_GRAPH_HAS_BIGFRAME
    UnlinkPinnedSlotShmChannels(prefix);
#endif
    return 1;
  }
#endif
#ifdef THYME_GRAPH_HAS_BROADCAST
  if (!PrepareBroadcastShmChannels(prefix)) {
    std::fprintf(stderr,
                 "supervisor: PrepareBroadcastShmChannels failed for %s\n",
                 prefix);
#ifdef THYME_GRAPH_HAS_EVENT_SHM
    UnlinkEventShmChannels(prefix);
#endif
    UnlinkBroadcastShmChannels(prefix);
    return 1;
  }
#endif
#ifdef THYME_GRAPH_HAS_BIGFRAME
  if (!PreparePinnedSlotShmChannels(prefix)) {
    std::fprintf(stderr,
                 "supervisor: PreparePinnedSlotShmChannels failed for %s\n",
                 prefix);
#ifdef THYME_GRAPH_HAS_EVENT_SHM
    UnlinkEventShmChannels(prefix);
#endif
#ifdef THYME_GRAPH_HAS_BROADCAST
    UnlinkBroadcastShmChannels(prefix);
#endif
    UnlinkPinnedSlotShmChannels(prefix);
    return 1;
  }
#endif

  if (seconds <= 0) seconds = 2;

  thyme::app::MpSupervisorSession session(cfg);
  if (!session.Prepare(record ? (record_shm != nullptr ? record_shm : "")
                              : nullptr)) {
#ifdef THYME_GRAPH_HAS_EVENT_SHM
    UnlinkEventShmChannels(prefix);
#endif
#ifdef THYME_GRAPH_HAS_BROADCAST
    UnlinkBroadcastShmChannels(prefix);
#endif
#ifdef THYME_GRAPH_HAS_BIGFRAME
    UnlinkPinnedSlotShmChannels(prefix);
#endif
    return 1;
  }
  if (record) {
    std::printf("record mirror: %s\n", session.mirror_name());
  }

  thyme::app::MpControl* ctrl = session.ctrl();
  if (relay_recorder_socket != nullptr && relay_recorder_socket[0] != '\0') {
    std::printf("trigger relay: recorder socket %s\n", relay_recorder_socket);
  }

  if (!session.ForkRoles(self_exe)) {
#ifdef THYME_GRAPH_HAS_EVENT_SHM
    UnlinkEventShmChannels(prefix);
#endif
#ifdef THYME_GRAPH_HAS_BROADCAST
    UnlinkBroadcastShmChannels(prefix);
#endif
#ifdef THYME_GRAPH_HAS_BIGFRAME
    UnlinkPinnedSlotShmChannels(prefix);
#endif
    return 1;
  }

  // OBS-14: machine-level counters live once per machine, not per role, so
  // the supervisor (the only long-lived process) owns the sampler and writes
  // <prefix>_host_<pid>.bin next to the role perf files.
  thyme::HostDumpThread host_dump;
  if (const char* perf_prefix = std::getenv("T9_PERF_LOG")) {
    char host_path[192];
    std::snprintf(host_path, sizeof(host_path), "%s_host_%d.bin", perf_prefix,
                  static_cast<int>(getpid()));
    if (!host_dump.Start(host_path, 1000000000ULL, kRecordManifestHash,
                         thyme::schema::kManifestVersion)) {
      std::fprintf(stderr, "supervisor: host perf dump disabled (%s)\n",
                   host_path);
    }
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  trigger_relay::RelayState relay_state;
  while (!thyme::App::stop_requested() &&
         std::chrono::steady_clock::now() < deadline) {
    // OPS-01: reap/respawn roles when the graph opts into restart.
    session.PollRestarts();
    if (relay_recorder_socket != nullptr && relay_recorder_socket[0] != '\0') {
      trigger_relay::PollAndRelay(ctrl, relay_recorder_socket, relay_state);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  host_dump.Stop();  // final sample + close before roles are reaped
  session.SignalStop();

  bool all_ok = session.ReapChildren();

  uint64_t proc_loops[thyme::app::MpControl::kMaxProcSlots]{};
  uint64_t cross_frames[thyme::app::MpControl::kMaxCrossFrameSlots]{};
  session.Collect(proc_loops, cross_frames);

  const thyme::SharedHealthPage* health_page = session.health_page();
  if (health_page == nullptr || !health_page->Validate()) {
    std::fprintf(stderr, "supervisor: invalid SharedHealthPage\n");
    all_ok = false;
  }
  std::printf("supervisor:");
  for (const auto& gate : cfg.cross_gates) {
    const std::size_t slot = gate.counter_slot;
    const uint64_t frames =
        slot < thyme::app::MpControl::kMaxCrossFrameSlots ? cross_frames[slot]
                                                          : 0;
    std::printf(" cross_frames[%u]=%llu", static_cast<unsigned>(slot),
                (unsigned long long)frames);
  }
  for (std::size_t i = 0; i < cfg.processes.size(); ++i) {
    std::printf(" %s_loops=%llu", cfg.processes[i].id,
                (unsigned long long)proc_loops[i]);
  }
  std::printf("\n");
  if (relay_recorder_socket != nullptr && relay_recorder_socket[0] != '\0') {
    std::printf(
        "supervisor: trigger relay sent=%llu debounced=%llu ack_seq=%u\n",
        static_cast<unsigned long long>(
            ctrl->trigger_relay_sent.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(
            ctrl->trigger_relay_debounced.load(std::memory_order_acquire)),
        ctrl->trigger_ack_seq.load(std::memory_order_acquire));
  }
  if (health_page != nullptr && health_page->Validate()) {
    const uint32_t worker_count = health_page->worker_count();
    const std::size_t health_n =
        worker_count > 0 ? static_cast<std::size_t>(worker_count)
                         : static_cast<std::size_t>(cfg.health_n);
    for (std::size_t i = 0; i < health_n; ++i) {
      const thyme::HealthSlot& hs = health_page->slots[i];
      std::printf(
          "supervisor: health[%zu] alive=%u state=%u miss=%u drop=%llu "
          "err=%u\n",
          i, hs.alive_counter.load(std::memory_order_acquire),
          hs.state.load(std::memory_order_acquire),
          hs.deadline_miss_cnt.load(std::memory_order_acquire),
          static_cast<unsigned long long>(
              hs.drop_cnt.load(std::memory_order_acquire)),
          hs.error_code.load(std::memory_order_acquire));
    }
  }
  thyme::app::MpHealthScanSummary health_scan{};
  if (health_page != nullptr && health_page->Validate()) {
    health_scan = thyme::app::ScanHealthPage(cfg, *health_page);
    std::printf(
        "supervisor: health_scan workers=%zu drops=%llu miss=%u err=%u\n",
        health_scan.workers_scanned,
        static_cast<unsigned long long>(health_scan.total_drops),
        health_scan.total_deadline_miss, health_scan.total_errors);
  }
  std::fflush(stdout);

  // Mirror gate + report, before Cleanup shuts the mirror down.
  bool mirror_ok = true;
  if (record) {
    thyme::RecordMirrorRingPage* page = thyme::app::record_mirror::RingPage();
    if (page == nullptr || !page->Validate()) {
      mirror_ok = false;
    } else {
      auto slot_written = [&](uint16_t sid) -> bool {
        return page->RingHead(sid) != 0;
      };
      // One gate per producer process, derived from graph IR (edge order).
      for (std::size_t i = 0; i < cfg.record_gates.size(); ++i) {
        if (!slot_written(cfg.record_gates[i].slot)) mirror_ok = false;
      }
      const uint64_t drops =
          page->mirror_odd_skip_count() + page->oversize_drop_count();
      std::printf("supervisor: mirror_drops=%llu oversize_drops=%llu",
                  static_cast<unsigned long long>(drops),
                  static_cast<unsigned long long>(page->oversize_drop_count()));
      for (std::size_t i = 0; i < cfg.record_gates.size(); ++i) {
        const uint16_t sid = cfg.record_gates[i].slot;
        std::printf(" %s_slot%u=%llu", cfg.record_gates[i].process,
                    static_cast<unsigned>(sid),
                    static_cast<unsigned long long>(page->RingHead(sid)));
      }
      std::printf("\n");
    }
  }

  session.Cleanup();

  // Spec §6: success = all children exit 0 AND each existing process meets
  // its own per-process loop bound. See LoopsOk: proc_loops[i] >=
  // MinLoopsForProcess(process i). Every configured cross-process frame gate
  // must have seen >= 1 frame (CrossGatesOk; empty table passes).
  const bool loops_ok = thyme::app::LoopsOk(
      cfg, seconds,
      thyme::span<const uint64_t>(proc_loops, cfg.processes.size()));
  const bool cross_ok = thyme::app::CrossGatesOk(
      cfg, thyme::span<const uint64_t>(
               cross_frames, thyme::app::MpControl::kMaxCrossFrameSlots));

  const bool health_strict =
      cfg.health_scan_strict ||
      std::getenv("T9_MP_HEALTH_STRICT") != nullptr;
  const bool health_ok =
      !health_strict ||
      thyme::app::HealthScanOk(health_scan, /*strict_drops=*/true,
                               /*strict_errors=*/true,
                               /*strict_deadline_miss=*/false);

  const bool ok = all_ok && loops_ok && cross_ok && mirror_ok && health_ok;
  if (!ok) {
    std::fprintf(stderr,
                 "supervisor: exit gates all_ok=%d loops_ok=%d cross_ok=%d "
                 "mirror_ok=%d health_ok=%d\n",
                 all_ok, loops_ok, cross_ok, mirror_ok, health_ok);
  }
#ifdef THYME_GRAPH_HAS_EVENT_SHM
  UnlinkEventShmChannels(prefix);
#endif
#ifdef THYME_GRAPH_HAS_BROADCAST
  UnlinkBroadcastShmChannels(prefix);
#endif
#ifdef THYME_GRAPH_HAS_BIGFRAME
  UnlinkPinnedSlotShmChannels(prefix);
#endif
  return ok ? 0 : 1;
}
