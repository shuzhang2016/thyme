// Code generated for the ports fixture graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/cons.cpp.
#pragma once
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/write_section.h"
#include "generated/mp_ports.h"
#include "generated/record_registry.h"

class ConsumerThreadGenerated : public thyme::CyclicThread {
 public:
  ConsumerThreadGenerated(const ConsumerThreadPorts& ports, const MpContext& ctx)
      : thyme::CyclicThread("cons", 10, ctx.health),
        my_health_(ctx.health),
        foo_(ports.foo_queue) {}

  void Loop() override final {
    TickIn in{};
    TickOut out{};
    decltype(foo_)::Read in_payload;
    uint32_t skipped_payload = 0;
    for (uint32_t budget = 0; budget < kRingCap; ++budget) {
      decltype(foo_)::Read next;
      if (!foo_.TryRead(next)) break;
      if (in_payload.slot != nullptr) {
        foo_.Release(in_payload);
        ++skipped_payload;
      }
      in_payload = next;
    }
    in.payload = in_payload.slot;
    in.skipped_payload = skipped_payload;
    const bool publish = OnTick(in, &out);
    if (in_payload.slot != nullptr) foo_.Release(in_payload);
  }

  struct TickIn {
    const thyme::Frame<Foo>* payload = nullptr;
    uint32_t skipped_payload = 0;
  };

  struct TickOut {
  };

 protected:
  virtual bool OnTick(const TickIn& in, TickOut* out) = 0;

  thyme::HealthSlot* my_health_;
  thyme::LocalIndexQueue<FooPool, kRingCap> foo_;
};
