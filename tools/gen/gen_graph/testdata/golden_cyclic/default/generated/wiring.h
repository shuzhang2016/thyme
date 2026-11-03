// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "nodes/tick.h"
#include "generated/graph_types.h"
#include "generated/graph_ir.h"
#include "generated/graph_check.h"
#include "generated/mp_ports.h"
#include "thyme/runtime/app.h"
#include "thyme/runtime/health_slot.h"

#include <array>
#include <memory>

struct Pipeline {

  std::array<thyme::HealthSlot, kGraphHealthN> health{};


  TickThread tick;
  thyme::App app_;

  static std::unique_ptr<Pipeline> Create();
  bool Start();
  void RequestStop();
  void Join();
  // Node-id dispatchers (emitted from kGraphNodes): graph-agnostic access
  // to the generated thread members without naming them at the call site.
  thyme::CyclicThread* ThreadByNodeId(const char* id);

 private:
  Pipeline();
};
