#include "graph_shard.h"

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace {

struct ShardNames {
  uint32_t threads = 1;
  std::vector<std::string> names;
};

}  // namespace

bool ProjectGraphShards(const Graph& logical, Graph* physical,
                        std::string* error) {
  if (physical == nullptr || error == nullptr) {
    return false;
  }
  error->clear();
  std::map<std::string, ShardNames> shards;
  std::set<std::string> logical_ids;
  for (const Node& n : logical.nodes) {
    logical_ids.insert(n.id);
  }
  std::set<std::string> physical_ids;
  for (const Node& n : logical.nodes) {
    ShardNames names;
    names.threads = n.threads;
    for (uint32_t shard = 0; shard < n.threads; ++shard) {
      const std::string id = n.threads == 1
                                 ? n.id
                                 : n.id + "__s" + std::to_string(shard);
      const bool inserted = physical_ids.insert(id).second;
      if (n.threads > 1 && (logical_ids.count(id) != 0 || !inserted)) {
        *error = "physical worker name collision: " + id;
        return false;
      }
      names.names.push_back(id);
    }
    shards.emplace(n.id, std::move(names));
  }

  Graph out = logical;
  out.nodes.clear();
  for (const Node& n : logical.nodes) {
    const auto& names = shards[n.id].names;
    for (uint32_t shard = 0; shard < n.threads; ++shard) {
      Node worker = n;
      worker.id = names[shard];
      worker.threads = 1;
      worker.shard_count = n.threads;
      out.nodes.push_back(std::move(worker));
    }
  }
  std::map<std::string, std::size_t> input_ordinals;
  for (Edge& e : out.edges) {
    const auto it = shards.find(e.to);
    if (it == shards.end() || it->second.threads == 1) {
      continue;
    }
    const std::size_t ordinal = input_ordinals[e.to]++;
    e.to = it->second.names[ordinal % it->second.threads];
  }
  *physical = std::move(out);
  return true;
}
