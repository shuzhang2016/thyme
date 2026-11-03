#pragma once

// ARCH-14: one topology-level writer lease shared by recorder, replay and
// real drivers. The holder is session id + pid + /proc starttime + heartbeat.
// A live second writer gets kBusy and does not replace the holder. A dead or
// expired holder is reclaimed explicitly (kStolen), which is how a restarted
// recorder reattaches the same page. Per-channel ProducerGate stays the
// channel lock; SessionLeaseGateMatches is the membership view (the channel
// is joined, and the gate owner is the same process as the session holder).

#include "thyme/runtime/pinned_slot_gate.h"
#include "thyme/runtime/shm_page.h"
#include "thyme/runtime/shm_region.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace thyme {

inline constexpr uint64_t kSessionLeaseMagic = 0x31534C534D594854ULL;  // THYMSLS1
inline constexpr uint32_t kSessionLeaseVersion = 1;
inline constexpr uint32_t kSessionLeaseMaxMembers = 32;
inline constexpr uint64_t kSessionLeaseUnheld = 0;
inline constexpr uint64_t kSessionLeaseAcquiring = ~uint64_t{0};

enum class SessionRole : uint32_t {
  kNone = 0,
  kRecorder = 1,
  kReplay = 2,
  kDriver = 3,
};

enum class SessionLeaseResult : uint8_t {
  kOk = 0,
  kBusy,             // a live holder already owns the topology
  kStolen,           // acquired by reclaiming a dead or expired holder
  kNotHeld,          // release/join/leave by a non-owner
  kInvalidArgument,
  kFull,             // member table is full
  kBadPage,          // missing or rejected described page
};

struct SessionLeaseMember {
  std::atomic<uint32_t> channel_id{0};
  std::atomic<uint32_t> joined{0};  // 0 empty, 1 published, 2 claim in progress
};

struct alignas(64) SessionLeasePage {
  static constexpr uint64_t kMagic = kSessionLeaseMagic;
  static constexpr uint32_t kVersion = kSessionLeaseVersion;

  ShmSuperHeader super;
  std::atomic<uint64_t> session_id{0};
  std::atomic<uint64_t> owner_token{kSessionLeaseUnheld};
  std::atomic<uint64_t> owner_pid{0};
  std::atomic<uint64_t> owner_start{0};
  std::atomic<uint64_t> heartbeat_ns{0};
  std::atomic<uint64_t> timeout_ns{0};
  std::atomic<uint32_t> role{0};
  std::atomic<uint32_t> generation{0};
  std::atomic<uint32_t> steals{0};
  std::atomic<uint32_t> member_count{0};
  SessionLeaseMember members[kSessionLeaseMaxMembers];

  static constexpr uint64_t LayoutHash() {
    return ShmLayoutHash(
        kMagic, kVersion, static_cast<uint64_t>(sizeof(SessionLeasePage)),
        static_cast<uint64_t>(alignof(SessionLeasePage)),
        static_cast<uint64_t>(offsetof(SessionLeasePage, super)),
        static_cast<uint64_t>(offsetof(SessionLeasePage, session_id)),
        static_cast<uint64_t>(offsetof(SessionLeasePage, owner_token)),
        static_cast<uint64_t>(offsetof(SessionLeasePage, owner_pid)),
        static_cast<uint64_t>(offsetof(SessionLeasePage, owner_start)),
        static_cast<uint64_t>(offsetof(SessionLeasePage, heartbeat_ns)),
        static_cast<uint64_t>(offsetof(SessionLeasePage, members)),
        static_cast<uint64_t>(kSessionLeaseMaxMembers));
  }

  static constexpr ShmPageSpec Spec() {
    return ShmPageSpec{kMagic, kVersion, LayoutHash(),
                       static_cast<uint64_t>(sizeof(SessionLeasePage)), 0};
  }
};

static_assert(sizeof(SessionLeasePage) == 384, "session lease page ABI");
static_assert(offsetof(SessionLeasePage, session_id) == 64,
              "holder line follows the super-header");
static_assert(offsetof(SessionLeasePage, members) == 128,
              "member table follows the holder line");
static_assert(std::is_standard_layout<SessionLeasePage>::value,
              "session lease page is a shared-memory object");

// `<prefix>_lease`. False when the name does not fit.
bool SessionLeaseName(const char* prefix, char* out, std::size_t capacity);

// Short stable shm name for a package root (paths are too long for shm names).
bool SessionLeaseNameForRoot(const char* root, char* out, std::size_t capacity);

// FNV-1a of `root`, never 0. Stored as the recorder's session id.
uint64_t SessionLeaseRootId(const char* root);

// Create the page when it is absent, otherwise attach it, then acquire.
// A live holder yields kBusy and leaves `region` unmapped. kOk/kStolen keep
// the mapping; `*token` is the holder token. timeout_ns == 0 uses the
// producer-gate default.
SessionLeaseResult SessionLeaseAcquire(ShmRegion* region, const char* name,
                                       SessionRole role, uint64_t session_id,
                                       uint64_t now_ns, uint64_t timeout_ns,
                                       uint64_t* token);

SessionLeaseResult SessionLeaseRelease(SessionLeasePage* page, uint64_t token);

// Refresh the heartbeat. False when `token` is not the live holder.
bool SessionLeaseHeartbeat(SessionLeasePage* page, uint64_t token,
                           uint64_t now_ns);

SessionLeaseResult SessionLeaseJoin(SessionLeasePage* page, uint64_t token,
                                    uint32_t channel_id);
SessionLeaseResult SessionLeaseLeave(SessionLeasePage* page, uint64_t token,
                                     uint32_t channel_id);

// True when `channel_id` is a joined member and the gate is unheld or owned
// by the same pid+start as the session holder.
bool SessionLeaseGateMatches(const SessionLeasePage& page, uint32_t channel_id,
                             const ProducerGate& gate);

}  // namespace thyme
