#pragma once

#include "graph.h"

#include <map>
#include <string>
#include <vector>

struct RateInfo {
  // Steady-state messages/s attributed to the node (sum of in-edge rates when gated).
  std::map<std::string, int> node_publish_rate;
  // Per-edge producer rate: one message on this channel per upstream frame.
  std::map<std::string, int> edge_producer_rate;
  // Frame/trigger rate per node (publish cycle rate).
  std::map<std::string, int> node_frame_rate;
  std::vector<std::string> unresolved;
  // Loops cut at feedback edges (GEN-11): rates across each cut are
  // loop-unresolved by design; derivation continues on the truncated DAG and
  // these notes are informational only (they do not fail DeriveRates).
  std::vector<std::string> loop_cuts;
};

// Derive steady-state publish rates from graph topology and optional publish gating.
// Report-only: does not mutate the graph or generated artifacts.
bool DeriveRates(const Graph& g, RateInfo* out, std::vector<std::string>* warnings);

// Compare declared node hz vs derived node_frame_rate; append RATE_WARN strings
// when relative deviation exceeds threshold (default 20%). Report-only.
void WarnHzDeviation(const Graph& g, const RateInfo& info, double threshold,
                     std::vector<std::string>* warnings);

// SCH-03: an exec=event node whose in-edges arrive at materially different
// rates can starve the fast input behind a slow callback (scheduling-model.md
// §7). Append a SCH03_WARN recommending a node split (first choice) or a
// per-input input_budget (fallback). Report-only: generation still succeeds.
void WarnMixedEventRates(const Graph& g, const RateInfo& info,
                         std::vector<std::string>* warnings);

// Emit rate warnings to stderr (informational; never affects exit code).
void PrintRateWarnings(const std::vector<std::string>& warnings);

// Pretty-print RateInfo as JSON to stdout.
void ExplainRatesJson(const RateInfo& info, const std::vector<std::string>& warnings);
