#pragma once
#include "generated/tick_generated.h"
class TickThread final : public TickThreadGenerated {
 public:
  using TickThreadGenerated::TickThreadGenerated;
 protected:
  void OnLoop() override;
};
