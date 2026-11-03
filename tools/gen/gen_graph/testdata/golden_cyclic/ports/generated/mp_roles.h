// Code generated for the ports fixture graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/graph_ir.h"
#include "generated/mp_ports.h"
#include "generated/mp_role_locals.h"
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/health_slot.h"
#include "nodes/cons.h"
#include "nodes/health_reporter.h"

// Worker frequencies for the noworker (health_reporter) node, in yaml order
// excluding noworker types. Borrowed by the health_reporter thread (inline
// constexpr: one copy per TU).
inline constexpr uint32_t kMpWorkerHz[] = {10, 10};
inline constexpr std::size_t kMpWorkerCount = 2;

// Constructs the generated thread for a known node type from RoleLocals.
// Returns nullptr for indices that are not known nodes (the role
// falls back to MakeUnknownThread); unknown types stay a compile-time
// gap only if the graph names a type nobody wired.
inline thyme::CyclicThread* MakeKnownThread(
    std::size_t node_index,
    RoleLocals& l,
    thyme::HealthSlot* health_slots) {
  (void)l;
  switch (node_index) {
    case 1: return new ConsumerThread(ConsumerThreadPorts{thyme::LocalIndexQueue<FooPool, kRingCap>(&l.foo_pool, &l.foo_ring)}, MpContext{&health_slots[1]});
    case 2: return new HealthReporterThread(health_slots, kMpWorkerCount, kMpWorkerHz, &health_slots[kGraphHealthN - 1]);
    default: return nullptr;
  }
}
