#pragma once

#include "graph.h"

#include <string>
#include <vector>

// GEN-11: find a directed cycle among nodes. Edges marked feedback
// (feedback: true + explicit delay) close loops on purpose and are cut for
// DAG purposes, so they never take part in a reported cycle. Returns true and
// fills cycle_path with the node ids in cycle order (first id repeated at the
// end, e.g. "a b c a") when a cycle exists. When `closing_edge_line` is
// non-null it receives the yaml line of the edge that closes the cycle (0
// when unknown), so diagnostics can point at the offending edge.
bool FindCycle(const Graph& g, std::vector<std::string>* cycle_path,
               int* closing_edge_line = nullptr);
