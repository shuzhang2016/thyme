#include "support/scheduling_setup.h"

void ApplyNodeScheduling(const char* node_id, thyme::CyclicThread* t,
                         const SchedulingRings& rings) {
  if (t == nullptr || node_id == nullptr) return;
#include "generated/scheduling_apply.inc"
}
