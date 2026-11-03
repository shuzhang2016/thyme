#pragma once
#include "thyme/app/mp_control.h"
#include "thyme/app/mp_shm_kind.h"
#include "thyme/app/mp_supervisor.h"
#include "thyme/app/span.h"
#include "thyme/runtime/app.h"
#include "thyme/runtime/mp_session_identity_page.h"
#include "thyme/runtime/shared_health.h"
#include "thyme/runtime/shm_region.h"

#include <cstddef>

namespace thyme {
namespace app {


// Role side of a supervised graph: opens the health page + control page and
// (optionally) attaches the record mirror created by the supervisor. Call
// Open once per process role; Close before exiting the role (children never
// unlink the shared names — the supervisor owns cleanup).
class MpRoleSession {
 public:
  MpRoleSession() = default;
  ~MpRoleSession() { Close(); }
  MpRoleSession(const MpRoleSession&) = delete;
  MpRoleSession& operator=(const MpRoleSession&) = delete;

  // health_n must equal the worker_count the supervisor initialized the page
  // with; a mismatch is a graph/version error and fails the open.
  //
  // OPS-02/ARCH-17 identity checks. Attach validates the headers
  // (SharedHealthPage::Header::session_id names the pid embedded in `prefix`;
  // health/control super-headers must match this build) and then opens the
  // separate `<prefix>_id` identity page. `expected_config_hash` and
  // `expected_graph_hash`, when non-zero, are compared against that page
  // cross-process; health_n must match. Zero graph identity is reserved for
  // legacy/unknown graphs and skips only the graph comparison; generated
  // applications must pass their non-zero graph hash. A page mismatch is a
  // hard failure. A missing page means a pre-ARCH-17 session: the open falls
  // back to header-only checks and logs a warning. The process-local registry
  // is still consulted for in-process attaches. The six-argument overload
  // selects the ring v4 ABI when record_ring is true and keeps graph checking
  // disabled; the original entry keeps the legacy v3 attach behavior.
  bool Open(const char* prefix, const char* record_shm, std::size_t health_n,
            uint64_t timeout_ns = kMpOpenTimeoutNs,
            uint64_t expected_config_hash = 0);
  bool Open(const char* prefix, const char* record_shm, std::size_t health_n,
            uint64_t timeout_ns, uint64_t expected_config_hash, bool record_ring);
  bool Open(const char* prefix, const char* record_shm, std::size_t health_n,
            uint64_t timeout_ns, uint64_t expected_config_hash, bool record_ring,
            uint64_t expected_graph_hash);
  void Close();

  bool stop_requested() const;
  // Polls ctrl->stop and thyme::App::stop_requested until either is set.
  void WaitUntilStop();

  thyme::SharedHealthPage* health_page() const { return health_page_; }
  thyme::HealthSlot* slots() const {
    return health_page_ != nullptr ? health_page_->slots : nullptr;
  }
  MpControl* ctrl() const { return ctrl_; }
  // ARCH-17: null for a pre-identity session (the open succeeded through the
  // legacy fallback and logged a warning).
  const MpSessionIdentityPage* identity_page() const { return identity_; }

 private:
  thyme::ShmRegion health_reg_, ctrl_reg_, identity_reg_;
  thyme::SharedHealthPage* health_page_ = nullptr;
  MpControl* ctrl_ = nullptr;
  const MpSessionIdentityPage* identity_ = nullptr;
};

// One ShmRegion per cross-process channel kind this role participates in
// (table-driven from the graph's channel list). A role that opens no
// cross-process channel ends with count()==0.
class MpRoleChannels {
 public:
  static constexpr std::size_t kMaxKinds = 16;

  bool Open(const char* prefix, const char* role,
            thyme::span<const MpShmChannel> channels,
            thyme::span<const MpShmKindSpec> slot_specs,
            uint64_t timeout_ns = kMpOpenTimeoutNs);
  void Unmap();

  std::size_t count() const { return n_; }
  int kind_at(std::size_t i) const { return kinds_[i]; }
  const char* name_at(std::size_t i) const { return names_[i]; }
  bool has(int kind) const;
  thyme::ShmRegion* region(int kind);

 private:
  thyme::ShmRegion regions_[kMaxKinds];
  int kinds_[kMaxKinds]{};
  char names_[kMaxKinds][64]{};
  std::size_t n_ = 0;
};

}  // namespace app
}  // namespace thyme
