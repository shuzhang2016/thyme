#include "thyme/check.h"
#include "thyme/runtime/latest_slot.h"
#include <atomic>
#include <cstdint>
#include <type_traits>

using namespace thyme;


struct Pod { uint32_t v; };

// Contract guards mirrored by static_assert in latest_slot.h.
static_assert(std::is_trivially_copyable<Pod>::value, "test Pod must be POD");

int main() {
  CHECK(std::atomic<uint64_t>::is_always_lock_free);
  LatestSlot<Pod> s;
  Pod out{0};
  CHECK(!s.Read(out));
  uint64_t seen = 0;
  CHECK(!s.ReadIfNew(seen, out));
  CHECK(!s.HasNew(seen));
  s.Write(Pod{7});
  CHECK(s.HasNew(seen));
  CHECK(s.Read(out) && out.v == 7);
  CHECK(s.ReadIfNew(seen, out) && out.v == 7 && seen == 2);
  CHECK(!s.HasNew(seen));
  CHECK(!s.ReadIfNew(seen, out));
  s.Write(Pod{8});
  CHECK(s.HasNew(seen));
  CHECK(s.ReadIfNew(seen, out) && out.v == 8 && seen == 4);
  s.Write(Pod{9});
  s.Write(Pod{10});
  CHECK(s.ReadIfNew(seen, out) && out.v == 10 && seen == 8);
  return 0;
}
