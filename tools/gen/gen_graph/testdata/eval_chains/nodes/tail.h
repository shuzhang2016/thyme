#pragma once
#include "generated/tail_generated.h"

class TailThread final : public TailThreadGenerated {
 public:
  using TailThreadGenerated::TailThreadGenerated;

 protected:
  void OnLoop() override;
};
