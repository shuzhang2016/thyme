// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once
#include "generated/graph_types.h"

// Ring handles for wait_inputs nodes, generated from the graph's
// wait_inputs in-edges (mirrors generated/scheduling_apply.inc).
struct SchedulingRings {
};

// One templated factory serves both owners: the single-process
// Pipeline and the MP RoleLocals expose the same <prefix>_ring members.
template <typename L>
inline SchedulingRings BuildSchedulingRings(L& l) {
  (void)l;
  return {};
}
