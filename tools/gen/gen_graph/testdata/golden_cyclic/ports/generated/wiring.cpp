// Code generated for the ports fixture graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#include "generated/wiring.h"

#include "support/scheduling_setup.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Worker frequencies for the 2 worker threads. Outlives
// the health reporter thread that borrows it (file-scope static const).
static const uint32_t kWorkerHz[2] = {10, 10};

}  // namespace

std::unique_ptr<Pipeline> Pipeline::Create() {
  auto pipeline = std::unique_ptr<Pipeline>(new Pipeline());
  return pipeline;
}

Pipeline::Pipeline()
    : prod(ProducerPorts{thyme::LocalIndexQueue<FooPool, kRingCap>(&foo_pool, &foo_ring)}, MpContext{&health[0]}),
      consumer(ConsumerThreadPorts{thyme::LocalIndexQueue<FooPool, kRingCap>(&foo_pool, &foo_ring)}, MpContext{&health[1]}),
      reporter(health.data(), 2, kWorkerHz, &health[kGraphHealthN - 1]) {
  if (!app_.Add(&prod)) {
    std::fprintf(stderr, "Pipeline: app_.Add failed for prod\n");
    std::exit(1);
  }
  if (!app_.Add(&consumer)) {
    std::fprintf(stderr, "Pipeline: app_.Add failed for cons\n");
    std::exit(1);
  }
  if (!app_.Add(&reporter)) {
    std::fprintf(stderr, "Pipeline: app_.Add failed for health_reporter\n");
    std::exit(1);
  }
  {
    thyme::ThreadPlacement p{};
    p.priority = 89;
    prod.SetPlacement(p);
  }
  {
    thyme::ThreadPlacement p{};
    p.priority = 89;
    consumer.SetPlacement(p);
  }
  {
    thyme::ThreadPlacement p{};
    p.priority = 89;
    reporter.SetPlacement(p);
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
  if (std::strcmp(id, "cons") == 0) return &consumer;
  if (std::strcmp(id, "health_reporter") == 0) return &reporter;
  return nullptr;
}
