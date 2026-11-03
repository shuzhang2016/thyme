#pragma once
#include "generated/health_reporter_generated.h"
class HealthReporterThread final : public HealthReporterThreadGenerated {
 public:
  using HealthReporterThreadGenerated::HealthReporterThreadGenerated;
 protected:
  void OnLoop() override;
};
