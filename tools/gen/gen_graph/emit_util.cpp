#include "emit_internal.h"

#include "meta.h"
#include "util.h"
#include "capability.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define GEN_GRAPH_GETPID _getpid
#else
#include <unistd.h>
#define GEN_GRAPH_GETPID ::getpid
#endif

namespace gen_emit {

std::string ReplaceAll(std::string s, const std::string& a, const std::string& b) {
  return gen_util::ReplaceAll(std::move(s), a, b);
}

bool ReadFile(const fs::path& p, std::string* out) {
  if (gen_util::ReadFile(p, out)) return true;
  std::fprintf(stderr, "EmitGenerated: cannot read %s\n", p.c_str());
  return false;
}

bool WriteFile(const fs::path& p, const std::string& data) {
  if (gen_util::WriteFile(p, data)) return true;
  std::fprintf(stderr, "EmitGenerated: cannot write %s\n", p.c_str());
  return false;
}

fs::path StagingDir(const fs::path& root, const char* tag) {
  return root / (std::string(".generated.") + tag + "." +
                 std::to_string(static_cast<long long>(GEN_GRAPH_GETPID())));
}

std::string BannerHeader(const TemplateCatalog& cat) {
  std::ostringstream os;
  os << "// Code generated for the " << cat.banner << ". DO NOT EDIT.\n";
  os << "// Hand logic belongs in src/nodes/.\n";
  // GEN-09: the generator identity is part of the emitted bytes, so a version
  // bump is visible in review and a stale tree is caught by --check even
  // before comparing the meta file.
  os << gen_meta::BannerStamp();
  return os.str();
}

bool IsKnownType(const TemplateCatalog& cat, const std::string& t) {
  return FindKnown(cat, t) != nullptr;
}

const Node* FindType(const Graph& g, const std::string& type) {
  for (const auto& n : g.nodes) if (n.type == type) return &n;
  return nullptr;
}

const Node* FindCyclicType(const Graph& g, const std::string& type) {
  const Node* fallback = nullptr;
  for (const auto& n : g.nodes) {
    if (n.type != type) continue;
    if (!IsEventNode(n)) return &n;
    if (fallback == nullptr) fallback = &n;
  }
  return fallback;
}

const Node* FindNodeById(const Graph& g, const std::string& id) {
  for (const auto& n : g.nodes) if (n.id == id) return &n;
  return nullptr;
}

bool IsEventNode(const Node& n) { return n.exec == "event"; }

bool EdgeToEventNode(const Graph& g, const Edge& e) {
  const Node* to = FindNodeById(g, e.to);
  return to != nullptr && IsEventNode(*to);
}

bool IsQueueDelivery(const Edge& e) { return e.delivery == "queue"; }

bool IsBroadcastDelivery(const Edge& e) { return e.delivery == "broadcast"; }

bool EdgeUsesEventPipe(const Graph& g, const Edge& e) {
  return EdgeToEventNode(g, e) && !IsBroadcastDelivery(e);
}

bool KnownTypeHasBroadcastPorts(const Graph& g, const std::string& type) {
  for (const auto& n : g.nodes) {
    if (n.type != type) continue;
    const NodeEdges ed = EdgesOf(g, n);
    for (const auto* e : ed.in) {
      if (IsBroadcastDelivery(*e)) return true;
    }
    for (const auto* e : ed.out) {
      if (IsBroadcastDelivery(*e)) return true;
    }
  }
  return false;
}

std::string BroadcastWriterPortsType(const Graph& g, const TemplateCatalog& cat,
                                     const Edge& e) {
  (void)g;
  return FrameRefType(g, cat, e);
}

std::string BroadcastReaderPortsType(const Graph& g, const TemplateCatalog& cat,
                                     const Edge& e) {
  (void)g;
  return FrameRefType(g, cat, e);
}

bool HasEventNode(const Graph& g) {
  for (const auto& n : g.nodes) if (IsEventNode(n)) return true;
  return false;
}

bool HasBroadcastEdge(const Graph& g, const TemplateCatalog& cat) {
  for (const auto& e : g.edges) {
    if (IsBroadcastDelivery(e) && !IsPinnedSlotDelivery(g, cat, e)) return true;
  }
  return false;
}

std::vector<const Edge*> UniqueBroadcastRegionEdges(const Graph& g,
                                                    const TemplateCatalog& cat) {
  std::vector<const Edge*> out;
  std::set<std::string> seen;
  for (const auto& e : g.edges) {
    if (!IsBroadcastDelivery(e) || IsPinnedSlotDelivery(g, cat, e) || e.shm.empty()) continue;
    if (!seen.insert(e.shm).second) continue;
    out.push_back(&e);
  }
  return out;
}

std::string BroadcastRingMember(const Edge& e) { return e.channel + "_w_"; }

std::string BroadcastReaderMember(const Edge& e) {
  return e.to + "_" + e.channel + "_r_";
}

std::string EventPipeMember(const Edge& e) {
  return e.to + "_" + e.channel + "_pipe";
}
std::string PascalCase(const std::string& s) { return gen_util::PascalCase(s); }

// Split a known_types.txt ctor column into trimmed tokens ("," separated).
std::vector<std::string> SplitCtorTokens(const std::string& ctor) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= ctor.size()) {
    std::size_t comma = ctor.find(',', start);
    if (comma == std::string::npos) comma = ctor.size();
    std::string tok = ctor.substr(start, comma - start);
    std::size_t a = 0;
    while (a < tok.size() && std::isspace(static_cast<unsigned char>(tok[a]))) ++a;
    std::size_t b = tok.size();
    while (b > a && std::isspace(static_cast<unsigned char>(tok[b - 1]))) --b;
    tok = tok.substr(a, b - a);
    if (!tok.empty()) out.push_back(std::move(tok));
    if (comma == ctor.size()) break;
    start = comma + 1;
  }
  return out;
}

int EventEdgeCap(const Graph& g, const Edge& e) {
  return e.depth > 0 ? e.depth : g.ring_cap;
}

// MP EventTransports storage: notify → NotifPipe; queue → ShmSpscRing.
// Single-process Pipeline always uses NotifPipe (see emit_wiring.cpp).
std::string EventEdgeTransportType(const Graph& g, const Edge& e) {
  const std::string cap = std::to_string(EventEdgeCap(g, e));
  if (IsQueueDelivery(e)) {
    return "thyme::ShmSpscRing<" + Framed(e.cpp_type) + ", " + cap + ">";
  }
  return "thyme::NotifPipe<" + Framed(e.cpp_type) + ", " + cap + ">";
}

// Ports / generated bases hold FrameChannelRef by value so SP NotifPipe and MP
// ShmSpscRing storage both bind without dual Ports structs.
std::string EventEdgePortsType(const Graph& g, const TemplateCatalog& cat,
                               const Edge& e) {
  return FrameRefType(g, cat, e);
}
// One edge's channel members for Ports structs and generated bases:
// broadcast in -> SpmcReader* channel_; broadcast out -> ShmSpmcRing* channel_w_;
// FrameChannelRef channel_ (edge into exec=event or pinned_slot) or pool*/ring* pair.
// skip_missing_pool: Ports omit unknown pools; generated bases always emit
// using the edge's pool name when the catalog row is missing.
void AppendEdgeChannelMembers(std::ostringstream& os, const Graph& g,
                              const TemplateCatalog& cat, const Edge& e,
                              bool skip_missing_pool, bool as_out) {
  if (IsPinnedSlotDelivery(g, cat, e)) {
    os << "  " << PinnedSlotRefType(g, cat, e) << " " << e.channel
       << (as_out ? "_w_" : "_") << ";\n";
    return;
  }
  if (IsBroadcastDelivery(e)) {
    if (as_out) {
      os << "  " << BroadcastWriterPortsType(g, cat, e) << " " << e.channel
         << "_w_;\n";
    } else {
      os << "  " << BroadcastReaderPortsType(g, cat, e) << " " << e.channel
         << "_;\n";
    }
    return;
  }
  if (EdgeUsesEventPipe(g, e)) {
    os << "  " << EventEdgePortsType(g, cat, e) << " " << e.channel << "_;\n";
    return;
  }
  const PoolCat* p = FindPool(cat, e.pool);
  if (p == nullptr) {
    if (skip_missing_pool) return;
    os << "  " << e.pool << "* " << e.channel << "_; ";
    os << "Ring<" << e.pool << ">* " << e.channel << "_r_;\n";
    return;
  }
  if (p->index_pipe) {
    os << "  " << p->type << "* " << e.channel << "_; IndexPipe* " << e.channel
       << "_r_;\n";
  } else {
    os << "  thyme::LocalIndexQueue<" << p->type << ", kRingCap> " << e.channel
       << "_;\n";
  }
}
NodeEdges EdgesOf(const Graph& g, const Node& n) {
  NodeEdges e;
  for (const auto& ed : g.edges) {
    if (ed.to == n.id) e.in.push_back(&ed);
    else if (ed.from == n.id) e.out.push_back(&ed);
  }
  return e;
}
// True when the graph has an exec=event node sharing this catalog type
// (cyclic wiring must use designated Ports initializers so event-only
// pipe members on the shared struct stay nullptr/default).
bool KnownTypeHasEventPorts(const Graph& g, const std::string& type) {
  for (const auto& n : g.nodes) {
    if (n.type == type && IsEventNode(n)) return true;
  }
  return false;
}

std::string NodeMemberName(const Graph& g, const TemplateCatalog& cat,
                           const Node& n) {
  const KnownCat* k = FindKnown(cat, n.type);
  if (k == nullptr) return n.id;
  std::size_t same_type = 0;
  for (const auto& x : g.nodes) if (x.type == n.type) ++same_type;
  if (same_type > 1 || IsEventNode(n)) return n.id;
  return k->member;
}

std::string HandThreadClassName(const TemplateCatalog& cat, const Node& n) {
  if (n.shard_count > 1) {
    return PascalCase(n.id) + "Thread";
  }
  const KnownCat* k = FindKnown(cat, n.type);
  if (k != nullptr && !IsEventNode(n)) return k->cls;
  if (k != nullptr && IsEventNode(n)) return PascalCase(n.id) + "Thread";
  return PascalCase(n.type) + "Thread";
}

bool NodeNeedsPlacement(const Node& n) {
  return !n.cpu_set.empty() || !n.sched_policy.empty() || n.priority >= 0 ||
         n.priority_effective >= 0 || n.placement_fail == "require" ||
         n.apply_phase;
}
int NodeEffectivePriority(const Node& n) {
  // ApplySchedPlan fills priority_effective for explicit and RM priorities.
  // Nodes without either keep the documented runtime default.
  if (n.priority_effective >= 0) return n.priority_effective;
  if (n.priority >= 0) return n.priority;
  return 10;
}
bool UsesSchedulingSetup(const std::string& tpl_dir) {
  return fs::exists(fs::path(tpl_dir) / "scheduling_rings.inc");
}
const std::string* ShmKindName(const TemplateCatalog& cat,
                               const std::string& cpp_type) {
  for (const auto& k : cat.shm_kinds) if (k.cpp_type == cpp_type) return &k.kind;
  return nullptr;
}

bool BuildEdgeManifest(const Graph& g, const TemplateCatalog& cat,
                       EdgeManifest* out) {
  EdgeManifest m;
  m.has_event_node = HasEventNode(g);
  for (const auto& e : g.edges) {
    EdgeManifestRow r;
    r.edge = &e;
    r.index = m.edges.size();
    r.recorded = e.record;
    r.from = FindNodeById(g, e.from);
    r.to = FindNodeById(g, e.to);
    r.from_process = r.from != nullptr ? r.from->process : "";
    r.to_process = r.to != nullptr ? r.to->process : "";
    const std::string* kind = ShmKindName(cat, e.cpp_type);
    r.shm_kind = kind != nullptr ? *kind : "";
    r.to_event = EdgeToEventNode(g, e);
    r.queue_delivery = IsQueueDelivery(e);
    r.mp_gate = e.mp_gate;
    r.record_priority = RecordPriorityForEdge(e);
    r.record_label = RecordPriorityLabel(e);
    std::string capability_error;
    if (!DeriveChannelCapability(g, cat, e, &r.capability, &capability_error)) {
      std::fprintf(stderr, "gen_graph: %s\n", capability_error.c_str());
      return false;
    }
    if (r.to_event) r.event_pipe_member = EventPipeMember(e);
    const PoolCat* p = FindPool(cat, e.pool);
    r.pool_found = p != nullptr;
    r.pool_prefix = p != nullptr ? p->prefix : "";
    m.edges.push_back(std::move(r));
    if (kind != nullptr && !e.shm.empty()) m.shm_edges.push_back(m.edges.back());
    if (e.mp_gate) m.cross_gates.push_back(m.edges.back());
    if (m.edges.back().to_event &&
        (m.edges.back().queue_delivery ||
         (!e.shm.empty() &&
          m.edges.back().capability.transport_kind == thyme::ChannelTransportKind::kPinnedSlot))) {
      m.has_event_shm = true;
    }
    if (IsBroadcastDelivery(e) && !IsPinnedSlotDelivery(g, cat, e)) m.has_broadcast = true;
  }
  for (auto& row : m.edges) {
    if (row.from == nullptr || row.to == nullptr ||
        row.from_process.empty() || row.from_process != row.to_process) {
      continue;
    }
    std::size_t local_readers = 0;
    bool dynamic = false;
    for (const auto& candidate : m.edges) {
      if (candidate.from == nullptr || candidate.to == nullptr ||
          candidate.from_process != row.from_process ||
          candidate.to_process != row.from_process ||
          candidate.edge->from != row.edge->from ||
          candidate.edge->channel != row.edge->channel) {
        continue;
      }
      ++local_readers;
      dynamic = dynamic || candidate.edge->dynamic;
    }
    row.capability.hub_required = local_readers > 1 || dynamic;
  }
  // GEN-12: manifest = unique recorded channels, lexicographic by channel.
  // Dedup keeps the first edge in graph order for the row metadata; the sort
  // makes the ID independent of YAML edge order.
  {
    std::set<std::string> seen_channel;
    std::set<std::string> seen_bulk;
    for (const auto& r : m.edges) {
      if (!r.recorded) continue;
      if (r.capability.transport_kind == thyme::ChannelTransportKind::kPinnedSlot) {
        if (!seen_bulk.insert(r.edge->channel).second) continue;
        m.bulk.push_back(r);
        continue;
      }
      if (!seen_channel.insert(r.edge->channel).second) continue;
      m.recorded.push_back(r);
    }
    const auto by_channel = [](const EdgeManifestRow& a,
                               const EdgeManifestRow& b) {
      return a.edge->channel < b.edge->channel;
    };
    std::sort(m.recorded.begin(), m.recorded.end(), by_channel);
    std::sort(m.bulk.begin(), m.bulk.end(), by_channel);
  }
  // First manifest slot per producer process, in manifest order. The supervisor
  // gates a process on this slot; omitting unrecorded channels keeps the gate
  // table aligned with RecordStructId.
  {
    std::set<std::string> seen_proc;
    for (std::size_t i = 0; i < m.recorded.size(); ++i) {
      const std::string& proc = m.recorded[i].from_process;
      if (proc.empty() || !m.recorded[i].edge->record_gate ||
          !seen_proc.insert(proc).second) {
        continue;
      }
      m.record_gates.emplace_back(proc, static_cast<uint16_t>(i));
    }
  }
  m.has_record = !m.recorded.empty() || !m.bulk.empty();
  *out = std::move(m);
  return true;
}

}  // namespace gen_emit
