#include "thyme/contracts/abi.h"
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

namespace {

// Mirror of thyme::ThreadPlacement::kMaxCpus: cpu_set entries beyond this
// cannot be emitted into the placement struct.
constexpr std::size_t kMaxCpus = 64;

}  // namespace

// Classify one ctor token into a pointer member type for its Ports struct:
//   {health}                    -> skipped (the own slot lives in MpContext)
//   <prefix>_pool / _ring/_pipe -> pool / Ring<pool> / IndexPipe (pools.txt)
//   <extra name>                -> extra cpp_type (extras.txt)
// Event out-edges are injected from the graph (not listed in known_types.txt).
// Returns false on an unclassifiable token (generator-time error).
bool CtorTokenMemberType(const Graph& /*g*/, const TemplateCatalog& cat,
                         const std::string& tok, std::string* out) {
  if (tok == "{health}") { out->clear(); return true; }
  for (const auto& p : cat.pools) {
    if (tok == p.prefix + "_pool") { *out = p.type + "*"; return true; }
    if (tok == p.prefix + "_queue") {
      if (p.index_pipe) {
        std::fprintf(stderr,
                     "gen_graph: ctor token '%s' uses _queue for index-pipe pool %s\n",
                     tok.c_str(), p.prefix.c_str());
        return false;
      }
      *out = "thyme::LocalIndexQueue<" + p.type + ", kRingCap>";
      return true;
    }
    if (tok == p.prefix + "_ring") {
      if (p.index_pipe) {
        std::fprintf(stderr,
                     "gen_graph: ctor token '%s' uses _ring for index-pipe pool %s\n",
                     tok.c_str(), p.prefix.c_str());
        return false;
      }
      *out = "Ring<" + p.type + ">*";
      return true;
    }
    if (tok == p.prefix + "_pipe") {
      if (!p.index_pipe) {
        std::fprintf(stderr,
                     "gen_graph: ctor token '%s' uses _pipe for non-index pool %s\n",
                     tok.c_str(), p.prefix.c_str());
        return false;
      }
      *out = "IndexPipe*";
      return true;
    }
  }
  for (const auto& x : cat.extras) {
    if (tok == x.name) { *out = x.cpp_type + "*"; return true; }
  }
  std::fprintf(stderr, "gen_graph: cannot classify ctor token '%s'\n",
               tok.c_str());
  return false;
}

// Append Ports members for this known node's out-edges into exec=event
// consumers (same channel_ naming as unknown nodes).
void AppendKnownEventOutMembers(std::ostringstream& os, const Graph& g,
                                const TemplateCatalog& cat, const Node& n) {
  const NodeEdges ed = EdgesOf(g, n);
  std::set<std::string> hub_channels;
  for (const auto* e : ed.out) {
    if (!EdgeUsesEventPipe(g, *e) ||
        (IsLocalFrameHubEdge(g, cat, *e) &&
         !hub_channels.insert(e->channel).second)) {
      continue;
    }
    os << "  " << EventEdgePortsType(g, cat, *e) << " " << e->channel
       << "_;\n";
  }
}

// Append Ports aggregate args for event out-edges (&ev.<to>_<channel>_pipe).
void AppendKnownEventOutAggregate(std::ostringstream& os, const Graph& g,
                                  const TemplateCatalog& cat, const Node& n,
                                  bool* first, const std::string& event_prefix) {
  const NodeEdges ed = EdgesOf(g, n);
  std::set<std::string> hub_channels;
  for (const auto* e : ed.out) {
    if (!EdgeUsesEventPipe(g, *e)) continue;
    if (IsLocalFrameHubEdge(g, cat, *e)) {
      if (!hub_channels.insert(e->channel).second) continue;
      if (!*first) os << ", ";
      os << LocalFrameHubInputRef(g, cat, *e, event_prefix);
    } else {
      if (!*first) os << ", ";
      os << "&" << event_prefix << EventPipeMember(*e);
    }
    *first = false;
  }
}

// Append aggregate args for event in-edges on a known exec=event node.
void AppendKnownEventInAggregate(std::ostringstream& os, const Graph& g,
                                 const Node& n, bool* first,
                                 const std::string& event_prefix) {
  const NodeEdges ed = EdgesOf(g, n);
  for (const auto* e : ed.in) {
    if (!EdgeUsesEventPipe(g, *e)) continue;
    if (!*first) os << ", ";
    os << "&" << event_prefix << EventPipeMember(*e);
    *first = false;
  }
}

// <cls>Ports: known_types.txt ctor tokens (pools/extras) plus graph-injected
// event pipe members for every node of this type (cyclic out-edges + event
// in/out when exec=event shares the catalog type with a cyclic twin).
bool AppendKnownPortsStruct(std::ostringstream& os, const Graph& g,
                            const TemplateCatalog& cat,
                            const std::string& type) {
  const KnownCat* k = FindKnown(cat, type);
  if (k == nullptr) {
    std::fprintf(stderr,
                 "gen_graph: known type '%s' missing catalog row while emitting ports\n",
                 type.c_str());
    return false;
  }
  os << "struct " << k->cls << "Ports {\n";
  for (const auto& t : SplitCtorTokens(k->ctor)) {
    std::string mt;
    if (!CtorTokenMemberType(g, cat, t, &mt)) return false;
    if (mt.empty()) continue;  // {health} lives in MpContext
    os << "  " << mt << " " << t << ";\n";
  }
  std::set<std::string> extra_members;
  auto add_member = [&](const std::string& name, const std::string& ty) {
    if (!extra_members.insert(name).second) return;
    os << "  " << ty << " " << name << ";\n";
  };
  for (const auto& n : g.nodes) {
    if (n.type != type || !IsEventNode(n)) continue;
    const NodeEdges ed = EdgesOf(g, n);
    for (const auto* e : ed.in) {
      if (IsPinnedSlotDelivery(g, cat, *e)) {
        add_member(e->channel + "_", PinnedSlotRefType(g, cat, *e));
      } else if (IsBroadcastDelivery(*e)) {
        add_member(e->channel + "_", BroadcastReaderPortsType(g, cat, *e));
      } else {
        add_member(e->channel + "_", EventEdgePortsType(g, cat, *e));
      }
    }
    for (const auto* e : ed.out) {
      if (IsPinnedSlotDelivery(g, cat, *e)) {
        add_member(e->channel + "_w_", PinnedSlotRefType(g, cat, *e));
      } else if (IsBroadcastDelivery(*e)) {
        add_member(e->channel + "_w_", BroadcastWriterPortsType(g, cat, *e));
      } else if (EdgeUsesEventPipe(g, *e)) {
        add_member(e->channel + "_", EventEdgePortsType(g, cat, *e));
      }
    }
  }
  for (const auto& n : g.nodes) {
    if (n.type != type || IsEventNode(n)) continue;
    const NodeEdges ed = EdgesOf(g, n);
    for (const auto* e : ed.in) {
      if (IsPinnedSlotDelivery(g, cat, *e)) {
        add_member(e->channel + "_", PinnedSlotRefType(g, cat, *e));
      } else if (IsBroadcastDelivery(*e)) {
        add_member(e->channel + "_", BroadcastReaderPortsType(g, cat, *e));
      }
    }
    for (const auto* e : ed.out) {
      if (IsPinnedSlotDelivery(g, cat, *e)) {
        add_member(e->channel + "_w_", PinnedSlotRefType(g, cat, *e));
      } else if (IsBroadcastDelivery(*e)) {
        add_member(e->channel + "_w_", BroadcastWriterPortsType(g, cat, *e));
      } else if (EdgeUsesEventPipe(g, *e)) {
        add_member(e->channel + "_", EventEdgePortsType(g, cat, *e));
      }
    }
  }
  os << "};\n";
  return true;
}

// "<cls>Ports{&tok1, &tok2, &ev.<pipe>, ...}" — cyclic: positional ctor
// tokens then event out-edges; exec=event known nodes use designated
// initializers so pipe members bind without skipping pool fields.
// locals_prefix: when non-empty (e.g. "l"), pool/ring/pipe/extra refs become
// &l.ob_pool, &l.ob_pipe(), &l.chassis, etc.
std::string LocalMemberRef(const TemplateCatalog& cat,
                           const std::string& locals_prefix,
                           const std::string& tok) {
  const std::string head = locals_prefix.empty() ? "" : locals_prefix + ".";
  // A queue token materialises the adapter from the pool/ring pair; the pair
  // itself stays in the role locals (the adapter only references it).
  for (const auto& p : cat.pools) {
    if (tok == p.prefix + "_queue") {
      return "thyme::LocalIndexQueue<" + p.type + ", kRingCap>(&" + head +
             p.prefix + "_pool, &" + head + p.prefix + "_ring)";
    }
  }
  if (locals_prefix.empty()) return "&" + tok;
  for (const auto& p : cat.pools) {
    if (tok == p.prefix + "_pool" || tok == p.prefix + "_ring")
      return "&" + locals_prefix + "." + tok;
    if (tok == p.prefix + "_pipe")
      return "&" + locals_prefix + "." + p.prefix + "_pipe()";
  }
  for (const auto& x : cat.extras) {
    if (tok == x.name) return "&" + locals_prefix + "." + x.name;
  }
  return "&" + tok;
}

std::string KnownPortsAggregate(const Graph& g, const TemplateCatalog& cat,
                                const Node& n, bool* ok,
                                const std::string& event_prefix,
                                const std::string& locals_prefix,
                                const std::string& broadcast_prefix,
                                const std::string& pinned_slot_prefix) {
  const KnownCat* k = FindKnown(cat, n.type);
  std::ostringstream os;
  os << k->cls << "Ports{";
  auto emit_broadcast_fields = [&](bool* first, bool designated, bool as_out) {
    const NodeEdges ed = EdgesOf(g, n);
    std::set<std::string> seen_w;
    const auto& edges = as_out ? ed.out : ed.in;
    for (const auto* e : edges) {
      if (IsPinnedSlotDelivery(g, cat, *e)) {
        if (as_out && !seen_w.insert(e->channel).second) continue;
        if (!*first) os << ", ";
        if (designated) {
          os << "." << e->channel << (as_out ? "_w_" : "_") << " = &" << pinned_slot_prefix
             << PinnedSlotStoreMember(*e, as_out);
        } else {
          os << "&" << pinned_slot_prefix << PinnedSlotStoreMember(*e, as_out);
        }
        *first = false;
        continue;
      }
      if (!IsBroadcastDelivery(*e)) continue;
      if (as_out && !seen_w.insert(e->channel).second) continue;
      if (!*first) os << ", ";
      if (designated) {
        os << "." << e->channel << (as_out ? "_w_" : "_") << " = &" << broadcast_prefix;
        if (as_out) {
          os << BroadcastRingMember(*e);
        } else {
          os << BroadcastReaderMember(*e);
        }
      } else {
        os << "&" << broadcast_prefix;
        if (as_out) {
          os << BroadcastRingMember(*e);
        } else {
          os << BroadcastReaderMember(*e);
        }
      }
      *first = false;
    }
  };
  if (!IsEventNode(n)) {
    const bool designated = KnownTypeHasEventPorts(g, n.type) ||
                            KnownTypeHasBroadcastPorts(g, n.type);
    bool first = true;
    for (const auto& t : SplitCtorTokens(k->ctor)) {
      std::string mt;
      if (!CtorTokenMemberType(g, cat, t, &mt)) { *ok = false; return ""; }
      if (mt.empty()) continue;  // {health}
      if (!first) os << ", ";
      if (designated) {
        os << "." << t << " = " << LocalMemberRef(cat, locals_prefix, t);
      } else {
        os << LocalMemberRef(cat, locals_prefix, t);
      }
      first = false;
    }
    if (designated) {
      const NodeEdges ed = EdgesOf(g, n);
      std::set<std::string> hub_channels;
      emit_broadcast_fields(&first, /*designated=*/true, /*as_out=*/false);
      for (const auto* e : ed.out) {
        if (!EdgeUsesEventPipe(g, *e)) continue;
        if (IsLocalFrameHubEdge(g, cat, *e) &&
            !hub_channels.insert(e->channel).second) {
          continue;
        }
        if (!first) os << ", ";
        os << "." << e->channel << "_ = "
           << (IsLocalFrameHubEdge(g, cat, *e)
                   ? LocalFrameHubInputRef(g, cat, *e, event_prefix)
                   : "&" + event_prefix + EventPipeMember(*e));
        first = false;
      }
      emit_broadcast_fields(&first, /*designated=*/true, /*as_out=*/true);
    } else {
      emit_broadcast_fields(&first, /*designated=*/false, /*as_out=*/false);
      AppendKnownEventOutAggregate(os, g, cat, n, &first, event_prefix);
      emit_broadcast_fields(&first, /*designated=*/false, /*as_out=*/true);
    }
  } else {
    bool first = true;
    auto emit_field = [&](const std::string& field, const std::string& expr) {
      if (!first) os << ", ";
      os << "." << field << " = " << expr;
      first = false;
    };
    const NodeEdges ed = EdgesOf(g, n);
    std::set<std::string> seen_w;
    for (const auto* e : ed.in) {
      if (IsPinnedSlotDelivery(g, cat, *e)) {
        emit_field(e->channel + "_",
                   "&" + pinned_slot_prefix + PinnedSlotStoreMember(*e, false));
      } else if (IsBroadcastDelivery(*e)) {
        emit_field(e->channel + "_",
                   "&" + broadcast_prefix + BroadcastReaderMember(*e));
      } else if (IsLocalFrameHubEdge(g, cat, *e)) {
        emit_field(e->channel + "_", LocalFrameHubOutputRef(g, cat, *e, event_prefix));
      } else if (EdgeUsesEventPipe(g, *e)) {
        emit_field(e->channel + "_",
                   "&" + event_prefix + EventPipeMember(*e));
      }
    }
    for (const auto* e : ed.out) {
      if (IsPinnedSlotDelivery(g, cat, *e)) {
        if (!seen_w.insert(e->channel).second) continue;
        emit_field(e->channel + "_w_",
                   "&" + pinned_slot_prefix + PinnedSlotStoreMember(*e, true));
      } else if (IsBroadcastDelivery(*e)) {
        if (!seen_w.insert(e->channel).second) continue;
        emit_field(e->channel + "_w_",
                   "&" + broadcast_prefix + BroadcastRingMember(*e));
      } else if (IsLocalFrameHubEdge(g, cat, *e)) {
        if (!seen_w.insert(e->channel).second) continue;
        emit_field(e->channel + "_", LocalFrameHubInputRef(g, cat, *e, event_prefix));
      } else if (EdgeUsesEventPipe(g, *e)) {
        emit_field(e->channel + "_",
                   "&" + event_prefix + EventPipeMember(*e));
      }
    }
  }
  os << "}";
  *ok = true;
  return os.str();
}
// <PascalCase(type)>Ports struct for an unknown node, from its in/out edges
// (same channel_/channel_r_ member naming as the generated base class).
// Edges into an exec=event node carry a NotifPipe pointer instead of the
// pool/ring pair (full-delivery by-value transport).
bool AppendUnknownPortsStruct(std::ostringstream& os, const Graph& g,
                              const Node& n, const TemplateCatalog& cat) {
  const NodeEdges ed = EdgesOf(g, n);
  os << "struct " << PascalCase(n.shard_count > 1 ? n.id : n.type) << "Ports {\n";
  for (const auto* e : ed.in)
    AppendEdgeChannelMembers(os, g, cat, *e, /*skip_missing_pool=*/true,
                             /*as_out=*/false);
  std::set<std::string> seen_w;
  for (const auto* e : ed.out) {
    if ((IsBroadcastDelivery(*e) || IsPinnedSlotDelivery(g, cat, *e) ||
         IsLocalFrameHubEdge(g, cat, *e)) &&
        !seen_w.insert(e->channel).second) continue;
    AppendEdgeChannelMembers(os, g, cat, *e, /*skip_missing_pool=*/true,
                             /*as_out=*/true);
  }
  os << "};\n";
  return true;
}

// "<PascalCase(type)>Ports{&<pool>, &<ring|pipe>, ...}" aggregate for an
// unknown node's edges (in-then-out order, one pair per edge).
// event_prefix qualifies event-edge pipe members (e.g. "ev.").
std::string UnknownPortsAggregate(const Graph& g, const Node& n,
                                  const TemplateCatalog& cat,
                                  const std::string& event_prefix,
                                  const std::string& locals_prefix,
                                  const std::string& broadcast_prefix,
                                  const std::string& pinned_slot_prefix) {
  const NodeEdges ed = EdgesOf(g, n);
  std::ostringstream os;
  os << PascalCase(n.shard_count > 1 ? n.id : n.type) << "Ports{";
  bool first = true;
  auto emit = [&](const Edge* e, bool as_out) {
    if (IsPinnedSlotDelivery(g, cat, *e)) {
      if (!first) os << ", ";
      os << "&" << pinned_slot_prefix << PinnedSlotStoreMember(*e, as_out);
      first = false;
      return;
    }
    if (IsBroadcastDelivery(*e)) {
      if (!first) os << ", ";
      if (as_out) {
        os << "&" << broadcast_prefix << BroadcastRingMember(*e);
      } else {
        os << "&" << broadcast_prefix << BroadcastReaderMember(*e);
      }
      first = false;
      return;
    }
    if (IsLocalFrameHubEdge(g, cat, *e)) {
      if (!first) os << ", ";
      os << (as_out ? LocalFrameHubInputRef(g, cat, *e, event_prefix)
                    : LocalFrameHubOutputRef(g, cat, *e, event_prefix));
      first = false;
      return;
    }
    if (EdgeUsesEventPipe(g, *e)) {
      if (!first) os << ", ";
      os << "&" << event_prefix << EventPipeMember(*e);
      first = false;
      return;
    }
    const PoolCat* p = FindPool(cat, e->pool);
    if (p == nullptr) return;
    if (!first) os << ", ";
    if (p->index_pipe) {
      os << LocalMemberRef(cat, locals_prefix, p->prefix + "_pool");
      os << ", " << LocalMemberRef(cat, locals_prefix, p->prefix + "_pipe");
    } else {
      os << LocalMemberRef(cat, locals_prefix, p->prefix + "_queue");
    }
    first = false;
  };
  for (const auto* e : ed.in) emit(e, /*as_out=*/false);
  std::set<std::string> seen_w;
  for (const auto* e : ed.out) {
    if ((IsBroadcastDelivery(*e) || IsPinnedSlotDelivery(g, cat, *e) ||
         IsLocalFrameHubEdge(g, cat, *e)) &&
        !seen_w.insert(e->channel).second) continue;
    emit(e, /*as_out=*/true);
  }
  os << "}";
  return os.str();
}

// "MpContext{<health_expr>, &extra1, ...}" — the shared construction context
// (own health slot + catalog extra buffers, in catalog order).
std::string MpContextAggregate(const TemplateCatalog& cat,
                               const std::string& health_expr,
                               const std::string& locals_prefix) {
  std::ostringstream os;
  os << "MpContext{" << health_expr;
  for (const auto& x : cat.extras) {
    os << ", " << LocalMemberRef(cat, locals_prefix, x.name);
  }
  os << "}";
  return os.str();
}

// Full grouped call-site text for a known type:
//   "<cls>Ports{...}, MpContext{...}" — replaces the flat 15-26 arg list.
std::string KnownCtorGrouped(const Graph& g, const TemplateCatalog& cat,
                             const Node& n, const std::string& health_expr,
                             bool* ok, const std::string& event_prefix,
                             const std::string& locals_prefix,
                             const std::string& broadcast_prefix,
                             const std::string& pinned_slot_prefix) {
  const std::string ports = KnownPortsAggregate(
      g, cat, n, ok, event_prefix, locals_prefix, broadcast_prefix,
      pinned_slot_prefix);
  if (!*ok) return "";
  return ports + ", " + MpContextAggregate(cat, health_expr, locals_prefix);
}

// Full grouped call-site text for an unknown node.
std::string UnknownCtorGrouped(const Graph& g, const Node& n,
                               const TemplateCatalog& cat,
                               const std::string& health_expr,
                               const std::string& event_prefix,
                               const std::string& locals_prefix,
                               const std::string& broadcast_prefix,
                               const std::string& pinned_slot_prefix) {
  return UnknownPortsAggregate(g, n, cat, event_prefix, locals_prefix,
                               broadcast_prefix, pinned_slot_prefix) +
         ", " + MpContextAggregate(cat, health_expr, locals_prefix);
}
static void AppendPhaseFields(std::ostringstream& os, const Graph& g,
                              const Node& n) {
  if (!n.apply_phase) return;
  if (g.phase_mode == "require") {
    os << "    p.phase_mode = thyme::ThreadPlacement::PhaseMode::Require;\n";
  } else {
    os << "    p.phase_mode = thyme::ThreadPlacement::PhaseMode::BestEffort;\n";
  }
  if (n.phase_in_period_ns != 0) {
    os << "    p.phase_ns = " << n.phase_in_period_ns << "ULL;\n";
  }
}

bool AppendPlacement(std::ostringstream& os, const Graph& g, const Node& n,
                     const std::string& member) {
  if (n.cpu_set.size() > kMaxCpus) {
    const std::string loc = GraphLoc(g, n.line);
    std::fprintf(stderr,
                 "gen_graph: %scpu_set exceeds ThreadPlacement::kMaxCpus (node %s)\n",
                 loc.c_str(), n.id.c_str());
    return false;
  }
  os << "  {\n";
  os << "    thyme::ThreadPlacement p{};\n";
  if (!n.cpu_set.empty()) {
    os << "    p.ncpus = " << n.cpu_set.size() << ";\n";
    for (std::size_t i = 0; i < n.cpu_set.size(); ++i) {
      os << "    p.cpus[" << i << "] = " << n.cpu_set[i] << ";\n";
    }
  }
  if (n.sched_policy == "fifo") {
    os << "    p.policy = thyme::ThreadPlacement::Policy::Fifo;\n";
  } else if (n.sched_policy == "rr") {
    os << "    p.policy = thyme::ThreadPlacement::Policy::Rr;\n";
  } else if (n.sched_policy == "other") {
    os << "    p.policy = thyme::ThreadPlacement::Policy::Other;\n";
  }
  os << "    p.priority = " << NodeEffectivePriority(n) << ";\n";
  if (n.placement_fail == "require") {
    os << "    p.fail = thyme::ThreadPlacement::Fail::Require;\n";
  }
  AppendPhaseFields(os, g, n);
  os << "    " << member << ".SetPlacement(p);\n";
  os << "  }\n";
  return true;
}

bool AppendWaitInputs(std::ostringstream& os, const Graph& g, const Node& n,
                      const TemplateCatalog& cat, const std::string& member) {
  if (n.wait_inputs != "period") return true;
  const std::string node_loc = GraphLoc(g, n.line);
  const NodeEdges ed = EdgesOf(g, n);
  for (const auto* e : ed.in) {
    if (!e->shm.empty()) {
      const std::string edge_loc = GraphLoc(g, e->line);
      std::fprintf(stderr,
                   "gen_graph: %swait_inputs not supported on shm edges (node %s)\n",
                   edge_loc.c_str(), n.id.c_str());
      return false;
    }
  }
  if (ed.in.empty()) {
    std::fprintf(stderr,
                 "gen_graph: %swait_inputs requires in-edges (node %s)\n",
                 node_loc.c_str(), n.id.c_str());
    return false;
  }
  std::vector<std::string> rings;
  std::set<std::string> seen;
  for (const auto* e : ed.in) {
    if (IsBroadcastDelivery(*e) || IsPinnedSlotDelivery(g, cat, *e)) continue;
    const PoolCat* p = FindPool(cat, e->pool);
    if (p == nullptr) {
      const std::string edge_loc = GraphLoc(g, e->line);
      std::fprintf(stderr,
                   "gen_graph: %sunknown pool %s on wait_inputs (node %s)\n",
                   edge_loc.c_str(), e->pool.c_str(), n.id.c_str());
      return false;
    }
    if (!seen.insert(p->prefix).second) continue;
    rings.push_back(p->prefix + "_ring");
  }
  if (rings.size() > thyme::kAbiMaxCyclicWaitInputs) {
    std::fprintf(stderr,
                 "gen_graph: %swait_inputs exceeds CyclicThread::kMaxWaitInputs (node %s)\n",
                 node_loc.c_str(), n.id.c_str());
    return false;
  }
  os << "  {\n";
  os << "    thyme::Waitable* ws[] = {";
  for (std::size_t i = 0; i < rings.size(); ++i) {
    if (i) os << ", ";
    os << "&" << rings[i];
  }
  os << "};\n";
  os << "    " << member << ".SetWaitInputs(ws, " << rings.size() << ");\n";
  os << "  }\n";
  return true;
}

std::vector<std::string> WaitInputRingMembers(const Graph& g, const Node& n,
                                              const TemplateCatalog& cat) {
  std::vector<std::string> rings;
  if (n.wait_inputs != "period") return rings;
  const NodeEdges ed = EdgesOf(g, n);
  std::set<std::string> seen;
  for (const auto* e : ed.in) {
    if (IsBroadcastDelivery(*e) || IsPinnedSlotDelivery(g, cat, *e)) continue;
    const PoolCat* p = FindPool(cat, e->pool);
    if (p == nullptr) continue;
    if (!seen.insert(p->prefix).second) continue;
    rings.push_back(p->prefix + "_ring");
  }
  return rings;
}

bool AppendSchedulingApplyBranch(std::ostringstream& os, const Graph& g,
                                 const Node& n, const TemplateCatalog& cat,
                                 bool first_branch) {
  if (!NodeNeedsPlacement(n) && n.wait_inputs != "period") return true;
  const std::string loc = GraphLoc(g, n.line);
  if (first_branch) {
    os << "  if (std::strcmp(node_id, \"" << n.id << "\") == 0) {\n";
  } else {
    os << "  } else if (std::strcmp(node_id, \"" << n.id << "\") == 0) {\n";
  }
  if (NodeNeedsPlacement(n)) {
    if (n.cpu_set.size() > kMaxCpus) {
      std::fprintf(stderr,
                   "gen_graph: %scpu_set exceeds ThreadPlacement::kMaxCpus (node %s)\n",
                   loc.c_str(), n.id.c_str());
      return false;
    }
    os << "    thyme::ThreadPlacement p{};\n";
    if (!n.cpu_set.empty()) {
      os << "    p.ncpus = " << n.cpu_set.size() << ";\n";
      for (std::size_t i = 0; i < n.cpu_set.size(); ++i) {
        os << "    p.cpus[" << i << "] = " << n.cpu_set[i] << ";\n";
      }
    }
    if (n.sched_policy == "fifo") {
      os << "    p.policy = thyme::ThreadPlacement::Policy::Fifo;\n";
    } else if (n.sched_policy == "rr") {
      os << "    p.policy = thyme::ThreadPlacement::Policy::Rr;\n";
    } else if (n.sched_policy == "other") {
      os << "    p.policy = thyme::ThreadPlacement::Policy::Other;\n";
    }
    os << "    p.priority = " << NodeEffectivePriority(n) << ";\n";
    if (n.placement_fail == "require") {
      os << "    p.fail = thyme::ThreadPlacement::Fail::Require;\n";
    }
    AppendPhaseFields(os, g, n);
    os << "    t->SetPlacement(p);\n";
  }
  if (n.wait_inputs == "period") {
    const auto rings = WaitInputRingMembers(g, n, cat);
    if (rings.empty()) {
      std::fprintf(stderr,
                   "gen_graph: %swait_inputs requires in-edges (node %s)\n",
                   loc.c_str(), n.id.c_str());
      return false;
    }
    if (rings.size() > thyme::kAbiMaxCyclicWaitInputs) {
      std::fprintf(stderr,
                   "gen_graph: %swait_inputs exceeds CyclicThread::kMaxWaitInputs (node %s)\n",
                   loc.c_str(), n.id.c_str());
      return false;
    }
    os << "    thyme::Waitable* ws[] = {";
    for (std::size_t i = 0; i < rings.size(); ++i) {
      if (i) os << ", ";
      os << "&rings." << rings[i];
    }
    os << "};\n";
    os << "    t->SetWaitInputs(ws, " << rings.size() << ");\n";
  }
  return true;
}

bool EmitSchedulingApplyInc(const std::string& tmp_dir, const Graph& g,
                            const TemplateCatalog& cat) {
  std::ostringstream os;
  os << BannerHeader(cat);
  bool any = false;
  bool first = true;
  for (const auto& n : g.nodes) {
    // Event threads take their placement at construction (see MakeEventThread
    // / wiring event blocks); this inc is applied to CyclicThreads only.
    if (IsEventNode(n)) continue;
    if (!NodeNeedsPlacement(n) && n.wait_inputs != "period") continue;
    if (!AppendSchedulingApplyBranch(os, g, n, cat, first)) return false;
    any = true;
    first = false;
  }
  if (any) os << "  }\n";
  return WriteFile(fs::path(tmp_dir) / "scheduling_apply.inc", os.str());
}

// The SchedulingRings aggregate owns one Ring<PoolT>& per wait_inputs ring
// member, in catalog (pools.txt) order. Both the single-process Pipeline and
// the MP RoleLocals expose the same <prefix>_ring member names, so a single
// templated factory serves both call sites and removes the hand-written ring
// literals from scheduling_rings.inc / mp_roles.cpp.
bool EmitSchedulingRings(const std::string& tmp_dir, const Graph& g,
                         const TemplateCatalog& cat) {
  std::set<std::string> used_prefixes;
  for (const auto& n : g.nodes) {
    if (IsEventNode(n)) continue;
    if (n.wait_inputs != "period") continue;
    for (const auto& m : WaitInputRingMembers(g, n, cat)) {
      // "<prefix>_ring" -> "<prefix>".
      if (m.size() > 5) used_prefixes.insert(m.substr(0, m.size() - 5));
    }
  }

  struct RingMember {
    std::string pool_type;
    std::string prefix;
  };
  std::vector<RingMember> members;
  for (const auto& p : cat.pools) {
    if (used_prefixes.count(p.prefix) == 0) continue;
    members.push_back({p.type, p.prefix});
  }

  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n";
  os << "#include \"generated/graph_types.h\"\n\n";
  os << "// Ring handles for wait_inputs nodes, generated from the graph's\n";
  os << "// wait_inputs in-edges (mirrors generated/scheduling_apply.inc).\n";
  os << "struct SchedulingRings {\n";
  for (const auto& m : members) {
    os << "  Ring<" << m.pool_type << ">& " << m.prefix << "_ring;\n";
  }
  os << "};\n\n";
  os << "// One templated factory serves both owners: the single-process\n";
  os << "// Pipeline and the MP RoleLocals expose the same <prefix>_ring members.\n";
  os << "template <typename L>\n";
  os << "inline SchedulingRings BuildSchedulingRings(L& l) {\n";
  if (members.empty()) {
    os << "  (void)l;\n";
    os << "  return {};\n";
  } else {
    os << "  return SchedulingRings{";
    for (std::size_t i = 0; i < members.size(); ++i) {
      if (i) os << ", ";
      os << "l." << members[i].prefix << "_ring";
    }
    os << "};\n";
  }
  os << "}\n";
  return WriteFile(fs::path(tmp_dir) / "scheduling_rings.h", os.str());
}

}  // namespace gen_emit
