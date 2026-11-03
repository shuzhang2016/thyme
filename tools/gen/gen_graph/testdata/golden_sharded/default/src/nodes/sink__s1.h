#pragma once
#include "generated/sink__s1_generated.h"
class SinkS1Thread final : public SinkS1ThreadGenerated {
 public:
  using SinkS1ThreadGenerated::SinkS1ThreadGenerated;
 protected:
  void OnC1(const FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8>::Sample& v) override;
  void OnC3(const FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8>::Sample& v) override;
};
