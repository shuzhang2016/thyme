#pragma once

// Shared helpers for the gen_graph emitters (split across emit_*.cpp by
// topic: record / replay / wiring / integration / ir).

#include "catalog.h"
#include "graph.h"
#include "hub.h"
#include "thyme/runtime/channel_capability.h"

#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace gen_emit {

namespace fs = std::filesystem;

std::string ReplaceAll(std::string s, const std::string& a,
                       const std::string& b);
// Frame wrapper for a bare contract cpp_type. Every transport/handler/replay/
// viz emitter uses this single helper so the frame wrapper stays a one-line
// change; registry/schema names keep the bare cpp_type (plan 02).
inline std::string Framed(const std::string& cpp_type) {
  return "thyme::Frame<" + cpp_type + ">";
}
bool ReadFile(const fs::path& p, std::string* out);
bool WriteFile(const fs::path& p, const std::string& data);
// GEN-03: staging dirs are pid-unique siblings of the output tree, so the
// final install is a rename within one filesystem (atomic per directory on
// POSIX) and two gen_graph runs on the same root never share a staging dir.
// tag distinguishes the callers ("tmp" / "trash" / "check" / "mp").
fs::path StagingDir(const fs::path& root, const char* tag);
// Removes a staging/trash directory on scope exit unless Dismiss() was called
// (e.g. the dir was renamed into its final place).
struct DirGuard {
  fs::path path;
  ~DirGuard() {
    if (path.empty()) return;
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  void Dismiss() { path.clear(); }
};
std::string BannerHeader(const TemplateCatalog& cat);
bool IsKnownType(const TemplateCatalog& cat, const std::string& t);
const Node* FindType(const Graph& g, const std::string& type);
// Prefer the cyclic catalog node when the same type also has exec=event twins
// (graph_check contracts describe the cyclic Loop, not event Dispatch).
const Node* FindCyclicType(const Graph& g, const std::string& type);
bool KnownTypeHasEventPorts(const Graph& g, const std::string& type);
const Node* FindNodeById(const Graph& g, const std::string& id);
// Edges annotated mp_gate: true, in graph edge order; the index in the
// returned vector is the edge's MpControl::cross_frames slot (S-D4). Shared
// by emit_ir (kGraphCrossGates table) and emit_nodes (counter wiring) so the
// slot allocation can never drift apart.
std::vector<const Edge*> CrossGateEdges(const Graph& g);

// Single-pass edge manifest (L8): one walk of g.edges builds the derived
// per-edge data every emitter consumes, replacing the four independent
// re-walks of g.edges in record/replay/ir/integration.
struct EdgeManifestRow {
  const Edge* edge = nullptr;
  const Node* from = nullptr;
  const Node* to = nullptr;
  std::size_t index = 0;          // record struct id == graph edge order
  std::string from_process;       // producer process ("" if unknown)
  std::string to_process;         // consumer process ("" if unknown)
  std::string shm_kind;           // "" when the cpp_type has no shm kind
  std::string pool_prefix;        // FindPool(cat, edge->pool)->prefix ("" if none)
  bool pool_found = false;        // FindPool(cat, edge->pool) != nullptr
  std::string event_pipe_member;  // EventPipeMember(edge) when to_event
  bool to_event = false;          // EdgeToEventNode
  bool queue_delivery = false;    // IsQueueDelivery
  bool mp_gate = false;           // edge.mp_gate (S-D4)
  bool recorded = true;           // edge.record (GEN-12 manifest membership)
  thyme::ChannelCapabilityInfo capability;  // ARCH-19 single routing result
  const char* record_priority = "RecordPriority::kP1";  // string literal
  const char* record_label = "P1";                       // string literal
};

struct EdgeManifest {
  std::vector<EdgeManifestRow> edges;          // all edges, graph order
  // GEN-12 record manifest: unique recorded CHANNELS (broadcast fan-out
  // contributes one row), sorted byte-wise by channel name. The vector index
  // is the public RecordStructId: stable across YAML edge reordering, and
  // independent of which other edges exist. Record/replay/viz/ir emitters
  // iterate this, never `edges`.
  std::vector<EdgeManifestRow> recorded;
  // OPS-13: recorded pinned_slot/h264 channels. Not RecordStructIds and not
  // included in `recorded`. Sorted by channel name.
  std::vector<EdgeManifestRow> bulk;
  std::vector<EdgeManifestRow> shm_edges;      // shm-backed (shm set + kind)
  std::vector<EdgeManifestRow> cross_gates;    // mp_gate edges, graph order
  // First record-mirror slot per producer process (in manifest order).
  std::vector<std::pair<std::string, uint16_t>> record_gates;
  bool has_record = false;     // any recorded channel (record/replay emit gate)
  bool has_event_node = false;
  bool has_event_shm = false;  // queue or SHM PinnedSlot edge into an event node
  bool has_broadcast = false;  // any delivery=broadcast edge
};

bool BuildEdgeManifest(const Graph& g, const TemplateCatalog& cat,
                       EdgeManifest* out);
bool ValidateGraphPools(const Graph& g, const TemplateCatalog& cat);
std::string PascalCase(const std::string& s);
// Formerly same-TU helpers in emit_util.cpp; declared here so the D1 split
// (emit_util / emit_ports / emit_nodes) can share them without duplicating.
std::vector<std::string> SplitCtorTokens(const std::string& ctor);
bool CtorTokenMemberType(const Graph& g, const TemplateCatalog& cat,
                         const std::string& tok, std::string* out);
void AppendEdgeChannelMembers(std::ostringstream& os, const Graph& g,
                              const TemplateCatalog& cat, const Edge& e,
                              bool skip_missing_pool, bool as_out);
std::string MpContextAggregate(const TemplateCatalog& cat,
                               const std::string& health_expr,
                               const std::string& locals_prefix = "");
std::string KnownCtorGrouped(const Graph& g, const TemplateCatalog& cat,
                             const Node& n, const std::string& health_expr,
                             bool* ok, const std::string& event_prefix = "",
                             const std::string& locals_prefix = "",
                             const std::string& broadcast_prefix = "",
                             const std::string& pinned_slot_prefix = "");
std::string UnknownCtorGrouped(const Graph& g, const Node& n,
                               const TemplateCatalog& cat,
                               const std::string& health_expr,
                               const std::string& event_prefix = "",
                               const std::string& locals_prefix = "",
                               const std::string& broadcast_prefix = "",
                               const std::string& pinned_slot_prefix = "");
bool EmitMpRoleLocals(const std::string& tmp_dir, const Graph& g,
                      const TemplateCatalog& cat);

struct PoolDecl {
  std::string pool_type;
  std::string prefix;
  bool index_pipe;
};
std::vector<PoolDecl> UniquePools(const Graph& g, const TemplateCatalog& cat);

struct NodeEdges {
  std::vector<const Edge*> in;
  std::vector<const Edge*> out;
};
NodeEdges EdgesOf(const Graph& g, const Node& n);

std::string NodeMemberName(const Graph& g, const TemplateCatalog& cat,
                           const Node& n);
std::string HandThreadClassName(const TemplateCatalog& cat, const Node& n);
bool NodeNeedsPlacement(const Node& n);
int NodeEffectivePriority(const Node& n);
bool AppendPlacement(std::ostringstream& os, const Graph& g, const Node& n,
                     const std::string& member);
bool AppendWaitInputs(std::ostringstream& os, const Graph& g, const Node& n,
                      const TemplateCatalog& cat, const std::string& member);
std::vector<std::string> WaitInputRingMembers(const Graph& g, const Node& n,
                                              const TemplateCatalog& cat);
bool AppendSchedulingApplyBranch(std::ostringstream& os, const Graph& g,
                                 const Node& n, const TemplateCatalog& cat,
                                 bool first_branch);
bool EmitSchedulingApplyInc(const std::string& tmp_dir, const Graph& g,
                            const TemplateCatalog& cat);
// Emit generated/scheduling_rings.h: the SchedulingRings aggregate of ring
// references (union of wait_inputs ring members) plus a BuildSchedulingRings
// factory templated on the owner (Pipeline or RoleLocals). Replaces the
// hand-written scheduling_rings.inc literals.
bool EmitSchedulingRings(const std::string& tmp_dir, const Graph& g,
                         const TemplateCatalog& cat);
bool EmitChannelCapabilities(const std::string& tmp_dir, const Graph& g,
                             const TemplateCatalog& cat, const EdgeManifest& m);
bool UsesSchedulingSetup(const std::string& tpl_dir);
bool AppendKnownPortsStruct(std::ostringstream& os, const Graph& g,
                            const TemplateCatalog& cat,
                            const std::string& type);
bool AppendUnknownPortsStruct(std::ostringstream& os, const Graph& g,
                              const Node& n, const TemplateCatalog& cat);
std::string BuildMpUnknown(const Graph& g, const TemplateCatalog& cat);
bool EmitUnknownGenerated(const fs::path& out_path, const Graph& g,
                          const Node& n, const TemplateCatalog& cat);
bool EmitEventGenerated(const fs::path& out_path, const Graph& g,
                        const Node& n, const TemplateCatalog& cat);

bool EmitKnownEventGenerated(const fs::path& out_path, const Graph& g,
                             const Node& n, const TemplateCatalog& cat);

bool EmitKnownCyclicGenerated(const fs::path& out_path, const Graph& g,
                              const Node& n, const TemplateCatalog& cat);

// exec=event nodes (EventThread bases on NotifPipe inputs).
bool IsEventNode(const Node& n);
bool EdgeToEventNode(const Graph& g, const Edge& e);
bool IsQueueDelivery(const Edge& e);
bool IsBroadcastDelivery(const Edge& e);
bool IsPinnedSlotDelivery(const Graph& g, const TemplateCatalog& cat, const Edge& e);
bool HasPinnedSlotEdge(const Graph& g, const TemplateCatalog& cat);
bool HasPinnedSlotShmEdge(const Graph& g, const TemplateCatalog& cat);
std::string FrameSemantics(const Graph& g, const TemplateCatalog& cat, const Edge& e);
std::string FrameRefType(const Graph& g, const TemplateCatalog& cat, const Edge& e);
void AppendFrameRefDefinition(std::ostringstream& os);
void AppendDualChannelRefDefinition(std::ostringstream& os);
bool IsTickQueueEdge(const Graph& g, const TemplateCatalog& cat, const Edge& e);
bool HasTickQueueEdge(const Graph& g, const TemplateCatalog& cat, const NodeEdges& ed);
bool IsTickLatestInput(const Graph& g, const TemplateCatalog& cat, const Edge& e);
bool HasTickEdge(const Graph& g, const TemplateCatalog& cat, const NodeEdges& ed);


std::string PinnedSlotRefType(const Graph& g, const TemplateCatalog& cat, const Edge& e);
std::string PinnedSlotLocalChannelType(const Graph& g, const TemplateCatalog& cat, const Edge& e);
std::string PinnedSlotShmChannelType(const Graph& g, const TemplateCatalog& cat, const Edge& e);
std::string PinnedSlotWriterMember(const Edge& e);
std::string PinnedSlotReaderMember(const Edge& e);
std::string PinnedSlotStoreMember(const Edge& e, bool as_out);
void AppendPinnedSlotRefDefinition(std::ostringstream& os);
void AppendSpPinnedSlotFields(std::ostringstream& os, const Graph& g, const TemplateCatalog& cat);
void AppendSpPinnedSlotCreate(std::ostringstream& os, const Graph& g, const TemplateCatalog& cat);
void AppendPinnedSlotShmHelpers(std::ostringstream& os, const Graph& g, const TemplateCatalog& cat);
// Event-pipe transport (NotifPipe / ShmSpscRing / EventPipeRef): exec=event
// edges that are not delivery=broadcast (those use SpmcReader / ShmSpmcRing).
bool EdgeUsesEventPipe(const Graph& g, const Edge& e);
bool KnownTypeHasBroadcastPorts(const Graph& g, const std::string& type);
std::string BroadcastWriterPortsType(const Graph& g, const TemplateCatalog& cat, const Edge& e);
std::string BroadcastReaderPortsType(const Graph& g, const TemplateCatalog& cat, const Edge& e);
bool HasEventNode(const Graph& g);
bool HasBroadcastEdge(const Graph& g, const TemplateCatalog& cat);
// First broadcast edge per unique shm suffix, in graph order.
std::vector<const Edge*> UniqueBroadcastRegionEdges(const Graph& g, const TemplateCatalog& cat);
// BroadcastTransports member names (writer ring / per-reader SpmcReader).
std::string BroadcastRingMember(const Edge& e);
std::string BroadcastReaderMember(const Edge& e);
// Pipeline member / role-local name of the NotifPipe carrying edge e
// (e.to must be an event node): "<to>_<channel>_pipe".
std::string EventPipeMember(const Edge& e);
// Single-process Pipeline bundles event pipes on event_transports_ (mirrors MP
// EventTransports). Prefix for aggregate / wait-input references.
inline constexpr const char* SpEventTransportPrefix = "event_transports_.";
inline constexpr const char* SpBroadcastTransportPrefix = "broadcast_transports_.";
// Per-edge pipe/ring capacity: explicit depth or graph ring_cap fallback.
int EventEdgeCap(const Graph& g, const Edge& e);
inline int EdgeCap(const Graph& g, const Edge& e) {
  return EventEdgeCap(g, e);
}
// MP/EventTransports storage type: NotifPipe (notify) or ShmSpscRing (queue).
std::string EventEdgeTransportType(const Graph& g, const Edge& e);
// Ports / generated member type: FrameChannelRef (Local or SHM backend).
std::string EventEdgePortsType(const Graph& g, const TemplateCatalog& cat, const Edge& e);
// Generator-time event-mode rules: event nodes may have at most
// kMaxWaitInputs in-edges.
bool ValidateEventNodes(const Graph& g, const TemplateCatalog& cat);
const std::string* ShmKindName(const TemplateCatalog& cat,
                               const std::string& cpp_type);

const char* RecordPriorityHeuristic(const std::string& cpp_type);
const char* RecordPriorityForEdge(const Edge& e);
const char* RecordPriorityLabel(const Edge& e);
void AppendChecklistItems(std::ostringstream& os, const std::string& checklist);

}  // namespace gen_emit
