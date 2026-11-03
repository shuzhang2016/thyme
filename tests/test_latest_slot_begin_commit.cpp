// RT-15: LatestSlot BeginWrite/CommitWrite in-place publish path — the
// seqlock protocol (odd during write, even + flipped published_idx_ after
// commit) is exactly Write's, Read/ReadIfNew semantics are unchanged, and a
// reader never returns a half-written value when the writer does not commit.
#include "thyme/check.h"
#include "thyme/runtime/latest_slot.h"

#include <atomic>
#include <cstdint>
#include <thread>

using namespace thyme;

namespace {

struct Pod {
  uint32_t v;
};

// Internally consistent payload for the half-written detection: every word
// carries the generation, so a torn read shows mixed generations.
struct Wide {
  uint64_t w[8];
};

Wide MakeWide(uint64_t gen) {
  Wide p{};
  for (uint64_t& word : p.w) word = gen;
  return p;
}

bool Consistent(const Wide& p) {
  for (const uint64_t word : p.w) {
    if (word != p.w[0]) return false;
  }
  return true;
}

}  // namespace

int main() {
  // In-place publish: Read and ReadIfNew see exactly what Write would
  // publish, with the same seen-sequence progression (seq advances by 2 per
  // committed write).
  {
    LatestSlot<Pod> s;
    Pod out{0};
    CHECK(!s.Read(out));
    uint64_t seen = 0;
    CHECK(!s.ReadIfNew(seen, out));

    Pod* p = s.BeginWrite();
    p->v = 7;
    s.CommitWrite();
    CHECK(s.Read(out) && out.v == 7);
    CHECK(s.ReadIfNew(seen, out) && out.v == 7 && seen == 2);
    CHECK(!s.ReadIfNew(seen, out));
    CHECK(!s.HasNew(seen));

    p = s.BeginWrite();
    p->v = 8;
    s.CommitWrite();
    CHECK(s.ReadIfNew(seen, out) && out.v == 8 && seen == 4);

    s.Write(Pod{9});  // interleave with the copy path
    CHECK(s.ReadIfNew(seen, out) && out.v == 9 && seen == 6);
  }

  // No CommitWrite: the seqlock stays open (seq odd), so readers retry and
  // fail instead of returning the half-filled slot; the previously published
  // value is untouched once the section is closed via AbortWrite.
  {
    LatestSlot<Pod> s;
    Pod out{0};
    s.Write(Pod{1});
    CHECK(s.Read(out) && out.v == 1);

    Pod* p = s.BeginWrite();
    p->v = 0xDEAD;
    // No commit: the reader must never observe 0xDEAD. Read retries while
    // seq is odd and returns false.
    out.v = 0;
    CHECK(!s.Read(out));
    CHECK(out.v == 0);
    uint64_t seen = 2;  // seq after the Write of 1
    CHECK(!s.ReadIfNew(seen, out));

    // AbortWrite closes the section without publishing: the old value is
    // still current. seq advanced, so ReadIfNew may re-report the old value
    // once — the documented abort wrinkle.
    s.AbortWrite();
    CHECK(s.Read(out) && out.v == 1);
    CHECK(s.ReadIfNew(seen, out) && out.v == 1);
    CHECK(!s.ReadIfNew(seen, out));

    // The slot is fully usable afterwards.
    p = s.BeginWrite();
    p->v = 2;
    s.CommitWrite();
    CHECK(s.Read(out) && out.v == 2);
  }

  // Concurrent reader never observes a half-written wide payload, including
  // while the writer holds the section open mid-fill.
  {
    LatestSlot<Wide> s;
    constexpr uint64_t kN = 100000;
    std::atomic<bool> bad{false};
    std::atomic<uint64_t> reads{0};
    std::atomic<bool> reader_ready{false};
    std::atomic<bool> writer_done{false};
    std::thread reader([&] {
      Wide out{};
      reader_ready.store(true, std::memory_order_release);
      while (!writer_done.load(std::memory_order_acquire)) {
        if (s.Read(out)) {
          if (!Consistent(out)) bad.store(true, std::memory_order_relaxed);
          reads.fetch_add(1, std::memory_order_relaxed);
        }
      }
      Wide final{};
      if (s.Read(final)) {  // drain the last published value
        if (!Consistent(final)) bad.store(true, std::memory_order_relaxed);
      }
    });
    // Wait until the reader thread is actually spinning before publishing;
    // otherwise a fast writer can finish all kN commits before the reader is
    // ever scheduled and the reads>0 check below flakes under load.
    while (!reader_ready.load(std::memory_order_acquire)) {
    }
    for (uint64_t i = 1; i <= kN; ++i) {
      Wide* p = s.BeginWrite();
      const Wide v = MakeWide(i);
      for (int k = 0; k < 4; ++k) p->w[k] = v.w[k];
      for (int k = 4; k < 8; ++k) p->w[k] = v.w[k];
      s.CommitWrite();
      if (i == 1) {
        // Establish that the reader is actively reading before racing ahead:
        // value 1 is published, so a spinning reader will observe it.
        while (reads.load(std::memory_order_relaxed) == 0) {
        }
      }
    }
    writer_done.store(true, std::memory_order_release);
    reader.join();
    CHECK(!bad.load(std::memory_order_relaxed));
    CHECK(reads.load(std::memory_order_relaxed) > 0);
    Wide last{};
    CHECK(s.Read(last) && Consistent(last) && last.w[0] == kN);
  }
  return 0;
}
