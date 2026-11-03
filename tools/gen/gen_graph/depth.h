#pragma once

#include "graph.h"
#include "rate.h"

#include <string>
#include <vector>

// Smallest power of two >= n (n >= 1).
int NextPow2(int n);

// SuggestDepth = NextPow2(max(2, ceil(k * producer_hz / consumer_hz))).
int SuggestDepth(int producer_hz, int consumer_hz, int k);

// Append DEPTH_WARN when explicit depth on queue/notify edges is below suggestion.
// Report-only; never affects exit code.
void WarnDepthBelowSuggest(const Graph& g, const RateInfo& info,
                           std::vector<std::string>* warnings);
