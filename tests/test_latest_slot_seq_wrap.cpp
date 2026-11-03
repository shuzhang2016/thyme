// RT-03: LatestSlot seqlock sequence is 64-bit. States near the counter
// boundaries (constructed through the test-only seam, since no feasible run
// can write 2^32 or 2^64 times) keep the odd=writing/even=published protocol:
// crossing the old u32 wrap point (2^32 seq increments = 2^31 publishes) no
// longer aliases seq==0 ("never written") or earlier `seen` values, and the
// theoretical u64 wrap preserves odd/even parity with exactly one aliased
// generation (documented in latest_slot.h).
#include "thyme/check.h"
#include "thyme/runtime/latest_slot.h"

#include <cstdint>

using thyme::LatestSlot;
using thyme::detail::LatestSlotTestAccess;

namespace {

struct Pod {
  uint32_t v;
};

// A second instantiation crossing the 64-byte cell boundary so the layout
// lock covers the stride formula, not just the small-Pod case.
struct Wide {
  uint8_t b[100];
};

}  // namespace

int main() {
  // Compile-time layout lock (ABI): sizeof forces a complete instantiation of
  // the seam, running its layout static_asserts for both cell sizes.
  (void)sizeof(LatestSlotTestAccess<Pod>);
  (void)sizeof(LatestSlotTestAccess<Wide>);

  // ---- Cross the old u32 wrap boundary (2^32): no aliasing anymore. ----
  {
    LatestSlot<Pod> s;
    Pod out{0};
    s.Write(Pod{1});  // seq 0 -> 2, published_idx 0 -> 1
    CHECK(LatestSlotTestAccess<Pod>::Seq(&s) == 2);
    CHECK(LatestSlotTestAccess<Pod>::PublishedIdx(&s) == 1);

    uint64_t seen = 0;
    CHECK(s.ReadIfNew(seen, out) && out.v == 1 && seen == 2);

    // Jump to the last even state a u32 counter could hold.
    LatestSlotTestAccess<Pod>::SetSeq(&s, 0xFFFFFFFEu);
    CHECK(s.Read(out) && out.v == 1);
    CHECK(s.HasNew(seen));
    CHECK(s.ReadIfNew(seen, out) && out.v == 1 && seen == 0xFFFFFFFEu);

    // The next publish would have wrapped a u32 seq to 0 ("never written").
    s.Write(Pod{2});
    CHECK(LatestSlotTestAccess<Pod>::Seq(&s) == 0x100000000ULL);
    CHECK(LatestSlotTestAccess<Pod>::PublishedIdx(&s) == 0);
    CHECK(s.Read(out) && out.v == 2);
    CHECK(s.HasNew(seen));
    CHECK(s.ReadIfNew(seen, out) && out.v == 2 && seen == 0x100000000ULL);
    CHECK(!s.HasNew(seen));
    CHECK(!s.ReadIfNew(seen, out));

    // Odd/even protocol above 2^32: a mid-write slot is invisible, and
    // AbortWrite closes the section without publishing.
    Pod* p = s.BeginWrite();
    CHECK((LatestSlotTestAccess<Pod>::Seq(&s) & 1u) == 1u);
    p->v = 3;
    CHECK(!s.Read(out));  // retries exhausted while the section is open
    CHECK(out.v == 2);
    s.AbortWrite();
    CHECK(LatestSlotTestAccess<Pod>::Seq(&s) == 0x100000002ULL);
    CHECK(s.Read(out) && out.v == 2);  // old value still current
    CHECK(s.ReadIfNew(seen, out) && out.v == 2 && seen == 0x100000002ULL);
  }

  // ---- Theoretical u64 wrap: odd/even survives, one aliased generation. ----
  {
    LatestSlot<Pod> s;
    Pod out{0};
    s.Write(Pod{7});
    uint64_t seen = 0;
    CHECK(s.ReadIfNew(seen, out) && out.v == 7 && seen == 2);

    // Last even u64 state: BeginWrite lands on UINT64_MAX (odd), AbortWrite
    // wraps to 0 (even). Parity survives the wrap; seq==0 then aliases
    // "never written" for exactly this one generation.
    LatestSlotTestAccess<Pod>::SetSeq(&s, 0xFFFFFFFFFFFFFFFEULL);
    CHECK(s.Read(out) && out.v == 7);
    Pod* p = s.BeginWrite();
    p->v = 8;
    CHECK(LatestSlotTestAccess<Pod>::Seq(&s) == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(!s.Read(out));  // odd: writer in progress
    s.AbortWrite();
    CHECK(LatestSlotTestAccess<Pod>::Seq(&s) == 0);
    CHECK(!s.Read(out));            // aliased "never written" generation
    CHECK(!s.HasNew(seen));
    CHECK(!s.ReadIfNew(seen, out));

    // The slot is fully usable from the next publish on. The stale `seen`
    // aliases the recycled seq value (same one-generation wrinkle as the
    // seq==0 alias above); a fresh cursor reads normally.
    s.Write(Pod{9});
    CHECK(LatestSlotTestAccess<Pod>::Seq(&s) == 2);
    CHECK(s.Read(out) && out.v == 9);
    CHECK(!s.ReadIfNew(seen, out));
    uint64_t fresh = 0;
    CHECK(s.ReadIfNew(fresh, out) && out.v == 9 && fresh == 2);

    // CommitWrite across the wrap: the committed value is published
    // (published_idx_ flips) but unreadable for the aliased generation, then
    // everything recovers.
    LatestSlotTestAccess<Pod>::SetSeq(&s, 0xFFFFFFFFFFFFFFFEULL);
    const uint32_t idx_before = LatestSlotTestAccess<Pod>::PublishedIdx(&s);
    p = s.BeginWrite();
    p->v = 10;
    s.CommitWrite();
    CHECK(LatestSlotTestAccess<Pod>::Seq(&s) == 0);
    CHECK(LatestSlotTestAccess<Pod>::PublishedIdx(&s) == (idx_before ^ 1u));
    CHECK(!s.Read(out));
    s.Write(Pod{11});
    CHECK(s.Read(out) && out.v == 11);
  }

  return 0;
}
