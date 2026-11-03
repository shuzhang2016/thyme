#pragma once

#include "generated/graph_ir.h"

#include <array>
#include <cstddef>

// HealthSlot i is owned by kGraphNodes[i] (same order as Pipeline::health in
// generated wiring). Cyclic bench rows skip event nodes when the graph defines
// kGraphNodeIsEvent (twin build with THYME_GRAPH_HAS_EVENT_NODES).

inline constexpr bool GraphCStrEq(const char* a, const char* b) {
  std::size_t i = 0;
  while (a[i] && b[i] && a[i] == b[i]) ++i;
  return a[i] == b[i];
}

inline constexpr int GraphHealthIndex(const char* id) {
  for (std::size_t i = 0; i < kGraphNodesCount; ++i) {
    if (GraphCStrEq(kGraphNodes[i].id, id)) return static_cast<int>(i);
  }
  return -1;
}

inline constexpr bool GraphNodeIsEventAt(std::size_t i) {
#if defined(THYME_GRAPH_HAS_EVENT_NODES)
  return kGraphNodeIsEvent[i];
#else
  (void)i;
  return false;
#endif
}

inline constexpr std::size_t GraphCountCyclicNodes() {
  std::size_t n = 0;
  for (std::size_t i = 0; i < kGraphNodesCount; ++i) {
    if (!GraphNodeIsEventAt(i)) ++n;
  }
  return n;
}

inline constexpr std::size_t kBenchCyclicCount = GraphCountCyclicNodes();

template <std::size_t N>
inline constexpr std::array<std::size_t, N> GraphBuildCyclicHealthIndices() {
  std::array<std::size_t, N> out{};
  std::size_t j = 0;
  for (std::size_t i = 0; i < kGraphNodesCount; ++i) {
    if (!GraphNodeIsEventAt(i)) out[j++] = i;
  }
  return out;
}

inline constexpr auto kBenchCyclicHealthIdx =
    GraphBuildCyclicHealthIndices<kBenchCyclicCount>();

static_assert(kGraphHealthN >= static_cast<int>(kGraphNodesCount));
static_assert(kBenchCyclicCount >= 1);
