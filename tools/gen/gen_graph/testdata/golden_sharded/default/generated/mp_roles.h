// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/graph_ir.h"
#include "generated/mp_ports.h"
#include "generated/mp_role_locals.h"
#include "generated/mp_unknown.h"
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/health_slot.h"
#include "nodes/prod.h"

// Constructs the generated thread for a known node type from RoleLocals.
// Returns nullptr for indices that are not known nodes (the role
// falls back to MakeUnknownThread); unknown types stay a compile-time
// gap only if the graph names a type nobody wired.
inline thyme::CyclicThread* MakeKnownThread(
    std::size_t node_index,
    RoleLocals& l,
    EventTransports& ev,
    thyme::HealthSlot* health_slots) {
  (void)ev;
  (void)l;
  switch (node_index) {
    case 0: return new ProducerThread(ProducerThreadPorts{&ev.sink__s0_c0_pipe, &ev.sink__s1_c1_pipe, &ev.sink__s0_c2_pipe, &ev.sink__s1_c3_pipe}, MpContext{&health_slots[0]});
    default: return nullptr;
  }
}
