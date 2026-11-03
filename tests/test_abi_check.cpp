// test_abi_check.cpp — OPS-06: the pre-flight ABI checker must flag a page
// written by an older build (synthetic v2 SPSC page vs this v3 reader), pass
// current pages, check the PinnedSlot lease companion, and verify the described
// v3 LatestSlot page while rejecting a legacy v2 body.

#include "thyme/check.h"
#include "thyme/app/mp_control.h"
#include "thyme/probe/abi_matrix.h"
#include "thyme/probe/probe.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/latest_slot_page.h"
#include "thyme/runtime/shared_health.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spsc_ring.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <unistd.h>
#include <vector>

using namespace thyme;
using namespace thyme::probe;

namespace {

struct Pod {
  uint32_t a;
  uint32_t b;
};

// v4 ring page prefix: the ARCH-11 super-header followed by the ring cap cell.
// Crafted here so a test can stamp an old version word without any
// writer/version API.
constexpr std::size_t kRawRingCapOffset = 64;

std::string UniquePrefix(const char* tag) {
  return std::string("thyme_abichk_") +
         std::to_string(static_cast<int>(getpid())) + "_" + tag + "_";
}

void MakeSpscPage(ShmRegion* region, const std::string& name,
                  uint32_t version) {
  ShmRegion::UnlinkName(name.c_str());
  CHECK(region->Create(name.c_str(), 4096));
  std::memset(region->data(), 0, 4096);
  auto* header = new (region->data()) ShmSuperHeader;
  auto* cap = reinterpret_cast<std::atomic<uint32_t>*>(
      static_cast<uint8_t*>(region->data()) + kRawRingCapOffset);
  cap->store(2, std::memory_order_relaxed);
  header->abi_version.store(version, std::memory_order_relaxed);
  header->magic.store(kShmSpscRingMagic, std::memory_order_release);
}

const AbiCheck* Find(const AbiReport& report, const std::string& name) {
  for (const auto& page : report.pages) {
    if (page.name == name) return &page;
  }
  return nullptr;
}

Options PrefixOptions(const std::string& prefix) {
  Options options;
  options.prefixes = {prefix};
  return options;
}

void TestOldPageVsNewReader() {
  const std::string prefix = UniquePrefix("old");
  const std::string old_name = "/" + prefix + "spsc_v2";
  const std::string new_name = "/" + prefix + "spsc_v3";

  ShmRegion old_page;
  ShmRegion new_page;
  MakeSpscPage(&old_page, old_name, kShmSpscRingVersion - 1);  // synthetic v2
  MakeSpscPage(&new_page, new_name, kShmSpscRingVersion);

  ProbeApi api;
  api.SetOptions(PrefixOptions(prefix));
  AbiReport report;
  CHECK(api.CheckAbi(&report) == Status::kBadGeometry);
  CHECK(!report.ok());
  CHECK(report.mismatch_count == 1);
  CHECK(report.compatible_count == 1);

  const AbiCheck* old = Find(report, old_name);
  CHECK(old != nullptr);
  CHECK(old->kind == Kind::kSpscRing);
  CHECK(old->object == std::string("ShmSpscRing"));
  CHECK(old->expected == kShmSpscRingVersion);
  CHECK(old->actual == kShmSpscRingVersion - 1);
  CHECK(old->checked);
  CHECK(!old->compatible);
  CHECK(!old->note.empty());

  const AbiCheck* current = Find(report, new_name);
  CHECK(current != nullptr);
  CHECK(current->compatible);
  CHECK(current->actual == kShmSpscRingVersion);

  const std::string json = AbiReportJson(report);
  CHECK(json == AbiReportJson(report));
  CHECK(json.find("\"ok\":false") != std::string::npos);
  CHECK(json.find("\"mismatches\":1") != std::string::npos);
  CHECK(json.find(old_name) != std::string::npos);
  CHECK(json.find("\"actual\":" + std::to_string(kShmSpscRingVersion - 1)) !=
        std::string::npos);

  old_page.Unmap();
  new_page.Unmap();
  ShmRegion::UnlinkName(old_name.c_str());
  ShmRegion::UnlinkName(new_name.c_str());
}

void TestPinnedSlotLeaseCompanion() {
  const std::string prefix = "thyme_abichk_" +
                             std::to_string(static_cast<int>(getpid())) +
                             "_bf_";
  const std::string name = "/" + prefix + "h264";
  ShmRegion::UnlinkName(name.c_str());
  ShmRegion::UnlinkName((name + ".lease").c_str());

  PinnedSlotWriter writer;
  CHECK(writer.Create(name.c_str(), 64, 2, 0));

  ProbeApi api;
  api.SetOptions(PrefixOptions(prefix));
  AbiReport report;
  CHECK(api.CheckAbi(&report) == Status::kOk);
  CHECK(report.ok());
  CHECK(report.pages.size() == 2);  // channel page + lease table
  CHECK(Find(report, name) != nullptr);
  const AbiCheck* lease = Find(report, name + ".lease");
  CHECK(lease != nullptr);
  CHECK(lease->object == std::string("PinnedSlotLeaseHeader"));
  CHECK(lease->expected == kPinnedSlotLeaseVersion);
  CHECK(lease->actual == kPinnedSlotLeaseVersion);
  CHECK(lease->compatible);

  // A v4 lease table against this v5 reader is the other mixed-version case.
  CHECK(writer.lease() != nullptr);
  writer.lease()->super.abi_version.store(kPinnedSlotLeaseVersion - 1,
                                          std::memory_order_relaxed);
  CHECK(api.CheckAbi(&report) == Status::kBadGeometry);
  CHECK(report.mismatch_count == 1);
  lease = Find(report, name + ".lease");
  CHECK(lease != nullptr);
  CHECK(!lease->compatible);
  CHECK(lease->actual == kPinnedSlotLeaseVersion - 1);

  writer.Close();
  ShmRegion::UnlinkName(name.c_str());
  ShmRegion::UnlinkName((name + ".lease").c_str());
}

void TestLatestSlotVerifiable() {
  const std::string prefix = "thyme_abichk_" +
                             std::to_string(static_cast<int>(getpid())) +
                             "_slot_";
  const std::string name = "/" + prefix + "latest";
  ShmRegion::UnlinkName(name.c_str());
  constexpr uint64_t kPayloadHash = 0x0102030405060708ULL;
  ShmRegion slot;
  auto* page = CreateLatestSlotPage<Pod>(slot, name.c_str(), kPayloadHash);
  CHECK(page != nullptr);

  ProbeApi api;
  api.SetOptions(PrefixOptions(prefix));
  AbiReport report;
  CHECK(api.CheckAbi(&report) == Status::kOk);
  CHECK(report.ok());
  CHECK(report.pages.size() == 1);
  CHECK(report.pages[0].kind == Kind::kLatestSlot);
  CHECK(report.pages[0].object == "LatestSlot<T>");
  CHECK(report.pages[0].expected == kLatestSlotLayoutVersion);
  CHECK(report.pages[0].actual == kLatestSlotPageAbiVersion);
  CHECK(report.pages[0].compatible);
  CHECK(report.unverifiable.empty());

  // A legacy v2 body is not a described page: the probe reports bad geometry
  // instead of reading it with guessed offsets.
  const std::string legacy_name = "/" + prefix + "legacy";
  ShmRegion::UnlinkName(legacy_name.c_str());
  ShmRegion legacy;
  CHECK(legacy.Create<LatestSlot<Pod>>(legacy_name.c_str(), 4096) != nullptr);
  ProbeApi legacy_api;
  legacy_api.SetOptions(PrefixOptions(prefix));
  legacy_api.AddManifestEntry(
      ManifestEntry{legacy_name, Kind::kLatestSlot, sizeof(Pod), 2, 0, "", false});
  RegionInfo legacy_info;
  CHECK(legacy_api.Info(legacy_name, legacy_info) == Status::kBadGeometry);
  CHECK(legacy_info.note.find("v2") != std::string::npos);

  slot.Unmap();
  ShmRegion::UnlinkName(name.c_str());
  ShmRegion::UnlinkName(legacy_name.c_str());
}

void TestMigratedPages() {
  const std::string prefix = UniquePrefix("mp");
  const std::string health_name = "/" + prefix + "health";
  const std::string ctrl_name = "/" + prefix + "control";
  ShmRegion::UnlinkName(health_name.c_str());
  ShmRegion::UnlinkName(ctrl_name.c_str());

  ShmRegion health;
  CHECK(health.CreateDescribed(health_name.c_str(), SharedHealthPage::Spec()));
  auto* hpage = static_cast<SharedHealthPage*>(health.data());
  CHECK(hpage->Init(static_cast<uint32_t>(getpid()), 2));
  ShmRegion ctrl;
  CHECK(ctrl.CreateDescribed(ctrl_name.c_str(), app::MpControl::Spec()));
  auto* cpage = static_cast<app::MpControl*>(ctrl.data());
  cpage->Init();

  ProbeApi api;
  api.SetOptions(PrefixOptions(prefix));
  AbiReport report;
  CHECK(api.CheckAbi(&report) == Status::kOk);
  CHECK(report.ok());
  CHECK(report.pages.size() == 2);
  const AbiCheck* hcheck = Find(report, health_name);
  CHECK(hcheck != nullptr);
  CHECK(hcheck->kind == Kind::kSharedHealth);
  CHECK(hcheck->object == std::string("SharedHealthPage"));
  CHECK(hcheck->expected == SharedHealthPage::kVersion);
  CHECK(hcheck->actual == SharedHealthPage::kVersion);
  CHECK(hcheck->compatible);
  const AbiCheck* ccheck = Find(report, ctrl_name);
  CHECK(ccheck != nullptr);
  CHECK(ccheck->kind == Kind::kMpControl);
  CHECK(ccheck->object == std::string("MpControl"));
  CHECK(ccheck->expected == app::MpControl::kMpControlAbiVersion);
  CHECK(ccheck->actual == app::MpControl::kMpControlAbiVersion);
  CHECK(ccheck->compatible);

  // A v2 health page is still classified by magic but rejected by the
  // super-header version check, so the pre-flight match count drops.
  hpage->super.abi_version.store(2, std::memory_order_relaxed);
  CHECK(api.CheckAbi(&report) == Status::kBadGeometry);
  hcheck = Find(report, health_name);
  CHECK(hcheck != nullptr);
  CHECK(hcheck->actual == 2);
  CHECK(!hcheck->compatible);
  CHECK(!hcheck->note.empty());

  health.Unmap();
  ctrl.Unmap();
  ShmRegion::UnlinkName(health_name.c_str());
  ShmRegion::UnlinkName(ctrl_name.c_str());
}

void TestNothingToCheck() {
  const std::string prefix = "thyme_abichk_" +
                             std::to_string(static_cast<int>(getpid())) +
                             "_none_";
  ProbeApi api;
  api.SetOptions(PrefixOptions(prefix));
  AbiReport report;
  CHECK(api.CheckAbi(&report) == Status::kNotFound);
  CHECK(report.ok());
  CHECK(report.pages.empty());
  CHECK(AbiReportJson(report).find("\"ok\":true") != std::string::npos);
}

}  // namespace

int main() {
  TestOldPageVsNewReader();
  TestPinnedSlotLeaseCompanion();
  TestLatestSlotVerifiable();
  TestMigratedPages();
  TestNothingToCheck();
  std::printf("test_abi_check: all checks passed\n");
  return 0;
}
