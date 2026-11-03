// test_pinned_slot_lease_copy.cpp — PERF-15: lease ABI v5 arbitrates plain
// memcpy with writer steal through pin_gen.copy_active. A second copier or a
// missing process-start token keeps the atomic fallback; dead copiers are
// recovered by pid/start identity rather than by timeout alone.
#include "thyme/check.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

uint32_t g_fixture_id = 0;

struct Fixture {
  Fixture(uint32_t slot_bytes, uint32_t slot_count, uint64_t timeout_ns) {
    std::snprintf(name, sizeof(name), "/c9lease_copy_%d_%u", static_cast<int>(getpid()), ++g_fixture_id);
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
    CHECK(writer.Create(name, slot_bytes, slot_count, 0, timeout_ns));
    CHECK(reader.Open(name, 1000000000ULL));
  }

  ~Fixture() {
    reader.Close();
    writer.Close();
    ShmRegion::UnlinkName(name);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  }

  void Write(const std::vector<uint8_t>& bytes) {
    PinnedSlotWriter::BeginView bv = writer.Begin(static_cast<uint32_t>(bytes.size()));
    CHECK(bv.payload != nullptr);
    std::memcpy(bv.payload, bytes.data(), bytes.size());
    writer.Commit();
  }

  PinnedSlotLeaseCell& Cell(uint64_t index) { return reinterpret_cast<PinnedSlotLeaseCell*>(writer.lease() + 1)[index]; }

  char name[96] = {};
  PinnedSlotWriter writer;
  PinnedSlotReader reader;
};

std::vector<uint8_t> Bytes(uint32_t size, uint32_t seed) {
  std::vector<uint8_t> out(size);
  for (uint32_t i = 0; i < size; ++i) {
    out[i] = static_cast<uint8_t>((i * 37u + seed) & 0xFFu);
  }
  return out;
}

uint64_t SelfPid() { return static_cast<uint64_t>(getpid()); }

void TestOrdinaryCopySizes() {
  Fixture f(4096, 8, 60000000000ULL);
  const uint32_t sizes[] = {1, 7, 64, 255, 1024, 4096};
  for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    f.Write(Bytes(sizes[i], i + 1));
  }
  for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    const std::vector<uint8_t> expected = Bytes(sizes[i], i + 1);
    PinnedSlotReader::Lease lease = f.reader.Acquire(i);
    CHECK(lease.valid());
    std::vector<uint8_t> actual(expected.size(), 0xCC);
    CHECK(lease.CopyPayloadTo(actual.data(), actual.size()));
    CHECK(std::memcmp(actual.data(), expected.data(), expected.size()) == 0);
    CHECK(!LeaseCopyActiveOf(f.Cell(i).pin_gen.load(std::memory_order_acquire)));
    CHECK(lease.Release() == LeaseReleaseResult::kOk);
  }
}

void TestFallbackAndInvalidLease() {
  Fixture f(256, 1, 60000000000ULL);
  const std::vector<uint8_t> expected = Bytes(256, 9);
  f.Write(expected);
  const uint64_t start = ProducerGateProcessStart(SelfPid());
  CHECK(start != 0);

  // A second begin cannot join the active memcpy. The view takes the atomic
  // fallback, leaves the first copier's active/owner state alone, and copies
  // exactly the published bytes.
  {
    PinnedSlotReader::View view;
    uint32_t gen = 0;
    CHECK(f.reader.AcquireLease(0, view, &gen) == PinnedSlotReader::ReadResult::kOk);
    CHECK(LeaseCopyBegin(f.Cell(0), gen, SelfPid(), start) == LeaseCopyBeginResult::kOk);
    std::vector<uint8_t> actual(expected.size(), 0xCC);
    CHECK(view.CopyPayloadTo(actual.data(), actual.size()));
    CHECK(std::memcmp(actual.data(), expected.data(), expected.size()) == 0);
    const uint64_t active = f.Cell(0).pin_gen.load(std::memory_order_acquire);
    CHECK(LeaseCopyActiveOf(active));
    CHECK(LeasePinOf(active) == 1);
    CHECK(f.Cell(0).copy_owner_pid.load(std::memory_order_acquire) == SelfPid());
    LeaseCopyEnd(f.Cell(0), gen, SelfPid());
    f.reader.ReleaseLease(0, gen);
  }

  // Without a reliable /proc start token the production view uses the same
  // atomic fallback and never publishes a half-valid owner identity.
  {
    f.reader.set_copy_owner_start_test_override(0);
    PinnedSlotReader::View view;
    uint32_t gen = 0;
    CHECK(f.reader.AcquireLease(0, view, &gen) == PinnedSlotReader::ReadResult::kOk);
    std::vector<uint8_t> actual(expected.size(), 0xCC);
    CHECK(view.CopyPayloadTo(actual.data(), actual.size()));
    CHECK(std::memcmp(actual.data(), expected.data(), expected.size()) == 0);
    CHECK(!LeaseCopyActiveOf(f.Cell(0).pin_gen.load(std::memory_order_acquire)));
    CHECK(f.Cell(0).copy_owner_pid.load(std::memory_order_acquire) == 0);
    CHECK(f.Cell(0).copy_owner_start.load(std::memory_order_acquire) == 0);
    f.reader.ReleaseLease(0, gen);
  }

  // A stale generation is not a fallback case: refuse before writing a byte.
  {
    PinnedSlotReader::View view;
    uint32_t gen = 0;
    CHECK(f.reader.AcquireLease(0, view, &gen) == PinnedSlotReader::ReadResult::kOk);
    f.Cell(0).pin_gen.store(LeasePack(gen + 1, 0), std::memory_order_release);
    std::vector<uint8_t> actual(expected.size(), 0xA5);
    CHECK(!view.CopyPayloadTo(actual.data(), actual.size()));
    for (uint8_t byte : actual) {
      CHECK(byte == 0xA5);
    }
  }
}

void TestActiveSurvivesOtherPinUpdates() {
  Fixture f(256, 1, 1000000ULL);
  f.Write(Bytes(256, 11));
  const uint64_t start = ProducerGateProcessStart(SelfPid());
  CHECK(start != 0);
  PinnedSlotReader::View first;
  uint32_t first_gen = 0;
  CHECK(f.reader.AcquireLease(0, first, &first_gen) == PinnedSlotReader::ReadResult::kOk);
  CHECK(LeaseCopyBegin(f.Cell(0), first_gen, SelfPid(), start) == LeaseCopyBeginResult::kOk);

  PinnedSlotReader::View second;
  uint32_t second_gen = 0;
  CHECK(f.reader.AcquireLease(0, second, &second_gen) == PinnedSlotReader::ReadResult::kOk);
  CHECK(second_gen == first_gen);
  uint64_t word = f.Cell(0).pin_gen.load(std::memory_order_acquire);
  CHECK(LeaseCopyActiveOf(word));
  CHECK(LeasePinOf(word) == 2);
  f.reader.ReleaseLease(0, second_gen);
  word = f.Cell(0).pin_gen.load(std::memory_order_acquire);
  CHECK(LeaseCopyActiveOf(word));
  CHECK(LeasePinOf(word) == 1);

  // Releasing even the last pin must not expose an in-flight memcpy. Active is
  // independently sufficient to make the writer skip until copy end.
  f.reader.ReleaseLease(0, first_gen);
  f.Cell(0).epoch.store(0, std::memory_order_seq_cst);
  PinnedSlotWriter::BeginView bv = f.writer.Begin(256);
  CHECK(bv.payload == nullptr);
  CHECK(f.writer.pending_skipped());
  f.writer.Commit();
  word = f.Cell(0).pin_gen.load(std::memory_order_acquire);
  CHECK(LeaseCopyActiveOf(word));
  CHECK(LeasePinOf(word) == 0);
  CHECK(f.writer.lease()->lease_steal.load(std::memory_order_acquire) == 0);
  LeaseCopyEnd(f.Cell(0), first_gen, SelfPid());
  CHECK(!LeaseCopyActiveOf(f.Cell(0).pin_gen.load(std::memory_order_acquire)));
}

void TestExpiredLiveOwnerAndClear() {
  Fixture f(256, 1, 1000000ULL);
  f.Write(Bytes(256, 13));
  const uint64_t start = ProducerGateProcessStart(SelfPid());
  CHECK(start != 0);
  PinnedSlotReader::View view;
  uint32_t gen = 0;
  CHECK(f.reader.AcquireLease(0, view, &gen) == PinnedSlotReader::ReadResult::kOk);
  CHECK(LeaseCopyBegin(f.Cell(0), gen, SelfPid(), start) == LeaseCopyBeginResult::kOk);
  f.Cell(0).epoch.store(0, std::memory_order_seq_cst);

  // Expiry alone is not evidence of death: a preempted live copier still owns
  // the payload, so the writer must skip and must not count a steal.
  PinnedSlotWriter::BeginView bv = f.writer.Begin(256);
  CHECK(bv.payload == nullptr);
  CHECK(f.writer.pending_skipped());
  f.writer.Commit();
  CHECK(f.writer.lease()->lease_steal.load(std::memory_order_acquire) == 0);
  CHECK(LeaseCopyActiveOf(f.Cell(0).pin_gen.load(std::memory_order_acquire)));

  LeaseCopyEnd(f.Cell(0), gen, SelfPid());
  bv = f.writer.Begin(256);
  CHECK(bv.payload != nullptr);
  std::memset(bv.payload, 0x5A, 256);
  f.writer.Commit();
  const uint64_t stolen = f.Cell(0).pin_gen.load(std::memory_order_acquire);
  CHECK(LeaseGenOf(stolen) == gen + 1);
  CHECK(LeasePinOf(stolen) == 0);
  CHECK(!LeaseCopyActiveOf(stolen));
  CHECK(f.writer.lease()->lease_steal.load(std::memory_order_acquire) == 1);
}

void TestDeadOwnerRecovery() {
  Fixture f(256, 1, 1000000ULL);
  f.Write(Bytes(256, 17));
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    PinnedSlotReader child_reader;
    if (!child_reader.Open(f.name, 1000000000ULL)) {
      _exit(2);
    }
    PinnedSlotReader::View view;
    uint32_t gen = 0;
    if (child_reader.AcquireLease(0, view, &gen) != PinnedSlotReader::ReadResult::kOk) {
      _exit(3);
    }
    const uint64_t pid = static_cast<uint64_t>(getpid());
    const uint64_t start = ProducerGateProcessStart(pid);
    if (start == 0) {
      _exit(4);
    }
    auto* cell = &f.Cell(0);
    cell->epoch.store(0, std::memory_order_seq_cst);
    if (LeaseCopyBegin(*cell, gen, pid, start) != LeaseCopyBeginResult::kOk) {
      _exit(5);
    }
    _exit(0);  // simulate death between copy begin and copy end
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  const uint64_t active = f.Cell(0).pin_gen.load(std::memory_order_acquire);
  CHECK(LeaseCopyActiveOf(active));
  CHECK(f.Cell(0).copy_owner_pid.load(std::memory_order_acquire) == static_cast<uint64_t>(child));
  CHECK(f.Cell(0).copy_owner_start.load(std::memory_order_acquire) != 0);

  PinnedSlotWriter::BeginView bv = f.writer.Begin(256);
  CHECK(bv.payload != nullptr);
  std::memset(bv.payload, 0x6B, 256);
  f.writer.Commit();
  const uint64_t stolen = f.Cell(0).pin_gen.load(std::memory_order_acquire);
  CHECK(LeaseGenOf(stolen) == LeaseGenOf(active) + 1);
  CHECK(LeasePinOf(stolen) == 0);
  CHECK(!LeaseCopyActiveOf(stolen));
  CHECK(f.Cell(0).copy_owner_pid.load(std::memory_order_acquire) == 0);
  CHECK(f.Cell(0).copy_owner_start.load(std::memory_order_acquire) == 0);
  CHECK(f.writer.lease()->lease_steal.load(std::memory_order_acquire) == 1);
}

void TestDeadOwnerBeforeActive() {
  Fixture f(256, 1, 1000000ULL);
  f.Write(Bytes(256, 23));
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    PinnedSlotReader child_reader;
    if (!child_reader.Open(f.name, 1000000000ULL)) {
      _exit(2);
    }
    PinnedSlotReader::View view;
    uint32_t gen = 0;
    if (child_reader.AcquireLease(0, view, &gen) !=
        PinnedSlotReader::ReadResult::kOk) {
      _exit(3);
    }
    const uint64_t pid = static_cast<uint64_t>(getpid());
    const uint64_t start = ProducerGateProcessStart(pid);
    if (start == 0) {
      _exit(4);
    }
    auto* cell = &f.Cell(0);
    cell->epoch.store(0, std::memory_order_seq_cst);
    cell->copy_owner_pid.store(pid, std::memory_order_release);
    cell->copy_owner_start.store(start, std::memory_order_release);
    _exit(0);  // simulate death after owner publication but before active CAS
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  const uint64_t before = f.Cell(0).pin_gen.load(std::memory_order_acquire);
  CHECK(!LeaseCopyActiveOf(before));
  CHECK(f.Cell(0).copy_owner_pid.load(std::memory_order_acquire) ==
        static_cast<uint64_t>(child));

  PinnedSlotWriter::BeginView bv = f.writer.Begin(256);
  CHECK(bv.payload != nullptr);
  std::memset(bv.payload, 0x7C, 256);
  f.writer.Commit();
  CHECK(LeaseGenOf(f.Cell(0).pin_gen.load(std::memory_order_acquire)) ==
        LeaseGenOf(before) + 1);
  CHECK(f.Cell(0).copy_owner_pid.load(std::memory_order_acquire) == 0);
  CHECK(f.Cell(0).copy_owner_start.load(std::memory_order_acquire) == 0);
  CHECK(f.writer.lease()->lease_steal.load(std::memory_order_acquire) == 1);
}

void TestCopyWriterContention() {
  Fixture f(256, 1, 1000000ULL);
  f.Write(std::vector<uint8_t>(256, 0));
  std::atomic<bool> stop{false};
  std::thread writer_thread([&]() {
    uint8_t tag = 1;
    while (!stop.load(std::memory_order_relaxed)) {
      PinnedSlotWriter::BeginView bv = f.writer.Begin(256);
      if (bv.payload != nullptr) {
        std::memset(bv.payload, tag++, 256);
      }
      f.writer.Commit();
    }
  });

  uint32_t copied = 0;
  std::vector<uint8_t> actual(256);
  for (uint32_t attempt = 0; attempt < 2000; ++attempt) {
    const uint64_t head = f.reader.head();
    if (head == 0) {
      continue;
    }
    PinnedSlotReader::View view;
    uint32_t gen = 0;
    const uint64_t index = head - 1;
    if (f.reader.AcquireLease(index, view, &gen) != PinnedSlotReader::ReadResult::kOk) {
      continue;
    }
    if (view.CopyPayloadTo(actual.data(), actual.size())) {
      ++copied;
      for (uint8_t byte : actual) {
        CHECK(byte == actual[0]);
      }
    }
    (void)f.reader.ReleaseLeaseStatus(index, gen);
  }
  stop.store(true, std::memory_order_relaxed);
  writer_thread.join();
  CHECK(copied > 0);
}

}  // namespace

int main() {
  CHECK(kPinnedSlotLeaseVersion == 6);
  CHECK(LeasePinOf(LeasePack(3, 7, true)) == 7);
  CHECK(LeaseCopyActiveOf(LeasePack(3, 7, true)));
  CHECK(!LeaseCopyActiveOf(LeasePack(3, kPinnedSlotLeaseMaxPin)));
  TestOrdinaryCopySizes();
  TestFallbackAndInvalidLease();
  TestActiveSurvivesOtherPinUpdates();
  TestExpiredLiveOwnerAndClear();
  TestDeadOwnerRecovery();
  TestDeadOwnerBeforeActive();
  TestCopyWriterContention();
  return 0;
}
