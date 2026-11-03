#include "emit_internal.h"

#include "thyme/contracts/abi.h"

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

bool ValidateEventNodes(const Graph& g, const TemplateCatalog& cat) {
  for (const auto& n : g.nodes) {
    if (!IsEventNode(n)) continue;
    if (n.shard_count > 1) {
      const KnownCat* known = FindKnown(cat, n.type);
      if (known != nullptr) {
        for (const auto& token : SplitCtorTokens(known->ctor)) {
          if (token == "{health}") {
            continue;
          }
          const std::string loc = GraphLoc(g, n.line);
          std::fprintf(stderr,
                       "gen_graph: %ssharded node %s cannot share known ctor resource '%s'\n",
                       loc.c_str(), n.id.c_str(), token.c_str());
          return false;
        }
      }
    }
    std::size_t in_edges = 0;
    for (const auto& e : g.edges) {
      if (e.to == n.id) ++in_edges;
    }
    if (in_edges > thyme::kAbiMaxEventWaitInputs) {
      const std::string loc = GraphLoc(g, n.line);
      std::fprintf(stderr,
                   "gen_graph: %sexec=event node %s has %zu in-edges "
                   "(EventThread::kMaxWaitInputs is %zu)\n",
                   loc.c_str(), n.id.c_str(), in_edges,
                   thyme::kAbiMaxEventWaitInputs);
      return false;
    }
  }
  return true;
}

// mp_gate edges in yaml edge order; vector index == cross_frames slot.
std::vector<const Edge*> CrossGateEdges(const Graph& g) {
  std::vector<const Edge*> gates;
  for (const auto& e : g.edges) {
    if (e.mp_gate) gates.push_back(&e);
  }
  return gates;
}

bool ValidateGraphPools(const Graph& g, const TemplateCatalog& cat) {
  for (const auto& e : g.edges) {
    if (EdgeToEventNode(g, e)) continue;  // NotifPipe transport, no pool
    if (IsBroadcastDelivery(e)) continue;  // payload lives in ShmSpmcRing
    if (FindPool(cat, e.pool) == nullptr) {
      const std::string loc = GraphLoc(g, e.line);
      std::fprintf(stderr,
                   "gen_graph: %sunknown pool %s on edge %s->%s (channel=%s)\n",
                   loc.c_str(), e.pool.c_str(), e.from.c_str(), e.to.c_str(),
                   e.channel.c_str());
      return false;
    }
  }
  return true;
}
std::vector<PoolDecl> UniquePools(const Graph& g, const TemplateCatalog& cat) {
  std::vector<PoolDecl> pools; std::set<std::string> seen;
  for (const auto& e : g.edges) {
    if (IsBroadcastDelivery(e)) continue;
    if (!seen.insert(e.pool).second) continue;
    const PoolCat* p = FindPool(cat, e.pool);
    if (p == nullptr) continue;
    PoolDecl d; d.pool_type = p->type; d.prefix = p->prefix;
    d.index_pipe = p->index_pipe; pools.push_back(d);
  }
  return pools;
}

}  // namespace gen_emit
