// LatestSlot seqlock stress matrix, layered on top of the basic
// test_latest_slot_concurrent coverage.
//
// Part A -- single writer + multiple readers under sustained writes. The
// payload carries a per-value CRC so torn copies are detectable independently
// of seq_: a reader that gets a "consistent" copy (Read/ReadIfNew returns
// true) must never see a CRC mismatch. Zero mismatches proves the seqlock
// validation actually rejects torn copies, rather than the reader merely
// trusting seq_ parity. Reader-observed values must be monotone (latest-value
// contract).
//
// Part B -- multi-writer smoke. LatestSlot documents SINGLE WRITER (Write()
// is not CAS-protected; concurrent writers are a contract violation). Payload
// access is atomic since RT-19, so TSan no longer flags the write-write
// overlap as a data race; value correctness is nevertheless undefined, so
// Part B only asserts the process does not crash and readers keep making
// progress. It runs only in non-TSan release builds: RT-21 deliberately aborts
// the same contract violation in debug builds.
#include "thyme/check.h"
#include "thyme/runtime/detail/tsan_annotations.h"
#include "thyme/runtime/latest_slot.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

using namespace thyme;

namespace {

// Payload with an independent integrity tag so a torn copy is detectable
// without consulting seq_.
struct Pinned {
  uint64_t seq;
  uint32_t crc;
  uint32_t pad;
};

uint32_t Crc32(const uint8_t* data, std::size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < n; ++i) {
    c ^= data[i];
    for (int b = 0; b < 8; ++b) {
      const uint32_t m = (c & 1u) ? 0xEDB88320u : 0u;
      c = (c >> 1) ^ m;
    }
  }
  return ~c;
}

Pinned MakePinned(uint64_t seq) {
  Pinned p{};
  p.seq = seq;
  p.crc = Crc32(reinterpret_cast<const uint8_t*>(&seq), sizeof(seq));
  p.pad = static_cast<uint32_t>(0xCAFEu ^ seq);
  return p;
}

bool CrcOk(const Pinned& p) {
  return p.crc == Crc32(reinterpret_cast<const uint8_t*>(&p.seq), sizeof(p.seq));
}

}  // namespace

int main() {
  // ---- Part A: single writer, 4 readers, pattern+CRC integrity ----
  {
    LatestSlot<Pinned> slot;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> last_written{0};
    std::atomic<uint64_t> total_ok{0};
    std::atomic<uint64_t> bad_crc{0};

    std::thread writer([&] {
      uint64_t i = 1;
      while (!stop.load(std::memory_order_relaxed)) {
        slot.Write(MakePinned(i));
        last_written.store(i, std::memory_order_release);
        ++i;
      }
    });

    constexpr int kReaders = 4;
    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
      readers.emplace_back([&] {
        uint64_t seen = 0;
        uint64_t prev = 0;
        while (!stop.load(std::memory_order_acquire)) {
          Pinned out{};
          if (slot.ReadIfNew(seen, out)) {
            if (!CrcOk(out)) bad_crc.fetch_add(1, std::memory_order_relaxed);
            // latest-value: a single reader's observations never go backwards
            if (out.seq < prev) CHECK(false);
            prev = out.seq;
            total_ok.fetch_add(1, std::memory_order_relaxed);
          } else if (slot.Read(out)) {
            if (!CrcOk(out)) bad_crc.fetch_add(1, std::memory_order_relaxed);
            if (out.seq < prev) CHECK(false);
            prev = out.seq;
            total_ok.fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop.store(true, std::memory_order_release);
    writer.join();
    for (auto& t : readers) t.join();

    CHECK(last_written.load(std::memory_order_acquire) > 10000);
    CHECK(total_ok.load(std::memory_order_relaxed) > 1000);
    // The seqlock must never deliver a torn copy as a "consistent" read.
    CHECK(bad_crc.load(std::memory_order_relaxed) == 0);
  }

  // ---- Part B: multi-writer smoke (release only; RT-21 aborts in debug) ----
#if defined(NDEBUG) && !defined(THYME_TSAN_ACTIVE)
  {
    LatestSlot<Pinned> slot;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reads{0};

    std::thread w1([&] {
      uint64_t i = 1;
      while (!stop.load(std::memory_order_relaxed)) {
        slot.Write(MakePinned(i));
        ++i;
      }
    });
    std::thread w2([&] {
      uint64_t i = 1ull << 40;
      while (!stop.load(std::memory_order_relaxed)) {
        slot.Write(MakePinned(i));
        ++i;
      }
    });
    std::thread reader([&] {
      while (!stop.load(std::memory_order_acquire)) {
        Pinned out{};
        uint64_t seen = 0;
        (void)slot.ReadIfNew(seen, out);
        (void)slot.Read(out);
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop.store(true, std::memory_order_release);
    w1.join();
    w2.join();
    reader.join();
    // No crash, readers made progress. Values are not checked: concurrent
    // writers violate the documented SINGLE WRITER contract and their
    // interplay is undefined.
    CHECK(reads.load(std::memory_order_relaxed) > 0);
  }
#endif

  return 0;
}
