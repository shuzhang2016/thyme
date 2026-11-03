#include "thyme/check.h"
#include "thyme/runtime/health_slot.h"

#include <cstdint>
#include <string>

using namespace thyme;


int main() {
  CHECK((alignof(HealthSlot) >= 64));
  HealthSlot h{};
  CHECK(h.alive_counter.load() == 0);
  h.MarkAlive(1000);
  CHECK(h.alive_counter.load() == 1);
  CHECK(h.last_finish_mono_ns.load() == 1000);
  h.MarkDeadlineMiss();
  CHECK(h.deadline_miss_cnt.load() == 1);
  CHECK(sizeof(HealthSlot) == 64);
  CHECK(offsetof(HealthSlot, memory_flags) == 40);
  CHECK(offsetof(HealthSlot, drop_cnt) == 24);
  CHECK(sizeof(HealthSlot::drop_cnt) == sizeof(uint64_t));
  h.AddDrops(UINT64_C(0x100000001));
  CHECK(h.drop_cnt.load() == UINT64_C(0x100000001));
  CHECK(h.memory_flags.load() == 0);
  CHECK(std::string(MemoryHealthStateName(0)) == "ok");
  h.SetMemoryFlags(kMemoryMlockDisabled);
  CHECK(h.memory_flags.load() == kMemoryMlockDisabled);
  CHECK(std::string(MemoryHealthStateName(h.memory_flags.load())) == "disabled");
  h.SetMemoryFlags(kMemoryMlockFailed);
  CHECK(std::string(MemoryHealthStateName(h.memory_flags.load())) == "degraded");
  return 0;
}
