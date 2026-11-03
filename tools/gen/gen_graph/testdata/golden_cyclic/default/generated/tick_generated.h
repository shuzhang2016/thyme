// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/tick.cpp.
#pragma once
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/write_section.h"
#include "generated/mp_ports.h"

class TickThreadGenerated : public thyme::CyclicThread {
 public:
  TickThreadGenerated(const TickPorts& ports, const MpContext& ctx)
      : thyme::CyclicThread("tick", 10, ctx.health), my_health_(ctx.health) {}

  // Record/replay cross-cutting lives in the generated base: Loop
  // pauses the producer while replay injects, and RecordTap mirrors
  // one recordable value. The hand node overrides OnLoop with the
  // algorithm only.
  void Loop() override final {
    OnLoop();
  }

 protected:
  virtual void OnLoop() = 0;

  thyme::HealthSlot* my_health_;
};
