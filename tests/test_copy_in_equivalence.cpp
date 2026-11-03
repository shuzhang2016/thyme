// RT-17: copy path (Push/Write) vs in-place path (BeginWrite/CommitWrite)
// equivalence. Both APIs are driven over the SAME input stream and must
// produce identical slot contents, ordering, and counters — this holds
// because Push/Write are thin wrappers over the shared publish sequence.
#include "thyme/check.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

using namespace thyme;

namespace {

struct Payload {
  uint64_t a;
  uint64_t b;
  uint32_t c;
  uint32_t d;
};

uint64_t Splitmix64(uint64_t& state) {
  state += 0x9E3779B97F4A7C15ULL;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

Payload MakePayload(uint64_t& rng) {
  Payload p{};
  p.a = Splitmix64(rng);
  p.b = Splitmix64(rng);
  p.c = static_cast<uint32_t>(Splitmix64(rng));
  p.d = static_cast<uint32_t>(Splitmix64(rng));
  return p;
}

bool Equal(const Payload& x, const Payload& y) {
  return std::memcmp(&x, &y, sizeof(Payload)) == 0;
}

// Fill the slot returned by BeginWrite with plain writes — the in-place user
// pattern (no AtomicPayloadCopyIn on the writer side, ADR 0001).
template <typename Slot>
void FillInPlace(Slot* dst, const Payload& v) {
  *dst = v;
}

}  // namespace

int main() {
  char name_a[64];
  char name_b[64];
  std::snprintf(name_a, sizeof(name_a), "/c9eqa_%d", static_cast<int>(getpid()));
  std::snprintf(name_b, sizeof(name_b), "/c9eqb_%d", static_cast<int>(getpid()));

  // --- ShmSpscRing: identical pop stream, identical counters ---
  {
    ShmRegion::UnlinkName(name_a);
    ShmRegion::UnlinkName(name_b);
    ShmSpscRing<Payload, 8> prod_a;  // copy path
    ShmSpscRing<Payload, 8> prod_b;  // in-place path
    CHECK(prod_a.Create(name_a));
    CHECK(prod_b.Create(name_b));
    ShmSpscRing<Payload, 8> cons_a;
    ShmSpscRing<Payload, 8> cons_b;
    CHECK(cons_a.Open(name_a, 1000000000ULL));
    CHECK(cons_b.Open(name_b, 1000000000ULL));

    uint64_t rng = 42;
    Payload pa{};
    Payload pb{};
    // Interleave publishes and pops, including full-ring refusals, so both
    // rings see identical backpressure points.
    for (uint64_t i = 0; i < 5000; ++i) {
      const Payload v = MakePayload(rng);
      const bool ok_a = prod_a.Push(v);
      Payload* slot = prod_b.BeginWrite();
      const bool ok_b = slot != nullptr;
      CHECK(ok_a == ok_b);
      if (ok_b) {
        FillInPlace(slot, v);
        prod_b.CommitWrite();
      }
      if ((i % 3) == 0) {  // drain one item every third iteration
        const bool got_a = cons_a.TryPop(pa);
        const bool got_b = cons_b.TryPop(pb);
        CHECK(got_a == got_b);
        if (got_a) CHECK(Equal(pa, pb));
      }
    }
    // The loop above left both rings with the same backlog; drain fully.
    for (;;) {
      const bool got_a = cons_a.TryPop(pa);
      const bool got_b = cons_b.TryPop(pb);
      CHECK(got_a == got_b);
      if (!got_a) break;
      CHECK(Equal(pa, pb));
    }
    CHECK(prod_a.push_fail_count() == prod_b.push_fail_count());
    CHECK(prod_a.epoch() == prod_b.epoch());

    prod_a.Close();
    prod_b.Close();
    cons_a.Close();
    cons_b.Close();
    ShmRegion::UnlinkName(name_a);
    ShmRegion::UnlinkName(name_b);
  }

  // --- ShmSpmcRing: identical head/epoch/slot seqs, identical reader
  // streams and drop counts (overwrite included) ---
  {
    ShmRegion::UnlinkName(name_a);
    ShmRegion::UnlinkName(name_b);
    constexpr std::size_t kCap = 8;
    ShmSpmcRing<Payload, kCap> prod_a;
    ShmSpmcRing<Payload, kCap> prod_b;
    CHECK(prod_a.Create(name_a));
    CHECK(prod_b.Create(name_b));
    ShmSpmcRing<Payload, kCap> cons_a;
    ShmSpmcRing<Payload, kCap> cons_b;
    CHECK(cons_a.Open(name_a, 1000000000ULL));
    CHECK(cons_b.Open(name_b, 1000000000ULL));

    constexpr uint64_t kN = 100;  // kN > kCap: overwrite engaged
    uint64_t rng = 7;
    for (uint64_t i = 0; i < kN; ++i) {
      const Payload v = MakePayload(rng);
      prod_a.Push(v);
      Payload* slot = prod_b.BeginWrite();
      CHECK(slot != nullptr);
      FillInPlace(slot, v);
      prod_b.CommitWrite();
      // Publish counters advance in lockstep.
      CHECK(prod_a.layout()->head.load(std::memory_order_relaxed) ==
            prod_b.layout()->head.load(std::memory_order_relaxed));
      CHECK(prod_a.layout()->epoch.load(std::memory_order_relaxed) ==
            prod_b.layout()->epoch.load(std::memory_order_relaxed));
    }
    // Per-slot seqlock values are identical.
    for (std::size_t s = 0; s < kCap; ++s) {
      CHECK(prod_a.layout()->slots[s].seq.load(std::memory_order_relaxed) ==
            prod_b.layout()->slots[s].seq.load(std::memory_order_relaxed));
      // And the surviving payloads match byte-for-byte.
      CHECK(std::memcmp(&prod_a.layout()->slots[s].value,
                        &prod_b.layout()->slots[s].value,
                        sizeof(Payload)) == 0);
    }

    SpmcReader<Payload, kCap> ra;
    SpmcReader<Payload, kCap> rb;
    CHECK(ra.Bind(&cons_a, SpmcRead::Follow));
    CHECK(rb.Bind(&cons_b, SpmcRead::Follow));
    Payload pa{};
    Payload pb{};
    for (;;) {
      const bool got_a = ra.TryPop(pa);
      const bool got_b = rb.TryPop(pb);
      CHECK(got_a == got_b);
      if (!got_a) break;
      CHECK(Equal(pa, pb));
    }
    CHECK(ra.drop_count() == rb.drop_count());

    SpmcReader<Payload, kCap> la;
    SpmcReader<Payload, kCap> lb;
    CHECK(la.Bind(&cons_a, SpmcRead::Latest));
    CHECK(lb.Bind(&cons_b, SpmcRead::Latest));
    CHECK(la.ReadLatest(pa) == lb.ReadLatest(pb));
    CHECK(Equal(pa, pb));

    prod_a.Close();
    prod_b.Close();
    cons_a.Close();
    cons_b.Close();
    ShmRegion::UnlinkName(name_a);
    ShmRegion::UnlinkName(name_b);
  }

  // --- LatestSlot: identical reads and identical ReadIfNew progression ---
  {
    LatestSlot<Payload> sa;  // copy path
    LatestSlot<Payload> sb;  // in-place path
    Payload pa{};
    Payload pb{};
    uint64_t seen_a = 0;
    uint64_t seen_b = 0;
    uint64_t rng = 99;
    for (uint64_t i = 0; i < 2000; ++i) {
      const Payload v = MakePayload(rng);
      sa.Write(v);
      Payload* slot = sb.BeginWrite();
      FillInPlace(slot, v);
      sb.CommitWrite();
      CHECK(sa.HasNew(seen_a) == sb.HasNew(seen_b));
      CHECK(sa.ReadIfNew(seen_a, pa) == sb.ReadIfNew(seen_b, pb));
      CHECK(seen_a == seen_b);
      CHECK(Equal(pa, pb));
    }
    CHECK(sa.Read(pa) == sb.Read(pb));
    CHECK(Equal(pa, pb));
  }
  return 0;
}
