// test_abi_matrix.cpp — OPS-06: the compiled-in ABI constants matrix must
// equal the header constants (no copied literals) and its JSON must be
// deterministic, because deployment scripts consume it verbatim.

#include "thyme/check.h"
#include "thyme/probe/abi_matrix.h"

#include "thyme/app/mp_control.h"
#include "thyme/record/package_integrity.h"
#include "thyme/record/record_schema.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/trigger_protocol.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/record_mirror.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shared_health.h"
#include "thyme/runtime/session_lease.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"

#include <cstdint>
#include <string>

using namespace thyme;
using namespace thyme::probe;

namespace {

const AbiConstant& Row(const char* object) {
  const AbiConstant* row = FindAbiConstant(object);
  CHECK(row != nullptr);
  return *row;
}

std::size_t CountOf(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

void TestVersionsMatchHeaders() {
  CHECK(!AbiConstants().empty());
  CHECK(AbiConstants().size() == 17);

  CHECK(Row("MpControl").version == app::MpControl::kMpControlAbiVersion);
  CHECK(Row("SharedHealthPage").version == SharedHealthPage::kVersion);
  CHECK(Row("PinnedSlotPageHeader").version == kPinnedSlotStoreVersion);
  CHECK(Row("PinnedSlotLeaseHeader").version == kPinnedSlotLeaseVersion);
  CHECK(Row("LatestSlot<T>").version == kLatestSlotLayoutVersion);
  CHECK(Row(".thyme.bin").version == kThymeBinFileAbiVersion);
  CHECK(Row("RecordMirrorPage").version == RecordMirrorPage::kVersion);
  CHECK(Row("RecordMirrorRingPage").version ==
        RecordMirrorRingPage::kVersion);
  CHECK(Row("ShmSpmcRing").version == kShmSpmcRingVersion);
  CHECK(Row("ShmSpscRing").version == kShmSpscRingVersion);
  CHECK(Row("TriggerRequest/TriggerResponse").version ==
        trigger_proto::kTriggerVersion);
  // ABI-09/ABI-10 rows.
  CHECK(Row(".tbin record-manifest").version == kThymeBinManifestBlockVersion);
  CHECK(Row("record manifest").version == schema::kManifestVersion);
  CHECK(Row("package schema.json").version == schema::kVersion);
  CHECK(Row("integrity.manifest").version == integrity::kManifestVersion);
  CHECK(Row("SessionLeasePage").version == kSessionLeaseVersion);

  // Page kinds the probe can classify all have a matrix row; the row that
  // describes the page's own version word is unique per kind.
  CHECK(PrimaryPageConstant(Kind::kSpscRing) == &Row("ShmSpscRing"));
  CHECK(PrimaryPageConstant(Kind::kSpmcRing) == &Row("ShmSpmcRing"));
  CHECK(PrimaryPageConstant(Kind::kPinnedSlot) ==
        &Row("PinnedSlotPageHeader"));
  CHECK(PrimaryPageConstant(Kind::kSharedHealth) == &Row("SharedHealthPage"));
  CHECK(PrimaryPageConstant(Kind::kMpControl) == &Row("MpControl"));
  CHECK(PrimaryPageConstant(Kind::kRecordMirrorRing) ==
        &Row("RecordMirrorRingPage"));
  CHECK(PrimaryPageConstant(Kind::kRecordMirror) == &Row("RecordMirrorPage"));
  // LatestSlot v3 is a described SHM page too.
  CHECK(PrimaryPageConstant(Kind::kLatestSlot) == &Row("LatestSlot<T>"));
  CHECK(PrimaryPageConstant(Kind::kUnknown) == nullptr);

  uint32_t version = 0;
  CHECK(ExpectedPageVersion(Kind::kSpscRing, &version));
  CHECK(version == kShmSpscRingVersion);
  CHECK(ExpectedPageVersion(Kind::kPinnedSlot, &version));
  CHECK(version == kPinnedSlotStoreVersion);
  CHECK(ExpectedPageVersion(Kind::kLatestSlot, &version));
  CHECK(version == kLatestSlotLayoutVersion);
  CHECK(!ExpectedPageVersion(Kind::kUnknown, &version));

  // Surfaces: every enum value is represented and named.
  CHECK(std::string(SurfaceName(AbiSurface::kShmPage)) == "shm_page");
  CHECK(std::string(SurfaceName(AbiSurface::kWire)) == "wire");
  CHECK(std::string(SurfaceName(AbiSurface::kFile)) == "file");
  CHECK(std::string(SurfaceName(AbiSurface::kLayout)) == "layout");
  CHECK(Row("ShmSpscRing").surface == AbiSurface::kShmPage);
  CHECK(Row("TriggerRequest/TriggerResponse").surface == AbiSurface::kWire);
  CHECK(Row(".thyme.bin").surface == AbiSurface::kFile);
  CHECK(Row("LatestSlot<T>").surface == AbiSurface::kShmPage);
  CHECK(Row("LatestSlot<T>").in_page_version);
  CHECK(Row("PinnedSlotPageHeader").in_page_version);
  CHECK(!Row("PinnedSlotLeaseHeader").primary_page_version);
}

void TestStableOrder() {
  const auto& rows = AbiConstants();
  for (std::size_t i = 1; i < rows.size(); ++i) {
    const std::string prev_component(rows[i - 1].component);
    const std::string component(rows[i].component);
    CHECK(prev_component <= component);
    if (prev_component == component) {
      CHECK(std::string(rows[i - 1].object) < std::string(rows[i].object));
    }
  }
}

void TestMatrixJson() {
  const std::string first = AbiMatrixJson();
  const std::string second = AbiMatrixJson();
  CHECK(first == second);
  CHECK(first.rfind("{\"schema\":1,\"entries\":[", 0) == 0);
  CHECK(first.size() > 2 && first[first.size() - 1] == '}');
  CHECK(CountOf(first, "\"constant\":") == AbiConstants().size());
  CHECK(CountOf(first, "\"component\":") == AbiConstants().size());

  for (const auto& row : AbiConstants()) {
    const std::string object = std::string("\"object\":\"") + row.object + "\"";
    CHECK(first.find(object) != std::string::npos);
    const std::string version =
        std::string("\"version\":") + std::to_string(row.version);
    CHECK(first.find(version) != std::string::npos);
    const std::string surface =
        std::string("\"surface\":\"") + SurfaceName(row.surface) + "\"";
    CHECK(first.find(surface) != std::string::npos);
  }
  // Non-page rows must not pretend to be probe pages.
  CHECK(first.find("\"probe_kind\":null") != std::string::npos);
  CHECK(first.find("\"in_page_version\":false") != std::string::npos);
}

void TestReportJson() {
  AbiReport report;
  AbiCheck ok;
  ok.name = "/ring_ok";
  ok.kind = Kind::kSpscRing;
  ok.object = "ShmSpscRing";
  ok.expected = kShmSpscRingVersion;
  ok.actual = kShmSpscRingVersion;
  ok.checked = true;
  ok.compatible = true;
  report.pages.push_back(ok);
  ++report.compatible_count;

  AbiCheck old;
  old.name = "/ring_old";
  old.kind = Kind::kSpscRing;
  old.object = "ShmSpscRing";
  old.expected = kShmSpscRingVersion;
  old.actual = kShmSpscRingVersion - 1;
  old.checked = true;
  old.compatible = false;
  old.note = "page version differs from this build";
  report.pages.push_back(old);
  ++report.mismatch_count;
  report.unverifiable.push_back("/mirror (no matrix row for this page kind)");

  CHECK(!report.ok());
  const std::string json = AbiReportJson(report);
  CHECK(json == AbiReportJson(report));
  CHECK(json.find("\"ok\":false") != std::string::npos);
  CHECK(json.find("\"mismatches\":1") != std::string::npos);
  CHECK(json.find("\"checked\":2") != std::string::npos);
  CHECK(json.find("\"name\":\"/ring_old\"") != std::string::npos);
  CHECK(json.find("\"compatible\":false") != std::string::npos);
  CHECK(json.find("no matrix row for this page kind") != std::string::npos);
}

}  // namespace

int main() {
  TestVersionsMatchHeaders();
  TestStableOrder();
  TestMatrixJson();
  TestReportJson();
  std::printf("test_abi_matrix: all checks passed\n");
  return 0;
}
