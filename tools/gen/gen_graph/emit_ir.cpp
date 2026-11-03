#include "emit.h"

#include "emit_internal.h"
#include "meta.h"

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

bool EmitGraphIr(const std::string& tmp_dir, const Graph& g,
                 const std::string& tpl_dir, const EdgeManifest& m) {
  TemplateCatalog cat;
  if (!LoadCatalog(tpl_dir, &cat)) return false;
  const std::string graph_hash = gen_meta::SourceGraphHash(g);
  if (graph_hash == "0000000000000000") {
    std::fprintf(stderr,
                 "gen_graph: source graph hash is zero; refusing to emit a "
                 "graph without runtime identity\n");
    return false;
  }
  std::vector<std::string> order;
  std::set<std::string> seen;
  for (const auto& k : cat.shm_kinds) {
    if (seen.insert(k.kind).second) order.push_back(k.kind);
  }
  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n";
  os << "#include <cstddef>\n";
  os << "#include <cstdint>\n\n";
  if (m.has_record) {
    // Compile switch for scaffold-side record/replay code: defined when the
    // graph has >= 1 recordable edge, so the generated record/replay headers
    // (and every #include of them) are only pulled in when they exist.
    os << "#ifndef THYME_GRAPH_HAS_RECORD\n";
    os << "#define THYME_GRAPH_HAS_RECORD 1\n";
    os << "#endif\n\n";
  }
  os << "inline constexpr int kGraphHealthN = " << g.health_n << ";\n";
  os << "inline constexpr const char* kGraphShmPrefixFmt = \"/c9_%d\";\n";
  os << "inline constexpr uint64_t kGraphHash = 0x" << graph_hash << "ULL;\n";
  os << "static_assert(kGraphHash != 0, \"generated graph hash must be non-zero\");\n\n";
  os << "enum class GraphShmKind : int { ";
  if (order.empty()) {
    os << "None = 0";
  } else {
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (i) os << ", ";
      os << order[i] << " = " << i;
    }
  }
  os << " };\n\n";

  os << "struct GraphProcess {\n  const char* id;\n  const char* role;\n};\n";
  os << "inline constexpr GraphProcess kGraphProcesses[] = {";
  for (std::size_t i = 0; i < g.processes.size(); ++i) {
    os << (i == 0 ? "\n" : ",\n") << "  {\"" << g.processes[i].id << "\", \""
       << g.processes[i].role << "\"}";
  }
  os << " };\n\n";

  os << "struct GraphNode {\n  const char* id;\n  const char* type;\n"
        "  const char* process;\n  int hz;\n};\n";
  os << "inline constexpr GraphNode kGraphNodes[] = {";
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    os << (i == 0 ? "\n" : ",\n") << "  {\"" << g.nodes[i].id << "\", \""
       << g.nodes[i].type << "\", \"" << g.nodes[i].process << "\", "
       << g.nodes[i].hz << "}";
  }
  os << " };\n";
  os << "inline constexpr std::size_t kGraphNodesCount = " << g.nodes.size()
     << ";\n\n";
  if (m.has_event_node) {
    // Only emitted when the graph has exec=event nodes so cyclic-only output
    // stays byte-identical; role code uses it to pick MakeEventThread.
    // THYME_GRAPH_HAS_EVENT_NODES is the scaffold-side compile switch that
    // gates kGraphNodeIsEvent access (guarded so a manual compile definition
    // never conflicts).
    os << "#ifndef THYME_GRAPH_HAS_EVENT_NODES\n";
    os << "#define THYME_GRAPH_HAS_EVENT_NODES 1\n";
    os << "#endif\n\n";
    // Queue and SHM PinnedSlot edges into event nodes get ring/wake Open +
    // Prepare/Unlink helpers in generated/mp_unknown.h; the scaffold mp glue
    // gates those calls on THYME_GRAPH_HAS_EVENT_SHM so purely local event
    // graphs still compile without SHM helpers.
    if (m.has_event_shm) {
      os << "#ifndef THYME_GRAPH_HAS_EVENT_SHM\n";
      os << "#define THYME_GRAPH_HAS_EVENT_SHM 1\n";
      os << "#endif\n\n";
    }
    if (HasLocalFrameHub(g, cat)) {
      os << "#ifndef THYME_GRAPH_HAS_LOCAL_HUB\n";
      os << "#define THYME_GRAPH_HAS_LOCAL_HUB 1\n";
      os << "#endif\n\n";
    }
    os << "inline constexpr bool kGraphNodeIsEvent[] = {";
    for (std::size_t i = 0; i < g.nodes.size(); ++i) {
      if (i) os << ", ";
      os << (IsEventNode(g.nodes[i]) ? "true" : "false");
    }
    os << "};\n\n";
  }

  // One SHM channel table for every shm-backed edge. delivery selects layout:
  // latest → LatestSlot (supervisor create_all); queue → ShmSpscRing (Prepare
  // in mp_unknown). Same-process notify edges carry no shm and stay out.
  os << "struct GraphShmChannel {\n  const char* suffix;\n  const char* channel;\n  GraphShmKind kind;\n"
        "  const char* from_id;\n  const char* to_id;\n"
        "  const char* from_process;\n  const char* to_process;\n"
        "  const char* delivery;\n};\n";
  os << "inline constexpr GraphShmChannel kGraphShmChannels[] = {";
  std::size_t shm_idx = 0;
  for (const auto& r : m.shm_edges) {
    const Edge& e = *r.edge;
    if (IsBroadcastDelivery(e) || IsPinnedSlotDelivery(g, cat, e)) continue;
    os << (shm_idx == 0 ? "\n" : ",\n") << "  {\"" << e.shm << "\", \""
       << e.channel << "\", GraphShmKind::" << r.shm_kind << ", \"" << e.from
       << "\", \"" << e.to << "\", \"" << r.from_process << "\", \""
       << r.to_process << "\", \"" << e.delivery << "\"}";
    ++shm_idx;
  }
  os << " };\n\n";

  if (HasPinnedSlotShmEdge(g, cat)) {
    os << "#ifndef THYME_GRAPH_HAS_BIGFRAME\n";
    os << "#define THYME_GRAPH_HAS_BIGFRAME 1\n";
    os << "#endif\n\n";
  }
  if (m.has_broadcast) {
    os << "#ifndef THYME_GRAPH_HAS_BROADCAST\n";
    os << "#define THYME_GRAPH_HAS_BROADCAST 1\n";
    os << "#endif\n";
    if (!cat.contracts_include.empty())
      os << "#include \"" << cat.contracts_include << "\"\n";
    os << "#include \"thyme/contracts/frame.h\"\n";
    os << "#include \"thyme/runtime/shm_spmc_ring.h\"\n\n";

    os << "struct GraphBroadcastRegion {\n  const char* suffix;\n"
          "  const char* cpp_type;\n  uint32_t cap;\n};\n";
    os << "inline constexpr GraphBroadcastRegion kGraphBroadcastRegions[] = {";
    const std::vector<const Edge*> regions = UniqueBroadcastRegionEdges(g, cat);
    for (std::size_t i = 0; i < regions.size(); ++i) {
      const Edge& e = *regions[i];
      os << (i == 0 ? "\n" : ",\n") << "  {\"" << e.shm << "\", \""
         << e.cpp_type << "\", " << static_cast<uint32_t>(EventEdgeCap(g, e))
         << "}";
    }
    os << " };\n";
    os << "inline constexpr std::size_t kGraphBroadcastSizes[] = {";
    for (std::size_t i = 0; i < regions.size(); ++i) {
      const Edge& e = *regions[i];
      os << (i == 0 ? "\n" : ",\n") << "  sizeof(thyme::ShmSpmcRing<"
         << Framed(e.cpp_type) << ", " << EventEdgeCap(g, e) << ">::Layout)";
    }
    os << " };\n\n";

    // GEN-08: broadcast region geometry. The generator emits the cap and the
    // region byte count independently of the container; assert both against
    // the documented SPMC page formula (5 header cells + Cap payload cells,
    // each cell RoundUp64(seqlock + payload end)) and the 64 KB Frame limit.
    for (std::size_t i = 0; i < regions.size(); ++i) {
      const Edge& e = *regions[i];
      const std::string framed = Framed(e.cpp_type);
      const int cap = EventEdgeCap(g, e);
      os << "static_assert(" << cap << " >= 2 && ((" << cap << " & (" << cap
         << " - 1)) == 0),\n"
         << "              \"generated broadcast " << e.shm
         << ": cap must be a power of two >= 2\");\n";
      os << "static_assert(thyme::FramePodWireBytes<" << framed
         << "> <= thyme::kAbiMaxTransportBytes,\n"
         << "              \"generated broadcast " << e.shm
         << ": contract exceeds the 64 KB Frame transport limit\");\n";
      // offsetof's macro arity cannot see template commas, so name the cell.
      os << "using GeneratedBroadcastCell" << i
         << " = thyme::ShmSpmcRing<" << framed << ", " << cap
         << ">::Layout::Cell;\n";
      os << "static_assert(sizeof(thyme::ShmSpmcRing<" << framed << ", " << cap
         << ">::Layout) ==\n"
         << "                  320u + " << cap
         << "u * (((offsetof(GeneratedBroadcastCell" << i
         << ", value) + sizeof(" << framed << ") + 63u) / 64u) *\n"
         << "                         64u),\n"
         << "              \"generated broadcast " << e.shm
         << ": SPMC page must be 5 header cells + Cap payload cells\");\n";
    }
    os << "\n";

    os << "struct GraphBroadcastReader {\n  const char* suffix;\n"
          "  const char* to_id;\n  const char* to_process;\n  bool follow;\n"
          "  bool event;\n};\n";
    os << "inline constexpr GraphBroadcastReader kGraphBroadcastReaders[] = {";
    std::size_t rd_idx = 0;
    for (const auto& e : g.edges) {
      if (!IsBroadcastDelivery(e)) continue;
      const Node* tn = FindNodeById(g, e.to);
      const bool follow = (e.read == "follow");
      const bool event = EdgeToEventNode(g, e);
      os << (rd_idx == 0 ? "\n" : ",\n") << "  {\"" << e.shm << "\", \""
         << e.to << "\", \"" << (tn != nullptr ? tn->process : "") << "\", "
         << (follow ? "true" : "false") << ", " << (event ? "true" : "false")
         << "}";
      ++rd_idx;
    }
    os << " };\n\n";
  }

  const auto& gates = m.record_gates;
  os << "struct GraphRecordGate {\n  const char* process;\n  uint16_t slot;\n};\n";
  os << "inline constexpr std::size_t kGraphRecordGateCount = " << gates.size()
     << ";\n";
  os << "inline constexpr GraphRecordGate kGraphRecordGates[] = {";
  if (gates.empty()) {
    os << " {nullptr, 0}";
  } else {
    for (std::size_t i = 0; i < gates.size(); ++i) {
      os << (i == 0 ? "\n" : ",\n") << "  {\"" << gates[i].first << "\", "
         << gates[i].second << "}";
    }
  }
  os << " };\n";
  return WriteFile(fs::path(tmp_dir) / "graph_ir.h", os.str());
}

bool EmitShmSlots(const std::string& tmp_dir, const Graph& g,
                  const std::string& tpl_dir, const EdgeManifest& m) {
  TemplateCatalog cat;
  if (!LoadCatalog(tpl_dir, &cat)) return false;
  std::vector<std::string> order;
  std::set<std::string> seen;
  for (const auto& k : cat.shm_kinds) {
    if (seen.insert(k.kind).second) order.push_back(k.kind);
  }
  // ARCH-11: kinds used by a latest-delivery edge need a described v3 page.
  // Queue/broadcast kinds keep the size row for index stability but carry no
  // LatestSlot identity.
  std::set<std::string> latest_kinds;
  for (const auto& r : m.shm_edges) {
    if (IsQueueDelivery(*r.edge) || IsBroadcastDelivery(*r.edge) ||
        IsPinnedSlotDelivery(g, cat, *r.edge)) continue;
    latest_kinds.insert(r.shm_kind);
  }
  if (!latest_kinds.empty() && cat.contracts_layout_include.empty()) {
    std::fprintf(stderr,
                 "gen_graph: graph has latest SHM edges but the template has "
                 "no contracts_layout.txt; a LatestSlot page needs the "
                 "generated contract layout identity (ABI-11)\n");
    return false;
  }

  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n";
  os << "#include \"generated/graph_ir.h\"\n";
  os << "#include \"thyme/app/mp_shm_kind.h\"\n";
  if (!order.empty()) {
    os << "#include \"thyme/contracts/abi.h\"\n";
    os << "#include \"thyme/contracts/frame_header.h\"\n";
    if (!cat.contracts_include.empty())
      os << "#include \"" << cat.contracts_include << "\"\n";
    if (!latest_kinds.empty()) {
      if (!cat.contracts_layout_include.empty())
        os << "#include \"" << cat.contracts_layout_include << "\"\n";
      os << "#include \"thyme/record/record_schema.h\"\n";
      os << "#include \"thyme/runtime/latest_slot.h\"\n";
      os << "#include \"thyme/runtime/latest_slot_page.h\"\n";
    } else {
      os << "#include \"thyme/runtime/latest_slot.h\"\n";
    }
  }
  os << "\n";

  // ARCH-11: every LatestSlot number comes from LatestSlotLayoutModel<T>. The
  // generator no longer re-derives 2*RoundUp64(sizeof(T)) + 64; the model is
  // the single source of truth shared with the supervisor, role and probe.
  if (!order.empty()) {
    os << "namespace generated_layout {\n";
    if (!latest_kinds.empty()) {
      // ABI-11 contract identity lookup at compile time. Returns 0 when the
      // generated contract layout has no row, which the static_asserts below
      // reject rather than publishing a page with an absent identity.
      os << "inline constexpr uint64_t ContractLayoutHash(const char* cpp_type) {\n";
      os << "  if (cpp_type == nullptr) return 0;\n";
      os << "  for (std::size_t i = 0; i < thyme_contract_layout::kContractCount; ++i) {\n";
      os << "    const char* a = thyme_contract_layout::kContracts[i].cpp_type;\n";
      os << "    const char* b = cpp_type;\n";
      os << "    while (*a != '\\0' && *a == *b) { ++a; ++b; }\n";
      os << "    if (*a == '\\0' && *b == '\\0') {\n";
      os << "      return thyme::schema::ComputeLayoutHashConstexpr(\n";
      os << "          thyme_contract_layout::kContracts[i]);\n";
      os << "    }\n";
      os << "  }\n";
      os << "  return 0;\n";
      os << "}\n";
    }
    os << "}  // namespace generated_layout\n\n";
  }

  // Per-kind LatestSlot<T> sizes, indexed by GraphShmKind value. Derived from
  // shm_kinds.txt (cpp_type -> kind) so the role/supervisor sides never
  // hand-write a sizeof per kind.
  os << "inline constexpr std::size_t kGraphShmKindCount = " << order.size()
     << ";\n";
  auto cpp_type_for = [&cat](const std::string& kind) -> std::string {
    for (const auto& k : cat.shm_kinds) {
      if (k.kind == kind) return k.cpp_type;
    }
    return "int";
  };
  os << "inline constexpr std::size_t kGraphShmSlotSizes[";
  os << (order.empty() ? "1" : "kGraphShmKindCount") << "] = {";
  if (order.empty()) {
    os << "0";
  } else {
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (i) os << ", ";
      os << "thyme::LatestSlotLayoutModel<"
         << Framed(cpp_type_for(order[i])) << ">::kRegionBytes";
    }
  }
  os << "};\n\n";

  // ABI-11 payload identity per kind: the generated contract layout hash, 0
  // for kinds that are not used by a latest-delivery edge.
  os << "inline constexpr uint64_t kGraphShmPayloadLayoutHashes[";
  os << (order.empty() ? "1" : "kGraphShmKindCount") << "] = {";
  if (order.empty()) {
    os << "0";
  } else {
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (i) os << ", ";
      const std::string cpp_type = cpp_type_for(order[i]);
      if (latest_kinds.count(order[i]) != 0) {
        os << "generated_layout::ContractLayoutHash(\"" << cpp_type << "\")";
      } else {
        os << "0";
      }
    }
  }
  os << "};\n\n";

  if (!order.empty()) {
    os << "static_assert(kGraphShmKindCount == " << order.size()
       << ", \"generated SHM kind count drift\");\n";
    for (std::size_t i = 0; i < order.size(); ++i) {
      const std::string cpp_type = cpp_type_for(order[i]);
      const std::string framed = Framed(cpp_type);
      os << "static_assert(static_cast<int>(GraphShmKind::" << order[i]
         << ") == " << i << ",\n"
         << "              \"generated SHM kind '" << order[i]
         << "' must index kGraphShmSlotSizes\");\n";
      os << "static_assert(kGraphShmSlotSizes[static_cast<std::size_t>("
         << "GraphShmKind::" << order[i] << ")] ==\n"
         << "                  thyme::LatestSlotLayoutModel<" << framed
         << ">::kRegionBytes,\n"
         << "              \"generated SHM kind '" << order[i]
         << "': region size must come from LatestSlotLayoutModel\");\n";
      if (latest_kinds.count(order[i]) != 0) {
        os << "static_assert(kGraphShmPayloadLayoutHashes[static_cast<std::size_t>("
           << "GraphShmKind::" << order[i] << ")] != 0,\n"
           << "              \"generated SHM kind '" << order[i]
           << "': latest page needs a non-zero generated contract layout "
              "identity; add the type to contract_layout.h\");\n";
      }
      bool pinned_slot = false;
      for (const auto& e : g.edges) {
        if (e.cpp_type == cpp_type && IsPinnedSlotDelivery(g, cat, e)) pinned_slot = true;
      }
      if (!pinned_slot) {
        os << "static_assert(thyme::FramePodWireBytes<" << framed
           << "> <= thyme::kAbiMaxTransportBytes,\n"
           << "              \"generated SHM kind '" << order[i]
           << "': contract exceeds the 64 KB Frame transport limit\");\n";
      }
    }
    os << "\n";
  }

  // Typed create/open callbacks plus the shared descriptor table. Both sides
  // consume kGraphShmKindSpecs; no caller reconstructs the page formula. The
  // table always exists (a placeholder row for an empty graph) so generated
  // code can expose a stable span.
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (latest_kinds.count(order[i]) == 0) continue;
    const std::string framed = Framed(cpp_type_for(order[i]));
    os << "inline bool CreateGraphShmPage_" << i
       << "(thyme::ShmRegion& region, const char* name,\n";
    os << "                             uint64_t feature_bits,\n";
    os << "                             thyme::ShmPageError* error) {\n";
    os << "  return thyme::CreateLatestSlotPage<" << framed
       << ">(\n";
    os << "             region, name, kGraphShmPayloadLayoutHashes[" << i
       << "], feature_bits, error) != nullptr;\n";
    os << "}\n";
    os << "inline bool OpenGraphShmPage_" << i
       << "(thyme::ShmRegion& region, const char* name,\n";
    os << "                           uint64_t timeout_ns,\n";
    os << "                           uint64_t feature_bits,\n";
    os << "                           thyme::ShmPageError* error) {\n";
    os << "  return thyme::OpenLatestSlotPage<" << framed
       << ">(\n";
    os << "             region, name, kGraphShmPayloadLayoutHashes[" << i
       << "], timeout_ns, feature_bits, error) != nullptr;\n";
    os << "}\n";
  }
  os << "inline constexpr thyme::app::MpShmKindSpec kGraphShmKindSpecs[";
  os << (order.empty() ? "1" : "kGraphShmKindCount") << "] = {";
  for (std::size_t i = 0; i < order.size(); ++i) {
    const std::string framed = Framed(cpp_type_for(order[i]));
    os << (i == 0 ? "\n" : ",\n") << "  {kGraphShmSlotSizes[" << i
       << "], sizeof(" << framed << "), thyme::LatestSlotLayoutModel<"
       << framed << ">::kCellStride,\n";
    os << "   kGraphShmPayloadLayoutHashes[" << i << "], 0, ";
    if (latest_kinds.count(order[i]) != 0) {
      os << "&CreateGraphShmPage_" << i << ",\n   &OpenGraphShmPage_" << i;
    } else {
      os << "nullptr, nullptr";
    }
    os << "}";
  }
  if (order.empty()) os << " {0, 0, 0, 0, 0, nullptr, nullptr}";
  os << " };\n\n";

  // Cross-process frame gates (S-D4): one MpControl::cross_frames slot per
  // edge annotated mp_gate: true, in graph edge order. The supervisor
  // requires each gated counter to reach >= 1 by end of run; an empty table
  // disables the check.
  {
    const std::vector<EdgeManifestRow>& gates = m.cross_gates;
    os << "\nstruct GraphCrossGate {\n  GraphShmKind kind;\n"
          "  uint32_t counter_slot;\n};\n";
    os << "inline constexpr std::size_t kGraphCrossGateCount = "
       << gates.size() << ";\n";
    os << "inline constexpr GraphCrossGate kGraphCrossGates[] = {";
    if (gates.empty()) {
      // Placeholder row (zero-sized arrays are not standard C++);
      // kGraphCrossGateCount is authoritative.
      os << " {static_cast<GraphShmKind>(-1), 0}";
    } else {
      for (std::size_t i = 0; i < gates.size(); ++i) {
        if (gates[i].shm_kind.empty()) {
          const std::string loc = GraphLoc(g, gates[i].edge->line);
          std::fprintf(stderr,
                       "gen_graph: %smp_gate edge %s->%s (%s): no shm kind "
                       "for type %s\n",
                       loc.c_str(), gates[i].edge->from.c_str(),
                       gates[i].edge->to.c_str(), gates[i].edge->channel.c_str(),
                       gates[i].edge->cpp_type.c_str());
          return false;
        }
        os << (i == 0 ? "\n" : ",\n")
           << "  {GraphShmKind::" << gates[i].shm_kind << ", " << i << "}";
      }
    }
    os << " };\n";
  }
  return WriteFile(fs::path(tmp_dir) / "shm_slots.h", os.str());
}

bool EmitGraphCheck(const std::string& tmp_dir, const Graph& g,
                    const std::string& tpl_dir) {
  TemplateCatalog cat;
  if (!LoadCatalog(tpl_dir, &cat)) return false;
  std::set<std::string> present_types;
  for (const auto& n : g.nodes) if (IsKnownType(cat, n.type)) present_types.insert(n.type);
  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n\n";
  os << "#include \"generated/graph_ir.h\"\n";
  // Graph-level constants (kRingCap/kPoolN) and, when the graph has edges, the
  // contract types + containers used by the GEN-08 layout/size asserts below.
  os << "#include \"generated/graph_types.h\"\n";
  os << "#include \"thyme/contracts/abi.h\"\n";
  if (!g.edges.empty()) {
    os << "#include \"thyme/contracts/frame_header.h\"\n";
    if (!cat.contracts_include.empty())
      os << "#include \"" << cat.contracts_include << "\"\n";
    for (const auto& e : g.edges) {
      if (IsQueueDelivery(e)) {
        os << "#include \"thyme/runtime/shm_spsc_ring.h\"\n";
        break;
      }
    }
  }
  bool has_health_reporter = false;
  for (const auto& n : g.nodes) {
    if (n.type == "health_reporter") { has_health_reporter = true; break; }
  }
  os << "\n";
  os << "static_assert(kGraphHealthN >= " << g.nodes.size() << ",\n"
     << "              \"supervisor.health_n < nodes.size(); raise health_n or remove nodes\");\n";
  // GEN-08: generated containers are sized from these two constants — a
  // non-power-of-two ring cap has no valid SpscRing/NotifPipe/ring layout, and
  // a zero pool would silently disable transport.
  os << "static_assert(kRingCap >= 2 && ((kRingCap & (kRingCap - 1u)) == 0u),\n"
     << "              \"generated: ring_cap must be a power of two >= 2\");\n";
  os << "static_assert(kPoolN > 0,\n"
     << "              \"generated: pool_n must be > 0\");\n";
  os << "static_assert(kGraphHealthN <= thyme::kAbiMaxThreads,\n"
     << "              \"supervisor.health_n exceeds kAbiMaxThreads (SharedHealthPage)\");\n\n";

  if (!g.edges.empty()) {
    // GEN-08: every graph edge carries a Frame<T>. Assert the framed wire
    // image against the 64 KB transport ceiling (uint16_t hdr.size) and, when
    // the template ships contract_sizes.txt (written by gen_contracts), the
    // bare contract size against the schema copy.
    std::set<std::string> emitted_type;
    for (const auto& e : g.edges) {
      if (!emitted_type.insert(e.cpp_type).second) continue;
      if (IsPinnedSlotDelivery(g, cat, e)) continue;
      const std::string framed = Framed(e.cpp_type);
      os << "static_assert(thyme::FramePodWireBytes<" << framed
         << "> <= thyme::kAbiMaxTransportBytes,\n"
         << "              \"generated edge contract " << e.cpp_type
         << ": framed wire bytes exceed the 64 KB transport limit\");\n";
      const ContractSizeCat* size = FindContractSize(cat, e.cpp_type);
      if (size != nullptr) {
        // The generator now has its own schema copy; ABI-03 owns the
        // authoritative bare `sizeof` assert in the contracts header, so this
        // one only pins the size the generator planned around.
        os << "static_assert(sizeof(" << e.cpp_type << ") == " << size->bytes
           << "u,\n"
           << "              \"generated edge contract " << e.cpp_type
           << ": size drift vs contract_sizes.txt (schema)\");\n";
      }
    }
    os << "\n";

    // GEN-08: effective queue/notify caps are emitted as template arguments.
    // Pin each literal and the depth override (power-of-two rule from yaml
    // validation) so a generator change cannot silently emit an invalid ring.
    std::set<std::string> emitted_cap;
    for (const auto& e : g.edges) {
      if (!EdgeUsesEventPipe(g, e)) continue;
      const int cap = EventEdgeCap(g, e);
      const std::string key = e.cpp_type + "|" + std::to_string(cap);
      if (!emitted_cap.insert(key).second) continue;
      const std::string framed = Framed(e.cpp_type);
      os << "static_assert(" << cap << " >= 2 && ((" << cap << " & (" << cap
         << " - 1u)) == 0u),\n"
         << "              \"generated edge " << e.from << "->" << e.to << " ("
         << e.channel << "): Cap must be a power of two >= 2\");\n";
      if (e.depth > 0) {
        os << "static_assert(" << e.depth << " >= 2 && ((" << e.depth << " & ("
           << e.depth << " - 1u)) == 0u),\n"
           << "              \"generated edge " << e.from << "->" << e.to
           << " (" << e.channel << "): depth must be a power of two\");\n";
      }
      if (IsQueueDelivery(e) && !IsPinnedSlotDelivery(g, cat, e)) {
        // ShmSpscRing v4 page ABI: super-header + 4 ring header cells
        // (320 B) + Cap * RoundUp64(T).
        os << "static_assert(sizeof(thyme::ShmSpscRing<" << framed << ", "
           << cap << ">::Layout) ==\n"
           << "                  320u + " << cap
           << "u * (((sizeof(" << framed << ") + 63u) / 64u) * 64u),\n"
           << "              \"generated edge " << e.from << "->" << e.to
           << " (" << e.channel
           << "): SPSC page must be 5 header cells + Cap payload cells\");\n";
      }
    }
    if (!emitted_cap.empty()) os << "\n";
  }

  for (const auto& contract : cat.contracts) {
    if (present_types.find(contract.type) == present_types.end()) continue;
    const Node* node = FindCyclicType(g, contract.type);
    bool ok = false;
    if (node != nullptr) {
      for (const auto& e : g.edges) {
        if (e.channel != contract.channel) continue;
        if (contract.is_out) { if (e.from == node->id) { ok = true; break; } }
        else { if (e.to == node->id) { ok = true; break; } }
      }
    }
    os << "inline constexpr bool kEdge_" << contract.type << "_" << contract.channel
       << " = " << (ok ? "true" : "false") << ";\n";
    os << "static_assert(kEdge_" << contract.type << "_" << contract.channel
       << ",\n              \"" << contract.type << " Loop requires edge channel="
       << contract.channel << "; fix graph.yaml or remove this node type\");\n\n";
  }
  if (has_health_reporter) {
    // The supervisor maps node index -> health slot and gives health_reporter
    // the final one (&health[kGraphHealthN - 1]); a graph with health_reporter
    // not last would overlap the worker range and corrupt the stall watchdog.
    // Keep this ordering contract compile-time enforced in the generated IR.
    // Use a hand constexpr compare: std::strcmp is not constexpr on Apple
    // Clang under C++17.
    os << "constexpr bool kGraphCStrEq(const char* a, const char* b) {\n";
    os << "  while (*a != '\\0' && *a == *b) { ++a; ++b; }\n";
    os << "  return *a == *b;\n";
    os << "}\n";
    os << "static_assert(kGraphCStrEq(kGraphNodes[kGraphNodesCount - 1].type,\n"
       << "                           \"health_reporter\"),\n"
       << "              \"health_reporter must be the last node in yaml order "
          "(supervisor maps node index to its health slot)\");\n";
  }
  return WriteFile(fs::path(tmp_dir) / "graph_check.h", os.str());
}


bool EmitGraphIrOnly(const std::string& root, const Graph& g,
                     const std::string& tpl_dir) {
  TemplateCatalog cat;
  if (!LoadCatalog(tpl_dir, &cat)) return false;
  if (g.shm_prefix != "/c9_%pid") {
    std::fprintf(stderr, "EmitGraphIrOnly: unsupported shm_prefix %s (only /c9_%%pid)\n",
                 g.shm_prefix.c_str());
    return false;
  }
  if (!ValidateGraphPools(g, cat)) return false;
  if (!ValidateEventNodes(g, cat)) return false;
  EdgeManifest manifest;
  if (!BuildEdgeManifest(g, cat, &manifest)) return false;
  const fs::path r(root);
  // --update mp overwrites only graph_ir.h + mp_unknown.h (partial refresh by
  // design); the staging dir is still pid-unique and guard-cleaned (GEN-03).
  const fs::path tmp = StagingDir(r, "mp");
  DirGuard tmp_guard{tmp};
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  if (ec) { std::fprintf(stderr, "EmitGraphIrOnly: mkdir tmp failed\n"); return false; }
  if (!EmitGraphIr(tmp.string(), g, tpl_dir, manifest)) return false;
  if (!EmitMpUnknown(tmp.string(), g, tpl_dir)) return false;
  const fs::path dest = r / "generated";
  fs::create_directories(dest, ec);
  fs::copy_file(tmp / "graph_ir.h", dest / "graph_ir.h",
                fs::copy_options::overwrite_existing, ec);
  if (ec) { std::fprintf(stderr, "EmitGraphIrOnly: copy graph_ir.h failed\n"); return false; }
  fs::copy_file(tmp / "mp_unknown.h", dest / "mp_unknown.h",
                fs::copy_options::overwrite_existing, ec);
  if (ec) { std::fprintf(stderr, "EmitGraphIrOnly: copy mp_unknown.h failed\n"); return false; }
  return true;
}

