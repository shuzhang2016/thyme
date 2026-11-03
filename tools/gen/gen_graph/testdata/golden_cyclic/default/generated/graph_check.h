// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/graph_ir.h"
#include "generated/graph_types.h"
#include "thyme/contracts/abi.h"

static_assert(kGraphHealthN >= 1,
              "supervisor.health_n < nodes.size(); raise health_n or remove nodes");
static_assert(kRingCap >= 2 && ((kRingCap & (kRingCap - 1u)) == 0u),
              "generated: ring_cap must be a power of two >= 2");
static_assert(kPoolN > 0,
              "generated: pool_n must be > 0");
static_assert(kGraphHealthN <= thyme::kAbiMaxThreads,
              "supervisor.health_n exceeds kAbiMaxThreads (SharedHealthPage)");

