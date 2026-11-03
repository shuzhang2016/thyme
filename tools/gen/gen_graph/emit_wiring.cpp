#include "emit.h"

#include "emit_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace gen_emit;

bool EmitMpUnknown(const std::string& tmp_dir, const Graph& g,
                   const std::string& tpl_dir) {
  TemplateCatalog cat;
  if (!LoadCatalog(tpl_dir, &cat)) return false;
  const std::string body = BuildMpUnknown(g, cat);
  if (body.empty()) {
    std::fprintf(stderr, "gen_graph: failed to build mp_unknown.h\n");
    return false;
  }
  return WriteFile(fs::path(tmp_dir) / "mp_unknown.h", body);
}

bool EmitMpRoles(const std::string& tmp_dir, const Graph& g,
                 const TemplateCatalog& cat) {
  std::vector<uint32_t> worker_hz;
  bool any_noworker = false;
  for (const auto& n : g.nodes) {
    const KnownCat* k = FindKnown(cat, n.type);
    const bool noworker =
        (k != nullptr && k->flags.find("noworker") != std::string::npos);
    if (noworker) {
      any_noworker = true;
      continue;
    }
    worker_hz.push_back(static_cast<uint32_t>(n.hz));
  }

  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n\n";
  os << "#include \"generated/graph_ir.h\"\n";
  os << "#include \"generated/mp_ports.h\"\n";
  os << "#include \"generated/mp_role_locals.h\"\n";
  if (HasEventNode(g) || HasBroadcastEdge(g, cat)) {
    os << "#include \"generated/mp_unknown.h\"\n";
  }
  os << "#include \"thyme/runtime/cyclic_thread.h\"\n";
  os << "#include \"thyme/runtime/health_slot.h\"\n";
  os << cat.mp_includes;
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    if (!IsKnownType(cat, g.nodes[i].type)) continue;
    os << "#include \"nodes/" << g.nodes[i].id << ".h\"\n";
  }
  os << "\n";

  if (any_noworker) {
    os << "// Worker frequencies for the noworker (health_reporter) node, in "
          "yaml order\n";
    os << "// excluding noworker types. Borrowed by the health_reporter thread "
          "(inline\n";
    os << "// constexpr: one copy per TU).\n";
    if (worker_hz.empty()) {
      os << "inline constexpr uint32_t kMpWorkerHz[1] = {0};\n";
    } else {
      os << "inline constexpr uint32_t kMpWorkerHz[] = {";
      for (std::size_t i = 0; i < worker_hz.size(); ++i) {
        if (i) os << ", ";
        os << worker_hz[i];
      }
      os << "};\n";
    }
    os << "inline constexpr std::size_t kMpWorkerCount = " << worker_hz.size()
       << ";\n\n";
  }

  os << "// Constructs the generated thread for a known node type from RoleLocals.\n";
  os << "// Returns nullptr for indices that are not known nodes (the role\n";
  os << "// falls back to MakeUnknownThread); unknown types stay a compile-time\n";
  os << "// gap only if the graph names a type nobody wired.\n";
  os << "inline thyme::CyclicThread* MakeKnownThread(\n";
  os << "    std::size_t node_index,\n";
  os << "    RoleLocals& l";
  if (HasEventNode(g)) {
    os << ",\n    EventTransports& ev";
  }
  if (HasBroadcastEdge(g, cat)) {
    os << ",\n    BroadcastTransports& bc";
  }
  if (HasPinnedSlotShmEdge(g, cat)) {
    os << ",\n    PinnedSlotTransports& bf";
  }
  os << ",\n    thyme::HealthSlot* health_slots) {\n";

  std::vector<std::size_t> known_idx;
  std::vector<std::size_t> noworker_idx;
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    const KnownCat* k = FindKnown(cat, g.nodes[i].type);
    if (k == nullptr) continue;
    if (IsEventNode(g.nodes[i])) continue;
    if (k->flags.find("noworker") != std::string::npos) noworker_idx.push_back(i);
    else known_idx.push_back(i);
  }
  if (known_idx.empty() && noworker_idx.empty()) {
    os << "  (void)node_index;\n";
    os << "  (void)l;\n";
    if (HasEventNode(g)) os << "  (void)ev;\n";
    if (HasBroadcastEdge(g, cat)) os << "  (void)bc;\n";
  if (HasPinnedSlotShmEdge(g, cat)) os << "  (void)bf;\n";
    os << "  (void)health_slots;\n";
    os << "  return nullptr;\n";
    os << "}\n";
    return WriteFile(fs::path(tmp_dir) / "mp_roles.h", os.str());
  }

  // EventTransports is only referenced by known producers that have graph
  // event out-edges; void-cast once so unused-parameter warnings stay quiet.
  if (HasEventNode(g)) {
    os << "  (void)ev;\n";
  }
  if (HasBroadcastEdge(g, cat)) {
    os << "  (void)bc;\n";
  }
  if (HasPinnedSlotShmEdge(g, cat)) {
    os << "  (void)bf;\n";
  }
  os << "  (void)l;\n";
  const std::string ev_prefix = HasEventNode(g) ? "ev." : "";
  const std::string bc_prefix = HasBroadcastEdge(g, cat) ? "bc." : "";
  const std::string loc = "l";
  os << "  switch (node_index) {\n";
  for (const auto i : known_idx) {
    const auto& n = g.nodes[i];
    const KnownCat* k = FindKnown(cat, n.type);
    bool ok = false;
    const std::string grouped = KnownCtorGrouped(
        g, cat, n, "&health_slots[" + std::to_string(i) + "]", &ok, ev_prefix,
        loc, bc_prefix, "bf.");
    if (!ok) return false;
    os << "    case " << i << ": return new " << k->cls << "(" << grouped
       << ");\n";
  }
  for (const auto i : noworker_idx) {
    const auto& n = g.nodes[i];
    const KnownCat* k = FindKnown(cat, n.type);
    os << "    case " << i << ": return new " << k->cls
       << "(health_slots, kMpWorkerCount, kMpWorkerHz, &health_slots[kGraphHealthN - 1]);\n";
  }
  os << "    default: return nullptr;\n";
  os << "  }\n";
  os << "}\n";
  return WriteFile(fs::path(tmp_dir) / "mp_roles.h", os.str());
}


bool EmitMpPorts(const std::string& tmp_dir, const Graph& g,
                 const TemplateCatalog& cat) {
  const auto pools = UniquePools(g, cat);
  bool any_index = false;
  for (const auto& p : pools) {
    if (p.index_pipe) { any_index = true; break; }
  }
  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n\n";
  os << "#include \"generated/graph_types.h\"\n";
  os << "#include \"thyme/runtime/health_slot.h\"\n";
  if (!pools.empty()) {
    // Queue ports wrap the pool + index ring pair; the adapter header carries
    // the only template that names both.
    os << "#include \"thyme/runtime/local_index_queue.h\"\n";
  }
  if (any_index) os << "#include \"support/index_pipe.h\"\n";
  if (HasEventNode(g)) {
    os << "#include \"thyme/runtime/notif_pipe.h\"\n";
    os << "#include \"thyme/runtime/shm_spsc_ring.h\"\n";
  }
  if (HasBroadcastEdge(g, cat)) {
    os << "#include \"thyme/runtime/shm_spmc_ring.h\"\n";
  }
  if (HasEventNode(g) || HasBroadcastEdge(g, cat) || HasPinnedSlotEdge(g, cat)) {
    os << "#include \"thyme/runtime/channel.h\"\n";
  }
  if (HasEventNode(g) || HasBroadcastEdge(g, cat) || HasPinnedSlotEdge(g, cat)) {
    AppendDualChannelRefDefinition(os);
  }
  if (HasEventNode(g) || HasBroadcastEdge(g, cat)) {
    AppendFrameRefDefinition(os);
  }
  if (HasPinnedSlotEdge(g, cat)) {
    AppendPinnedSlotRefDefinition(os);
  }
  os << "\n";
  os << "// Shared per-node construction context: the own health slot plus the\n";
  os << "// catalog extra buffers. Constructed once per node at the call site;\n";
  os << "// generated bases take (const <type>Ports&, const MpContext&) instead\n";
  os << "// of 15-26 flat pool/ring/extra arguments.\n";
  os << "struct MpContext {\n";
  os << "  thyme::HealthSlot* health;\n";
  for (const auto& x : cat.extras) os << "  " << x.cpp_type << "* " << x.name << ";\n";
  os << "};\n\n";
  std::set<std::string> emitted_known;
  std::set<std::string> emitted_unknown;
  for (const auto& n : g.nodes) {
    const KnownCat* k = FindKnown(cat, n.type);
    if (k != nullptr) {
      if (k->flags.find("noworker") != std::string::npos) continue;
      if (!emitted_known.insert(n.type).second) continue;
      if (!AppendKnownPortsStruct(os, g, cat, n.type)) return false;
      continue;
    }
    const std::string ports_name = PascalCase(n.shard_count > 1 ? n.id : n.type);
    if (!emitted_unknown.insert(ports_name).second) continue;
    if (!AppendUnknownPortsStruct(os, g, n, cat)) return false;
  }
  return WriteFile(fs::path(tmp_dir) / "mp_ports.h", os.str());
}

bool EmitWiringH(const std::string& tmp_dir, const Graph& g,
                 const std::string& tpl_dir) {
  TemplateCatalog cat;
  if (!LoadCatalog(tpl_dir, &cat)) return false;
  const auto pools = UniquePools(g, cat);
  bool any_index = false;
  for (const auto& p : pools) if (p.index_pipe) { any_index = true; break; }

  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n\n";
  for (const auto& n : g.nodes) os << "#include \"nodes/" << n.id << ".h\"\n";
  os << "#include \"generated/graph_types.h\"\n";
  os << "#include \"generated/graph_ir.h\"\n";
  os << "#include \"generated/graph_check.h\"\n";
  os << "#include \"generated/mp_ports.h\"\n";
  if (any_index) os << "#include \"support/index_pipe.h\"\n";
  if (HasEventNode(g)) {
    os << "#include \"thyme/runtime/notif_pipe.h\"\n";
  }
  if (HasLocalFrameHub(g, cat)) {
    os << "#include \"thyme/runtime/local_hub.h\"\n";
  }
  if (HasBroadcastEdge(g, cat)) {
    os << "#include \"generated/mp_unknown.h\"\n";
  }
  os << "#include \"thyme/runtime/app.h\"\n";
  os << "#include \"thyme/runtime/health_slot.h\"\n\n#include <array>\n#include <memory>\n\n";
  os << "struct Pipeline {\n";
  for (const auto& p : pools)
    os << "  " << p.pool_type << " " << p.prefix << "_pool; Ring<" << p.pool_type << "> " << p.prefix << "_ring;\n";
  os << "\n";
  if (HasEventNode(g)) {
    // Single-process event plumbing: direct edges own a pipe; local fan-out
    // groups own one input and N outputs behind a shared dispatcher.
    os << "  struct SpEventTransports {\n";
    for (const auto& e : g.edges) {
      if (!EdgeUsesEventPipe(g, e) || IsLocalFrameHubEdge(g, cat, e)) continue;
      const std::string cap = std::to_string(EventEdgeCap(g, e));
      os << "    thyme::NotifPipe<" << Framed(e.cpp_type) << ", " << cap << "> "
         << EventPipeMember(e) << ";\n";
    }
    for (const auto& hub : BuildLocalFrameHubs(g, cat)) {
      os << "    " << LocalFrameHubType(g, hub) << " " << LocalFrameHubMember(hub)
         << ";\n";
    }
    os << "  };\n";
    if (HasLocalFrameHub(g, cat)) {
      os << "  class SpHubDispatcher final : public thyme::EventThread {\n"
            "   public:\n"
            "    explicit SpHubDispatcher(SpEventTransports* transports)\n"
            "        : thyme::EventThread(\"local_hubs\", &health_, 100), transports_(transports) {\n"
            "      thyme::Waitable* ws[] = {";
      const auto hubs = BuildLocalFrameHubs(g, cat);
      for (std::size_t i = 0; i < hubs.size(); ++i) {
        if (i != 0) os << ", ";
        os << "&transports->" << LocalFrameHubMember(hubs[i]);
      }
      os << "};\n      SetWaitInputs(ws, sizeof(ws) / sizeof(ws[0]));\n    }\n"
            "    void Dispatch() override {\n";
      for (const auto& hub : hubs) {
        os << "      transports_->" << LocalFrameHubMember(hub) << ".Dispatch();\n";
      }
      os << "    }\n   private:\n    thyme::HealthSlot health_{};\n"
            "    SpEventTransports* transports_;\n  };\n";
    }
    os << "  SpEventTransports event_transports_;\n";
    if (HasLocalFrameHub(g, cat)) os << "  SpHubDispatcher hub_dispatcher_{&event_transports_};\n";
    os << "\n";
  }
  if (HasBroadcastEdge(g, cat)) {
    os << "  BroadcastTransports broadcast_transports_;\n\n";
  }
  AppendSpPinnedSlotFields(os, g, cat);
  for (const auto& x : cat.extras) {
    os << "  " << x.cpp_type << " " << x.name << ";\n";
  }
  if (!cat.extras.empty()) os << "\n";
  os << "  std::array<thyme::HealthSlot, kGraphHealthN> health{};\n\n";
  for (const auto& p : pools) if (p.index_pipe) os << "  RingIndexPipe " << p.prefix << "_pipe;\n";
  os << "\n";
  for (const auto& n : g.nodes) {
    const KnownCat* k = FindKnown(cat, n.type);
    if (k != nullptr) {
      os << "  " << HandThreadClassName(cat, n) << " "
         << NodeMemberName(g, cat, n) << ";\n";
    } else {
      os << "  " << HandThreadClassName(cat, n) << " " << n.id << ";\n";
    }
  }
  os << "  thyme::App app_;\n\n  static std::unique_ptr<Pipeline> Create();\n  bool Start();\n  void RequestStop();\n  void Join();\n";
  os << "  // Node-id dispatchers (emitted from kGraphNodes): graph-agnostic access\n";
  os << "  // to the generated thread members without naming them at the call site.\n";
  os << "  thyme::CyclicThread* ThreadByNodeId(const char* id);\n";
  if (HasEventNode(g)) {
    os << "  thyme::EventThread* EventThreadByNodeId(const char* id);\n";
  }
  os << "\n private:\n  Pipeline();\n";
  os << "};\n";
  return WriteFile(fs::path(tmp_dir) / "wiring.h", os.str());
}

bool EmitWiringCpp(const std::string& tmp_dir, const Graph& g,
                   const std::string& tpl_dir) {
  TemplateCatalog cat;
  if (!LoadCatalog(tpl_dir, &cat)) return false;
  const auto pools = UniquePools(g, cat);
  bool has_transport_record_tap = false;
  for (const auto& e : g.edges) {
    if (e.record && !IsPinnedSlotDelivery(g, cat, e) &&
        (EdgeUsesEventPipe(g, e) || IsBroadcastDelivery(e))) {
      has_transport_record_tap = true;
      break;
    }
  }
  std::vector<int> worker_hz;
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    const auto& n = g.nodes[i];
    const KnownCat* k = FindKnown(cat, n.type);
    bool noworker = (k != nullptr && k->flags.find("noworker") != std::string::npos);
    if (!noworker) worker_hz.push_back(n.hz);
  }
  std::ostringstream hz_list;
  for (std::size_t i = 0; i < worker_hz.size(); ++i) {
    if (i) hz_list << ", ";
    hz_list << worker_hz[i];
  }

  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#include \"generated/wiring.h\"\n\n";
  if (has_transport_record_tap) {
    os << "#include \"support/record_mirror_ctx.h\"\n\n";
  }
  if (UsesSchedulingSetup(tpl_dir)) {
    os << "#include \"support/scheduling_setup.h\"\n\n";
  }
  os << "#include <cstdio>\n#include <cstdlib>\n#include <cstring>\n\nnamespace {\n\n";
  if (!cat.hz_comment.empty()) {
    std::string cmt = ReplaceAll(cat.hz_comment, "{n}", std::to_string(worker_hz.size()));
    os << cmt;
    if (cmt.empty() || cmt.back() != '\n') os << "\n";
  } else {
    os << "// Worker frequencies for the " << worker_hz.size() << " worker threads. Outlives\n";
    os << "// the health reporter thread that borrows it (file-scope static const).\n";
  }
  os << "static const uint32_t kWorkerHz[" << worker_hz.size() << "] = {" << hz_list.str() << "};\n\n";
  os << "}  // namespace\n\nstd::unique_ptr<Pipeline> Pipeline::Create() {\n";
  os << "  auto pipeline = std::unique_ptr<Pipeline>(new Pipeline());\n";
  AppendSpPinnedSlotCreate(os, g, cat);
  for (const auto& hub : BuildLocalFrameHubs(g, cat)) {
    for (const auto* edge : hub.edges) {
      std::size_t health_index = 0;
      for (std::size_t i = 0; i < g.nodes.size(); ++i) {
        if (g.nodes[i].id == edge->to) health_index = i;
      }
      os << "  if (!pipeline->event_transports_." << LocalFrameHubMember(hub)
         << ".BindOutput(" << LocalFrameHubOutputIndex(hub, *edge)
         << ", &pipeline->health[" << health_index << "])) return nullptr;\n";
    }
  }
  os << "  return pipeline;\n}\n\nPipeline::Pipeline()\n    : ";
  bool first = true;
  for (const auto& p : pools) {
    if (!p.index_pipe) continue;
    if (!first) os << ",\n      ";
    os << p.prefix << "_pipe(" << p.prefix << "_ring)";
    first = false;
  }
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    const auto& n = g.nodes[i];
    const KnownCat* k = FindKnown(cat, n.type);
    if (!first) os << ",\n      ";
    if (k) {
      if (k->flags.find("noworker") != std::string::npos)
        os << NodeMemberName(g, cat, n)
           << "(health.data(), " << worker_hz.size() << ", kWorkerHz, &health[kGraphHealthN - 1])";
      else {
        bool ok = false;
        const std::string grouped = KnownCtorGrouped(
            g, cat, n, "&health[" + std::to_string(i) + "]", &ok,
            HasEventNode(g) ? SpEventTransportPrefix : "",
            /*locals_prefix=*/"",
            HasBroadcastEdge(g, cat) ? SpBroadcastTransportPrefix : "",
            "pinned_slot_transports_.");
        if (!ok) return false;
        os << NodeMemberName(g, cat, n) << "(" << grouped << ")";
      }
    } else {
      os << n.id << "("
         << UnknownCtorGrouped(
                g, n, cat, "&health[" + std::to_string(i) + "]",
                HasEventNode(g) ? SpEventTransportPrefix : "",
                /*locals_prefix=*/"",
                HasBroadcastEdge(g, cat) ? SpBroadcastTransportPrefix : "",
                "pinned_slot_transports_.")
         << ")";
    }
    first = false;
  }
  os << " {\n";
  if (has_transport_record_tap) {
    os << "  if (auto* record_page = record_mirror::Page(); record_page != nullptr) {\n";
    std::set<std::string> tapped_broadcast;
    std::set<std::string> tapped_hubs;
    for (const auto& e : g.edges) {
      if (!e.record || IsPinnedSlotDelivery(g, cat, e) ||
          (!EdgeUsesEventPipe(g, e) && !IsBroadcastDelivery(e))) {
        continue;
      }
      if (IsBroadcastDelivery(e) && !tapped_broadcast.insert(e.channel).second) {
        continue;
      }
      if (IsLocalFrameHubEdge(g, cat, e) && !tapped_hubs.insert(e.channel).second) {
        continue;
      }
      os << "    ";
      if (IsLocalFrameHubEdge(g, cat, e)) {
        LocalFrameHub hub;
        if (!FindLocalFrameHub(g, cat, e, &hub)) return false;
        os << SpEventTransportPrefix << LocalFrameHubMember(hub) << ".input()";
      } else if (EdgeUsesEventPipe(g, e)) {
        os << SpEventTransportPrefix << EventPipeMember(e);
      } else {
        os << SpBroadcastTransportPrefix << BroadcastRingMember(e);
      }
      os << ".SetRecordTap(record_page, static_cast<uint16_t>(RecordStructId::k"
         << PascalCase(e.channel) << "));\n";
    }
    os << "  }\n";
  }
  if (HasBroadcastEdge(g, cat)) {
    for (const auto& e : g.edges) {
      if (!IsBroadcastDelivery(e) || IsPinnedSlotDelivery(g, cat, e)) continue;
      const char* mode = (e.read == "follow") ? "Follow" : "Latest";
      os << "  if (!broadcast_transports_." << BroadcastReaderMember(e)
         << ".Bind(&broadcast_transports_." << BroadcastRingMember(e)
         << ", thyme::SpmcRead::" << mode << ")) {\n";
      os << "    std::fprintf(stderr, \"Pipeline: Bind failed for broadcast "
         << e.to << "_" << e.channel
         << "\\n\");\n    std::exit(1);\n  }\n";
    }
  }
  // Event consumers before cyclic producers: App::Start launches the event
  // list first; Add order matches so generated wiring is readable (dummy:
  // decision_ev before fusion).
  if (HasLocalFrameHub(g, cat)) {
    os << "  if (!app_.Add(&hub_dispatcher_)) {\n"
          "    std::fprintf(stderr, \"Pipeline: app_.Add failed for local hubs\\n\");\n"
          "    std::exit(1);\n  }\n";
  }
  for (const auto& n : g.nodes) {
    if (!IsEventNode(n)) continue;
    const std::string m = NodeMemberName(g, cat, n);
    os << "  if (!app_.Add(&" << m << ")) {\n";
    os << "    std::fprintf(stderr, \"Pipeline: app_.Add failed for " << n.id
       << "\\n\");\n    std::exit(1);\n  }\n";
  }
  for (const auto& n : g.nodes) {
    if (IsEventNode(n)) continue;
    const std::string m = NodeMemberName(g, cat, n);
    os << "  if (!app_.Add(&" << m << ")) {\n";
    os << "    std::fprintf(stderr, \"Pipeline: app_.Add failed for " << n.id
       << "\\n\");\n    std::exit(1);\n  }\n";
  }
  for (const auto& n : g.nodes) {
    if (!IsEventNode(n)) continue;
    std::vector<std::string> waits;
    for (const auto& e : g.edges) {
      if (e.to != n.id) continue;
      if (IsPinnedSlotDelivery(g, cat, e)) {
        waits.push_back(std::string("&pinned_slot_transports_.") +
                        PinnedSlotReaderMember(e));
      } else if (IsBroadcastDelivery(e)) {
        waits.push_back(std::string("&") + SpBroadcastTransportPrefix +
                        BroadcastReaderMember(e));
      } else if (IsLocalFrameHubEdge(g, cat, e)) {
        waits.push_back(LocalFrameHubOutputRef(g, cat, e, SpEventTransportPrefix));
      } else if (EdgeUsesEventPipe(g, e)) {
        waits.push_back(std::string("&") + SpEventTransportPrefix +
                        EventPipeMember(e));
      }
    }
    if (!waits.empty()) {
      os << "  {\n";
      os << "    thyme::Waitable* ws[] = {";
      for (std::size_t i = 0; i < waits.size(); ++i) {
        if (i) os << ", ";
        os << waits[i];
      }
      os << "};\n";
      os << "    " << NodeMemberName(g, cat, n) << ".SetWaitInputs(ws, "
         << waits.size() << ");\n";
      os << "  }\n";
    }
    if (NodeNeedsPlacement(n)) {
      if (!AppendPlacement(os, g, n, NodeMemberName(g, cat, n))) return false;
    }
  }
  // delivery=queue is downgraded to NotifPipe in single-process Pipeline; no
  // POSIX shm Create / wake-word wiring here (MP roles use EventTransports).
  const bool use_sched = UsesSchedulingSetup(tpl_dir);
  if (use_sched) {
    // scheduling_rings.inc is a marker file: its presence selects this path.
    // The SchedulingRings aggregate + BuildSchedulingRings factory are
    // generated into generated/scheduling_rings.h from the graph's wait_inputs
    // in-edges; the Pipeline exposes the same <prefix>_ring members as the MP
    // RoleLocals, so one factory serves both owners. sched_rings is
    // constructed only when a node actually consumes it.
    //
    // Plain ThreadPlacement does not need the scheduling support aggregate.
    // Emit it directly here so a template whose app library does not compile
    // src/support/scheduling_setup.cpp still links (PERF-02 adds placement to
    // every RM-prioritised node, not only wait_inputs nodes).
    bool any_sched = false;
    for (const auto& n : g.nodes) {
      // Event threads are not CyclicThreads; their placement/wait inputs are
      // emitted with their construction above.
      if (IsEventNode(n)) continue;
      const std::string member = NodeMemberName(g, cat, n);
      if (n.wait_inputs == "period") {
        if (!any_sched) {
          os << "  const SchedulingRings sched_rings = BuildSchedulingRings(*this);\n";
          any_sched = true;
        }
        os << "  ApplyNodeScheduling(\"" << n.id << "\", &" << member
           << ", sched_rings);\n";
      } else if (NodeNeedsPlacement(n)) {
        if (!AppendPlacement(os, g, n, member)) return false;
      }
    }
  } else {
    for (const auto& n : g.nodes) {
      if (IsEventNode(n)) continue;
      const std::string member = NodeMemberName(g, cat, n);
      if (NodeNeedsPlacement(n)) {
        if (!AppendPlacement(os, g, n, member)) return false;
      }
      if (!AppendWaitInputs(os, g, n, cat, member)) return false;
    }
  }
  os << "}\n\nbool Pipeline::Start() {\n  thyme::App::InstallStopSignals();\n  if (!app_.Start()) {\n    std::fprintf(stderr, \"Pipeline::Start: app_.Start() failed\\n\");\n    std::exit(1);\n  }\n  return true;\n}\n\n";
  os << "void Pipeline::RequestStop() {\n  app_.RequestStop();\n}\n\n";
  os << "void Pipeline::Join() {\n  app_.Join();\n}\n";
  os << "\nthyme::CyclicThread* Pipeline::ThreadByNodeId(const char* id) {\n";
  for (const auto& n : g.nodes) {
    if (IsEventNode(n)) continue;
    os << "  if (std::strcmp(id, \"" << n.id << "\") == 0) return &"
       << NodeMemberName(g, cat, n) << ";\n";
  }
  os << "  return nullptr;\n}\n";
  if (HasEventNode(g)) {
    os << "\nthyme::EventThread* Pipeline::EventThreadByNodeId(const char* id) {\n";
    for (const auto& n : g.nodes) {
      if (!IsEventNode(n)) continue;
      os << "  if (std::strcmp(id, \"" << n.id << "\") == 0) return &"
         << NodeMemberName(g, cat, n) << ";\n";
    }
    os << "  return nullptr;\n}\n";
  }
  return WriteFile(fs::path(tmp_dir) / "wiring.cpp", os.str());
}

