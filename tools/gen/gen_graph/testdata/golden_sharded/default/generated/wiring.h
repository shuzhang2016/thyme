// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "nodes/prod.h"
#include "nodes/sink__s0.h"
#include "nodes/sink__s1.h"
#include "generated/graph_types.h"
#include "generated/graph_ir.h"
#include "generated/graph_check.h"
#include "generated/mp_ports.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/app.h"
#include "thyme/runtime/health_slot.h"

#include <array>
#include <memory>

struct Pipeline {

  struct SpEventTransports {
    thyme::NotifPipe<thyme::Frame<Reading>, 8> sink__s0_c0_pipe;
    thyme::NotifPipe<thyme::Frame<Reading>, 8> sink__s1_c1_pipe;
    thyme::NotifPipe<thyme::Frame<Reading>, 8> sink__s0_c2_pipe;
    thyme::NotifPipe<thyme::Frame<Reading>, 8> sink__s1_c3_pipe;
  };
  SpEventTransports event_transports_;

  std::array<thyme::HealthSlot, kGraphHealthN> health{};


  ProducerThread prod;
  SinkS0Thread sink__s0;
  SinkS1Thread sink__s1;
  thyme::App app_;

  static std::unique_ptr<Pipeline> Create();
  bool Start();
  void RequestStop();
  void Join();
  // Node-id dispatchers (emitted from kGraphNodes): graph-agnostic access
  // to the generated thread members without naming them at the call site.
  thyme::CyclicThread* ThreadByNodeId(const char* id);
  thyme::EventThread* EventThreadByNodeId(const char* id);

 private:
  Pipeline();
};
