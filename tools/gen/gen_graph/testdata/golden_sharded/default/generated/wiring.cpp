// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#include "generated/wiring.h"

#include "support/record_mirror_ctx.h"

#include "support/scheduling_setup.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Worker frequencies for the 3 worker threads. Outlives
// the health reporter thread that borrows it (file-scope static const).
static const uint32_t kWorkerHz[3] = {50, 50, 50};

}  // namespace

std::unique_ptr<Pipeline> Pipeline::Create() {
  auto pipeline = std::unique_ptr<Pipeline>(new Pipeline());
  return pipeline;
}

Pipeline::Pipeline()
    : prod(ProducerThreadPorts{&event_transports_.sink__s0_c0_pipe, &event_transports_.sink__s1_c1_pipe, &event_transports_.sink__s0_c2_pipe, &event_transports_.sink__s1_c3_pipe}, MpContext{&health[0]}),
      sink__s0(SinkS0Ports{&event_transports_.sink__s0_c0_pipe, &event_transports_.sink__s0_c2_pipe}, MpContext{&health[1]}),
      sink__s1(SinkS1Ports{&event_transports_.sink__s1_c1_pipe, &event_transports_.sink__s1_c3_pipe}, MpContext{&health[2]}) {
  if (auto* record_page = record_mirror::Page(); record_page != nullptr) {
    event_transports_.sink__s0_c0_pipe.SetRecordTap(record_page, static_cast<uint16_t>(RecordStructId::kC0));
    event_transports_.sink__s1_c1_pipe.SetRecordTap(record_page, static_cast<uint16_t>(RecordStructId::kC1));
    event_transports_.sink__s0_c2_pipe.SetRecordTap(record_page, static_cast<uint16_t>(RecordStructId::kC2));
    event_transports_.sink__s1_c3_pipe.SetRecordTap(record_page, static_cast<uint16_t>(RecordStructId::kC3));
  }
  if (!app_.Add(&sink__s0)) {
    std::fprintf(stderr, "Pipeline: app_.Add failed for sink__s0\n");
    std::exit(1);
  }
  if (!app_.Add(&sink__s1)) {
    std::fprintf(stderr, "Pipeline: app_.Add failed for sink__s1\n");
    std::exit(1);
  }
  if (!app_.Add(&prod)) {
    std::fprintf(stderr, "Pipeline: app_.Add failed for prod\n");
    std::exit(1);
  }
  {
    thyme::Waitable* ws[] = {&event_transports_.sink__s0_c0_pipe, &event_transports_.sink__s0_c2_pipe};
    sink__s0.SetWaitInputs(ws, 2);
  }
  {
    thyme::ThreadPlacement p{};
    p.ncpus = 1;
    p.cpus[0] = 0;
    p.priority = 40;
    sink__s0.SetPlacement(p);
  }
  {
    thyme::Waitable* ws[] = {&event_transports_.sink__s1_c1_pipe, &event_transports_.sink__s1_c3_pipe};
    sink__s1.SetWaitInputs(ws, 2);
  }
  {
    thyme::ThreadPlacement p{};
    p.ncpus = 1;
    p.cpus[0] = 0;
    p.priority = 40;
    sink__s1.SetPlacement(p);
  }
  {
    thyme::ThreadPlacement p{};
    p.ncpus = 1;
    p.cpus[0] = 0;
    p.priority = 89;
    prod.SetPlacement(p);
  }
}

bool Pipeline::Start() {
  thyme::App::InstallStopSignals();
  if (!app_.Start()) {
    std::fprintf(stderr, "Pipeline::Start: app_.Start() failed\n");
    std::exit(1);
  }
  return true;
}

void Pipeline::RequestStop() {
  app_.RequestStop();
}

void Pipeline::Join() {
  app_.Join();
}

thyme::CyclicThread* Pipeline::ThreadByNodeId(const char* id) {
  if (std::strcmp(id, "prod") == 0) return &prod;
  return nullptr;
}

thyme::EventThread* Pipeline::EventThreadByNodeId(const char* id) {
  if (std::strcmp(id, "sink__s0") == 0) return &sink__s0;
  if (std::strcmp(id, "sink__s1") == 0) return &sink__s1;
  return nullptr;
}
