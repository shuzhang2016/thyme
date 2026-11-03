#pragma once
#include "generated/prod_generated.h"
class ProducerThread final : public ProducerThreadGenerated {
 public:
  using ProducerThreadGenerated::ProducerThreadGenerated;
 protected:
  bool OnTick(const TickIn& in, TickOut* out) override;
};
