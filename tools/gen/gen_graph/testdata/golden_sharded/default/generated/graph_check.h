// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/graph_ir.h"
#include "generated/graph_types.h"
#include "thyme/contracts/abi.h"
#include "thyme/contracts/frame_header.h"
#include "dummy/contracts/stub_types.h"

static_assert(kGraphHealthN >= 3,
              "supervisor.health_n < nodes.size(); raise health_n or remove nodes");
static_assert(kRingCap >= 2 && ((kRingCap & (kRingCap - 1u)) == 0u),
              "generated: ring_cap must be a power of two >= 2");
static_assert(kPoolN > 0,
              "generated: pool_n must be > 0");
static_assert(kGraphHealthN <= thyme::kAbiMaxThreads,
              "supervisor.health_n exceeds kAbiMaxThreads (SharedHealthPage)");

static_assert(thyme::FramePodWireBytes<thyme::Frame<Reading>> <= thyme::kAbiMaxTransportBytes,
              "generated edge contract Reading: framed wire bytes exceed the 64 KB transport limit");

static_assert(8 >= 2 && ((8 & (8 - 1u)) == 0u),
              "generated edge prod->sink__s0 (c0): Cap must be a power of two >= 2");

