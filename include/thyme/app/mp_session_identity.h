#pragma once
#include "thyme/app/mp_supervisor.h"
#include "thyme/runtime/mp_session_identity_page.h"

#include <cstdint>

namespace thyme {
namespace app {

// OPS-02 + ARCH-17: session/config identity for the multi-process SHM set.
//
// The frozen health/control pages cannot grow a config-hash field, so a
// separate described page `<prefix>_id` carries the identity instead. It is
// created by the supervisor before forking roles and removed with the rest of
// the session. Roles attach it read-only and reject a mismatched config hash,
// graph hash or health_n; a genuinely missing page is a pre-ARCH-17 session
// and falls back to the header checks with an explicit warning (never silent).
//
// The process-local registry below remains as a fast in-process path; it is
// not consulted for cross-process roles because they cannot see it.

// FNV-1a 64 over every config input that shapes the SHM set or the role
// behaviour: prefix format, health slot count, process/node tables, SHM
// channels, slot sizes, and both gate tables. Deterministic; no allocation.
uint64_t MpConfigHash(const MpSupervisorConfig& cfg) noexcept;

enum class SessionIdentityCheck : uint8_t {
  kVerified = 0,   // prefix registered locally with the same hash
  kMismatch,       // prefix registered locally with a different hash
  kUnregistered,   // no local record (typical cross-process attach)
};

// Registers/updates the (prefix -> config hash) pair for this process.
bool RegisterMpSessionIdentity(const char* prefix,
                               uint64_t config_hash) noexcept;

// Looks up `prefix` and compares. kUnregistered means the caller cannot
// verify locally and must fall back to the in-header checks.
SessionIdentityCheck VerifyMpSessionIdentity(
    const char* prefix, uint64_t expected_config_hash) noexcept;

}  // namespace app
}  // namespace thyme
