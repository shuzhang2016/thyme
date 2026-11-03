#pragma once

#include "catalog.h"
#include "graph.h"
#include "thyme/runtime/channel_capability.h"

#include <string>

// Derive one edge's transport contract. All emitters consume this result;
// no emitter re-implements size-based routing or treats broadcast as retention.
bool DeriveChannelCapability(const Graph& g, const TemplateCatalog& cat,
                             const Edge& edge,
                             thyme::ChannelCapabilityInfo* out,
                             std::string* error);
