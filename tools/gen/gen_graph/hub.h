#pragma once

#include "catalog.h"
#include "graph.h"

#include <cstddef>
#include <string>
#include <vector>

namespace gen_emit {

struct LocalFrameHub {
  std::string channel;
  std::string cpp_type;
  std::string process;
  std::size_t cap = 0;
  std::vector<const Edge*> edges;
};

std::vector<LocalFrameHub> BuildLocalFrameHubs(const Graph& g,
                                               const TemplateCatalog& cat);
bool HasLocalFrameHub(const Graph& g, const TemplateCatalog& cat);
bool FindLocalFrameHub(const Graph& g, const TemplateCatalog& cat,
                       const Edge& edge, LocalFrameHub* out);
bool IsLocalFrameHubEdge(const Graph& g, const TemplateCatalog& cat,
                         const Edge& edge);
std::size_t LocalFrameHubOutputIndex(const LocalFrameHub& hub, const Edge& edge);
std::string LocalFrameHubMember(const LocalFrameHub& hub);
std::string LocalFrameHubType(const Graph& g, const LocalFrameHub& hub);
std::string LocalFrameHubInputRef(const Graph& g, const TemplateCatalog& cat,
                                  const Edge& edge, const std::string& prefix);
std::string LocalFrameHubOutputRef(const Graph& g, const TemplateCatalog& cat,
                                   const Edge& edge, const std::string& prefix);

}  // namespace gen_emit
