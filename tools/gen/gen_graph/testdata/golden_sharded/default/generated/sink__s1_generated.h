// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/sink__s1.cpp.
#pragma once
#include "thyme/runtime/event_thread.h"
#include "thyme/runtime/write_section.h"
#include "generated/mp_ports.h"
#include "generated/replay_stages.h"
#include "thyme/app/record_mirror.h"
#include "thyme/app/time.h"
#include <ctime>
#include "generated/record_registry.h"

class SinkS1ThreadGenerated : public thyme::EventThread {
 public:
  SinkS1ThreadGenerated(const SinkS1Ports& ports, const MpContext& ctx)
      : thyme::EventThread("sink__s1", ctx.health, 50, 1000000ULL), my_health_(ctx.health),
        c1_(ports.c1_),
        c3_(ports.c3_) {
    // SCH-03: cap each in-edge at 2 message(s) per Dispatch cycle (split node first if rates differ).
    SetInputBudget(2u, 2u);
    // SCH-04: deadline-miss policy.
    SetOverrunPolicy(thyme::OverrunPolicy::kDegrade);
  }

  // Record/replay cross-cutting lives in the generated base: Dispatch
  // pauses the producer while replay injects, and RecordTap mirrors
  // one recordable value. The hand node overrides the On<Channel>
  // handlers with the algorithm only. hz in graph.yaml is expected
  // steady-state messages/s (stall 3/hz; MinLoops uses process min),
  // not a cyclic Loop period.
  void Dispatch() override final {
    if (ReplayProducerShouldPause(name())) {
      // Paused for replay injection: the input pipe stays non-empty,
      // so returning at once would busy-spin ThreadMain. Bounded wait.
      struct timespec pause_ts {0, 1000000};
      nanosleep(&pause_ts, nullptr);
      return;
    }
    {
      FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8>::Sample v{};
      const auto drops0 = c1_.drop_count();
      while (InputBudgetAvailable(0) && c1_.TryRead(&v) == thyme::ChannelStatus::kOk) {
        ConsumeInputBudget(0);
        NoteHandled();
        NoteInFrame(v.sequence & 0xF);
        const uint64_t perf_now = thyme::app::NowMonoNs();
        const uint64_t perf_lat = perf_now >= v.mono_ns
                                      ? perf_now - v.mono_ns
                                      : 0;
        NoteLatency(perf_lat);
        NoteLatencyEdge(static_cast<uint8_t>(RecordStructId::kC1),
                        perf_lat,
                        static_cast<uint8_t>(v.sequence & 0xF));
        const uint64_t callback_begin = NowNs();
        OnC1(v);
        NoteCallbackWork(static_cast<uint8_t>(RecordStructId::kC1), NowNs() - callback_begin);
      }
      const auto drops1 = c1_.drop_count();
      if (drops1 > drops0) my_health_->AddDrops(static_cast<uint32_t>(drops1 - drops0));
    }
    {
      FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8>::Sample v{};
      const auto drops0 = c3_.drop_count();
      while (InputBudgetAvailable(1) && c3_.TryRead(&v) == thyme::ChannelStatus::kOk) {
        ConsumeInputBudget(1);
        NoteHandled();
        NoteInFrame(v.sequence & 0xF);
        const uint64_t perf_now = thyme::app::NowMonoNs();
        const uint64_t perf_lat = perf_now >= v.mono_ns
                                      ? perf_now - v.mono_ns
                                      : 0;
        NoteLatency(perf_lat);
        NoteLatencyEdge(static_cast<uint8_t>(RecordStructId::kC3),
                        perf_lat,
                        static_cast<uint8_t>(v.sequence & 0xF));
        const uint64_t callback_begin = NowNs();
        OnC3(v);
        NoteCallbackWork(static_cast<uint8_t>(RecordStructId::kC3), NowNs() - callback_begin);
      }
      const auto drops1 = c3_.drop_count();
      if (drops1 > drops0) my_health_->AddDrops(static_cast<uint32_t>(drops1 - drops0));
    }
  }

 protected:
  virtual void OnC1(const FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8>::Sample& v) = 0;
  virtual void OnC3(const FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8>::Sample& v) = 0;

  thyme::HealthSlot* my_health_;
  FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8> c1_;
  FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8> c3_;
};
