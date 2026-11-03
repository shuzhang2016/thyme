// Code generated for the generated graph. DO NOT EDIT.
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
  ProducerThreadGenerated(const ProducerThreadPorts& ports, const MpContext& ctx)
      : thyme::CyclicThread("prod", 50, ctx.health),
        my_health_(ctx.health),
        c0_(ports.c0_),
        c1_(ports.c1_),
        c2_(ports.c2_),
        c3_(ports.c3_),
        record_page_(thyme::app::record_mirror::RingPage()) {}

  void Loop() override final {
    if (ReplayProducerShouldPause(name())) return;
    TickIn in{};
    TickOut out{};
    auto out_c0 = c0_.BeginWrite();
    out.c0 = out_c0.payload;
    out.ready_c0 = out_c0.status == thyme::ChannelStatus::kOk && out.c0 != nullptr;
    auto out_c1 = c1_.BeginWrite();
    out.c1 = out_c1.payload;
    out.ready_c1 = out_c1.status == thyme::ChannelStatus::kOk && out.c1 != nullptr;
    auto out_c2 = c2_.BeginWrite();
    out.c2 = out_c2.payload;
    out.ready_c2 = out_c2.status == thyme::ChannelStatus::kOk && out.c2 != nullptr;
    auto out_c3 = c3_.BeginWrite();
    out.c3 = out_c3.payload;
    out.ready_c3 = out_c3.status == thyme::ChannelStatus::kOk && out.c3 != nullptr;
    const bool publish = OnTick(in, &out);
    if (publish && out.ready_c0) {
      if (c0_.CommitWrite(&out_c0) == thyme::ChannelStatus::kOk) {
        RecordTap(RecordStructId::kC0, *out_c0.frame_ptr());
      }
    } else {
      c0_.AbortWrite(&out_c0);
    }
    if (publish && out.ready_c1) {
      if (c1_.CommitWrite(&out_c1) == thyme::ChannelStatus::kOk) {
        RecordTap(RecordStructId::kC1, *out_c1.frame_ptr());
      }
    } else {
      c1_.AbortWrite(&out_c1);
    }
    if (publish && out.ready_c2) {
      if (c2_.CommitWrite(&out_c2) == thyme::ChannelStatus::kOk) {
        RecordTap(RecordStructId::kC2, *out_c2.frame_ptr());
      }
    } else {
      c2_.AbortWrite(&out_c2);
    }
    if (publish && out.ready_c3) {
      if (c3_.CommitWrite(&out_c3) == thyme::ChannelStatus::kOk) {
        RecordTap(RecordStructId::kC3, *out_c3.frame_ptr());
      }
    } else {
      c3_.AbortWrite(&out_c3);
    }
  }

  struct TickIn {
  };

  struct TickOut {
    Reading* c0 = nullptr;
    bool ready_c0 = false;
    Reading* c1 = nullptr;
    bool ready_c1 = false;
    Reading* c2 = nullptr;
    bool ready_c2 = false;
    Reading* c3 = nullptr;
    bool ready_c3 = false;
  };

 protected:
  virtual bool OnTick(const TickIn& in, TickOut* out) = 0;

  // Record/replay cross-cutting lives in the generated base: the page is
  // captured once at construction; Loop only carries the algorithm callback.
  template <typename T>
  void RecordTap(RecordStructId id, const T& v) {
    NoteOutFrame(v.hdr.sequence & 0xF);
    RecordTapMirror(record_page_, id, &v, sizeof(v),
                    thyme::app::NowMonoNs(), v.hdr.sequence);
  }

  // One typed publish shape for Frame and PinnedSlot. The status keeps
  // backpressure (reject), overwrite, gate and pin failures visible to the
  // caller; [[nodiscard]] makes ignoring a failed command a warning.
  template <typename Channel, typename T>
  [[nodiscard]] thyme::ChannelStatus Publish(Channel& channel,
                                            const T& value) {
    auto out = channel.BeginWrite();
    if (out.status != thyme::ChannelStatus::kOk) return out.status;
    if (out.payload == nullptr) {
      channel.AbortWrite(&out);
      return thyme::ChannelStatus::kBadLayout;
    }
    *out.payload = value;
    return channel.CommitWrite(&out);
  }

  thyme::HealthSlot* my_health_;
  FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8> c0_;
  FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8> c1_;
  FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8> c2_;
  FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8> c3_;
  thyme::RecordMirrorRingPage* record_page_;
};
