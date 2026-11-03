#include "dag.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace {

enum class Mark : uint8_t { kWhite = 0, kGray = 1, kBlack = 2 };

struct Adjacent {
  std::string to;
  int line;
};

using Adjacency = std::unordered_map<std::string, std::vector<Adjacent>>;

bool Visit(const Adjacency& adj, const std::string& u,
           std::unordered_map<std::string, Mark>* mark,
           std::vector<std::string>* stack, std::vector<std::string>* cycle_path,
           int* closing_edge_line) {
  (*mark)[u] = Mark::kGray;
  stack->push_back(u);
  const auto it = adj.find(u);
  if (it != adj.end()) {
    for (const Adjacent& v : it->second) {
      const Mark m = (*mark)[v.to];
      if (m == Mark::kGray) {
        // Back edge into the active stack: the cycle runs from the gray
        // ancestor to the stack top and closes back to it.
        const auto begin = std::find(stack->begin(), stack->end(), v.to);
        cycle_path->assign(begin, stack->end());
        cycle_path->push_back(v.to);
        if (closing_edge_line != nullptr) *closing_edge_line = v.line;
        return true;
      }
      if (m == Mark::kWhite &&
          Visit(adj, v.to, mark, stack, cycle_path, closing_edge_line)) {
        return true;
      }
    }
  }
  stack->pop_back();
  (*mark)[u] = Mark::kBlack;
  return false;
}

}  // namespace

bool FindCycle(const Graph& g, std::vector<std::string>* cycle_path,
               int* closing_edge_line) {
  if (cycle_path == nullptr) return false;
  cycle_path->clear();
  if (closing_edge_line != nullptr) *closing_edge_line = 0;

  std::unordered_map<std::string, bool> known;
  for (const Node& n : g.nodes) known[n.id] = true;

  Adjacency adj;
  for (const Edge& e : g.edges) {
    if (e.feedback) continue;  // cut edge: closes a loop on purpose
    if (!known.count(e.from) || !known.count(e.to)) continue;
    adj[e.from].push_back(Adjacent{e.to, e.line});
  }

  std::unordered_map<std::string, Mark> mark;
  std::vector<std::string> stack;
  for (const Node& n : g.nodes) {
    if (mark[n.id] == Mark::kWhite &&
        Visit(adj, n.id, &mark, &stack, cycle_path, closing_edge_line)) {
      return true;
    }
  }
  return false;
}
