// Code generated for the ports fixture graph. DO NOT EDIT.
// gen_graph 3.11.1 (output format 3)
// Hand logic belongs in src/nodes/health_reporter.cpp.
#pragma once
#include "thyme/runtime/cyclic_thread.h"

class HealthReporterThreadGenerated : public thyme::CyclicThread {
 public:
  HealthReporterThreadGenerated(thyme::HealthSlot* worker_slots,
                                uint32_t worker_count, const uint32_t* worker_hz,
                                thyme::HealthSlot* own)
      : thyme::CyclicThread("health_reporter", 10, own), my_health_(own),
        slots_(worker_slots), count_(worker_count), hz_(worker_hz) {}

  void Loop() override final { OnLoop(); }

 protected:
  virtual void OnLoop() = 0;

  thyme::HealthSlot* my_health_;
  thyme::HealthSlot* slots_;
  uint32_t count_;
  const uint32_t* hz_;
};
