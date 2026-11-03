#pragma once

#include "graph.h"

#include <string>

// Options for --derive-template: emit the Group 1 template overlay (catalog
// tables + graph fragments) from graph.yaml alone.
struct DeriveOptions {
  std::string template_out;       // output dir (created if missing)
  std::string project_name;       // optional -> banner.txt + graph_types banner
  std::string contracts_include;  // --contracts-include fallback (may be empty)
  bool force = false;             // allow overwriting existing files
};

// Derives and writes the Group 1 template files into opt.template_out:
//   pools.txt, contracts.txt, known_types.txt, channel_defaults.txt,
//   shm_kinds.txt (only if a shm edge exists), graph_types.h.inc,
//   mp_includes.inc, banner.txt (only if --project-name given).
// The contracts include path is resolved once:
//   <template_out>/contracts_include.txt  →  opt.contracts_include  →
//   "contracts/types.h".
// Never overwrites an existing file without opt.force; prints `wrote <path>`
// / `kept <path>` for each file. Returns false on error (prints to stderr).
bool DeriveTemplate(const Graph& g, const DeriveOptions& opt);
