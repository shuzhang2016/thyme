// ARCH-11 end-to-end: supervisor creates a v3 LatestSlot page, the role
// attaches through the generated-style descriptor table and publishes, and the
// read-only probe identifies/reads the same page from its header + geometry
// block. Also covers the attach rejection matrix and the v2 body refusal.
#include "thyme/app/mp_runtime.h"
#include "thyme/app/mp_supervisor.h"
#include "thyme/check.h"
#include "thyme/contracts/frame.h"
#include "thyme/probe/probe.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/latest_slot_page.h"
#include "thyme/runtime/shm_region.h"

#include "test_wait.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace thyme;
using namespace thyme::probe;

namespace {

using Wire = Frame<struct PipelinePayload>;
struct PipelinePayload {
  uint32_t counter;
  uint32_t marker;
};
using Model = LatestSlotLayoutModel<Wire>;

// A stable payload identity in the shape the generator emits from its ABI-11
// contract layout table. The value is arbitrary but never 0.
constexpr uint64_t kPayloadHash = 0x8F1E2D3C4B5A6978ULL;
constexpr uint32_t kWrittenCounter = 0x5A5A1234u;
constexpr uint32_t kWrittenMarker = 0xC3C3ABCDu;
constexpr const char* kSuffix = "_latest";

bool CreatePage(ShmRegion& region, const char* name, uint64_t feature_bits,
                ShmPageError* error) {
  return CreateLatestSlotPage<Wire>(region, name, kPayloadHash, feature_bits,
                                    error) != nullptr;
}

bool OpenPage(ShmRegion& region, const char* name, uint64_t timeout_ns,
              uint64_t feature_bits, ShmPageError* error) {
  return OpenLatestSlotPage<Wire>(region, name, kPayloadHash, timeout_ns,
                                  feature_bits, error) != nullptr;
}

// Mirrors the generated kGraphShmKindSpecs row.
constexpr app::MpShmKindSpec kSpecs[] = {
    {Model::kRegionBytes, sizeof(Wire), Model::kCellStride, kPayloadHash, 0,
     &CreatePage, &OpenPage},
};
static_assert(kSpecs[0].region_bytes == Model::kRegionBytes,
              "generated region size must come from the model");

constexpr app::MpProcess kProcs[] = {{"p", "p"}, {"c", "c"}};
constexpr app::MpNode kNodes[] = {{"n0", "producer", "p", 10},
                                  {"n1", "consumer", "c", 10}};
constexpr app::MpShmChannel kChannels[] = {{kSuffix, 0, "p", "c"}};

app::MpSupervisorConfig MakeConfig() {
  app::MpSupervisorConfig cfg;
  cfg.processes = kProcs;
  cfg.nodes = kNodes;
  cfg.shm_channels = kChannels;
  cfg.slot_specs = kSpecs;
  cfg.health_n = 2;
  return cfg;
}

std::string Prefix() {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/c9_%d", static_cast<int>(getpid()));
  return buf;
}

uint64_t HashShm(const std::string& name) {
  const int fd = shm_open(name.c_str(), O_RDONLY, 0600);
  CHECK(fd >= 0);
  struct stat st {};
  CHECK(fstat(fd, &st) == 0);
  const auto size = static_cast<std::size_t>(st.st_size);
  void* p = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  CHECK(p != MAP_FAILED);
  const uint8_t* bytes = static_cast<const uint8_t*>(p);
  uint64_t h = 14695981039346656037ULL;
  for (std::size_t i = 0; i < size; ++i) {
    h ^= bytes[i];
    h *= 1099511628211ULL;
  }
  munmap(p, size);
  close(fd);
  return h;
}

int RunRole(int argc, char** argv) {
  const char* role = nullptr;
  const char* prefix = nullptr;
  int proc_idx = -1;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--role") == 0 && i + 1 < argc) role = argv[++i];
    else if (std::strcmp(argv[i], "--shm-prefix") == 0 && i + 1 < argc)
      prefix = argv[++i];
    else if (std::strcmp(argv[i], "--proc-idx") == 0 && i + 1 < argc)
      proc_idx = std::atoi(argv[++i]);
  }
  if (role == nullptr || prefix == nullptr || proc_idx < 0) return 2;

  app::MpRoleSession session;
  if (!session.Open(prefix, nullptr, 2, app::kMpOpenTimeoutNs)) return 2;
  app::MpRoleChannels channels;
  if (!channels.Open(prefix, role, kChannels, kSpecs)) return 2;
  ShmRegion* region = channels.region(0);
  if (region == nullptr || region->data() == nullptr) return 2;
  auto* page = reinterpret_cast<LatestSlotPage<Wire>*>(region->data());

  bool ok = false;
  if (std::strcmp(role, "p") == 0) {
    Wire* slot = page->slot.BeginWrite();
    slot->payload.counter = kWrittenCounter;
    slot->payload.marker = kWrittenMarker;
    page->slot.CommitWrite();
    ok = true;
  } else {
    ok = thyme_test::WaitFor(
        [&] {
          Wire out{};
          return page->slot.Read(out) && out.payload.counter == kWrittenCounter &&
                 out.payload.marker == kWrittenMarker;
        },
        2000000000ULL);
  }
  if (!ok) return 3;
  session.ctrl()->proc_loops[proc_idx].store(1, std::memory_order_release);
  session.WaitUntilStop();
  return 0;
}

void TestSupervisorRoleProbe() {
  const std::string prefix = Prefix();
  const std::string name = prefix + kSuffix;
  ShmRegion::UnlinkName(name.c_str());

  app::MpSupervisorSession session(MakeConfig());
  CHECK(session.Prepare(nullptr));
  CHECK(session.ForkRoles("/proc/self/exe"));
  CHECK(thyme_test::WaitFor(
      [&] {
        return session.ctrl()->proc_loops[0].load(std::memory_order_acquire) >= 1 &&
               session.ctrl()->proc_loops[1].load(std::memory_order_acquire) >= 1;
      },
      5000000000ULL));

  const uint64_t hash_before = HashShm(name);

  ProbeApi api;
  Options options;
  options.prefixes = {(prefix + kSuffix).substr(1)};
  api.SetOptions(options);

  std::vector<RegionInfo> rows;
  CHECK(api.List(rows) == Status::kOk);
  const RegionInfo* row = nullptr;
  for (const auto& r : rows) {
    if (r.name == name) row = &r;
  }
  CHECK(row != nullptr);
  CHECK(row->kind == Kind::kLatestSlot);
  CHECK(row->abi_version == kLatestSlotPageAbiVersion);
  CHECK(row->slots == 2);
  CHECK(row->slot_bytes == sizeof(Wire));
  CHECK(row->stride == Model::kCellStride);
  CHECK(row->region_bytes >= Model::kRegionBytes);
  CHECK(row->geometry_source == "page");
  CHECK(row->compatible);

  RegionInfo info;
  CHECK(api.Info(name, info) == Status::kOk);
  CHECK(info.kind == Kind::kLatestSlot);
  CHECK(info.slot_bytes == sizeof(Wire));
  CHECK(info.stride == Model::kCellStride);

  Sample newest;
  CHECK(api.Newest(name, &newest) == Status::kOk);
  CHECK(newest.sequence >= 1);
  CHECK(newest.size == sizeof(Wire));

  std::vector<EchoSample> echoes;
  CHECK(api.Echo(name, 1, sizeof(Wire), 2000000000ULL, &echoes) ==
        Status::kOk);
  CHECK(echoes.size() == 1);
  CHECK(echoes[0].bytes.size() == sizeof(Wire));
  Wire echo{};
  std::memcpy(&echo, echoes[0].bytes.data(), sizeof(Wire));
  CHECK(echo.payload.counter == kWrittenCounter);
  CHECK(echo.payload.marker == kWrittenMarker);

  // Probe is read-only: page bytes are unchanged by List/Info/Newest/Echo.
  CHECK(HashShm(name) == hash_before);

  // Attach rejection matrix against the live supervisor-created page.
  ShmRegion wrong_layout;
  ShmPageError error;
  CHECK(OpenLatestSlotPage<Wire>(wrong_layout, name.c_str(), kPayloadHash ^ 1u,
                                 0, 0, &error) == nullptr);
  CHECK(error.code == ShmPageErrc::kBadLayoutHash);
  ShmRegion wrong_feature;
  CHECK(OpenLatestSlotPage<Wire>(wrong_feature, name.c_str(), kPayloadHash, 0,
                                 1, &error) == nullptr);
  CHECK(error.code == ShmPageErrc::kBadFeatureBits);

  const uint64_t hash_after_attach_checks = HashShm(name);
  CHECK(hash_after_attach_checks == hash_before);

  session.SignalStop();
  CHECK(session.ReapChildren());
  session.Cleanup();
  ShmRegion::UnlinkName(name.c_str());
}

void TestLegacyV2Rejected() {
  const std::string name = Prefix() + "_legacy";
  ShmRegion::UnlinkName(name.c_str());
  ShmRegion legacy;
  CHECK(legacy.Create<LatestSlot<Wire>>(name.c_str(), 4096) != nullptr);

  ProbeApi api;
  Options options;
  options.prefixes = {(name).substr(1)};
  api.SetOptions(options);
  api.AddManifestEntry(
      ManifestEntry{name, Kind::kLatestSlot, sizeof(Wire), 2, 0, "", false});
  RegionInfo info;
  CHECK(api.Info(name, info) == Status::kBadGeometry);
  CHECK(!info.compatible);
  CHECK(info.note.find("legacy v2") != std::string::npos);
  ShmRegion::UnlinkName(name.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--role") == 0) {
    return RunRole(argc, argv);
  }
  TestSupervisorRoleProbe();
  TestLegacyV2Rejected();
  std::printf("test_latest_slot_pipeline: all checks passed\n");
  return 0;
}
