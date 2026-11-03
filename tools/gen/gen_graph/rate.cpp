#include "rate.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <queue>
#include <set>
#include <unordered_map>

namespace {

// Scratch buffer for one warning/loop-cut line (snprintf-truncated).
constexpr std::size_t kWarnLineBufSize = 256;

const Node* FindNode(const Graph& g, const std::string& id) {
  for (const Node& n : g.nodes) {
    if (n.id == id) return &n;
  }
  return nullptr;
}

std::string EdgeKey(const Edge& e) {
  return e.from + "->" + e.to + ":" + e.channel;
}

// Default publish gating for nodes that wait for all inputs (fusion/decision hand code).
std::string EffectivePublish(const Node& n) {
  if (!n.publish.empty()) return n.publish;
  if (n.type == "fusion" || n.type == "decision") return "on_all_inputs";
  return "";
}

bool IsCyclic(const Node& n) { return n.exec.empty() || n.exec == "cyclic"; }

std::vector<std::string> TopoSort(const Graph& g, std::vector<std::string>* unresolved) {
  std::unordered_map<std::string, int> indeg;
  std::unordered_map<std::string, std::vector<std::string>> adj;
  for (const Node& n : g.nodes) indeg[n.id] = 0;
  for (const Edge& e : g.edges) {
    if (e.feedback) continue;  // GEN-11: loop cut at the feedback edge
    if (FindNode(g, e.from) == nullptr || FindNode(g, e.to) == nullptr) continue;
    adj[e.from].push_back(e.to);
    ++indeg[e.to];
  }
  std::queue<std::string> q;
  for (const Node& n : g.nodes) {
    if (indeg[n.id] == 0) q.push(n.id);
  }
  std::vector<std::string> order;
  while (!q.empty()) {
    const std::string u = q.front();
    q.pop();
    order.push_back(u);
    for (const std::string& v : adj[u]) {
      if (--indeg[v] == 0) q.push(v);
    }
  }
  if (order.size() != g.nodes.size()) {
    unresolved->push_back("graph has cycles or disconnected unknown nodes");
  }
  return order;
}

int MinUpstreamFrame(const Graph& g, const std::string& id,
                     const std::map<std::string, int>& frame_rate,
                     std::vector<std::string>* unresolved) {
  int up_min = std::numeric_limits<int>::max();
  bool any = false;
  for (const Edge& e : g.edges) {
    if (e.to != id || e.feedback) continue;
    any = true;
    const auto it = frame_rate.find(e.from);
    if (it == frame_rate.end()) {
      unresolved->push_back("missing frame rate for upstream " + e.from);
      continue;
    }
    up_min = std::min(up_min, it->second);
  }
  if (!any) return -1;
  return up_min;
}

int SumInEdgeFrameRates(const Graph& g, const std::string& id,
                        const std::map<std::string, int>& frame_rate,
                        std::vector<std::string>* unresolved) {
  int sum = 0;
  bool any = false;
  for (const Edge& e : g.edges) {
    if (e.to != id || e.feedback) continue;
    any = true;
    const auto it = frame_rate.find(e.from);
    if (it == frame_rate.end()) {
      unresolved->push_back("missing frame rate for upstream " + e.from);
      continue;
    }
    sum += it->second;
  }
  if (!any) return -1;
  return sum;
}

}  // namespace

bool DeriveRates(const Graph& g, RateInfo* out, std::vector<std::string>* /*warnings*/) {
  if (out == nullptr) return false;
  out->node_publish_rate.clear();
  out->edge_producer_rate.clear();
  out->node_frame_rate.clear();
  out->unresolved.clear();
  out->loop_cuts.clear();

  // GEN-11: feedback edges close loops on purpose. The DAG is truncated at
  // each of them (TopoSort and the upstream-rate walks above skip them) and
  // the rate across the cut is reported as loop-unresolved.
  for (const Edge& e : g.edges) {
    if (!e.feedback) continue;
    if (FindNode(g, e.from) == nullptr || FindNode(g, e.to) == nullptr) continue;
    char buf[kWarnLineBufSize];
    std::snprintf(buf, sizeof(buf),
                  "loop unresolved at feedback edge %s (delay %llu ns)",
                  EdgeKey(e).c_str(),
                  static_cast<unsigned long long>(e.delay_ns));
    out->loop_cuts.push_back(buf);
  }

  const std::vector<std::string> order = TopoSort(g, &out->unresolved);

  for (const std::string& id : order) {
    const Node* n = FindNode(g, id);
    if (n == nullptr) continue;

    const int up_min = MinUpstreamFrame(g, id, out->node_frame_rate, &out->unresolved);
    int frame = n->hz;
    if (up_min < 0) {
      frame = n->hz;
    } else if (IsCyclic(*n)) {
      frame = std::min(n->hz, up_min);
    } else {
      // event: trigger when all inputs ready (on_all_inputs) or synchronized min.
      (void)EffectivePublish(*n);
      frame = up_min;
    }
    out->node_frame_rate[id] = frame;

    for (const Edge& e : g.edges) {
      if (e.from != id) continue;
      out->edge_producer_rate[EdgeKey(e)] = frame;
    }

    const int in_sum = SumInEdgeFrameRates(g, id, out->node_frame_rate, &out->unresolved);
    if (in_sum < 0) {
      out->node_publish_rate[id] = n->hz;
    } else {
      out->node_publish_rate[id] = in_sum;
    }
  }

  for (const Node& n : g.nodes) {
    if (out->node_frame_rate.find(n.id) == out->node_frame_rate.end()) {
      out->unresolved.push_back("no frame rate for node " + n.id);
    }
  }

  return out->unresolved.empty();
}

void WarnHzDeviation(const Graph& g, const RateInfo& info, double threshold,
                     std::vector<std::string>* warnings) {
  if (warnings == nullptr || threshold <= 0.0) return;
  for (const Node& n : g.nodes) {
    const auto it = info.node_frame_rate.find(n.id);
    if (it == info.node_frame_rate.end()) continue;
    const int derived = it->second;
    if (n.hz <= 0) continue;
    const double rel =
        static_cast<double>(std::abs(n.hz - derived)) / static_cast<double>(n.hz);
    if (rel > threshold) {
      char buf[kWarnLineBufSize];
      std::snprintf(buf, sizeof(buf),
                    "RATE_WARN node %s: declared hz=%d derived_frame=%d (%.0f%% deviation)",
                    n.id.c_str(), n.hz, derived, rel * 100.0);
      warnings->push_back(buf);
    }
  }
}

void WarnMixedEventRates(const Graph& g, const RateInfo& info,
                         std::vector<std::string>* warnings) {
  if (warnings == nullptr) return;
  for (const Node& n : g.nodes) {
    if (n.exec != "event") continue;
    int min_rate = std::numeric_limits<int>::max();
    int max_rate = 0;
    std::string slow, fast;
    int inputs = 0;
    for (const Edge& e : g.edges) {
      if (e.to != n.id || e.feedback) continue;
      ++inputs;
      const auto it = info.node_frame_rate.find(e.from);
      if (it == info.node_frame_rate.end()) continue;
      const int r = it->second;
      if (r <= 0) continue;
      if (r < min_rate) {
        min_rate = r;
        slow = e.from + "->" + e.channel;
      }
      if (r > max_rate) {
        max_rate = r;
        fast = e.from + "->" + e.channel;
      }
    }
    if (inputs < 2 || min_rate <= 0 || max_rate <= 0) continue;
    // "Materially mixed" = at least 2x and at least 2 messages/s apart.
    if (max_rate < min_rate * 2 || max_rate - min_rate < 2) continue;
    char buf[kWarnLineBufSize];
    std::snprintf(
        buf, sizeof(buf),
        "SCH03_WARN event node %s mixes in-edge rates (%d Hz via %s, %d Hz "
        "via %s); split the node (preferred) or set input_budget to bound "
        "each input per Dispatch (docs/reference/scheduling-model.md §7)",
        n.id.c_str(), min_rate, slow.c_str(), max_rate, fast.c_str());
    warnings->push_back(buf);
  }
}

void PrintRateWarnings(const std::vector<std::string>& warnings) {
  for (const std::string& w : warnings) {
    if (w.rfind("RATE_WARN", 0) == 0 || w.rfind("DEPTH_WARN", 0) == 0 ||
        w.rfind("SCH03_WARN", 0) == 0) {
      std::fprintf(stderr, "%s\n", w.c_str());
    }
  }
}

void ExplainRatesJson(const RateInfo& info, const std::vector<std::string>& warnings) {
  std::printf("{\n");
  std::printf("  \"node_publish_rate\": {\n");
  bool first = true;
  for (const auto& kv : info.node_publish_rate) {
    std::printf("%s    \"%s\": %d", first ? "" : ",\n", kv.first.c_str(), kv.second);
    first = false;
  }
  std::printf("\n  },\n");
  std::printf("  \"node_frame_rate\": {\n");
  first = true;
  for (const auto& kv : info.node_frame_rate) {
    std::printf("%s    \"%s\": %d", first ? "" : ",\n", kv.first.c_str(), kv.second);
    first = false;
  }
  std::printf("\n  },\n");
  std::printf("  \"edge_producer_rate\": {\n");
  first = true;
  for (const auto& kv : info.edge_producer_rate) {
    std::printf("%s    \"%s\": %d", first ? "" : ",\n", kv.first.c_str(), kv.second);
    first = false;
  }
  std::printf("\n  },\n");
  std::printf("  \"unresolved\": [");
  for (size_t i = 0; i < info.unresolved.size(); ++i) {
    std::printf("%s\"%s\"", i ? ", " : "", info.unresolved[i].c_str());
  }
  std::printf("],\n");
  std::printf("  \"loop_unresolved\": [");
  for (size_t i = 0; i < info.loop_cuts.size(); ++i) {
    std::printf("%s\"%s\"", i ? ", " : "", info.loop_cuts[i].c_str());
  }
  std::printf("],\n");
  std::printf("  \"warnings\": [");
  for (size_t i = 0; i < warnings.size(); ++i) {
    std::printf("%s\"%s\"", i ? ", " : "", warnings[i].c_str());
  }
  std::printf("]\n");
  std::printf("}\n");
}
