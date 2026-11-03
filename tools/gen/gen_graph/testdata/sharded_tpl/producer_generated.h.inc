// Code generated for the event fixture. DO NOT EDIT.
// Hand logic belongs in src/nodes/{{ID}}.cpp.
#pragma once
#include "thyme/runtime/cyclic_thread.h"
#include "generated/mp_ports.h"
#include "generated/replay_stages.h"
#include "thyme/app/record_mirror.h"
#include "thyme/app/time.h"

class ProducerThreadGenerated : public thyme::CyclicThread {
 public:
  ProducerThreadGenerated(const ProducerThreadPorts& ports,
                          const MpContext& ctx)
      : thyme::CyclicThread("{{ID}}", {{HZ}}, ctx.health),
        my_health_(ctx.health),
        readings_(ports.readings_),
        record_page_(thyme::app::record_mirror::RingPage()) {}

  void Loop() override final {
    if (ReplayProducerShouldPause(name())) return;
    OnLoop();
  }

 protected:
  virtual void OnLoop() = 0;

  template <typename T>
  void RecordTap(RecordStructId id, const T& v) {
    RecordTapMirror(record_page_, id, &v, sizeof(v), thyme::app::NowMonoNs(),
                    v.hdr.sequence);
  }

  // Notify (in-proc): credit drop immediately on full pipe.
  // RT-16: in-place publish; a full pipe hands back nullptr (drop).
  template <typename Pipe, typename T>
  void PublishAndFork(Pipe& pipe, const T& v) {
    T* slot = pipe.BeginWrite();
    if (slot == nullptr) {
      if (pipe.uses_notif_pipe()) my_health_->AddDrops(1);
      return;
    }
    *slot = v;
    pipe.CommitWrite();
  }

  thyme::HealthSlot* my_health_;
  EventPipeRef<thyme::Frame<Reading>, 8> readings_;
  thyme::RecordMirrorRingPage* record_page_;
};
