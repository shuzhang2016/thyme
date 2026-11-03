#include "emit_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace gen_emit {

// Scratch buffer size emitted for thyme::app::MpName shm-name composition
// (prefix + suffix) in the generated Prepare/Open/Unlink helpers.
constexpr int kMpNameBufSize = 64;

// Ctor init from Ports: broadcast in is channel_; broadcast out is channel_w_;
// FrameChannelRef is channel_ only; cyclic edges also bind the ring/pipe as
// channel_r_.
static void AppendEdgeChannelInit(std::ostringstream& os, const Graph& g,
                           const TemplateCatalog& cat, const Edge& e, bool as_out) {
  if (IsPinnedSlotDelivery(g, cat, e)) {
    os << ",\n        " << e.channel << (as_out ? "_w_" : "_")
       << "(ports." << e.channel << (as_out ? "_w_" : "_") << ")";
    return;
  }
  if (IsBroadcastDelivery(e)) {
    if (as_out) {
      os << ",\n        " << e.channel << "_w_(ports." << e.channel << "_w_)";
    } else {
      os << ",\n        " << e.channel << "_(ports." << e.channel << "_)";
    }
    return;
  }
  if (EdgeUsesEventPipe(g, e)) {
    os << ",\n        " << e.channel << "_(ports." << e.channel << "_)";
  } else {
    const PoolCat* p = FindPool(cat, e.pool);
    if (p != nullptr && !p->index_pipe) {
      os << ",\n        " << e.channel << "_(ports." << e.channel << "_)";
    } else {
      os << ",\n        " << e.channel << "_(ports." << e.channel << "_), "
         << e.channel << "_r_(ports." << e.channel << "_r_)";
    }
  }
}

static void AppendGeneratedRecordIncludes(std::ostringstream& os, bool recordable,
                                   bool need_ctime) {
  if (!recordable) return;
  os << "#include \"generated/replay_stages.h\"\n";
  os << "#include \"thyme/app/record_mirror.h\"\n";
  os << "#include \"thyme/app/time.h\"\n";
  if (need_ctime) os << "#include <ctime>\n";
}

static void AppendGeneratedRecordTap(std::ostringstream& os) {
  os << "  template <typename T>\n";
  os << "  void RecordTap(RecordStructId id, const T& v) {\n";
  os << "    NoteOutFrame(v.hdr.sequence & 0xF);\n";
  os << "    RecordTapMirror(record_page_, id, &v, sizeof(v),\n";
  os << "                    thyme::app::NowMonoNs(), v.hdr.sequence);\n";
  os << "  }\n\n";
}
static void AppendBroadcastShmHelpers(std::ostringstream& os, const Graph& g,
                                      const TemplateCatalog& cat) {
  const std::vector<const Edge*> regions = UniqueBroadcastRegionEdges(g, cat);
  if (regions.empty()) return;

  os << "\n";
  os << "// Cross-process SPMC broadcast: supervisor Prepare (Unlink+Init);\n";
  os << "// roles Open the writer ring and Bind each SpmcReader (Follow or\n";
  os << "// Latest from edge.read; empty read = latest).\n";
  os << "struct BroadcastTransports {\n";
  for (const auto* e : regions) {
    const std::string cap = std::to_string(EventEdgeCap(g, *e));
    os << "  thyme::ShmSpmcRing<" << Framed(e->cpp_type) << ", " << cap
       << "> " << BroadcastRingMember(*e) << ";\n";
  }
  for (const auto& e : g.edges) {
    if (!IsBroadcastDelivery(e) || IsPinnedSlotDelivery(g, cat, e)) continue;
    const std::string cap = std::to_string(EventEdgeCap(g, e));
    os << "  thyme::SpmcReader<" << Framed(e.cpp_type) << ", " << cap << "> "
       << BroadcastReaderMember(e) << ";\n";
  }
  os << "};\n\n";

  os << "inline void UnlinkBroadcastShmChannels(const char* prefix) {\n";
  os << "  char nm[" << kMpNameBufSize << "];\n";
  for (const auto* e : regions) {
    os << "  thyme::app::MpName(nm, sizeof(nm), prefix, \"" << e->shm
       << "\");\n";
    os << "  thyme::ShmRegion::UnlinkName(nm);\n";
  }
  os << "}\n\n";

  os << "inline bool PrepareBroadcastShmChannels(const char* prefix) {\n";
  os << "  char nm[" << kMpNameBufSize << "];\n";
  for (const auto* e : regions) {
    const std::string cap = std::to_string(EventEdgeCap(g, *e));
    os << "  thyme::app::MpName(nm, sizeof(nm), prefix, \"" << e->shm
       << "\");\n";
    os << "  if (!thyme::ShmSpmcRing<" << Framed(e->cpp_type) << ", " << cap
       << ">::Prepare(nm)) return false;\n";
  }
  os << "  return true;\n";
  os << "}\n\n";

  os << "inline bool OpenBroadcastShmChannels(BroadcastTransports& l,\n";
  os << "                                     const char* prefix, const char* role,\n";
  os << "                                     uint64_t timeout_ns) {\n";
  os << "  char nm[" << kMpNameBufSize << "];\n";
  for (const auto* e : regions) {
    const Node* fn = FindNodeById(g, e->from);
    std::set<std::string> procs;
    if (fn != nullptr && !fn->process.empty()) procs.insert(fn->process);
    for (const auto& r : g.edges) {
      if (!IsBroadcastDelivery(r) || r.shm != e->shm) continue;
      const Node* tn = FindNodeById(g, r.to);
      if (tn != nullptr && !tn->process.empty()) procs.insert(tn->process);
    }
    os << "  thyme::app::MpName(nm, sizeof(nm), prefix, \"" << e->shm
       << "\");\n";
    os << "  if (";
    bool first_proc = true;
    for (const auto& p : procs) {
      if (!first_proc) os << " ||\n      ";
      first_proc = false;
      os << "std::strcmp(role, \"" << p << "\") == 0";
    }
    if (procs.empty()) os << "false";
    os << ") {\n";
    os << "    if (!l." << BroadcastRingMember(*e)
       << ".Open(nm, timeout_ns)) return false;\n";
    os << "  }\n";
  }
  for (const auto& e : g.edges) {
    if (!IsBroadcastDelivery(e) || IsPinnedSlotDelivery(g, cat, e)) continue;
    const Node* tn = FindNodeById(g, e.to);
    const std::string to_proc = tn != nullptr ? tn->process : "";
    const char* mode =
        (e.read == "follow") ? "Follow" : "Latest";
    os << "  if (std::strcmp(role, \"" << to_proc << "\") == 0) {\n";
    os << "    if (!l." << BroadcastReaderMember(e) << ".Bind(&l."
       << BroadcastRingMember(e) << ", thyme::SpmcRead::" << mode
       << ")) return false;\n";
    os << "  }\n";
  }
  os << "  return true;\n";
  os << "}\n";
}

std::string BuildMpUnknown(const Graph& g, const TemplateCatalog& cat) {
  const bool has_event = HasEventNode(g);
  const bool has_broadcast = HasBroadcastEdge(g, cat);
  const bool has_pinned_slot = HasPinnedSlotShmEdge(g, cat);
  const bool has_hub = HasLocalFrameHub(g, cat);
  bool has_transport_record_tap = false;
  for (const auto& e : g.edges) {
    if (e.record && !IsPinnedSlotDelivery(g, cat, e) &&
        (EdgeUsesEventPipe(g, e) || IsBroadcastDelivery(e))) {
      has_transport_record_tap = true;
      break;
    }
  }
  bool build_ok = true;
  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n\n";
  os << "#include \"generated/graph_ir.h\"\n";
  os << "#include \"thyme/runtime/cyclic_thread.h\"\n";
  bool has_queue = false;
  if (has_event) {
    os << "#include \"thyme/runtime/event_thread.h\"\n";
    os << "#include \"thyme/runtime/notif_pipe.h\"\n";
    if (has_hub) {
      os << "#include \"thyme/runtime/local_hub.h\"\n";
      os << "#include <cstring>\n";
    }
    for (const auto& e : g.edges) {
      if (IsQueueDelivery(e)) has_queue = true;
    }
    if (has_queue) {
      os << "#include \"thyme/runtime/shm_region.h\"\n";
      os << "#include \"thyme/runtime/shm_spsc_ring.h\"\n";
      os << "#include \"thyme/app/mp_control.h\"\n";
      os << "#include <atomic>\n";
      os << "#include <cstdio>\n";
      os << "#include <cstring>\n";
      os << "#include <new>\n";
    }
  }
  if (has_broadcast) {
    os << "#include \"thyme/runtime/shm_spmc_ring.h\"\n";
    if (!has_queue) {
      os << "#include \"thyme/app/mp_control.h\"\n";
      os << "#include <cstdio>\n";
      os << "#include <cstring>\n";
    }
  }
  if (has_pinned_slot) {
    os << "#include \"thyme/runtime/pinned_slot_store.h\"\n";
    if (!has_queue) {
      os << "#include <cstdio>\n";
      os << "#include <cstring>\n";
    }
  }
  os << "#include \"thyme/runtime/health_slot.h\"\n";
  if (has_transport_record_tap) {
    os << "#include \"support/record_mirror_ctx.h\"\n";
  }
  os << "#include \"generated/mp_role_locals.h\"\n";
  os << cat.mp_includes;
  for (const auto& n : g.nodes) {
    if (IsKnownType(cat, n.type) && !IsEventNode(n)) continue;
    os << "#include \"nodes/" << n.id << ".h\"\n";
  }
  os << "\n\n";

  // EventTransports owns every edge into an exec=event node (in-proc NotifPipe
  // and/or ShmSpscRing) plus wake words for queue consumers. Factories take
  // one bundle so adding an event edge only regenerates this struct — RunRole
  // argument lists stay fixed.
  std::vector<const Edge*> q_edges;
  std::vector<std::string> wake_nodes;
  if (has_event) {
    for (const auto& e : g.edges) {
      if (IsQueueDelivery(e)) q_edges.push_back(&e);
      if (!e.shm.empty() && IsPinnedSlotDelivery(g, cat, e) &&
          std::find(wake_nodes.begin(), wake_nodes.end(), e.to) == wake_nodes.end()) {
        wake_nodes.push_back(e.to);
      }
    }
    for (const auto* e : q_edges) {
      if (std::find(wake_nodes.begin(), wake_nodes.end(), e->to) ==
          wake_nodes.end()) {
        wake_nodes.push_back(e->to);
      }
    }
    os << "// Role-local event plumbing: one member per edge into an\n";
    os << "// exec=event node, plus futex wake state for queue consumers.\n";
    os << "// OpenEventShmChannels maps the shm rings/wakes; in-proc pipes\n";
    os << "// need no Open. Factories take EventTransports& instead of a flat\n";
    os << "// per-edge parameter list.\n";
    os << "struct EventTransports {\n";
    for (const auto& e : g.edges) {
      if (!EdgeUsesEventPipe(g, e) || IsLocalFrameHubEdge(g, cat, e)) continue;
      os << "  " << EventEdgeTransportType(g, e) << " " << EventPipeMember(e)
         << ";\n";
    }
    for (const auto& hub : BuildLocalFrameHubs(g, cat)) {
      os << "  " << LocalFrameHubType(g, hub) << " " << LocalFrameHubMember(hub)
         << ";\n";
    }
    for (const auto& id : wake_nodes) {
      os << "  thyme::ShmRegion " << id << "_wake_region;\n";
      os << "  std::atomic<uint32_t>* " << id << "_wake = nullptr;\n";
    }
    os << "};\n\n";
  }
  if (has_hub) {
    const auto hubs = BuildLocalFrameHubs(g, cat);
    os << "inline bool BindLocalFrameHubOutputs(EventTransports& l,\n"
          "                                     thyme::HealthSlot* health) {\n";
    for (const auto& hub : hubs) {
      for (const auto* edge : hub.edges) {
        std::size_t health_index = 0;
        for (std::size_t i = 0; i < g.nodes.size(); ++i) {
          if (g.nodes[i].id == edge->to) health_index = i;
        }
        os << "  if (!l." << LocalFrameHubMember(hub) << ".BindOutput("
           << LocalFrameHubOutputIndex(hub, *edge) << ", &health["
           << health_index << "])) return false;\n";
      }
    }
    os << "  return true;\n}\n\n";
    os << "class LocalHubDispatcher final : public thyme::EventThread {\n"
          " public:\n"
          "  LocalHubDispatcher(const char* role, EventTransports* transports,\n"
          "                     thyme::HealthSlot* health)\n"
          "      : thyme::EventThread(\"local_hubs\", health, 100), transports_(transports) {\n";
    for (std::size_t i = 0; i < hubs.size(); ++i) {
      os << "    active_[" << i << "] = std::strcmp(role, \"" << hubs[i].process
         << "\") == 0;\n";
    }
    os << "    thyme::Waitable* ws[" << hubs.size() << "];\n"
          "    std::size_t wake_count = 0;\n";
    for (std::size_t i = 0; i < hubs.size(); ++i) {
      os << "    if (active_[" << i << "]) ws[wake_count++] = &transports_->"
         << LocalFrameHubMember(hubs[i]) << ";\n";
    }
    os << "    SetWaitInputs(ws, wake_count);\n  }\n"
          "  void Dispatch() override {\n";
    for (std::size_t i = 0; i < hubs.size(); ++i) {
      os << "    if (active_[" << i << "]) transports_->"
         << LocalFrameHubMember(hubs[i]) << ".Dispatch();\n";
    }
    os << "  }\n"
          "  bool active() const {\n"
          "    for (bool value : active_) { if (value) return true; }\n"
          "    return false;\n  }\n"
          " private:\n"
          "  EventTransports* transports_;\n"
          "  bool active_[" << hubs.size() << "]{};\n};\n\n";
  }

  if (has_broadcast) AppendBroadcastShmHelpers(os, g, cat);
  if (has_pinned_slot) AppendPinnedSlotShmHelpers(os, g, cat);

  if (has_event) {
    os << "inline void SetEventRecordTaps(EventTransports& l) {\n";
    std::set<std::string> tapped_hubs;
    for (const auto& e : g.edges) {
      if (!e.record || IsPinnedSlotDelivery(g, cat, e) || !EdgeUsesEventPipe(g, e)) continue;
      if (IsLocalFrameHubEdge(g, cat, e)) {
        if (!tapped_hubs.insert(e.channel).second) continue;
        LocalFrameHub hub;
        if (!FindLocalFrameHub(g, cat, e, &hub)) return {};
        os << "  l." << LocalFrameHubMember(hub) << ".input()";
      } else {
        os << "  l." << EventPipeMember(e);
      }
      os << ".SetRecordTap(record_mirror::Page(), static_cast<uint16_t>(RecordStructId::k"
         << PascalCase(e.channel) << "));\n";
    }
    os << "}\n\n";
    os << "inline void ClearEventRecordTaps(EventTransports& l) {\n";
    tapped_hubs.clear();
    for (const auto& e : g.edges) {
      if (!e.record || IsPinnedSlotDelivery(g, cat, e) || !EdgeUsesEventPipe(g, e)) continue;
      if (IsLocalFrameHubEdge(g, cat, e)) {
        if (!tapped_hubs.insert(e.channel).second) continue;
        LocalFrameHub hub;
        if (!FindLocalFrameHub(g, cat, e, &hub)) return {};
        os << "  l." << LocalFrameHubMember(hub) << ".input()";
      } else {
        os << "  l." << EventPipeMember(e);
      }
      os << ".ClearRecordTap();\n";
    }
    os << "}\n\n";
  }
  if (has_broadcast) {
    os << "inline void SetBroadcastRecordTaps(BroadcastTransports& l) {\n";
    std::set<std::string> tapped;
    for (const auto& e : g.edges) {
      if (!e.record || IsPinnedSlotDelivery(g, cat, e) || !IsBroadcastDelivery(e) || !tapped.insert(e.channel).second) {
        continue;
      }
      os << "  l." << BroadcastRingMember(e)
         << ".SetRecordTap(record_mirror::Page(), static_cast<uint16_t>(RecordStructId::k"
         << PascalCase(e.channel) << "));\n";
    }
    os << "}\n\n";
    os << "inline void ClearBroadcastRecordTaps(BroadcastTransports& l) {\n";
    std::set<std::string> cleared;
    for (const auto& e : g.edges) {
      if (!e.record || IsPinnedSlotDelivery(g, cat, e) || !IsBroadcastDelivery(e) || !cleared.insert(e.channel).second) {
        continue;
      }
      os << "  l." << BroadcastRingMember(e) << ".ClearRecordTap();\n";
    }
    os << "}\n\n";
  }

  os << "// Constructs the handwritten thread for an unknown node type, using\n";
  os << "// RoleLocals the role code assembles before dispatch.\n";
  os << "// Returns nullptr for indices that are not unknown nodes (or if the\n";
  os << "// graph has no unknown nodes); RunRole then stderr+exit 2.\n";
  os << "inline thyme::CyclicThread* MakeUnknownThread(\n";
  os << "    std::size_t node_index,\n";
  os << "    RoleLocals& l";
  if (has_event) {
    os << ",\n    EventTransports& ev";
  }
  if (has_broadcast) {
    os << ",\n    BroadcastTransports& bc";
  }
  if (has_pinned_slot) {
    os << ",\n    PinnedSlotTransports& bf";
  }
  os << ",\n    thyme::HealthSlot* health_slot) {\n";
  os << "  (void)node_index;\n";
  os << "  (void)l;\n";
  if (has_event) os << "  (void)ev;\n";
  if (has_broadcast) os << "  (void)bc;\n";
  if (has_pinned_slot) os << "  (void)bf;\n";
  os << "  (void)health_slot;\n";
  const std::string ev_prefix = has_event ? "ev." : "";
  const std::string bc_prefix = has_broadcast ? "bc." : "";
  const std::string loc = "l";
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    const auto& n = g.nodes[i];
    if (IsKnownType(cat, n.type)) continue;
    if (IsEventNode(n)) continue;  // EventThread bases: MakeEventThread below
    const std::string cls = PascalCase(n.type) + "Thread";
    os << "  if (node_index == " << i << ") {\n";
    os << "    return new " << cls << "("
       << UnknownCtorGrouped(g, n, cat, "health_slot", ev_prefix, loc,
                             bc_prefix, "bf.")
       << ");\n";
    os << "  }\n";
  }
  os << "  return nullptr;\n";
  os << "}\n";
  if (!has_event) {
    if (!build_ok) return std::string{};
    return os.str();
  }

  // Event-node factory:
  os << "\n";
  os << "// Constructs the handwritten thread for an exec=event node, with its\n";
  os << "// in-edge pipes registered as wait inputs. Queue consumers also get\n";
  os << "// SetWakeWord on the shared _wk_<node> word (mandatory; roles must\n";
  os << "// not hand-wire). Returns nullptr for indices that are not event\n";
  os << "// nodes.\n";
  os << "inline thyme::EventThread* MakeEventThread(\n";
  os << "    std::size_t node_index,\n";
  os << "    EventTransports& ev,\n";
  os << "    RoleLocals& l";
  if (has_broadcast) {
    os << ",\n    BroadcastTransports& bc";
  }
  if (has_pinned_slot) {
    os << ",\n    PinnedSlotTransports& bf";
  }
  os << ",\n    thyme::HealthSlot* health_slot) {\n";
  os << "  (void)node_index;\n";
  os << "  (void)ev;\n";
  os << "  (void)l;\n";
  if (has_broadcast) os << "  (void)bc;\n";
  if (has_pinned_slot) os << "  (void)bf;\n";
  os << "  (void)health_slot;\n";
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    const auto& n = g.nodes[i];
    if (!IsEventNode(n)) continue;
    const KnownCat* k = FindKnown(cat, n.type);
    const std::string cls = HandThreadClassName(cat, n);
    os << "  if (node_index == " << i << ") {\n";
    if (k != nullptr) {
      bool ok = false;
      const std::string grouped =
          KnownCtorGrouped(g, cat, n, "health_slot", &ok, "ev.", "l",
                           bc_prefix, "bf.");
      if (!ok) {
        build_ok = false;
        break;
      }
      os << "    auto* t = new " << cls << "(" << grouped << ");\n";
    } else {
      os << "    auto* t = new " << cls << "("
         << UnknownCtorGrouped(g, n, cat, "health_slot", "ev.", "l", bc_prefix,
                               "bf.")
         << ");\n";
    }
    std::vector<std::string> ins;
    for (const auto& e : g.edges) {
      if (e.to != n.id) continue;
      if (IsPinnedSlotDelivery(g, cat, e)) {
        ins.push_back("&bf." + PinnedSlotReaderMember(e));
      } else if (IsBroadcastDelivery(e)) {
        ins.push_back("&bc." + BroadcastReaderMember(e));
      } else if (IsLocalFrameHubEdge(g, cat, e)) {
        ins.push_back(LocalFrameHubOutputRef(g, cat, e, "ev."));
      } else if (EdgeUsesEventPipe(g, e)) {
        ins.push_back("&ev." + EventPipeMember(e));
      }
    }
    if (!ins.empty()) {
      os << "    thyme::Waitable* ws[] = {";
      for (std::size_t j = 0; j < ins.size(); ++j) {
        if (j != 0) os << ", ";
        os << ins[j];
      }
      os << "};\n";
      os << "    t->SetWaitInputs(ws, sizeof(ws) / sizeof(ws[0]));\n";
    }
    if (NodeNeedsPlacement(n) && !AppendPlacement(os, g, n, "(*t)")) {
      return {};
    }
    if (std::find(wake_nodes.begin(), wake_nodes.end(), n.id) !=
        wake_nodes.end()) {
      os << "    if (ev." << n.id << "_wake == nullptr) {\n";
      os << "      std::fprintf(stderr, \"MakeEventThread: missing wake word "
            "for event node '%s'\\n\", \""
         << n.id << "\");\n";
      os << "      delete t;\n";
      os << "      return nullptr;\n";
      os << "    }\n";
      os << "    t->SetWakeWord(ev." << n.id << "_wake);\n";
    }
    os << "    return t;\n";
    os << "  }\n";
  }
  os << "  return nullptr;\n";
  os << "}\n";

  // Queue-channel Open/Prepare: EventTransports already owns the rings and
  // wake words. PinnedSlot channels own their rings but share these wake words.
  // Supervisor Prepares (Unlink+Init); roles only Open. Producers attach the
  // shared wake word on rings here; consumers attach via MakeEventThread above.
  if (!q_edges.empty() || !wake_nodes.empty()) {
    os << "\n";
    os << "// Queue channels (shm edges into exec=event nodes): supervisor\n";
    os << "// Prepare (Unlink+Init); roles Open only. Producer rings get\n";
    os << "// SetWakeWord here; consumer EventThreads get it inside\n";
    os << "// MakeEventThread (no role-side wake switch).\n";
    os << "inline void UnlinkEventShmChannels(const char* prefix) {\n";
    os << "  char nm[" << kMpNameBufSize << "];\n";
    for (const auto* e : q_edges) {
      os << "  thyme::app::MpName(nm, sizeof(nm), prefix, \"" << e->shm
         << "\");\n";
      os << "  thyme::ShmRegion::UnlinkName(nm);\n";
    }
    for (const auto& id : wake_nodes) {
      os << "  thyme::app::MpName(nm, sizeof(nm), prefix, \"_wk_" << id
         << "\");\n";
      os << "  thyme::ShmRegion::UnlinkName(nm);\n";
    }
    os << "}\n\n";
    os << "// Supervisor path: Unlink leftovers, Create+RawInit rings and wake\n";
    os << "// words, then Unmap. Named objects remain for role Open.\n";
    os << "inline bool PrepareEventShmChannels(const char* prefix) {\n";
    os << "  char nm[" << kMpNameBufSize << "];\n";
    for (const auto* e : q_edges) {
      const std::string ring_ty = EventEdgeTransportType(g, *e);
      os << "  thyme::app::MpName(nm, sizeof(nm), prefix, \"" << e->shm
         << "\");\n";
      os << "  if (!" << ring_ty << "::Prepare(nm)) return false;\n";
    }
    for (const auto& id : wake_nodes) {
      os << "  thyme::app::MpName(nm, sizeof(nm), prefix, \"_wk_" << id
         << "\");\n";
      os << "  thyme::ShmRegion::UnlinkName(nm);\n";
      os << "  {\n";
      os << "    thyme::ShmRegion wake_prep;\n";
      os << "    if (!wake_prep.Create(nm, sizeof(std::atomic<uint32_t>))) "
            "return false;\n";
      os << "    new (wake_prep.data()) std::atomic<uint32_t>(0);\n";
      os << "  }\n";
    }
    os << "  return true;\n";
    os << "}\n\n";
    os << "inline bool OpenEventShmChannels(EventTransports& l,\n";
    os << "                                 const char* prefix, const char* role,\n";
    os << "                                 uint64_t timeout_ns) {\n";
    os << "  char nm[" << kMpNameBufSize << "];\n";
    for (const auto* e : q_edges) {
      const Node* tn = FindNodeById(g, e->to);
      const Node* fn = FindNodeById(g, e->from);
      const std::string m = EventPipeMember(*e);
      os << "  thyme::app::MpName(nm, sizeof(nm), prefix, \"" << e->shm
         << "\");\n";
      os << "  if (std::strcmp(role, \"" << (tn ? tn->process : "")
         << "\") == 0 ||\n";
      os << "      std::strcmp(role, \"" << (fn ? fn->process : "")
         << "\") == 0) {\n";
      os << "    if (!l." << m << ".Open(nm, timeout_ns)) return false;\n";
      os << "  }\n";
    }
    for (const auto& id : wake_nodes) {
      os << "  thyme::app::MpName(nm, sizeof(nm), prefix, \"_wk_" << id
         << "\");\n";
      os << "  if (!l." << id
         << "_wake_region.OpenRetry(nm, sizeof(std::atomic<uint32_t>), "
            "timeout_ns)) return false;\n";
      os << "  l." << id << "_wake = static_cast<std::atomic<uint32_t>*>(l."
         << id << "_wake_region.data());\n";
    }
    // Producer side: every ring it feeds bumps the consuming node's word.
    for (const auto* e : q_edges) {
      const Node* fn = FindNodeById(g, e->from);
      os << "  if (std::strcmp(role, \"" << (fn ? fn->process : "")
         << "\") == 0) {\n";
      os << "    if (l." << e->to << "_wake == nullptr) return false;\n";
      os << "    l." << EventPipeMember(*e) << ".SetWakeWord(l." << e->to
         << "_wake);\n";
      os << "  }\n";
    }
    os << "  return true;\n";
    os << "}\n";
  }
  if (!build_ok) return std::string{};
  return os.str();
}

// Shared unknown-node generated base: record/Ports/members are common;
// only Loop (cyclic) vs Dispatch (event) is the strategy split.
enum class GeneratedExecKind { CyclicLoop, EventDispatch };

// SCH-04: graph.yaml `overrun` -> generated enum expression.
static const char* OverrunPolicyExpr(const std::string& value) {
  if (value == "degrade") return "thyme::OverrunPolicy::kDegrade";
  if (value == "restart") return "thyme::OverrunPolicy::kRestart";
  return "thyme::OverrunPolicy::kSkip";
}

static bool HasQueueEventOut(const Graph& g, const NodeEdges& ed) {
  for (const auto* e : ed.out) {
    if (EdgeToEventNode(g, *e) && IsQueueDelivery(*e)) return true;
  }
  return false;
}

// GEN-12: record cross-cutting (page/tap/replay pause) is emitted only when the
// node produces a recorded channel. Consumers still need the registry when
// they read a recorded channel (NoteLatencyEdge ids).
static bool HasRecordedOut(const NodeEdges& ed) {
  for (const auto* e : ed.out) if (e->record) return true;
  return false;
}
static bool HasRecordedIn(const NodeEdges& ed) {
  for (const auto* e : ed.in) if (e->record) return true;
  return false;
}

static void AppendPublishHelper(std::ostringstream& os) {
  os << "  // One typed publish shape for Frame and PinnedSlot. The status keeps\n";
  os << "  // backpressure (reject), overwrite, gate and pin failures visible to the\n";
  os << "  // caller; [[nodiscard]] makes ignoring a failed command a warning.\n";
  os << "  template <typename Channel, typename T>\n";
  os << "  [[nodiscard]] thyme::ChannelStatus Publish(Channel& channel,\n";
  os << "                                            const T& value) {\n";
  os << "    auto out = channel.BeginWrite();\n";
  os << "    if (out.status != thyme::ChannelStatus::kOk) return out.status;\n";
  os << "    if (out.payload == nullptr) {\n";
  os << "      channel.AbortWrite(&out);\n";
  os << "      return thyme::ChannelStatus::kBadLayout;\n";
  os << "    }\n";
  os << "    *out.payload = value;\n";
  os << "    return channel.CommitWrite(&out);\n";
  os << "  }\n\n";
}

static void AppendCreditQueuePushFailsHelper(std::ostringstream& os,
                                             const Graph& g,
                                             const NodeEdges& ed) {
  os << "  // M4.4: aggregate ShmSpscRing push_fail_count onto producer health.\n";
  os << "  void CreditQueuePushFails() {\n";
  for (const auto* e : ed.out) {
    if (!EdgeToEventNode(g, *e) || !IsQueueDelivery(*e)) continue;
    os << "    { uint32_t d = " << e->channel << "_.push_fail_count();\n";
    os << "      my_health_->AddDrops(d - drop_" << e->channel << "_);\n";
    os << "      drop_" << e->channel << "_ = d; }\n";
  }
  os << "  }\n\n";
}

static void AppendQueueDropShadowMembers(std::ostringstream& os, const Graph& g,
                                         const NodeEdges& ed) {
  for (const auto* e : ed.out) {
    if (!EdgeToEventNode(g, *e) || !IsQueueDelivery(*e)) continue;
    os << "  uint32_t drop_" << e->channel << "_{0};\n";
  }
}


// ADR-0016: local index queues and raw pool/IndexPipe edges keep their
// existing zero-payload-copy protocols; only the generated transaction differs.
bool IsTickQueueEdge(const Graph& g, const TemplateCatalog& cat, const Edge& e) {
  if (IsPinnedSlotDelivery(g, cat, e) || IsBroadcastDelivery(e) ||
      EdgeUsesEventPipe(g, e)) return false;
  const PoolCat* p = FindPool(cat, e.pool);
  return p != nullptr && !p->index_pipe;
}

bool IsTickIndexPipeEdge(const Graph& g, const TemplateCatalog& cat, const Edge& e) {
  if (IsPinnedSlotDelivery(g, cat, e) || IsBroadcastDelivery(e) ||
      EdgeUsesEventPipe(g, e)) return false;
  const PoolCat* p = FindPool(cat, e.pool);
  return p == nullptr || p->index_pipe;
}

static std::string KnownCtorMemberBase(const std::string& tok);

bool HasTickQueueEdge(const Graph& g, const TemplateCatalog& cat,
                      const NodeEdges& ed) {
  for (const auto* e : ed.in) if (IsTickQueueEdge(g, cat, *e)) return true;
  for (const auto* e : ed.out) if (IsTickQueueEdge(g, cat, *e)) return true;
  return false;
}

bool IsTickFrameInput(const Graph& g, const TemplateCatalog& cat, const Edge& e) {
  return IsBroadcastDelivery(e) && !IsPinnedSlotDelivery(g, cat, e);
}

bool IsTickPinnedInput(const Graph& g, const TemplateCatalog& cat, const Edge& e) {
  return IsPinnedSlotDelivery(g, cat, e);
}

bool IsTickFrameOutput(const Graph& g, const TemplateCatalog& cat, const Edge& e) {
  return EdgeUsesEventPipe(g, e) ||
         (IsBroadcastDelivery(e) && !IsPinnedSlotDelivery(g, cat, e));
}

bool IsTickInputSupported(const Graph& g, const TemplateCatalog& cat, const Edge& e) {
  return IsTickQueueEdge(g, cat, e) || IsTickIndexPipeEdge(g, cat, e) ||
         IsTickFrameInput(g, cat, e) || IsTickPinnedInput(g, cat, e);
}

bool IsTickOutputSupported(const Graph& g, const TemplateCatalog& cat, const Edge& e) {
  return IsTickQueueEdge(g, cat, e) || IsTickIndexPipeEdge(g, cat, e) ||
         IsTickFrameOutput(g, cat, e) || IsPinnedSlotDelivery(g, cat, e);
}

// Convert only when every edge is represented in TickIn/TickOut. This avoids
// silently dropping an unsupported input just because the outputs are supported.
bool HasTickEdge(const Graph& g, const TemplateCatalog& cat,
                 const NodeEdges& ed) {
  for (const auto* e : ed.in) {
    if (!IsTickInputSupported(g, cat, *e)) return false;
  }
  for (const auto* e : ed.out) {
    if (!IsTickOutputSupported(g, cat, *e)) return false;
  }
  return !ed.in.empty() || !ed.out.empty();
}

// Four-step Loop: replay pause, read inputs + borrow outputs, OnTick, then
// commit-ready/abort/release plus queue credit. The hand node only fills the
// tick's result; slot lifetime, recording and counters stay generated.
void EmitTickLoop(std::ostringstream& os, const Graph& g,
                  const TemplateCatalog& cat, const Node& n,
                  const NodeEdges& ed, const KnownCat* known,
                  bool replay_pause, bool recordable, bool has_queue_out) {
  auto member_of = [&](const Edge& e) {
    if (known != nullptr) {
      const PoolCat* pool = FindPool(cat, e.pool);
      if (pool != nullptr) return KnownCtorMemberBase(pool->prefix + "_queue");
    }
    return e.channel + "_";
  };
  auto pool_of = [&](const Edge& e) {
    if (known != nullptr) {
      const PoolCat* pool = FindPool(cat, e.pool);
      if (pool != nullptr) return KnownCtorMemberBase(pool->prefix + "_pool");
    }
    return e.channel + "_";
  };
  auto pipe_of = [&](const Edge& e) {
    if (known != nullptr) {
      const PoolCat* pool = FindPool(cat, e.pool);
      if (pool != nullptr) return KnownCtorMemberBase(pool->prefix + "_pipe");
    }
    return e.channel + "_r_";
  };
  auto out_member_of = [&](const Edge& e) {
    return e.channel + ((IsBroadcastDelivery(e) || IsPinnedSlotDelivery(g, cat, e))
                            ? "_w_"
                            : "_");
  };
  std::vector<const Edge*> tick_outs;
  std::set<std::string> out_channels;
  for (const auto* e : ed.out) {
    if (out_channels.insert(e->channel).second) tick_outs.push_back(e);
  }
  bool has_pinned_out = false;
  for (const auto* e : tick_outs) {
    if (IsPinnedSlotDelivery(g, cat, *e)) has_pinned_out = true;
  }

  auto emit_begin = [&](const Edge& e, const std::string& ind) {
    const std::string& ch = e.channel;
    if (IsTickQueueEdge(g, cat, e)) {
      const std::string m = member_of(e);
      os << ind << "auto out_" << ch << " = " << m << ".BeginWrite();\n";
      os << ind << "out." << ch << " = out_" << ch << ".slot;\n";
      os << ind << "out.ready_" << ch << " = out_" << ch << ".ok();\n";
    } else if (IsTickIndexPipeEdge(g, cat, e)) {
      const std::string pool = pool_of(e);
      os << ind << "const uint32_t out_" << ch << "_index = " << pool << "->acquire();\n";
      os << ind << "uint32_t out_" << ch << "_generation = 0;\n";
      os << ind << "if (out_" << ch << "_index != " << e.pool << "::kInvalidIndex) {\n";
      os << ind << "  out_" << ch << "_generation = " << pool << "->generation(out_"
         << ch << "_index);\n";
      os << ind << "  out." << ch << " = &" << pool << "->at(out_" << ch << "_index);\n";
      os << ind << "  out.ready_" << ch << " = true;\n";
      os << ind << "}\n";
    } else {
      const std::string m = out_member_of(e);
      if (IsPinnedSlotDelivery(g, cat, e)) {
        os << ind << "auto out_" << ch << " = " << m << ".BeginWriteAt(request.size_"
           << ch << ", 0);\n";
        os << ind << "out." << ch << " = out_" << ch << ".data;\n";
        os << ind << "out.size_" << ch << " = out_" << ch << ".size;\n";
      } else {
        os << ind << "auto out_" << ch << " = " << m << ".BeginWrite();\n";
        os << ind << "out." << ch << " = out_" << ch << ".payload;\n";
      }
      os << ind << "out.ready_" << ch << " = out_" << ch
         << ".status == thyme::ChannelStatus::kOk && out." << ch << " != nullptr;\n";
    }
  };

  auto emit_abort = [&](const Edge& e, const std::string& ind) {
    const std::string& ch = e.channel;
    if (IsTickQueueEdge(g, cat, e)) {
      os << ind << member_of(e) << ".AbortWrite(out_" << ch << ");\n";
    } else if (IsTickIndexPipeEdge(g, cat, e)) {
      os << ind << "if (out_" << ch << "_index != " << e.pool << "::kInvalidIndex) {\n";
      os << ind << "  " << pool_of(e) << "->abort_write(out_" << ch << "_index);\n";
      os << ind << "}\n";
    } else {
      os << ind << out_member_of(e) << ".AbortWrite(&out_" << ch << ");\n";
    }
  };

  auto emit_commit = [&](const Edge& e, const std::string& ind) {
    const std::string& ch = e.channel;
    if (IsTickIndexPipeEdge(g, cat, e)) {
      os << ind << pool_of(e) << "->publish(out_" << ch << "_index);\n";
      os << ind << "uint32_t dropped_generation = 0;\n";
      os << ind << "const uint32_t dropped = " << pipe_of(e) << "->push_latest(out_" << ch
         << "_index, out_" << ch << "_generation, &dropped_generation);\n";
      os << ind << "if (dropped != 0xFFFFFFFFu) {\n";
      os << ind << "  (void)" << pool_of(e) << "->release(dropped, dropped_generation);\n";
      os << ind << "}\n";
      if (recordable && e.record) {
        os << ind << "RecordTap(RecordStructId::k" << PascalCase(ch) << ", *out." << ch << ");\n";
      }
      return;
    }
    if (IsTickQueueEdge(g, cat, e)) {
      os << ind << "if (" << member_of(e) << ".CommitWrite(out_" << ch << ")) {\n";
    } else {
      os << ind << "if (" << out_member_of(e) << ".CommitWrite(&out_" << ch
         << ") == thyme::ChannelStatus::kOk) {\n";
    }
    if (recordable && e.record && !IsPinnedSlotDelivery(g, cat, e)) {
      if (IsTickQueueEdge(g, cat, e)) {
        os << ind << "  RecordTap(RecordStructId::k" << PascalCase(ch) << ", *out." << ch << ");\n";
      } else {
        os << ind << "  RecordTap(RecordStructId::k" << PascalCase(ch) << ", *out_" << ch
           << ".frame_ptr());\n";
      }
    }
    os << ind << "}\n";
  };

  os << "  void Loop() override final {\n";
  if (replay_pause) {
    os << "    if (ReplayProducerShouldPause(name())) return;\n";
  }
  os << "    TickIn in{};\n";
  os << "    TickOut out{};\n";
  for (const auto* e : ed.in) {
    if (!IsTickQueueEdge(g, cat, *e)) continue;
    const std::string m = member_of(*e);
    os << "    decltype(" << m << ")::Read in_" << e->channel << ";\n";
    os << "    uint32_t skipped_" << e->channel << " = 0;\n";
    os << "    for (uint32_t budget = 0; budget < kRingCap; ++budget) {\n";
    os << "      decltype(" << m << ")::Read next;\n";
    os << "      if (!" << m << ".TryRead(next)) break;\n";
    os << "      if (in_" << e->channel << ".slot != nullptr) {\n";
    os << "        " << m << ".Release(in_" << e->channel << ");\n";
    os << "        ++skipped_" << e->channel << ";\n";
    os << "      }\n";
    os << "      in_" << e->channel << " = next;\n";
    os << "    }\n";
    os << "    in." << e->channel << " = in_" << e->channel << ".slot;\n";
    os << "    in.skipped_" << e->channel << " = skipped_" << e->channel << ";\n";
  }
  for (const auto* e : ed.in) {
    if (!IsTickIndexPipeEdge(g, cat, *e)) continue;
    const std::string pool = pool_of(*e);
    const std::string pipe = pipe_of(*e);
    os << "    uint32_t in_" << e->channel << "_index = " << e->pool << "::kInvalidIndex;\n";
    os << "    uint32_t in_" << e->channel << "_generation = 0;\n";
    os << "    uint32_t skipped_" << e->channel << " = 0;\n";
    os << "    for (uint32_t budget = 0; budget < kRingCap; ++budget) {\n";
    os << "      uint32_t next_index = 0;\n";
    os << "      uint32_t next_generation = 0;\n";
    os << "      if (!" << pipe << "->try_pop(next_index, next_generation)) break;\n";
    os << "      if (in_" << e->channel << "_index != " << e->pool << "::kInvalidIndex) {\n";
    os << "        (void)" << pool << "->release(in_" << e->channel << "_index, in_"
       << e->channel << "_generation);\n";
    os << "        ++skipped_" << e->channel << ";\n";
    os << "      }\n";
    os << "      in_" << e->channel << "_index = next_index;\n";
    os << "      in_" << e->channel << "_generation = next_generation;\n";
    os << "    }\n";
    os << "    in.skipped_" << e->channel << " = skipped_" << e->channel << ";\n";
    os << "    if (in_" << e->channel << "_index != " << e->pool << "::kInvalidIndex) {\n";
    os << "      in." << e->channel << " = &" << pool << "->at(in_" << e->channel << "_index);\n";
    os << "    }\n";
  }
  for (const auto* e : ed.in) {
    if (!IsTickFrameInput(g, cat, *e)) continue;
    const std::string m = member_of(*e);
    os << "    typename decltype(" << m << ")::Sample latest_" << e->channel << ";\n";
    if (e->read == "follow") {
      os << "    uint32_t skipped_" << e->channel << " = 0;\n";
      os << "    for (uint32_t budget = 0; budget < " << EventEdgeCap(g, *e) << "; ++budget) {\n";
      os << "      typename decltype(" << m << ")::Sample next;\n";
      os << "      if (" << m << ".TryRead(&next) != thyme::ChannelStatus::kOk) break;\n";
      os << "      if (latest_" << e->channel << ".frame() != nullptr) ++skipped_"
         << e->channel << ";\n";
      os << "      latest_" << e->channel << " = std::move(next);\n";
      os << "    }\n";
      os << "    in.skipped_" << e->channel << " = skipped_" << e->channel << ";\n";
    } else {
      os << "    (void)" << m << ".TryRead(&latest_" << e->channel << ");\n";
    }
    os << "    in." << e->channel << " = latest_" << e->channel << ".frame();\n";
    if (n.produce != "many") {
      os << "    if (" << m << ".drop_count_observed()) {\n";
      os << "      const uint64_t d = " << m << ".drop_count();\n";
      os << "      if (d > drop_" << e->channel << "_) my_health_->AddDrops("
            "static_cast<uint32_t>(d - drop_" << e->channel << "_));\n";
      os << "      drop_" << e->channel << "_ = d;\n";
      os << "    }\n";
    }
  }
  for (const auto* e : ed.in) {
    if (!IsTickPinnedInput(g, cat, *e)) continue;
    const std::string m = member_of(*e);
    os << "    typename decltype(" << m << ")::Sample pinned_" << e->channel << ";\n";
    if (e->read == "follow") {
      os << "    uint32_t skipped_" << e->channel << " = 0;\n";
      os << "    for (uint32_t budget = 0; budget < " << EventEdgeCap(g, *e) << "; ++budget) {\n";
      os << "      typename decltype(" << m << ")::Sample next;\n";
      os << "      if (" << m << ".TryRead(&next) != thyme::ChannelStatus::kOk) break;\n";
      os << "      if (pinned_" << e->channel << ".data != nullptr) ++skipped_"
         << e->channel << ";\n";
      os << "      pinned_" << e->channel << " = std::move(next);\n";
      os << "    }\n";
      os << "    in.skipped_" << e->channel << " = skipped_" << e->channel << ";\n";
    } else {
      os << "    (void)" << m << ".TryRead(&pinned_" << e->channel << ");\n";
    }
    os << "    in." << e->channel << " = pinned_" << e->channel << ".data;\n";
    os << "    in.size_" << e->channel << " = pinned_" << e->channel << ".size;\n";
  }
  if (n.produce != "many") {
    if (has_pinned_out) {
      os << "    TickRequest request{};\n";
      os << "    PrepareOutput(in, &request);\n";
    }
    for (const auto* e : tick_outs) emit_begin(*e, "    ");
    os << "    const bool publish = OnTick(in, &out);\n";
    for (const auto* e : tick_outs) {
      os << "    if (publish && out.ready_" << e->channel << ") {\n";
      emit_commit(*e, "      ");
      os << "    } else {\n";
      emit_abort(*e, "      ");
      os << "    }\n";
    }
  } else {
    for (const auto* e : tick_outs) {
      os << "    for (uint32_t budget = 0; budget < kRingCap; ++budget) {\n";
      if (IsPinnedSlotDelivery(g, cat, *e)) {
        os << "      TickRequest request{};\n";
        os << "      PrepareOutput(in, &request);\n";
      }
      emit_begin(*e, "      ");
      os << "      if (!out.ready_" << e->channel << ") break;\n";
      os << "      if (!OnProduce(in, &out)) {\n";
      emit_abort(*e, "        ");
      os << "        break;\n";
      os << "      }\n";
      emit_commit(*e, "      ");
      os << "    }\n";
    }
  }
  for (const auto* e : ed.in) {
    if (IsTickQueueEdge(g, cat, *e)) {
      const std::string m = member_of(*e);
      os << "    if (in_" << e->channel << ".slot != nullptr) " << m << ".Release(in_"
         << e->channel << ");\n";
    } else if (IsTickIndexPipeEdge(g, cat, *e)) {
      os << "    if (in_" << e->channel << "_index != " << e->pool << "::kInvalidIndex) {\n";
      os << "      (void)" << pool_of(*e) << "->release(in_" << e->channel << "_index, in_"
         << e->channel << "_generation);\n";
      os << "    }\n";
    }
  }
  for (const auto* e : ed.in) {
    if (IsTickPinnedInput(g, cat, *e)) {
      os << "    pinned_" << e->channel << ".Reset();\n";
    }
  }
  if (has_queue_out) os << "    CreditQueuePushFails();\n";
  os << "  }\n\n";

  os << "  struct TickIn {\n";
  for (const auto* e : ed.in) {
    if (IsTickPinnedInput(g, cat, *e)) {
      os << "    const uint8_t* " << e->channel << " = nullptr;\n";
      os << "    uint32_t size_" << e->channel << " = 0;\n";
    } else {
      os << "    const " << Framed(e->cpp_type) << "* " << e->channel << " = nullptr;\n";
    }
    // Frames discarded between this tick and the previous one: exactly the
    // older entries the drain loop released. 0 for a pure latest read (the
    // backend cannot see intermediate publishes), so treat 0 as "unknown"
    // there and as "no skip" for queue/follow inputs.
    os << "    uint32_t skipped_" << e->channel << " = 0;\n";
  }
  os << "  };\n\n";
  os << "  struct TickOut {\n";
  for (const auto* e : tick_outs) {
    if (IsTickQueueEdge(g, cat, *e) || IsTickIndexPipeEdge(g, cat, *e)) {
      os << "    " << Framed(e->cpp_type) << "* " << e->channel << " = nullptr;\n";
    } else if (IsPinnedSlotDelivery(g, cat, *e)) {
      os << "    uint8_t* " << e->channel << " = nullptr;\n";
      os << "    uint32_t size_" << e->channel << " = 0;\n";
    } else {
      os << "    " << e->cpp_type << "* " << e->channel << " = nullptr;\n";
    }
    os << "    bool ready_" << e->channel << " = false;\n";
  }
  os << "  };\n\n";
  if (has_pinned_out) {
    os << "  struct TickRequest {\n";
    for (const auto* e : tick_outs) {
      if (IsPinnedSlotDelivery(g, cat, *e)) {
        os << "    uint32_t size_" << e->channel << " = sizeof(" << e->cpp_type << ");\n";
      }
    }
    os << "  };\n\n";
  }
  for (const auto* e : ed.in) {
    if (!IsTickFrameInput(g, cat, *e)) continue;
    os << "  uint32_t drop_" << e->channel << "_{0};\n";
  }
  os << " protected:\n";
  if (has_pinned_out) {
    os << "  virtual void PrepareOutput(const TickIn&, TickRequest*) {}\n\n";
  }
  if (n.produce == "many") {
    os << "  virtual bool OnProduce(const TickIn& in, TickOut* out) = 0;\n\n";
  } else {
    os << "  virtual bool OnTick(const TickIn& in, TickOut* out) = 0;\n\n";
  }
  if (has_queue_out) AppendCreditQueuePushFailsHelper(os, g, ed);
}

bool EmitNodeGenerated(const fs::path& out_path, const Graph& g, const Node& n,
                       const TemplateCatalog& cat, GeneratedExecKind kind,
                       const KnownCat* known) {
  const NodeEdges ed = EdgesOf(g, n);
  const bool event = kind == GeneratedExecKind::EventDispatch;
  const std::string ports_name =
      known ? (known->cls + "Ports")
            : (PascalCase(n.shard_count > 1 ? n.id : n.type) + "Ports");
  const std::string cls =
      n.shard_count > 1
          ? PascalCase(n.id) + (event && known ? "EventGenerated" : "ThreadGenerated")
          : known ? (known->cls + (event ? "EventGenerated" : "Generated"))
                  : (PascalCase(n.type) + "ThreadGenerated");
  const bool recordable = HasRecordedOut(ed);
  const bool record_in = HasRecordedIn(ed);
  bool has_event_out = false;
  for (const auto* e : ed.out) {
    if (EdgeUsesEventPipe(g, *e)) {
      has_event_out = true;
      break;
    }
  }
  const bool has_queue_out = HasQueueEventOut(g, ed);
  bool has_publish = has_event_out;
  for (const auto* e : ed.out) {
    has_publish = has_publish || IsBroadcastDelivery(*e) || IsPinnedSlotDelivery(g, cat, *e);
  }
  std::ostringstream os;
  os << "// Code generated for the " << cat.banner << ". DO NOT EDIT.\n";
  os << "// Hand logic belongs in src/nodes/" << n.id << ".cpp.\n";
  os << "#pragma once\n";
  os << "#include \"thyme/runtime/"
     << (event ? "event_thread.h" : "cyclic_thread.h") << "\"\n";
  os << "#include \"thyme/runtime/write_section.h\"\n";
  os << "#include \"generated/mp_ports.h\"\n";
  AppendGeneratedRecordIncludes(os, recordable || record_in, /*need_ctime=*/event);
  if (!recordable && record_in) {
    os << "#include \"generated/record_registry.h\"\n";
  }
  os << "\nclass " << cls << " : public thyme::"
     << (event ? "EventThread" : "CyclicThread")
     << " {\n public:\n  " << cls << "(";
  os << "const " << ports_name << "& ports, const MpContext& ctx)\n";
  if (event) {
    os << "      : thyme::EventThread(\"" << n.id << "\", ctx.health, " << n.hz;
    if (n.deadline_ns > 0) os << ", " << n.deadline_ns << "ULL";
    os << "), my_health_(ctx.health)";
  } else {
    os << "      : thyme::CyclicThread(\"" << n.id << "\", " << n.hz
       << ", ctx.health), my_health_(ctx.health)";
  }
  for (const auto* e : ed.in) AppendEdgeChannelInit(os, g, cat, *e, /*as_out=*/false);
  {
    std::set<std::string> seen_w;
    for (const auto* e : ed.out) {
      if ((IsBroadcastDelivery(*e) || IsPinnedSlotDelivery(g, cat, *e) ||
         IsLocalFrameHubEdge(g, cat, *e)) &&
          !seen_w.insert(e->channel).second) continue;
      AppendEdgeChannelInit(os, g, cat, *e, /*as_out=*/true);
    }
  }
  if (recordable) os << ",\n        record_page_(thyme::app::record_mirror::RingPage())";
  const bool has_sched_setup =
      (event && n.input_budget > 0) || !n.overrun.empty();
  if (!has_sched_setup) {
    os << " {}\n\n";
  } else {
    os << " {\n";
    if (event && n.input_budget > 0) {
      os << "    // SCH-03: cap each in-edge at " << n.input_budget
         << " message(s) per Dispatch cycle (split node first if rates differ).\n";
      os << "    SetInputBudget(" << n.input_budget << "u, " << ed.in.size()
         << "u);\n";
    }
    if (!n.overrun.empty()) {
      os << "    // SCH-04: deadline-miss policy.\n";
      os << "    SetOverrunPolicy(" << OverrunPolicyExpr(n.overrun)
         << ");\n";
    }
    os << "  }\n\n";
  }

  if (event) {
    // EventThread: Dispatch drains each input pipe and invokes one pure
    // virtual On<Channel> handler per message; NoteHandled() ticks the
    // message-granular loops count.
    os << "  // Record/replay cross-cutting lives in the generated base: Dispatch\n";
    os << "  // pauses the producer while replay injects, and RecordTap mirrors\n";
    os << "  // one recordable value. The hand node overrides the On<Channel>\n";
    os << "  // handlers with the algorithm only. hz in graph.yaml is expected\n";
    os << "  // steady-state messages/s (stall 3/hz; MinLoops uses process min),\n";
    os << "  // not a cyclic Loop period.\n";
    os << "  void Dispatch() override final {\n";
    if (recordable || record_in) {
      os << "    if (ReplayProducerShouldPause(name())) {\n";
      os << "      // Paused for replay injection: the input pipe stays non-empty,\n";
      os << "      // so returning at once would busy-spin ThreadMain. Bounded wait.\n";
      os << "      struct timespec pause_ts {0, 1000000};\n";
      os << "      nanosleep(&pause_ts, nullptr);\n";
      os << "      return;\n";
      os << "    }\n";
    }
    for (std::size_t ii = 0; ii < ed.in.size(); ++ii) {
      const auto* e = ed.in[ii];
      const std::string budget_guard =
          n.input_budget > 0
              ? "InputBudgetAvailable(" + std::to_string(ii) + ") && "
              : "";
      os << "    {\n";
      const bool big = IsPinnedSlotDelivery(g, cat, *e);
      os << "      " << (big ? PinnedSlotRefType(g, cat, *e)
                              : FrameRefType(g, cat, *e)) << "::Sample v{};\n";
      auto append_handler = [&]() {
        if (n.input_budget > 0) {
          os << "        ConsumeInputBudget(" << ii << ");\n";
        }
        os << "        NoteHandled();\n";
        if (e->record) {
          os << "        NoteInFrame(v.sequence & 0xF);\n";
          os << "        const uint64_t perf_now = thyme::app::NowMonoNs();\n";
          os << "        const uint64_t perf_lat = perf_now >= v.mono_ns\n";
          os << "                                      ? perf_now - v.mono_ns\n";
          os << "                                      : 0;\n";
          os << "        NoteLatency(perf_lat);\n";
          if (!big) {
            os << "        NoteLatencyEdge(static_cast<uint8_t>(RecordStructId::k"
               << PascalCase(e->channel) << "),\n";
            os << "                        perf_lat,\n";
            os << "                        static_cast<uint8_t>(v.sequence & 0xF));\n";
          }
        }
        if (e->record && !big) {
          os << "        const uint64_t callback_begin = NowNs();\n";
        }
        os << "        On" << PascalCase(e->channel) << "(v);\n";
        if (e->record && !big) {
          os << "        NoteCallbackWork(static_cast<uint8_t>(RecordStructId::k"
             << PascalCase(e->channel) << "), NowNs() - callback_begin);\n";
        }
      };
      const bool single = e->read != "follow" && e->delivery == "broadcast";
      if (!big) {
        os << "      const auto drops0 = " << e->channel << "_.drop_count();\n";
      }
      os << (single ? "      if (" : "      while (") << budget_guard << e->channel
         << "_.TryRead(&v) == thyme::ChannelStatus::kOk) {\n";
      append_handler();
      os << "      }\n";
      if (!big) {
        os << "      const auto drops1 = " << e->channel << "_.drop_count();\n";
        os << "      if (drops1 > drops0) my_health_->AddDrops("
              "static_cast<uint32_t>(drops1 - drops0));\n";
      }
      os << "    }\n";
    }
    os << "  }\n\n";
    os << " protected:\n";
    for (const auto* e : ed.in) {
      os << "  virtual void On" << PascalCase(e->channel) << "(const "
         << (IsPinnedSlotDelivery(g, cat, *e)
                 ? PinnedSlotRefType(g, cat, *e) + "::Sample"
                 : FrameRefType(g, cat, *e) + "::Sample")
         << "& v) = 0;\n";
    }
    os << "\n";
    if (has_publish) AppendPublishHelper(os);
  } else if (HasTickEdge(g, cat, ed)) {
    EmitTickLoop(os, g, cat, n, ed, known, recordable || record_in,
                 recordable, has_queue_out);
  } else {
    os << "  // Record/replay cross-cutting lives in the generated base: Loop\n";
    os << "  // pauses the producer while replay injects, and RecordTap mirrors\n";
    os << "  // one recordable value. The hand node overrides OnLoop with the\n";
    os << "  // algorithm only.\n";
    os << "  void Loop() override final {\n";
    if (recordable || record_in) {
      os << "    if (ReplayProducerShouldPause(name())) return;\n";
    }
    os << "    OnLoop();\n";
    if (has_queue_out) os << "    CreditQueuePushFails();\n";
    os << "  }\n\n";
    os << " protected:\n";
    os << "  virtual void OnLoop() = 0;\n\n";
    if (has_publish) AppendPublishHelper(os);
    if (has_queue_out) AppendCreditQueuePushFailsHelper(os, g, ed);
  }
  if (recordable) AppendGeneratedRecordTap(os);
  os << "  thyme::HealthSlot* my_health_;\n";
  if (recordable) os << "  thyme::RecordMirrorRingPage* record_page_;\n";
  for (const auto* e : ed.in)
    AppendEdgeChannelMembers(os, g, cat, *e, /*skip_missing_pool=*/false,
                             /*as_out=*/false);
  {
    std::set<std::string> seen_w;
    for (const auto* e : ed.out) {
      if ((IsBroadcastDelivery(*e) || IsPinnedSlotDelivery(g, cat, *e) ||
         IsLocalFrameHubEdge(g, cat, *e)) &&
          !seen_w.insert(e->channel).second) continue;
      AppendEdgeChannelMembers(os, g, cat, *e, /*skip_missing_pool=*/false,
                               /*as_out=*/true);
    }
  }
  if (has_queue_out && !event) AppendQueueDropShadowMembers(os, g, ed);
  os << "};\n";
  return WriteFile(out_path, os.str());
}

// Map known_types.txt ctor token to generated-base member basename (ob_pool→ob_).
static std::string KnownCtorMemberBase(const std::string& tok) {
  if (tok.size() > 5 && tok.compare(tok.size() - 5, 5, "_pool") == 0)
    return tok.substr(0, tok.size() - 5) + "_";
  if (tok.size() > 5 && tok.compare(tok.size() - 5, 5, "_pipe") == 0)
    return tok.substr(0, tok.size() - 5) + "_r_";
  if (tok.size() > 6 && tok.compare(tok.size() - 6, 6, "_queue") == 0)
    return tok.substr(0, tok.size() - 6) + "_";
  if (tok.size() > 5 && tok.compare(tok.size() - 5, 5, "_ring") == 0)
    return tok.substr(0, tok.size() - 5) + "_r_";
  return tok + "_";
}

bool EmitKnownCyclicGenerated(const fs::path& out_path, const Graph& g,
                              const Node& n, const TemplateCatalog& cat) {
  const KnownCat* k = FindKnown(cat, n.type);
  if (k == nullptr) {
    const std::string loc = GraphLoc(g, n.line);
    std::fprintf(stderr, "gen_graph: %sknown type '%s' missing catalog row\n",
                 loc.c_str(), n.type.c_str());
    return false;
  }
  const NodeEdges ed = EdgesOf(g, n);
  bool has_event_out = false;
  for (const auto* e : ed.out) {
    if (EdgeUsesEventPipe(g, *e)) {
      has_event_out = true;
      break;
    }
  }
  const bool has_queue_out = HasQueueEventOut(g, ed);
  bool has_publish = has_event_out;
  for (const auto* e : ed.out) {
    has_publish = has_publish || IsBroadcastDelivery(*e) || IsPinnedSlotDelivery(g, cat, *e);
  }
  // Record production members (page/tap/replay-pause) exist only when this
  // node produces a recorded out-edge (L8/GEN-12). RecordStructId is still
  // pulled in when the node reads a recorded channel so hand-written in-edge
  // latency taps (NoteLatencyEdge) can name their struct ids.
  const bool recordable = HasRecordedOut(ed);
  const bool record_in = HasRecordedIn(ed);
  std::ostringstream os;
  os << "// Code generated for the " << cat.banner << ". DO NOT EDIT.\n";
  os << "// Hand logic belongs in src/nodes/" << n.id << ".cpp.\n";
  os << "#pragma once\n";
  os << "#include \"thyme/runtime/cyclic_thread.h\"\n";
  os << "#include \"thyme/runtime/write_section.h\"\n";
  os << "#include \"generated/mp_ports.h\"\n";
  if (recordable) {
    os << "#include \"generated/replay_stages.h\"\n";
    os << "#include \"thyme/app/record_mirror.h\"\n";
    os << "#include \"thyme/app/time.h\"\n";
  } else if (record_in) {
    os << "#include \"generated/record_registry.h\"\n";
  }
  os << "\n";
  os << "class " << k->cls << "Generated : public thyme::CyclicThread {\n";
  os << " public:\n  " << k->cls << "Generated(const " << k->cls
     << "Ports& ports, const MpContext& ctx)\n";
  os << "      : thyme::CyclicThread(\"" << n.id << "\", " << n.hz
     << ", ctx.health),\n        my_health_(ctx.health)";
  for (const auto& tok : SplitCtorTokens(k->ctor)) {
    std::string mt;
    if (!CtorTokenMemberType(g, cat, tok, &mt)) return false;
    if (mt.empty()) continue;
    os << ",\n        " << KnownCtorMemberBase(tok) << "(ports." << tok << ")";
  }
  for (const auto* e : ed.in) {
    if (!IsBroadcastDelivery(*e) && !IsPinnedSlotDelivery(g, cat, *e)) continue;
    os << ",\n        " << e->channel << "_(ports." << e->channel << "_)";
  }
  {
    std::set<std::string> seen_w;
    for (const auto* e : ed.out) {
      if (IsPinnedSlotDelivery(g, cat, *e)) {
        if (!seen_w.insert(e->channel).second) continue;
        os << ",\n        " << e->channel << "_w_(ports." << e->channel
           << "_w_)";
      } else if (IsBroadcastDelivery(*e)) {
        if (!seen_w.insert(e->channel).second) continue;
        os << ",\n        " << e->channel << "_w_(ports." << e->channel
           << "_w_)";
      } else if (IsLocalFrameHubEdge(g, cat, *e)) {
        if (!seen_w.insert(e->channel).second) continue;
        os << ",\n        " << e->channel << "_(ports." << e->channel << "_)";
      } else if (EdgeUsesEventPipe(g, *e)) {
        os << ",\n        " << e->channel << "_(ports." << e->channel << "_)";
      }
    }
  }
  if (recordable) os << ",\n        record_page_(thyme::app::record_mirror::RingPage())";
  if (n.overrun.empty()) {
    os << " {}\n\n";
  } else {
    os << " {\n";
    os << "    // SCH-04: deadline-miss policy.\n";
    os << "    SetOverrunPolicy(" << OverrunPolicyExpr(n.overrun) << ");\n";
    os << "  }\n\n";
  }
  if (HasTickEdge(g, cat, ed)) {
    EmitTickLoop(os, g, cat, n, ed, k, recordable, recordable, has_queue_out);
  } else {
    os << "  void Loop() override final {\n";
    if (recordable) os << "    if (ReplayProducerShouldPause(name())) return;\n";
    os << "    OnLoop();\n";
    if (has_queue_out) os << "    CreditQueuePushFails();\n";
    os << "  }\n\n protected:\n  virtual void OnLoop() = 0;\n\n";
  }
  if (recordable) {
    os << "  // Record/replay cross-cutting lives in the generated base: the page is\n";
    os << "  // captured once at construction; Loop only carries the algorithm callback.\n";
    AppendGeneratedRecordTap(os);
  }
  if (has_publish) AppendPublishHelper(os);
  if (has_queue_out && !HasTickEdge(g, cat, ed)) AppendCreditQueuePushFailsHelper(os, g, ed);
  os << "  thyme::HealthSlot* my_health_;\n";
  for (const auto& tok : SplitCtorTokens(k->ctor)) {
    std::string mt;
    if (!CtorTokenMemberType(g, cat, tok, &mt)) return false;
    if (mt.empty()) continue;
    os << "  " << mt << " " << KnownCtorMemberBase(tok) << ";\n";
  }
  for (const auto* e : ed.in) {
    if (IsPinnedSlotDelivery(g, cat, *e)) {
      os << "  " << PinnedSlotRefType(g, cat, *e) << " " << e->channel << "_;\n";
    } else if (IsBroadcastDelivery(*e)) {
      os << "  " << BroadcastReaderPortsType(g, cat, *e) << " " << e->channel
         << "_;\n";
    }
  }
  {
    std::set<std::string> seen_w;
    for (const auto* e : ed.out) {
      if (IsPinnedSlotDelivery(g, cat, *e)) {
        if (!seen_w.insert(e->channel).second) continue;
        os << "  " << PinnedSlotRefType(g, cat, *e) << " " << e->channel
           << "_w_;\n";
      } else if (IsBroadcastDelivery(*e)) {
        if (!seen_w.insert(e->channel).second) continue;
        os << "  " << BroadcastWriterPortsType(g, cat, *e) << " " << e->channel
           << "_w_;\n";
      } else if (EdgeUsesEventPipe(g, *e)) {
        os << "  " << EventEdgePortsType(g, cat, *e) << " " << e->channel << "_;\n";
      }
    }
  }
  if (has_queue_out) AppendQueueDropShadowMembers(os, g, ed);
  if (recordable) os << "  thyme::RecordMirrorRingPage* record_page_;\n";
  os << "};\n";
  return WriteFile(out_path, os.str());
}

bool EmitUnknownGenerated(const fs::path& out_path, const Graph& g,
                          const Node& n, const TemplateCatalog& cat) {
  return EmitNodeGenerated(out_path, g, n, cat, GeneratedExecKind::CyclicLoop,
                           nullptr);
}

bool EmitEventGenerated(const fs::path& out_path, const Graph& g,
                        const Node& n, const TemplateCatalog& cat) {
  return EmitNodeGenerated(out_path, g, n, cat,
                           GeneratedExecKind::EventDispatch, nullptr);
}

bool EmitKnownEventGenerated(const fs::path& out_path, const Graph& g,
                             const Node& n, const TemplateCatalog& cat) {
  const KnownCat* k = FindKnown(cat, n.type);
  if (k == nullptr) {
    const std::string loc = GraphLoc(g, n.line);
    std::fprintf(stderr, "gen_graph: %sknown type '%s' missing catalog row\n",
                 loc.c_str(), n.type.c_str());
    return false;
  }
  return EmitNodeGenerated(out_path, g, n, cat,
                           GeneratedExecKind::EventDispatch, k);
}
bool EmitMpRoleLocals(const std::string& tmp_dir, const Graph& g,
                      const TemplateCatalog& cat) {
  const auto pools = UniquePools(g, cat);
  struct LatestShmRow {
    std::string channel;
    std::string cpp_type;
    std::string kind;
    std::string pool_type;
    std::string prefix;
    // MpControl::cross_frames slot wired to this channel's consumer source;
    // -1 = not gated. From the edge's mp_gate annotation (S-D4).
    int cross_slot = -1;
  };
  const std::vector<const Edge*> cross_gates = CrossGateEdges(g);
  std::vector<LatestShmRow> latest_rows;
  std::set<std::string> seen_channels;
  for (const auto& e : g.edges) {
    if (e.shm.empty() || e.delivery != "latest") continue;
    if (!seen_channels.insert(e.channel).second) continue;
    const std::string* kind = ShmKindName(cat, e.cpp_type);
    if (kind == nullptr) continue;
    const PoolCat* p = FindPool(cat, e.pool);
    LatestShmRow row;
    row.channel = e.channel;
    row.cpp_type = e.cpp_type;
    row.kind = *kind;
    row.pool_type = p != nullptr ? p->type : e.pool;
    row.prefix = p != nullptr ? p->prefix : "";
    for (std::size_t slot = 0; slot < cross_gates.size(); ++slot) {
      if (cross_gates[slot] == &e) {
        row.cross_slot = static_cast<int>(slot);
        break;
      }
    }
    latest_rows.push_back(std::move(row));
  }

  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n\n";
  os << "#include \"thyme/app/mp_runtime.h\"\n";
  os << "#include \"thyme/app/mp_control.h\"\n";
  os << "#include <memory>\n";
  bool any_index = false;
  for (const auto& p : pools) {
    if (p.index_pipe) { any_index = true; break; }
  }
  if (!latest_rows.empty()) {
    os << "#include \"support/shm_pipes.h\"\n";
  }
  os << "#include \"generated/graph_ir.h\"\n";
  os << "#include \"generated/mp_ports.h\"\n";
  if (!pools.empty()) {
    os << "#include \"generated/graph_types.h\"\n";
    if (any_index && cat.mp_includes.find("index_pipe.h") == std::string::npos) {
      os << "#include \"support/index_pipe.h\"\n";
    }
  }
  if (!latest_rows.empty()) os << "#include <cstring>\n";
  os << cat.mp_includes;
  if (!cat.mp_includes.empty() && cat.mp_includes.back() != '\n') os << "\n";
  os << "\n";

  os << "// Per-role pools, rings, SHM latest adapters, and catalog extras.\n";
  os << "// AssembleRoleLocals wires cross-process pipes before Make*Thread.\n";
  os << "struct RoleLocals {\n";
  for (const auto& p : pools) {
    os << "  " << p.pool_type << " " << p.prefix << "_pool;\n";
    os << "  Ring<" << p.pool_type << "> " << p.prefix << "_ring;\n";
    if (p.index_pipe) {
      os << "  RingIndexPipe " << p.prefix << "_local;\n";
      os << "  IndexPipe* " << p.prefix << "_pipe_p = nullptr;\n";
    }
  }
  for (const auto& row : latest_rows) {
    os << "  std::unique_ptr<ShmLatestSink<" << Framed(row.cpp_type) << ", "
       << row.pool_type << ">> " << row.channel << "_sink;\n";
    os << "  std::unique_ptr<ShmLatestSource<" << Framed(row.cpp_type) << ", "
       << row.pool_type << ">> " << row.channel << "_src;\n";
  }
  for (const auto& x : cat.extras) {
    os << "  " << x.cpp_type << " " << x.name << ";\n";
  }
  os << "\n  static std::unique_ptr<RoleLocals> Create();\n\n";
  for (const auto& p : pools) {
    if (!p.index_pipe) continue;
    os << "  IndexPipe& " << p.prefix << "_pipe() {\n";
    os << "    return " << p.prefix << "_pipe_p != nullptr ? *"
       << p.prefix << "_pipe_p : " << p.prefix << "_local;\n";
    os << "  }\n";
  }
  os << "\n private:\n  RoleLocals()";
  bool first_init = true;
  for (const auto& p : pools) {
    if (!p.index_pipe) continue;
    os << (first_init ? " : " : ", ") << p.prefix << "_local(" << p.prefix
       << "_ring)";
    first_init = false;
  }
  os << " {}\n";
  os << "};\n\n";
  os << "inline std::unique_ptr<RoleLocals> RoleLocals::Create() {\n";
  os << "  return std::unique_ptr<RoleLocals>(new RoleLocals());\n";
  os << "}\n\n";

  os << "inline bool AssembleRoleLocals(RoleLocals& l, const char* role,\n";
  if (!latest_rows.empty()) {
    os << "                                 thyme::app::MpRoleChannels& channels,\n";
    os << "                                 thyme::app::MpControl* ctrl) {\n";
  } else {
    os << "                                 thyme::app::MpRoleChannels& channels,\n";
    os << "                                 thyme::app::MpControl* ctrl) {\n";
    os << "  (void)l;\n";
    os << "  (void)role;\n";
    os << "  (void)channels;\n";
    os << "  (void)ctrl;\n";
    os << "  return true;\n";
    os << "}\n";
    return WriteFile(fs::path(tmp_dir) / "mp_role_locals.h", os.str());
  }
  os << "  struct RoleEdge {\n";
  os << "    bool participates = false;\n";
  os << "    bool cross_process = false;\n";
  os << "    bool am_from = false;\n";
  os << "    bool am_to = false;\n";
  os << "  };\n";
  os << "  auto role_on_edge = [](const GraphShmChannel& ch, const char* r) {\n";
  os << "    RoleEdge out;\n";
  os << "    if (std::strcmp(ch.delivery, \"latest\") != 0) return out;\n";
  os << "    out.am_from = std::strcmp(ch.from_process, r) == 0;\n";
  os << "    out.am_to = std::strcmp(ch.to_process, r) == 0;\n";
  os << "    if (!out.am_from && !out.am_to) return out;\n";
  os << "    out.participates = true;\n";
  os << "    out.cross_process = std::strcmp(ch.from_process, ch.to_process) != 0;\n";
  os << "    return out;\n";
  os << "  };\n";
  os << "  auto pick = [&](const GraphShmChannel& ch, IndexPipe*& pipe) -> bool {\n";
  os << "    RoleEdge re = role_on_edge(ch, role);\n";
  os << "    if (!re.participates || !re.cross_process) {\n";
  os << "      pipe = nullptr;\n";
  os << "      return true;\n";
  os << "    }\n";
  os << "    thyme::ShmRegion* r = channels.region(static_cast<int>(ch.kind));\n";
  os << "    if (r == nullptr || r->data() == nullptr) {\n";
  os << "      pipe = nullptr;\n";
  os << "      return true;\n";
  os << "    }\n";
  os << "    if (re.am_from) {\n";
  os << "      switch (ch.kind) {\n";
    for (const auto& row : latest_rows) {
      os << "        case GraphShmKind::" << row.kind << ":\n";
      os << "          l." << row.channel << "_sink = std::make_unique<ShmLatestSink<"
         << Framed(row.cpp_type) << ", " << row.pool_type << ">>(\n";
      os << "              reinterpret_cast<thyme::LatestSlotPage<"
         << Framed(row.cpp_type) << ">*>(r->data())->slot, l." << row.prefix
         << "_pool);\n";
      os << "          pipe = l." << row.channel << "_sink.get();\n";
      os << "          return true;\n";
    }
  os << "      }\n";
  os << "    } else if (re.am_to) {\n";
  os << "      switch (ch.kind) {\n";
    for (const auto& row : latest_rows) {
      os << "        case GraphShmKind::" << row.kind << ":\n";
      os << "          l." << row.channel << "_src = std::make_unique<ShmLatestSource<"
         << Framed(row.cpp_type) << ", " << row.pool_type << ">>(\n";
      os << "              reinterpret_cast<thyme::LatestSlotPage<"
         << Framed(row.cpp_type) << ">*>(r->data())->slot, l." << row.prefix
         << "_pool";
      if (row.cross_slot >= 0) {
        os << ", &ctrl->cross_frames[" << row.cross_slot << "]";
      }
      os << ");\n";
      os << "          pipe = l." << row.channel << "_src.get();\n";
      os << "          return true;\n";
    }
  os << "      }\n";
  os << "    }\n";
  os << "    pipe = nullptr;\n";
  os << "    return false;\n";
  os << "  };\n\n";
  if (!latest_rows.empty()) {
    os << "  for (const auto& ch : kGraphShmChannels) {\n";
    os << "    if (std::strcmp(ch.delivery, \"latest\") != 0) continue;\n";
    os << "    switch (ch.kind) {\n";
    for (const auto& row : latest_rows) {
      if (row.prefix.empty()) continue;
      os << "      case GraphShmKind::" << row.kind << ":\n";
      os << "        if (!pick(ch, l." << row.prefix << "_pipe_p)) return false;\n";
      os << "        break;\n";
    }
    os << "      default: break;\n";
    os << "    }\n";
    os << "  }\n";
  }
  for (const auto& p : pools) {
    if (!p.index_pipe) continue;
    os << "  if (l." << p.prefix << "_pipe_p == nullptr) l." << p.prefix
       << "_pipe_p = &l." << p.prefix << "_local;\n";
  }
  if (latest_rows.empty()) os << "  (void)ctrl;\n";
  os << "  return true;\n";
  os << "}\n";

  return WriteFile(fs::path(tmp_dir) / "mp_role_locals.h", os.str());
}

}  // namespace gen_emit
