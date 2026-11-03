#pragma once
#include "generated/sink__s0_generated.h"
class SinkS0Thread final : public SinkS0ThreadGenerated {
 public:
  using SinkS0ThreadGenerated::SinkS0ThreadGenerated;
 protected:
  void OnC0(const FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8>::Sample& v) override;
  void OnC2(const FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8>::Sample& v) override;
};
