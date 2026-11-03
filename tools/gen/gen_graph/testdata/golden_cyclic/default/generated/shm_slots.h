// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once
#include "generated/graph_ir.h"
#include "thyme/app/mp_shm_kind.h"

inline constexpr std::size_t kGraphShmKindCount = 0;
inline constexpr std::size_t kGraphShmSlotSizes[1] = {0};

inline constexpr uint64_t kGraphShmPayloadLayoutHashes[1] = {0};

inline constexpr thyme::app::MpShmKindSpec kGraphShmKindSpecs[1] = { {0, 0, 0, 0, 0, nullptr, nullptr} };


struct GraphCrossGate {
  GraphShmKind kind;
  uint32_t counter_slot;
};
inline constexpr std::size_t kGraphCrossGateCount = 0;
inline constexpr GraphCrossGate kGraphCrossGates[] = { {static_cast<GraphShmKind>(-1), 0} };
