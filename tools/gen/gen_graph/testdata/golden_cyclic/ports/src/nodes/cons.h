#pragma once
#include "generated/cons_generated.h"
class ConsumerThread final : public ConsumerThreadGenerated {
 public:
  using ConsumerThreadGenerated::ConsumerThreadGenerated;
 protected:
  bool OnTick(const TickIn& in, TickOut* out) override;
};
