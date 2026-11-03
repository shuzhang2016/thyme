// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/graph_ir.h"
#include "generated/mp_ports.h"
#include "generated/mp_role_locals.h"
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/health_slot.h"

// Constructs the generated thread for a known node type from RoleLocals.
// Returns nullptr for indices that are not known nodes (the role
// falls back to MakeUnknownThread); unknown types stay a compile-time
// gap only if the graph names a type nobody wired.
inline thyme::CyclicThread* MakeKnownThread(
    std::size_t node_index,
    RoleLocals& l,
    thyme::HealthSlot* health_slots) {
  (void)node_index;
  (void)l;
  (void)health_slots;
  return nullptr;
}
