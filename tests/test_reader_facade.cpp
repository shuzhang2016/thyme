// test_reader_facade.cpp — BF-14: Next()/NextLease()/FrameCursor combine the
// cursor advance with pin+single validation and return a move-only
// BorrowedFrame. Covers kOk, kEmpty, kLost (eviction) and kTorn, the
// still_valid()/Release() results, and that the facade never exposes gen,
// ReleaseLease or ReadResult.
#include "thyme/check.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <type_traits>
#include <unistd.h>

using namespace thyme;

static_assert(!std::is_copy_constructible<PinnedSlotReader::BorrowedFrame>::value,
              "BorrowedFrame must be move-only");
static_assert(!std::is_copy_assignable<PinnedSlotReader::BorrowedFrame>::value,
              "BorrowedFrame must be move-only");
static_assert(std::is_move_constructible<PinnedSlotReader::BorrowedFrame>::value,
              "BorrowedFrame must be movable");
static_assert(std::is_move_assignable<PinnedSlotReader::BorrowedFrame>::value,
              "BorrowedFrame must be move-assignable");

namespace {

constexpr uint64_t kStealTimeoutNs = 20000000ULL;  // 20 ms

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

void SleepNs(uint64_t ns) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
  nanosleep(&ts, nullptr);
}

void WriteFrame(PinnedSlotWriter& w, uint8_t tag, uint64_t mono_ns) {
  PinnedSlotWriter::BeginView bv = w.Begin(128);
  CHECK(bv.payload != nullptr);
  std::memset(bv.payload, tag, 128);
  w.Commit(mono_ns);
}

bool ForceSteal(PinnedSlotWriter& w, uint64_t steal_before) {
  const uint64_t deadline = NowNs() + 3000000000ULL;
  while (NowNs() < deadline) {
    PinnedSlotWriter::BeginView bv = w.Begin(128);
    if (bv.payload != nullptr) {
      std::memset(bv.payload, 0xEE, 128);
    }
    w.Commit();
    if (w.lease()->lease_steal.load(std::memory_order_acquire) > steal_before) {
      return true;
    }
    SleepNs(2000000ULL);
  }
  return false;
}

}  // namespace

int main() {
  using FS = PinnedSlotReader::FrameStatus;

  // --- kEmpty on an empty channel; default BorrowedFrame is empty. ---
  {
    char name[96];
    std::snprintf(name, sizeof(name), "/c9facade_empty_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
    PinnedSlotWriter w;
    CHECK(w.Create(name, 128, 4, 0));
    PinnedSlotReader r;
    CHECK(r.Open(name, 1000000000ULL));
    PinnedSlotReader::FrameCursor fc(r);
    PinnedSlotReader::BorrowedFrame f = fc.Next();
    CHECK(f.status() == FS::kEmpty);
    CHECK(!f.ok());
    CHECK(f.data() == nullptr);
    CHECK(f.size() == 0);
    CHECK(f.mono_ns() == 0);
    CHECK(f.index() == 0);
    CHECK(!f.still_valid());
    CHECK(f.Release() == LeaseReleaseResult::kAlreadyReleased);
    CHECK(fc.position() == 0);  // kEmpty does not move the cursor
    r.Close();
    w.Close();
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  }

  // --- kOk: one step does try+lease; data/size/mono/index and Release(). ---
  {
    char name[96];
    std::snprintf(name, sizeof(name), "/c9facade_ok_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
    PinnedSlotWriter w;
    CHECK(w.Create(name, 128, 4, 0));
    WriteFrame(w, 0x10, 1000);
    WriteFrame(w, 0x20, 2000);
    WriteFrame(w, 0x30, 3000);
    PinnedSlotReader r;
    CHECK(r.Open(name, 1000000000ULL));
    PinnedSlotReader::FrameCursor fc(r);

    PinnedSlotReader::BorrowedFrame f = fc.Next();
    CHECK(f.status() == FS::kOk);
    CHECK(f.ok());
    CHECK(f.index() == 0);
    CHECK(f.size() == 128);
    CHECK(f.mono_ns() == 1000);
    CHECK(f.data() != nullptr);
    CHECK(f.data()[0] == 0x10);
    CHECK(f.crc_ok());
    CHECK(f.still_valid());
    CHECK(fc.position() == 1);
    uint8_t buf[128];
    CHECK(f.CopyPayloadTo(buf, sizeof(buf)));
    CHECK(buf[0] == 0x10);
    CHECK(f.Release() == LeaseReleaseResult::kOk);
    CHECK(f.status() == FS::kEmpty);
    CHECK(f.data() == nullptr);
    CHECK(f.Release() == LeaseReleaseResult::kAlreadyReleased);

    PinnedSlotReader::BorrowedFrame f2 = fc.NextLease();
    CHECK(f2.status() == FS::kOk);
    CHECK(f2.index() == 1);
    CHECK(f2.mono_ns() == 2000);
    CHECK(f2.Release() == LeaseReleaseResult::kOk);

    PinnedSlotReader::BorrowedFrame f3 = fc.Next();
    CHECK(f3.status() == FS::kOk);
    CHECK(f3.index() == 2);
    CHECK(f3.Release() == LeaseReleaseResult::kOk);

    CHECK(fc.Next().status() == FS::kEmpty);  // drained
    CHECK(fc.position() == 3);
    r.Close();
    w.Close();
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  }

  // --- kLost: lagging cursor (whole oldest window evicted). ---
  {
    char name[96];
    std::snprintf(name, sizeof(name), "/c9facade_lost_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
    PinnedSlotWriter w;
    CHECK(w.Create(name, 128, 2, 0));
    WriteFrame(w, 0x00, 1000);
    WriteFrame(w, 0x01, 2000);
    WriteFrame(w, 0x02, 3000);
    WriteFrame(w, 0x03, 4000);  // slots 0/1 now hold frames 2/3
    PinnedSlotReader r;
    CHECK(r.Open(name, 1000000000ULL));
    PinnedSlotReader::FrameCursor fc(r);

    PinnedSlotReader::BorrowedFrame lost = fc.Next();
    CHECK(lost.status() == FS::kLost);
    CHECK(!lost.ok());
    CHECK(lost.data() == nullptr);
    CHECK(fc.position() == 2);  // jumped to the retained window
    CHECK(r.drop_count() == 2);

    PinnedSlotReader::BorrowedFrame f2 = fc.Next();
    CHECK(f2.status() == FS::kOk);
    CHECK(f2.index() == 2);
    CHECK(f2.data()[0] == 0x02);
    CHECK(f2.Release() == LeaseReleaseResult::kOk);
    PinnedSlotReader::BorrowedFrame f3 = fc.Next();
    CHECK(f3.status() == FS::kOk);
    CHECK(f3.index() == 3);
    CHECK(f3.Release() == LeaseReleaseResult::kOk);
    CHECK(fc.Next().status() == FS::kEmpty);
    r.Close();
    w.Close();
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  }

  // --- kTorn: slot seq stomped to 0 (writer mid-commit / killed). ---
  {
    char name[96];
    std::snprintf(name, sizeof(name), "/c9facade_torn_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
    PinnedSlotWriter w;
    CHECK(w.Create(name, 128, 2, 0));
    WriteFrame(w, 0xAA, 1000);
    WriteFrame(w, 0xBB, 2000);

    ShmRegion rw;
    CHECK(rw.OpenRetry(name, PinnedSlotWriter::RegionBytes(128, 2, 0),
                       1000000000ULL, /*read_only=*/false));
    auto* h = static_cast<PinnedSlotPageHeader*>(rw.data());
    uint8_t* slot0 = reinterpret_cast<uint8_t*>(h) +
                     PinnedSlotWriter::SlotsOffset(0);
    reinterpret_cast<PinnedSlotHeader*>(slot0)->seq.store(
        0, std::memory_order_release);

    PinnedSlotReader r;
    CHECK(r.Open(name, 1000000000ULL));
    PinnedSlotReader::FrameCursor fc(r);
    PinnedSlotReader::BorrowedFrame torn = fc.Next();
    CHECK(torn.status() == FS::kTorn);
    CHECK(!torn.ok());
    CHECK(fc.position() == 1);  // torn frame is skipped, stream continues
    CHECK(r.torn_read_count() == 1);

    PinnedSlotReader::BorrowedFrame f1 = fc.Next();
    CHECK(f1.status() == FS::kOk);
    CHECK(f1.index() == 1);
    CHECK(f1.data()[0] == 0xBB);
    CHECK(f1.Release() == LeaseReleaseResult::kOk);
    CHECK(fc.Next().status() == FS::kEmpty);
    r.Close();
    rw.Unmap();
    w.Close();
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  }

  // --- Steal: facade still_valid()/Release() surface the low-level result
  // without exposing gen or ReleaseLease. ---
  {
    char name[96];
    std::snprintf(name, sizeof(name), "/c9facade_steal_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
    PinnedSlotWriter w;
    CHECK(w.Create(name, 128, 2, 0, kStealTimeoutNs));
    WriteFrame(w, 0x11, 1000);
    WriteFrame(w, 0x22, 2000);
    PinnedSlotReader r;
    CHECK(r.Open(name, 1000000000ULL));
    PinnedSlotReader::FrameCursor fc(r);

    PinnedSlotReader::BorrowedFrame f = fc.Next();
    CHECK(f.status() == FS::kOk);
    CHECK(f.index() == 0);
    CHECK(f.still_valid());
    const uint64_t steal_before =
        w.lease()->lease_steal.load(std::memory_order_acquire);
    CHECK(ForceSteal(w, steal_before));

    CHECK(!f.still_valid());
    CHECK(f.Release() == LeaseReleaseResult::kStolen);
    CHECK(r.lease_invalidated_count() == 1);
    CHECK(f.Release() == LeaseReleaseResult::kAlreadyReleased);
    CHECK(fc.position() == 1);  // the cursor advanced when the frame was handed out
    r.Close();
    w.Close();
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  }

  return 0;
}
