// Code generated for the ports fixture graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/prod.cpp.
#pragma once
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/write_section.h"
#include "generated/mp_ports.h"
#include "generated/replay_stages.h"
#include "thyme/app/record_mirror.h"
#include "thyme/app/time.h"

class ProducerThreadGenerated : public thyme::CyclicThread {
 public:
  ProducerThreadGenerated(const ProducerPorts& ports, const MpContext& ctx)
      : thyme::CyclicThread("prod", 10, ctx.health), my_health_(ctx.health),
        payload_(ports.payload_),
        record_page_(thyme::app::record_mirror::RingPage()) {}

  void Loop() override final {
    if (ReplayProducerShouldPause(name())) return;
    TickIn in{};
    TickOut out{};
    auto out_payload = payload_.BeginWrite();
    out.payload = out_payload.slot;
    out.ready_payload = out_payload.ok();
    const bool publish = OnTick(in, &out);
    if (publish && out.ready_payload) {
      if (payload_.CommitWrite(out_payload)) {
        RecordTap(RecordStructId::kPayload, *out.payload);
      }
    } else {
      payload_.AbortWrite(out_payload);
    }
  }

  struct TickIn {
  };

  struct TickOut {
    thyme::Frame<Foo>* payload = nullptr;
    bool ready_payload = false;
  };

 protected:
  virtual bool OnTick(const TickIn& in, TickOut* out) = 0;

  template <typename T>
  void RecordTap(RecordStructId id, const T& v) {
    NoteOutFrame(v.hdr.sequence & 0xF);
    RecordTapMirror(record_page_, id, &v, sizeof(v),
                    thyme::app::NowMonoNs(), v.hdr.sequence);
  }

  thyme::HealthSlot* my_health_;
  thyme::RecordMirrorRingPage* record_page_;
  thyme::LocalIndexQueue<FooPool, kRingCap> payload_;
};
