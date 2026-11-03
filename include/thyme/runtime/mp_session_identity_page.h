#pragma once
#include "thyme/runtime/shm_page.h"
#include "thyme/runtime/shm_region.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>

namespace thyme {

// Identity page name: `<prefix>_id`. Returns false when the buffer is too
// small (the page is then not created/attached).
bool MpSessionIdentityName(const char* prefix, char* out, std::size_t capacity);

// ARCH-17 page v1: super-header + the identity words. `graph_hash == 0` means
// "not provided by this build" and is not compared; `config_hash` is always
// compared when the attacher supplies a non-zero expected value.
inline constexpr uint64_t kMpSessionIdentityMagic = 0x5448594D53494431ULL;
inline constexpr uint32_t kMpSessionIdentityVersion = 1;

struct alignas(64) MpSessionIdentityPage {
  static constexpr uint64_t kMagic = kMpSessionIdentityMagic;
  static constexpr uint32_t kVersion = kMpSessionIdentityVersion;

  ShmSuperHeader super;
  std::atomic<uint64_t> config_hash{0};
  std::atomic<uint64_t> graph_hash{0};
  std::atomic<uint64_t> supervisor_start_ns{0};
  std::atomic<uint32_t> supervisor_pid{0};
  std::atomic<uint32_t> health_n{0};
  std::atomic<uint32_t> role_count{0};
  uint32_t reserved0 = 0;
  uint64_t reserved[2] = {};

  static constexpr uint64_t LayoutHash() {
    return ShmLayoutHash(
        kMagic, kVersion, static_cast<uint64_t>(sizeof(MpSessionIdentityPage)),
        static_cast<uint64_t>(alignof(MpSessionIdentityPage)),
        static_cast<uint64_t>(offsetof(MpSessionIdentityPage, super)),
        static_cast<uint64_t>(offsetof(MpSessionIdentityPage, config_hash)),
        static_cast<uint64_t>(offsetof(MpSessionIdentityPage, graph_hash)),
        static_cast<uint64_t>(
            offsetof(MpSessionIdentityPage, supervisor_start_ns)),
        static_cast<uint64_t>(offsetof(MpSessionIdentityPage, supervisor_pid)),
        static_cast<uint64_t>(offsetof(MpSessionIdentityPage, health_n)),
        static_cast<uint64_t>(offsetof(MpSessionIdentityPage, role_count)));
  }

  static constexpr ShmPageSpec Spec() {
    return ShmPageSpec{kMagic, kVersion, LayoutHash(),
                       static_cast<uint64_t>(sizeof(MpSessionIdentityPage)), 0};
  }
};

static_assert(sizeof(MpSessionIdentityPage) == 128,
              "identity page is super-header + one identity cache line");
static_assert(offsetof(MpSessionIdentityPage, super) == 0,
              "super-header leads the identity page");
static_assert(offsetof(MpSessionIdentityPage, config_hash) == 64,
              "config hash offset is page ABI");

// Result of attaching the identity page.
enum class SessionPageCheck : uint8_t {
  kOk = 0,
  kMissing,   // pre-ARCH-17 session: no `<prefix>_id`
  kMismatch,  // config hash / health_n / graph hash disagree
  kBadPage,   // page exists but magic/version/layout/size is wrong
};

// Creates `<prefix>_id` in `region` (already sized to Spec().page_bytes or
// larger). Returns false on region/validation failure.
bool CreateMpSessionIdentityPage(ShmRegion& region, const char* prefix,
                                 uint64_t config_hash, uint64_t graph_hash,
                                 uint32_t supervisor_pid,
                                 uint64_t supervisor_start_ns,
                                 uint32_t health_n);

// Attaches `<prefix>_id` read-only. `expected_config_hash == 0` skips the
// config comparison; `expected_graph_hash == 0` skips the graph comparison.
// On kOk, `page` points into `region` (valid until Unmap).
SessionPageCheck OpenMpSessionIdentityPage(ShmRegion& region,
                                           const char* prefix,
                                           uint64_t expected_config_hash,
                                           uint64_t expected_graph_hash,
                                           uint32_t expected_health_n,
                                           const MpSessionIdentityPage** page);


inline bool MpSessionIdentityName(const char* prefix, char* out,
                                 std::size_t capacity) {
  if (prefix == nullptr || out == nullptr || capacity == 0) return false;
  const int n = std::snprintf(out, capacity, "%s_id", prefix);
  return n > 0 && static_cast<std::size_t>(n) < capacity;
}

inline bool CreateMpSessionIdentityPage(ShmRegion& region, const char* prefix,
                                        uint64_t config_hash,
                                        uint64_t graph_hash,
                                        uint32_t supervisor_pid,
                                        uint64_t supervisor_start_ns,
                                        uint32_t health_n) {
  char name[64];
  if (!MpSessionIdentityName(prefix, name, sizeof(name))) return false;
  if (!region.Create(name, sizeof(MpSessionIdentityPage))) return false;
  auto* page = new (region.data()) MpSessionIdentityPage;
  page->config_hash.store(config_hash, std::memory_order_relaxed);
  page->graph_hash.store(graph_hash, std::memory_order_relaxed);
  page->supervisor_pid.store(supervisor_pid, std::memory_order_relaxed);
  page->supervisor_start_ns.store(supervisor_start_ns,
                                  std::memory_order_relaxed);
  page->health_n.store(health_n, std::memory_order_relaxed);
  page->role_count.store(0, std::memory_order_relaxed);
  InitShmSuperHeader(&page->super, MpSessionIdentityPage::Spec());
  ShmPageError error;
  if (ValidateShmSuperHeader(region.data(), region.size(),
                             MpSessionIdentityPage::Spec(), &error) !=
      ShmPageErrc::kOk) {
    region.Unmap();
    ShmRegion::UnlinkName(name);
    return false;
  }
  return true;
}

inline SessionPageCheck OpenMpSessionIdentityPage(
    ShmRegion& region, const char* prefix, uint64_t expected_config_hash,
    uint64_t expected_graph_hash, uint32_t expected_health_n,
    const MpSessionIdentityPage** page) {
  if (page != nullptr) *page = nullptr;
  char name[64];
  if (!MpSessionIdentityName(prefix, name, sizeof(name))) {
    return SessionPageCheck::kBadPage;
  }
  ShmPageError error;
  if (!region.OpenDescribed(name, MpSessionIdentityPage::Spec(), 0, true,
                            &error)) {
    return error.code == ShmPageErrc::kNotReady
               ? SessionPageCheck::kMissing
               : SessionPageCheck::kBadPage;
  }
  const auto* p = static_cast<const MpSessionIdentityPage*>(region.data());
  if (expected_config_hash != 0 &&
      p->config_hash.load(std::memory_order_acquire) != expected_config_hash) {
    return SessionPageCheck::kMismatch;
  }
  if (expected_graph_hash != 0 &&
      p->graph_hash.load(std::memory_order_acquire) != expected_graph_hash) {
    return SessionPageCheck::kMismatch;
  }
  if (expected_health_n != 0 &&
      p->health_n.load(std::memory_order_acquire) != expected_health_n) {
    return SessionPageCheck::kMismatch;
  }
  if (page != nullptr) *page = p;
  return SessionPageCheck::kOk;
}

}  // namespace thyme
