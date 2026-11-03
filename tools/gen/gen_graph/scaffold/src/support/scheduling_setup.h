#pragma once

#include "generated/graph_types.h"
#include "generated/scheduling_rings.h"
#include "thyme/runtime/cyclic_thread.h"

#include <cstring>

// Apply thyme::ThreadPlacement and wait_inputs for node_id, matching
// generated/wiring.cpp. The SchedulingRings aggregate is generated from the
// graph's wait_inputs in-edges (see generated/scheduling_rings.h).
void ApplyNodeScheduling(const char* node_id, thyme::CyclicThread* t,
                         const SchedulingRings& rings);
