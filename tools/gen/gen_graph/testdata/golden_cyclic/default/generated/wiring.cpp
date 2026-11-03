// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#include "generated/wiring.h"

#include "support/scheduling_setup.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Worker frequencies for the 1 worker threads. Outlives
// the health reporter thread that borrows it (file-scope static const).
static const uint32_t kWorkerHz[1] = {10};

}  // namespace

std::unique_ptr<Pipeline> Pipeline::Create() {
  auto pipeline = std::unique_ptr<Pipeline>(new Pipeline());
  return pipeline;
}

Pipeline::Pipeline()
    : tick(TickPorts{}, MpContext{&health[0]}) {
  if (!app_.Add(&tick)) {
    std::fprintf(stderr, "Pipeline: app_.Add failed for tick\n");
    std::exit(1);
  }
  {
    thyme::ThreadPlacement p{};
    p.priority = 89;
    tick.SetPlacement(p);
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
  if (std::strcmp(id, "tick") == 0) return &tick;
  return nullptr;
}
