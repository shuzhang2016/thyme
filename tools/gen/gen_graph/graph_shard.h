#pragma once

#include "graph.h"

#include <string>

// Preserves the graph exactly when every node has threads == 1. Otherwise
// expands sink-only event nodes into independent physical workers.
bool ProjectGraphShards(const Graph& logical, Graph* physical,
                        std::string* error);
