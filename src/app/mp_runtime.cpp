#include "thyme/app/mp_runtime.h"

#include "thyme/app/mp_session_identity.h"
#include "thyme/app/record_mirror.h"
#include "thyme/runtime/log.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace thyme {
namespace app {
namespace {

// True when `pid` appears in `prefix` as a standalone decimal run. The SHM
// prefix contract embeds the owning supervisor's pid (mp_control.h), but the
// format itself is configurable, so match the number rather than a position.
bool PrefixNamesSession(const char* prefix, uint32_t pid) {
  if (prefix == nullptr || pid == 0) return false;
  char needle[16];
  const int n = std::snprintf(needle, sizeof(needle), "%u",
                              static_cast<unsigned>(pid));
  if (n <= 0) return false;
  const std::size_t len = static_cast<std::size_t>(n);
  for (const char* p = std::strstr(prefix, needle); p != nullptr;
       p = std::strstr(p + 1, needle)) {
    const bool left_ok = p == prefix || p[-1] < '0' || p[-1] > '9';
    const char after = p[len];
    const bool right_ok = after == '\0' || after < '0' || after > '9';
    if (left_ok && right_ok) return true;
  }
  return false;
}

}  // namespace

bool MpRoleSession::Open(const char* prefix, const char* record_shm,
                         std::size_t health_n, uint64_t timeout_ns,
                         uint64_t expected_config_hash) {
  return Open(prefix, record_shm, health_n, timeout_ns, expected_config_hash,
              /*record_ring=*/false, /*expected_graph_hash=*/0);
}

bool MpRoleSession::Open(const char* prefix, const char* record_shm,
                         std::size_t health_n, uint64_t timeout_ns,
                         uint64_t expected_config_hash, bool record_ring) {
  return Open(prefix, record_shm, health_n, timeout_ns, expected_config_hash,
              record_ring, /*expected_graph_hash=*/0);
}

bool MpRoleSession::Open(const char* prefix, const char* record_shm,
                         std::size_t health_n, uint64_t timeout_ns,
                         uint64_t expected_config_hash, bool record_ring,
                         uint64_t expected_graph_hash) {
  if (prefix == nullptr) return false;
  char nm_h[64], nm_c[64];
  MpName(nm_h, sizeof(nm_h), prefix, "_h");
  MpName(nm_c, sizeof(nm_c), prefix, "_c");
  ShmPageError page_error;
  char diagnostic[160];
  if (!health_reg_.OpenDescribed(nm_h, thyme::SharedHealthPage::Spec(),
                                 timeout_ns, false, &page_error)) {
    FormatShmPageError(page_error, diagnostic, sizeof(diagnostic));
    THYME_LOG_ERROR("MpRoleSession: SharedHealthPage attach failed at %s: %s",
                    nm_h, diagnostic);
    return false;
  }
  if (!ctrl_reg_.OpenDescribed(nm_c, MpControl::Spec(), timeout_ns, false,
                               &page_error)) {
    FormatShmPageError(page_error, diagnostic, sizeof(diagnostic));
    THYME_LOG_ERROR("MpRoleSession: MpControl attach failed at %s: %s", nm_c,
                    diagnostic);
    health_reg_.Unmap();
    return false;
  }
  auto* page = static_cast<thyme::SharedHealthPage*>(health_reg_.data());
  if (page == nullptr || !page->Validate(&page_error)) {
    FormatShmPageError(page_error, diagnostic, sizeof(diagnostic));
    THYME_LOG_ERROR("MpRoleSession: invalid SharedHealthPage at %s: %s", nm_h,
                    diagnostic);
    Close();
    return false;
  }
  const uint32_t page_workers = page->worker_count();
  if (page_workers != static_cast<uint32_t>(health_n)) {
    THYME_LOG_ERROR(
        "MpRoleSession: SharedHealthPage worker_count=%u != health_n=%zu",
        page_workers, health_n);
    Close();
    return false;
  }
  // OPS-02 session identity: the page carries the supervisor pid, and the
  // name prefix must embed that same pid. A prefix pointing at a foreign or
  // recycled session is rejected before any role data is read.
  const uint32_t session_id = page->session_id();
  if (!PrefixNamesSession(prefix, session_id)) {
    THYME_LOG_ERROR("MpRoleSession: session id %u not named by shm prefix '%s'",
                    static_cast<unsigned>(session_id), prefix);
    Close();
    return false;
  }
  auto* ctrl = static_cast<MpControl*>(ctrl_reg_.data());
  if (ctrl == nullptr || !ctrl->Validate(&page_error)) {
    FormatShmPageError(page_error, diagnostic, sizeof(diagnostic));
    THYME_LOG_ERROR("MpRoleSession: invalid MpControl at %s: %s", nm_c,
                    diagnostic);
    Close();
    return false;
  }
  if (expected_config_hash != 0 &&
      VerifyMpSessionIdentity(prefix, expected_config_hash) ==
          SessionIdentityCheck::kMismatch) {
    THYME_LOG_ERROR("MpRoleSession: config hash mismatch for prefix '%s'",
                    prefix);
    Close();
    return false;
  }
  // ARCH-17: cross-process identity page. This is the check the process-local
  // registry cannot provide for a forked role.
  {
    const MpSessionIdentityPage* identity = nullptr;
    const SessionPageCheck check = OpenMpSessionIdentityPage(
        identity_reg_, prefix, expected_config_hash, expected_graph_hash,
        static_cast<uint32_t>(health_n), &identity);
    if (check == SessionPageCheck::kMismatch) {
      THYME_LOG_ERROR(
          "MpRoleSession: identity page mismatch for prefix '%s' "
          "(config/graph/health_n)",
          prefix);
      Close();
      return false;
    }
    if (check == SessionPageCheck::kBadPage) {
      THYME_LOG_ERROR("MpRoleSession: invalid identity page for prefix '%s'",
                      prefix);
      Close();
      return false;
    }
    if (check == SessionPageCheck::kMissing) {
      THYME_LOG_WARN(
          "MpRoleSession: no identity page for prefix '%s' (pre-ARCH-17 "
          "session); falling back to header checks",
          prefix);
    }
    identity_ = identity;
  }
  health_page_ = page;
  ctrl_ = ctrl;

  if (record_shm != nullptr && record_shm[0] != '\0') {
    const bool attached = record_ring
                              ? record_mirror::AttachRingReadWrite(record_shm, timeout_ns)
                              : record_mirror::AttachReadWrite(record_shm, timeout_ns);
    if (!attached) {
      THYME_LOG_ERROR("MpRoleSession: record mirror attach %s failed",
                      record_shm);
      Close();
      return false;
    }
  }
  return true;
}

void MpRoleSession::Close() {
  health_reg_.Unmap();
  ctrl_reg_.Unmap();
  identity_reg_.Unmap();
  health_page_ = nullptr;
  ctrl_ = nullptr;
  identity_ = nullptr;
}

bool MpRoleSession::stop_requested() const {
  if (thyme::App::stop_requested()) return true;
  return ctrl_ != nullptr &&
         ctrl_->stop.load(std::memory_order_acquire) != 0;
}

void MpRoleSession::WaitUntilStop() {
  while (!stop_requested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

bool MpRoleChannels::Open(const char* prefix, const char* role,
                          thyme::span<const MpShmChannel> channels,
                          thyme::span<const MpShmKindSpec> slot_specs,
                          uint64_t timeout_ns) {
  n_ = 0;
  for (const auto& ch : channels) {
    if (std::strcmp(ch.from_process, ch.to_process) == 0) continue;
    // Spec §6: only open channels this role is the from or to of.
    if (std::strcmp(ch.from_process, role) != 0 &&
        std::strcmp(ch.to_process, role) != 0)
      continue;
    if (ch.kind < 0 ||
        static_cast<std::size_t>(ch.kind) >= slot_specs.size()) {
      THYME_LOG_ERROR(
          "MpRoleChannels: channel kind %d out of range for role '%s'",
          ch.kind, role);
      Unmap();
      return false;
    }
    const MpShmKindSpec& spec =
        slot_specs[static_cast<std::size_t>(ch.kind)];
    // One ShmRegion per kind: refuse a second same-kind channel rather than
    // silently OpenRetry-overwriting the first mapping.
    if (has(ch.kind)) {
      THYME_LOG_ERROR("MpRoleChannels: duplicate SHM kind %d for role '%s'",
                      ch.kind, role);
      Unmap();
      return false;
    }
    if (n_ >= kMaxKinds) {
      THYME_LOG_ERROR("MpRoleChannels: too many kinds for role '%s'", role);
      Unmap();
      return false;
    }
    if (spec.open_page == nullptr || spec.payload_layout_hash == 0) {
      THYME_LOG_ERROR(
          "MpRoleChannels: SHM kind %d has no described LatestSlot page spec",
          ch.kind);
      Unmap();
      return false;
    }
    char* nm = names_[n_];
    MpName(nm, 64, prefix, ch.suffix);
    thyme::ShmPageError error;
    if (!spec.open_page(regions_[n_], nm, timeout_ns, spec.feature_bits,
                        &error)) {
      THYME_LOG_ERROR("MpRoleChannels: open LatestSlot page %s failed: %s",
                      nm, ShmPageErrorName(error.code));
      Unmap();
      return false;
    }
    kinds_[n_] = ch.kind;
    ++n_;
  }
  return true;
}

void MpRoleChannels::Unmap() {
  for (std::size_t i = 0; i < n_; ++i) regions_[i].Unmap();
  n_ = 0;
}

bool MpRoleChannels::has(int kind) const {
  for (std::size_t i = 0; i < n_; ++i)
    if (kinds_[i] == kind) return true;
  return false;
}

thyme::ShmRegion* MpRoleChannels::region(int kind) {
  for (std::size_t i = 0; i < n_; ++i)
    if (kinds_[i] == kind) return &regions_[i];
  return nullptr;
}

}  // namespace app
}  // namespace thyme
