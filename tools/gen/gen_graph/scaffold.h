#pragma once

#include "graph.h"

#include <string>

// Scaffold handwritten src/ files from templates. `update` is the --update
// value (nodes|app|all); `force` overwrites existing handwritten files (and,
// with nodes, empties Loop bodies). `tpl_dir` is the runtime template
// directory (defaults to GEN_GRAPH_TEMPLATE_DIR in main).
bool Scaffold(const std::string& root, const Graph& g,
              const std::string& update, bool force,
              const std::string& tpl_dir);

// Materialize the effective template view `merged` as base ∪ overlay (same
// relative path: overlay shadows base; overlay may add new files). `merged`
// must not already exist. Returns false when overlay is not a directory or a
// copy fails.
bool MergeTemplateDir(const std::string& base, const std::string& overlay,
                      const std::string& merged);
