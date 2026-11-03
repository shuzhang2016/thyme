// Code generated for the ports fixture graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/graph_ir.h"
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/health_slot.h"
#include "generated/mp_role_locals.h"
#include "nodes/prod.h"


// Constructs the handwritten thread for an unknown node type, using
// RoleLocals the role code assembles before dispatch.
// Returns nullptr for indices that are not unknown nodes (or if the
// graph has no unknown nodes); RunRole then stderr+exit 2.
inline thyme::CyclicThread* MakeUnknownThread(
    std::size_t node_index,
    RoleLocals& l,
    thyme::HealthSlot* health_slot) {
  (void)node_index;
  (void)l;
  (void)health_slot;
  if (node_index == 0) {
    return new ProducerThread(ProducerPorts{thyme::LocalIndexQueue<FooPool, kRingCap>(&l.foo_pool, &l.foo_ring)}, MpContext{health_slot});
  }
  return nullptr;
}
