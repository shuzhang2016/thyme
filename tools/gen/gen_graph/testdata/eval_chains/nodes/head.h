#pragma once
#include "generated/head_generated.h"

class HeadThread final : public HeadThreadGenerated {
 public:
  using HeadThreadGenerated::HeadThreadGenerated;

 protected:
  void OnLoop() override;

 private:
  uint32_t seq_ = 0;
};
