// Code generated for the ports fixture graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/{{ID}}.cpp.
#pragma once
#include "thyme/runtime/cyclic_thread.h"
#include "generated/mp_ports.h"
#include "generated/replay_stages.h"
#include "thyme/app/record_mirror.h"
#include "thyme/app/time.h"

class ConsumerThreadGenerated : public thyme::CyclicThread {
 public:
  ConsumerThreadGenerated(const ConsumerThreadPorts& ports, const MpContext& ctx)
      : thyme::CyclicThread("{{ID}}", {{HZ}}, ctx.health), my_health_(ctx.health),
        foo_(ports.foo_pool), foo_r_(ports.foo_ring),
        record_page_(thyme::app::record_mirror::RingPage()) {}

  void Loop() override final {
    if (ReplayProducerShouldPause(name())) return;
    OnLoop();
  }

 protected:
  virtual void OnLoop() = 0;

  // Record/replay cross-cutting lives in the generated base: the page is
  // captured once at construction; Loop only carries the algorithm (OnLoop).
  template <typename T>
  void RecordTap(RecordStructId id, const T& v) {
    RecordTapMirror(record_page_, id, &v, sizeof(v), thyme::app::NowMonoNs(),
                    v.hdr.sequence);
  }

  thyme::HealthSlot* my_health_;
  FooPool* foo_;
  Ring<FooPool>* foo_r_;
  thyme::RecordMirrorRingPage* record_page_;
};
