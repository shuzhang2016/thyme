#pragma once

#include "catalog.h"
#include "graph.h"

#include <string>
#include <vector>

// GEN-02 semantic validation. Runs after LoadGraphYaml (and the GEN-11 cycle
// check), before any emit: the graph must not just parse, it must produce
// coherent generated C++. Every violation appends one message to `errors`;
// the caller prints them and exits 2. Returns true when no violation was
// found.
//
// `cat` may be nullptr; the catalog-dependent rule (shm kind resolution) is
// skipped then. --explain-rates / --derive-template validate the graph
// as-written (no catalog); the codegen/scaffold path validates against the
// effective template catalog.
bool ValidateGraphSemantics(const Graph& g, const TemplateCatalog* cat,
                            std::vector<std::string>* errors);
