#include "thyme/check.h"
#include "thyme/app/mp_control.h"
#include "thyme/runtime/shared_health.h"
#include "thyme/runtime/shm_region.h"

#include "test_shm.h"

#include <chrono>
#include <cstring>
#include <new>
#include <string>
#include <thread>

using namespace thyme;

constexpr uint64_t kLayoutA = ShmLayoutHash(1, 64, 128, 4096);
constexpr uint64_t kLayoutB = ShmLayoutHash(1, 64, 128, 8192);
static_assert(kLayoutA != kLayoutB, "layout hash must cover every part");
static_assert(kLayoutA == ShmLayoutHash(1, 64, 128, 4096),
              "layout hash must be deterministic");

namespace {

// Archived ARCH-11 pre-migration images. Health v2 had magic@0/version@8 but
// no super-header (page bytes = 4160); MpControl v2 had a bare abi_version
// word at offset 0 and no magic. Only the fields the attach path inspects are
// written; the rest stays zero.
void WriteHealthV2Image(void* base, uint32_t version, uint32_t session_id,
                        uint32_t workers) {
  auto* bytes = static_cast<uint8_t*>(base);
  std::memset(bytes, 0, 4160);
  std::memcpy(bytes + 0, &SharedHealthPage::kMagic, sizeof(uint64_t));
  std::memcpy(bytes + 8, &version, sizeof(uint32_t));
  std::memcpy(bytes + 12, &session_id, sizeof(uint32_t));
  std::memcpy(bytes + 16, &workers, sizeof(uint32_t));
}

void WriteMpControlV2Image(void* base, uint32_t version) {
  auto* bytes = static_cast<uint8_t*>(base);
  std::memset(bytes, 0, 232);
  std::memcpy(bytes + 0, &version, sizeof(uint32_t));
}

}  // namespace

int main() {
  const ShmPageSpec spec{0x53594d50u, 1, kLayoutA, 4096, 0};
  CHECK(IsShmPageSpecValid(spec));
  CHECK(!IsShmPageSpecValid(ShmPageSpec{}));

  ShmSuperHeader header;
  InitShmSuperHeader(&header, spec);
  ShmPageError error;
  CHECK(ValidateShmSuperHeader(&header, sizeof(header), spec, &error) ==
        ShmPageErrc::kOk);
  CHECK(error.code == ShmPageErrc::kOk);
  CHECK(ValidateShmSuperHeader(&header, 12, spec, &error) ==
        ShmPageErrc::kTooSmall);
  CHECK(error.expected == sizeof(ShmSuperHeader) && error.actual == 12);
  CHECK(ValidateShmSuperHeader(&header, sizeof(header), ShmPageSpec{}, &error) ==
        ShmPageErrc::kInvalidSpec);

  auto changed = spec;
  changed.magic ^= 1;
  CHECK(ValidateShmSuperHeader(&header, sizeof(header), changed, &error) ==
        ShmPageErrc::kBadMagic);
  CHECK(error.expected == changed.magic && error.actual == spec.magic);
  changed = spec;
  changed.abi_version = 2;
  CHECK(ValidateShmSuperHeader(&header, sizeof(header), changed, &error) ==
        ShmPageErrc::kBadAbiVersion);
  changed = spec;
  changed.layout_hash ^= 1;
  CHECK(ValidateShmSuperHeader(&header, sizeof(header), changed, &error) ==
        ShmPageErrc::kBadLayoutHash);
  changed = spec;
  changed.page_bytes = 8192;
  CHECK(ValidateShmSuperHeader(&header, sizeof(header), changed, &error) ==
        ShmPageErrc::kBadPageBytes);
  changed = spec;
  changed.feature_bits = 1;
  CHECK(ValidateShmSuperHeader(&header, sizeof(header), changed, &error) ==
        ShmPageErrc::kBadFeatureBits);

  header.reserved0 = 1;
  CHECK(ValidateShmSuperHeader(&header, sizeof(header), spec, &error) ==
        ShmPageErrc::kBadReserved);
  InitShmSuperHeader(&header, spec);
  header.reserved[2] = 1;
  CHECK(ValidateShmSuperHeader(&header, sizeof(header), spec, &error) ==
        ShmPageErrc::kBadReserved);

  InitShmSuperHeader(&header, ShmPageSpec{});
  CHECK(header.magic.load(std::memory_order_acquire) == 0);
  CHECK(ValidateShmSuperHeader(&header, sizeof(header), spec, &error) ==
        ShmPageErrc::kNotReady);
  CHECK(std::strcmp(ShmPageErrorName(error.code), "not_ready") == 0);

  const std::string name = test::ShmTestName("described_page");
  ShmRegion::UnlinkName(name.c_str());
  ShmRegion writer;
  CHECK(writer.CreateDescribed(name.c_str(), spec));
  ShmRegion reader;
  CHECK(reader.OpenDescribed(name.c_str(), spec, 0, true, &error));
  CHECK(ValidateShmSuperHeader(reader.data(), reader.size(), spec, &error) ==
        ShmPageErrc::kOk);
  changed = spec;
  changed.layout_hash ^= 1;
  CHECK(!reader.OpenDescribed(name.c_str(), changed, 0, true, &error));
  CHECK(error.code == ShmPageErrc::kBadLayoutHash);
  CHECK(reader.data() == nullptr);
  CHECK(!reader.OpenDescribed(name.c_str(), ShmPageSpec{}, 0, true, &error));
  CHECK(error.code == ShmPageErrc::kInvalidSpec);
  reader.Unmap();
  writer.Unmap();
  ShmRegion::UnlinkName(name.c_str());

  const std::string racing = test::ShmTestName("described_race");
  ShmRegion::UnlinkName(racing.c_str());
  ShmRegion late_writer;
  CHECK(late_writer.Create(racing.c_str(), spec.page_bytes));
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): object lives in SHM.
  auto* late_header = new (late_writer.data()) ShmSuperHeader;
  InitShmSuperHeader(late_header, ShmPageSpec{});
  std::thread publisher([late_header, &spec]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    InitShmSuperHeader(late_header, spec);
  });
  ShmRegion racing_reader;
  CHECK(racing_reader.OpenDescribed(racing.c_str(), spec, 100000000ULL, true,
                                    &error));
  publisher.join();
  CHECK(ValidateShmSuperHeader(racing_reader.data(), racing_reader.size(), spec,
                               &error) == ShmPageErrc::kOk);
  racing_reader.Unmap();
  late_writer.Unmap();
  ShmRegion::UnlinkName(racing.c_str());

  // ---- ARCH-11 migration: the two pages that moved to the super-header ----
  // All nine SHM page kinds carry an explicit version gate; these two were
  // still v2-shaped. Health v2 is rejected by abi_version; MpControl v2 had
  // no magic at all, so its old first word is read as a bad magic. Neither is
  // dual-read.
  {
    const std::string old_health = test::ShmTestName("described_health_v2");
    ShmRegion::UnlinkName(old_health.c_str());
    ShmRegion old_writer;
    CHECK(old_writer.Create(old_health.c_str(), 4160));
    WriteHealthV2Image(old_writer.data(), 2, static_cast<uint32_t>(getpid()), 2);
    ShmRegion old_reader;
    CHECK(!old_reader.OpenDescribed(old_health.c_str(),
                                    SharedHealthPage::Spec(), 0, true,
                                    &error));
    CHECK(error.code == ShmPageErrc::kBadAbiVersion);
    CHECK(error.expected == SharedHealthPage::kVersion);
    CHECK(error.actual == 2);
    CHECK(old_reader.data() == nullptr);
    old_writer.Unmap();
    ShmRegion::UnlinkName(old_health.c_str());

    const std::string old_ctrl = test::ShmTestName("described_control_v2");
    ShmRegion::UnlinkName(old_ctrl.c_str());
    ShmRegion old_ctrl_writer;
    CHECK(old_ctrl_writer.Create(old_ctrl.c_str(), 232));
    WriteMpControlV2Image(old_ctrl_writer.data(), 2);
    ShmRegion old_ctrl_reader;
    CHECK(!old_ctrl_reader.OpenDescribed(old_ctrl.c_str(),
                                         app::MpControl::Spec(), 0, true,
                                         &error));
    CHECK(error.code == ShmPageErrc::kBadMagic);
    CHECK(error.expected == app::MpControl::kMagic);
    CHECK(error.actual == 2);
    CHECK(old_ctrl_reader.data() == nullptr);
    old_ctrl_writer.Unmap();
    ShmRegion::UnlinkName(old_ctrl.c_str());
  }

  // Same-version pages with a wrong layout hash/page size/feature bits are
  // rejected with the exact field in `error`.
  {
    const std::string hname = test::ShmTestName("described_health_v3");
    ShmRegion::UnlinkName(hname.c_str());
    ShmRegion hwriter;
    CHECK(hwriter.CreateDescribed(hname.c_str(), SharedHealthPage::Spec()));
    auto* hpage = static_cast<SharedHealthPage*>(hwriter.data());
    CHECK(hpage->Init(static_cast<uint32_t>(getpid()), 2));

    ShmRegion health_reader;
    auto bad_spec = SharedHealthPage::Spec();
    bad_spec.layout_hash ^= 1;
    CHECK(!health_reader.OpenDescribed(hname.c_str(), bad_spec, 0, true,
                                       &error));
    CHECK(error.code == ShmPageErrc::kBadLayoutHash);
    bad_spec = SharedHealthPage::Spec();
    bad_spec.page_bytes += 64;
    CHECK(!health_reader.OpenDescribed(hname.c_str(), bad_spec, 0, true,
                                       &error));
    CHECK(error.code == ShmPageErrc::kBadPageBytes);
    bad_spec = SharedHealthPage::Spec();
    bad_spec.feature_bits ^= 1;
    CHECK(!health_reader.OpenDescribed(hname.c_str(), bad_spec, 0, true,
                                       &error));
    CHECK(error.code == ShmPageErrc::kBadFeatureBits);
    bad_spec = SharedHealthPage::Spec();
    bad_spec.abi_version += 1;
    CHECK(!reader.OpenDescribed(hname.c_str(), bad_spec, 0, true, &error));
    CHECK(error.code == ShmPageErrc::kBadAbiVersion);
    CHECK(reader.OpenDescribed(hname.c_str(), SharedHealthPage::Spec(), 0, true,
                               &error));
    reader.Unmap();

    hpage->super.reserved[0] = 1;
    CHECK(!reader.OpenDescribed(hname.c_str(), SharedHealthPage::Spec(), 0,
                                true, &error));
    CHECK(error.code == ShmPageErrc::kBadReserved);
    hpage->super.reserved[0] = 0;
    hpage->header.reserved[0] = 1;
    ShmPageError local_error;
    CHECK(!hpage->Validate(&local_error));
    CHECK(local_error.code == ShmPageErrc::kBadReserved);
    hpage->header.reserved[0] = 0;
    CHECK(hpage->Validate(&local_error));
    hwriter.Unmap();
    ShmRegion::UnlinkName(hname.c_str());

    const std::string cname = test::ShmTestName("described_control_v3");
    ShmRegion::UnlinkName(cname.c_str());
    ShmRegion cwriter;
    CHECK(cwriter.CreateDescribed(cname.c_str(), app::MpControl::Spec()));
    auto* cpage = static_cast<app::MpControl*>(cwriter.data());
    cpage->Init();
    CHECK(cpage->Validate(&local_error));
    bad_spec = app::MpControl::Spec();
    bad_spec.layout_hash ^= 1;
    CHECK(!reader.OpenDescribed(cname.c_str(), bad_spec, 0, true, &error));
    CHECK(error.code == ShmPageErrc::kBadLayoutHash);
    bad_spec = app::MpControl::Spec();
    bad_spec.page_bytes -= 64;
    CHECK(!reader.OpenDescribed(cname.c_str(), bad_spec, 0, true, &error));
    CHECK(error.code == ShmPageErrc::kBadPageBytes);
    bad_spec = app::MpControl::Spec();
    bad_spec.feature_bits ^= 1;
    CHECK(!reader.OpenDescribed(cname.c_str(), bad_spec, 0, true, &error));
    CHECK(error.code == ShmPageErrc::kBadFeatureBits);
    CHECK(reader.OpenDescribed(cname.c_str(), app::MpControl::Spec(), 0, true,
                               &error));
    reader.Unmap();
    cpage->reserved[1] = 7;
    CHECK(!cpage->Validate(&local_error));
    CHECK(local_error.code == ShmPageErrc::kBadReserved);
    cpage->reserved[1] = 0;
    CHECK(cpage->Validate(&local_error));
    cwriter.Unmap();
    ShmRegion::UnlinkName(cname.c_str());
  }

  std::printf("test_shm_page_header: OK\n");
  return 0;
}
