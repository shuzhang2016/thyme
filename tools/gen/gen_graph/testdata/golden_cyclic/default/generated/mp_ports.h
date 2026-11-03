// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/graph_types.h"
#include "thyme/runtime/health_slot.h"

// Shared per-node construction context: the own health slot plus the
// catalog extra buffers. Constructed once per node at the call site;
// generated bases take (const <type>Ports&, const MpContext&) instead
// of 15-26 flat pool/ring/extra arguments.
struct MpContext {
  thyme::HealthSlot* health;
};

struct TickPorts {
};
