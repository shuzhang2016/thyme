#include "hub.h"

#include "emit_internal.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace gen_emit {

std::vector<LocalFrameHub> BuildLocalFrameHubs(const Graph& g,
                                               const TemplateCatalog& cat) {
  std::map<std::tuple<std::string, std::string, std::string>, LocalFrameHub> groups;
  for (const auto& edge : g.edges) {
    if (IsPinnedSlotDelivery(g, cat, edge) || !EdgeUsesEventPipe(g, edge) ||
        !edge.shm.empty()) {
      continue;
    }
    const Node* from = FindNodeById(g, edge.from);
    const Node* to = FindNodeById(g, edge.to);
    if (from == nullptr || to == nullptr || from->process != to->process) {
      continue;
    }
    const auto key = std::make_tuple(edge.from, edge.channel, from->process);
    auto& hub = groups[key];
    hub.channel = edge.channel;
    hub.cpp_type = edge.cpp_type;
    hub.process = from->process;
    hub.cap = static_cast<std::size_t>(EventEdgeCap(g, edge));
    hub.edges.push_back(&edge);
  }
  std::vector<LocalFrameHub> hubs;
  for (auto& entry : groups) {
    auto& hub = entry.second;
    const bool dynamic = std::any_of(hub.edges.begin(), hub.edges.end(),
                                     [](const Edge* edge) { return edge->dynamic; });
    if (hub.edges.size() > 1 || dynamic) {
      hubs.push_back(std::move(hub));
    }
  }
  return hubs;
}

bool HasLocalFrameHub(const Graph& g, const TemplateCatalog& cat) {
  return !BuildLocalFrameHubs(g, cat).empty();
}

bool FindLocalFrameHub(const Graph& g, const TemplateCatalog& cat,
                       const Edge& edge, LocalFrameHub* out) {
  for (const auto& hub : BuildLocalFrameHubs(g, cat)) {
    for (const auto* candidate : hub.edges) {
      if (candidate == &edge) {
        if (out != nullptr) *out = hub;
        return true;
      }
    }
  }
  return false;
}

bool IsLocalFrameHubEdge(const Graph& g, const TemplateCatalog& cat,
                         const Edge& edge) {
  return FindLocalFrameHub(g, cat, edge, nullptr);
}

std::size_t LocalFrameHubOutputIndex(const LocalFrameHub& hub, const Edge& edge) {
  for (std::size_t i = 0; i < hub.edges.size(); ++i) {
    if (hub.edges[i] == &edge) return i;
  }
  return hub.edges.size();
}

std::string LocalFrameHubMember(const LocalFrameHub& hub) {
  return hub.channel + "_hub_";
}

std::string LocalFrameHubType(const Graph& g, const LocalFrameHub& hub) {
  (void)g;
  return "thyme::LocalFrameHub<" + hub.cpp_type + ", " + std::to_string(hub.cap) +
         ", " + std::to_string(hub.edges.size()) + ">";
}

std::string LocalFrameHubInputRef(const Graph& g, const TemplateCatalog& cat,
                                  const Edge& edge, const std::string& prefix) {
  LocalFrameHub hub;
  if (!FindLocalFrameHub(g, cat, edge, &hub)) return {};
  return "&" + prefix + LocalFrameHubMember(hub) + ".input()";
}

std::string LocalFrameHubOutputRef(const Graph& g, const TemplateCatalog& cat,
                                   const Edge& edge, const std::string& prefix) {
  LocalFrameHub hub;
  if (!FindLocalFrameHub(g, cat, edge, &hub)) return {};
  return "&" + prefix + LocalFrameHubMember(hub) + ".output(" +
         std::to_string(LocalFrameHubOutputIndex(hub, edge)) + ")";
}

}  // namespace gen_emit
