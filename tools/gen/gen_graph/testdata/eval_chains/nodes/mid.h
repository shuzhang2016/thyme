#pragma once
#include "generated/mid_generated.h"

class MidThread final : public MidThreadGenerated {
 public:
  using MidThreadGenerated::MidThreadGenerated;

 protected:
  void OnLoop() override;
};
