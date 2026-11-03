// OPS-02: session/config identity at the attach API level. The MP SHM wire
// format has no config-hash field (by design), so identity lives in a
// process-local registry plus the in-header session_id/abi_version checks.
#include "thyme/app/mp_runtime.h"
#include "thyme/app/mp_session_identity.h"
#include "thyme/check.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <new>
#include <cstdio>
#include <unistd.h>

using namespace thyme;
using namespace thyme::app;

namespace {

MpSupervisorConfig BaseConfig() {
  static const MpProcess procs[] = {{"a", "a"}, {"b", "b"}};
  static const MpNode nodes[] = {{"n0", "worker", "a", 10},
                                 {"n1", "worker", "b", 20}};
  static const MpShmChannel channels[] = {{"_ab", 0, "a", "b"}};
  static const MpShmKindSpec specs[] = {{4096, 16, 64, 0x1234, 0, nullptr,
                                         nullptr}};
  static const MpRecordGate gates[] = {{"a", 0}};
  static const MpCrossGate cross[] = {{0, 0}};
  MpSupervisorConfig cfg;
  cfg.processes = procs;
  cfg.nodes = nodes;
  cfg.shm_channels = channels;
  cfg.slot_specs = specs;
  cfg.record_gates = gates;
  cfg.cross_gates = cross;
  cfg.health_n = 2;
  return cfg;
}

}  // namespace

int main() {
  MpSupervisorConfig cfg = BaseConfig();
  const uint64_t h = MpConfigHash(cfg);

  // Deterministic for the same config.
  CHECK(MpConfigHash(cfg) == h);

  // Every identity input perturbs the hash.
  {
    MpSupervisorConfig c = BaseConfig();
    c.health_n = 3;
    CHECK(MpConfigHash(c) != h);
  }
  {
    MpSupervisorConfig c = BaseConfig();
    static const MpProcess procs[] = {{"a", "a"}, {"b", "renamed"}};
    c.processes = procs;
    CHECK(MpConfigHash(c) != h);
  }
  {
    MpSupervisorConfig c = BaseConfig();
    static const MpShmChannel channels[] = {{"_ab", 0, "a", "b"},
                                            {"_ba", 0, "b", "a"}};
    c.shm_channels = channels;
    CHECK(MpConfigHash(c) != h);
  }
  {
    MpSupervisorConfig c = BaseConfig();
    static const MpShmKindSpec specs[] = {{8192, 16, 64, 0x1234, 0, nullptr,
                                           nullptr}};
    c.slot_specs = specs;
    CHECK(MpConfigHash(c) != h);
  }
  {
    MpSupervisorConfig c = BaseConfig();
    c.shm_prefix_fmt = "/other_%d";
    CHECK(MpConfigHash(c) != h);
  }

  // Registry: exact match verifies, a different hash is a hard mismatch, and
  // an unknown prefix is explicitly unverifiable (cross-process attach).
  CHECK(RegisterMpSessionIdentity("/c9_4242", h));
  CHECK(VerifyMpSessionIdentity("/c9_4242", h) ==
        SessionIdentityCheck::kVerified);
  CHECK(VerifyMpSessionIdentity("/c9_4242", h ^ 0x1ULL) ==
        SessionIdentityCheck::kMismatch);
  CHECK(VerifyMpSessionIdentity("/c9_9999", h) ==
        SessionIdentityCheck::kUnregistered);
  // expected == 0 means "no expected identity": never a mismatch.
  CHECK(VerifyMpSessionIdentity("/c9_4242", 0) ==
        SessionIdentityCheck::kUnregistered);
  // Re-registering the same prefix updates the hash.
  CHECK(RegisterMpSessionIdentity("/c9_4242", h ^ 0x2ULL));
  CHECK(VerifyMpSessionIdentity("/c9_4242", h) ==
        SessionIdentityCheck::kMismatch);

  // ARCH-17: the shared identity page carries the same identity across
  // processes and rejects config/graph/health_n mismatches.
  char prefix[64];
  std::snprintf(prefix, sizeof(prefix), "/c9sid_%d",
                static_cast<int>(getpid()));
  char page_name[64];
  CHECK(MpSessionIdentityName(prefix, page_name, sizeof(page_name)));
  thyme::ShmRegion::UnlinkName(page_name);
  {
    thyme::ShmRegion region;
    CHECK(CreateMpSessionIdentityPage(region, prefix, h, 0xABCD, 4242, 99, 2));
    const MpSessionIdentityPage* page = nullptr;
    CHECK(OpenMpSessionIdentityPage(region, prefix, h, 0xABCD, 2, &page) ==
          SessionPageCheck::kOk);
    CHECK(page != nullptr);
    CHECK(page->config_hash.load() == h);
    CHECK(page->graph_hash.load() == 0xABCD);
    CHECK(page->supervisor_pid.load() == 4242);
    CHECK(page->supervisor_start_ns.load() == 99);
    CHECK(page->health_n.load() == 2);
    region.Unmap();

    thyme::ShmRegion opened;
    CHECK(OpenMpSessionIdentityPage(opened, prefix, h ^ 1, 0xABCD, 2,
                                    &page) == SessionPageCheck::kMismatch);
    opened.Unmap();
    CHECK(OpenMpSessionIdentityPage(opened, prefix, h, 0xABCE, 2, &page) ==
          SessionPageCheck::kMismatch);
    opened.Unmap();
    CHECK(OpenMpSessionIdentityPage(opened, prefix, h, 0xABCD, 3, &page) ==
          SessionPageCheck::kMismatch);
    opened.Unmap();
    // expected 0 skips the corresponding comparison.
    CHECK(OpenMpSessionIdentityPage(opened, prefix, 0, 0, 2, &page) ==
          SessionPageCheck::kOk);
    opened.Unmap();
  }

  // ARCH-17/FUP-39: the role runtime forwards the expected graph identity to
  // the shared page instead of silently disabling the graph comparison.
  char role_prefix[64];
  std::snprintf(role_prefix, sizeof(role_prefix), "/c9role_%d",
                static_cast<int>(getpid()));
  char health_name[64];
  char control_name[64];
  char role_identity_name[64];
  MpName(health_name, sizeof(health_name), role_prefix, "_h");
  MpName(control_name, sizeof(control_name), role_prefix, "_c");
  CHECK(MpSessionIdentityName(role_prefix, role_identity_name,
                              sizeof(role_identity_name)));
  thyme::ShmRegion::UnlinkName(health_name);
  thyme::ShmRegion::UnlinkName(control_name);
  thyme::ShmRegion::UnlinkName(role_identity_name);
  {
    constexpr uint64_t graph = 0xABCD;
    thyme::ShmRegion health;
    CHECK(health.Create(health_name, sizeof(thyme::SharedHealthPage)));
    auto* health_page = static_cast<thyme::SharedHealthPage*>(health.data());
    CHECK(health_page->Init(static_cast<uint32_t>(getpid()), 2));
    thyme::ShmRegion control;
    CHECK(control.CreateDescribed(control_name, MpControl::Spec()));
    thyme::ShmRegion identity;
    CHECK(CreateMpSessionIdentityPage(identity, role_prefix, h, graph,
                                      static_cast<uint32_t>(getpid()), 99, 2));

    MpRoleSession role;
    CHECK(role.Open(role_prefix, nullptr, 2, 0, h, false, graph));
    CHECK(role.identity_page() != nullptr);
    CHECK(role.identity_page()->graph_hash.load() == graph);
    role.Close();

    CHECK(!role.Open(role_prefix, nullptr, 2, 0, h, false, graph ^ 1));
    CHECK(role.identity_page() == nullptr);

    CHECK(role.Open(role_prefix, nullptr, 2, 0, h, false, 0));
    CHECK(role.identity_page() != nullptr);
    role.Close();

    CHECK(!role.Open(role_prefix, nullptr, 2, 0, h ^ 1, false, graph));
    CHECK(role.identity_page() == nullptr);
  }
  thyme::ShmRegion::UnlinkName(health_name);
  thyme::ShmRegion::UnlinkName(control_name);
  thyme::ShmRegion::UnlinkName(role_identity_name);

  // Legacy session: no page at all is reported as missing, not as a mismatch.
  thyme::ShmRegion::UnlinkName(page_name);
  {
    thyme::ShmRegion missing;
    const MpSessionIdentityPage* page = nullptr;
    CHECK(OpenMpSessionIdentityPage(missing, prefix, h, 0, 2, &page) ==
          SessionPageCheck::kMissing);
  }

  // A page with the wrong magic/version is a hard bad-page, never a fallback.
  {
    thyme::ShmRegion region;
    CHECK(region.Create(page_name, sizeof(MpSessionIdentityPage)));
    auto* raw = new (region.data()) MpSessionIdentityPage;
    thyme::InitShmSuperHeader(&raw->super, MpSessionIdentityPage::Spec());
    raw->super.abi_version.store(99);
    const MpSessionIdentityPage* page = nullptr;
    CHECK(OpenMpSessionIdentityPage(region, prefix, h, 0, 2, &page) ==
          SessionPageCheck::kBadPage);
    region.Unmap();
  }
  thyme::ShmRegion::UnlinkName(page_name);
  return 0;
}
