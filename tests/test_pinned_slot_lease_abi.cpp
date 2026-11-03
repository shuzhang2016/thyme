// test_pinned_slot_lease_abi.cpp — the lease table is a separate shm
// object ("<name>.lease"), RW-mapped by readers while the payload region
// stays RO; channel version is 4 (super-header) and
// version/magic/slot_count mismatches are rejected at attach.
#include "thyme/check.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

using namespace thyme;

int main() {
  static_assert(sizeof(PinnedSlotLeaseCell) == 64, "lease cell cache line");
  static_assert(PinnedSlotPageHeader::kVersion == 4,
                "v4 = super-header over the gate page");
  static_assert(kPinnedSlotLeaseVersion == 6,
                "v6 = super-header over the v5 lease protocol");

  char name[96];
  std::snprintf(name, sizeof(name), "/c9lease_abi_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());

  PinnedSlotWriter w;
  CHECK(w.Create(name, 512, 4, 64, 60000000000ULL));  // steal timeout 60 s
  CHECK(w.lease() != nullptr);
  CHECK(w.lease()->super.magic.load(std::memory_order_acquire) ==
        kPinnedSlotLeaseMagic);
  CHECK(w.lease()->super.abi_version.load(std::memory_order_acquire) ==
        kPinnedSlotLeaseVersion);
  CHECK(w.lease()->slot_count.load(std::memory_order_acquire) == 4);
  CHECK(w.lease()->steal_timeout_ns.load(std::memory_order_acquire) ==
        60000000000ULL);

  // The lease object is a distinct shm with the expected size.
  {
    const std::string lname = PinnedSlotWriter::LeaseShmName(name);
    ShmRegion probe;
    CHECK(probe.OpenRetry(lname.c_str(),
                          PinnedSlotWriter::LeaseRegionBytes(4), 1000000000ULL,
                          true));
    probe.Unmap();
  }

  // Reader attaches: payload RO + lease RW both mapped.
  PinnedSlotReader r;
  CHECK(r.Open(name, 1000000000ULL));
  CHECK(r.lease() != nullptr);

  // Stomp the channel version -> reader rejects (old/new ABI never mixes).
  {
    ShmRegion rw;
    CHECK(rw.OpenRetry(name, PinnedSlotWriter::RegionBytes(512, 4, 64),
                       1000000000ULL, false));
    auto* h = static_cast<PinnedSlotPageHeader*>(rw.data());
    h->super.abi_version.store(1, std::memory_order_release);
    PinnedSlotReader old;
    CHECK(!old.Open(name, 1000000000ULL));
    h->super.abi_version.store(PinnedSlotPageHeader::kVersion,
                               std::memory_order_release);
    rw.Unmap();
  }

  // Stomp the lease magic -> reader rejects.
  {
    const std::string lname = PinnedSlotWriter::LeaseShmName(name);
    ShmRegion rw;
    CHECK(rw.OpenRetry(lname.c_str(), PinnedSlotWriter::LeaseRegionBytes(4),
                       1000000000ULL, false));
    auto* lh = static_cast<PinnedSlotLeaseHeader*>(rw.data());
    lh->super.magic.store(0, std::memory_order_release);
    PinnedSlotReader bad;
    CHECK(!bad.Open(name, 1000000000ULL));
    lh->super.magic.store(kPinnedSlotLeaseMagic, std::memory_order_release);
    rw.Unmap();
  }
  // Recovery after the stomps: a fresh attach works.
  PinnedSlotReader r2;
  CHECK(r2.Open(name, 1000000000ULL));

  // Read-only observer mode: the lease table is mapped RO, TryRead/ReadFrame
  // work as usual, AcquireLease is explicitly rejected (kTorn), ReleaseLease is
  // harmless.
  {
    PinnedSlotWriter::BeginView bv = w.Begin(256);
    CHECK(bv.payload != nullptr);
    std::memset(bv.payload, 0x5A, 256);
    w.Commit();
    PinnedSlotReader obs;
    CHECK(obs.Open(name, 1000000000ULL, /*lease_read_only=*/true));
    PinnedSlotReader::View v;
    CHECK(obs.TryRead(v));
    CHECK(v.data_len == 256);
    CHECK(obs.ReadFrame(0, v) == PinnedSlotReader::ReadResult::kOk);
    uint32_t gen = 0;
    CHECK(obs.AcquireLease(0, v, &gen) == PinnedSlotReader::ReadResult::kTorn);
    obs.ReleaseLease(0, gen);  // harmless
    obs.Close();
  }

  // steal_timeout_ns == 0 is rejected (0 means "steal immediately", almost
  // certainly a config error; to disable steal, pass a huge timeout).
  {
    PinnedSlotWriter bad;
    CHECK(!bad.Create(name, 512, 4, 0, 0));  // rejected even though the name already exists
    ShmRegion::UnlinkName(name);             // the old object was cleared before Create rejected
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
    CHECK(w.Create(name, 512, 4, 0, 60000000000ULL));  // recreate for final cleanup
  }

  r.Close();
  r2.Close();
  w.Close();
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  return 0;
}
