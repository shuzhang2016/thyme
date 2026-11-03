#include "thyme/runtime/session_lease.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace thyme {
namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t HashBytes(const char* text) {
  uint64_t h = kFnvOffset;
  if (text == nullptr) return 1;
  for (const auto* p = reinterpret_cast<const unsigned char*>(text); *p != 0;
       ++p) {
    h ^= *p;
    h *= kFnvPrime;
  }
  return h == 0 ? 1 : h;
}

void ClearMembers(SessionLeasePage* page) {
  for (uint32_t i = 0; i < kSessionLeaseMaxMembers; ++i) {
    page->members[i].joined.store(0, std::memory_order_relaxed);
    page->members[i].channel_id.store(0, std::memory_order_relaxed);
  }
  page->member_count.store(0, std::memory_order_relaxed);
}

bool HolderLive(const SessionLeasePage& page, uint64_t now_ns) {
  const uint64_t owner = page.owner_token.load(std::memory_order_acquire);
  if (owner == kSessionLeaseUnheld) return false;
  const uint64_t heartbeat = page.heartbeat_ns.load(std::memory_order_acquire);
  if (heartbeat == 0) return false;
  const uint64_t pid = page.owner_pid.load(std::memory_order_acquire);
  const uint64_t start = page.owner_start.load(std::memory_order_acquire);
  if (pid != 0 && !ProducerGateProcessAlive(pid, start)) return false;
  uint64_t timeout = page.timeout_ns.load(std::memory_order_acquire);
  if (timeout == 0) timeout = kProducerGateDefaultTimeoutNs;
  if (now_ns <= heartbeat) return true;
  return now_ns - heartbeat <= timeout;
}

bool TokenOwns(const SessionLeasePage& page, uint64_t token) {
  return token != kSessionLeaseUnheld && token != kSessionLeaseAcquiring &&
         page.owner_token.load(std::memory_order_acquire) == token;
}

SessionLeaseResult AcquireToken(SessionLeasePage* page, uint64_t token,
                                SessionRole role, uint64_t session_id,
                                uint64_t now_ns, uint64_t timeout_ns) {
  if (page == nullptr || token == kSessionLeaseUnheld ||
      token == kSessionLeaseAcquiring || role == SessionRole::kNone ||
      session_id == 0) {
    return SessionLeaseResult::kInvalidArgument;
  }
  if (timeout_ns == 0) timeout_ns = kProducerGateDefaultTimeoutNs;
  const uint64_t pid = static_cast<uint64_t>(getpid());
  const uint64_t start = ProducerGateProcessStart(pid);
  for (int attempt = 0; attempt < 64; ++attempt) {
    const uint64_t owner = page->owner_token.load(std::memory_order_acquire);
    if (owner == token) {
      page->heartbeat_ns.store(now_ns, std::memory_order_release);
      return SessionLeaseResult::kOk;
    }
    if (owner != kSessionLeaseUnheld && HolderLive(*page, now_ns)) {
      return SessionLeaseResult::kBusy;
    }
    page->heartbeat_ns.store(now_ns, std::memory_order_relaxed);
    page->owner_pid.store(0, std::memory_order_relaxed);
    page->owner_start.store(0, std::memory_order_relaxed);
    uint64_t expected = owner;
    if (!page->owner_token.compare_exchange_weak(
            expected, token, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      continue;
    }
    ClearMembers(page);
    page->session_id.store(session_id, std::memory_order_relaxed);
    page->role.store(static_cast<uint32_t>(role), std::memory_order_relaxed);
    page->owner_pid.store(pid, std::memory_order_relaxed);
    page->owner_start.store(start, std::memory_order_relaxed);
    page->timeout_ns.store(timeout_ns, std::memory_order_relaxed);
    page->generation.fetch_add(1, std::memory_order_acq_rel);
    if (owner != kSessionLeaseUnheld) {
      page->steals.fetch_add(1, std::memory_order_relaxed);
      return SessionLeaseResult::kStolen;
    }
    return SessionLeaseResult::kOk;
  }
  return SessionLeaseResult::kBusy;
}

}  // namespace

bool SessionLeaseName(const char* prefix, char* out, std::size_t capacity) {
  if (prefix == nullptr || prefix[0] == '\0' || out == nullptr ||
      capacity == 0) {
    return false;
  }
  const int n = std::snprintf(out, capacity, "%s_lease", prefix);
  return n > 0 && static_cast<std::size_t>(n) < capacity;
}

bool SessionLeaseNameForRoot(const char* root, char* out,
                             std::size_t capacity) {
  if (root == nullptr || root[0] == '\0' || out == nullptr || capacity == 0) {
    return false;
  }
  const int n = std::snprintf(out, capacity, "/tsess_%016llx",
                              static_cast<unsigned long long>(HashBytes(root)));
  return n > 0 && static_cast<std::size_t>(n) < capacity;
}

uint64_t SessionLeaseRootId(const char* root) { return HashBytes(root); }

SessionLeaseResult SessionLeaseAcquire(ShmRegion* region, const char* name,
                                       SessionRole role, uint64_t session_id,
                                       uint64_t now_ns, uint64_t timeout_ns,
                                       uint64_t* token) {
  if (region == nullptr || name == nullptr || name[0] == '\0' ||
      token == nullptr) {
    return SessionLeaseResult::kInvalidArgument;
  }
  *token = 0;
  if (region->data() == nullptr) {
    // Attach an existing page first. Creating first would let a stale-segment
    // sweep unlink a dead holder's lease and hide the kStolen reclaim.
    ShmPageError error;
    if (!region->OpenDescribed(name, SessionLeasePage::Spec(), 0, false,
                               &error)) {
      if (error.code != ShmPageErrc::kNotReady ||
          !region->CreateDescribed(name, SessionLeasePage::Spec())) {
        const uint64_t wait =
            timeout_ns == 0 ? kProducerGateDefaultTimeoutNs : timeout_ns;
        if (!region->OpenDescribed(name, SessionLeasePage::Spec(), wait, false,
                                   &error)) {
          return SessionLeaseResult::kBadPage;
        }
      }
    }
  }
  auto* page = static_cast<SessionLeasePage*>(region->data());
  const uint64_t minted = ProducerGateNewToken();
  const SessionLeaseResult result =
      AcquireToken(page, minted, role, session_id, now_ns, timeout_ns);
  if (result != SessionLeaseResult::kOk &&
      result != SessionLeaseResult::kStolen) {
    region->Unmap();
    return result;
  }
  *token = minted;
  return result;
}

SessionLeaseResult SessionLeaseRelease(SessionLeasePage* page, uint64_t token) {
  if (page == nullptr || token == kSessionLeaseUnheld ||
      token == kSessionLeaseAcquiring) {
    return SessionLeaseResult::kInvalidArgument;
  }
  uint64_t owner = page->owner_token.load(std::memory_order_acquire);
  if (owner != token) return SessionLeaseResult::kNotHeld;
  if (!page->owner_token.compare_exchange_strong(
          owner, kSessionLeaseUnheld, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return SessionLeaseResult::kNotHeld;
  }
  ClearMembers(page);
  page->generation.fetch_add(1, std::memory_order_relaxed);
  page->heartbeat_ns.store(0, std::memory_order_relaxed);
  page->owner_pid.store(0, std::memory_order_relaxed);
  page->owner_start.store(0, std::memory_order_relaxed);
  page->timeout_ns.store(0, std::memory_order_relaxed);
  page->session_id.store(0, std::memory_order_relaxed);
  page->role.store(0, std::memory_order_relaxed);
  return SessionLeaseResult::kOk;
}

bool SessionLeaseHeartbeat(SessionLeasePage* page, uint64_t token,
                           uint64_t now_ns) {
  if (page == nullptr || !TokenOwns(*page, token)) return false;
  page->heartbeat_ns.store(now_ns, std::memory_order_release);
  return true;
}

SessionLeaseResult SessionLeaseJoin(SessionLeasePage* page, uint64_t token,
                                    uint32_t channel_id) {
  if (page == nullptr || !TokenOwns(*page, token)) {
    return SessionLeaseResult::kNotHeld;
  }
  for (uint32_t i = 0; i < kSessionLeaseMaxMembers; ++i) {
    if (page->members[i].joined.load(std::memory_order_acquire) == 1 &&
        page->members[i].channel_id.load(std::memory_order_acquire) ==
            channel_id) {
      return SessionLeaseResult::kOk;
    }
  }
  for (uint32_t i = 0; i < kSessionLeaseMaxMembers; ++i) {
    uint32_t expected = 0;
    if (!page->members[i].joined.compare_exchange_strong(
            expected, 2, std::memory_order_acq_rel, std::memory_order_acquire)) {
      continue;
    }
    page->members[i].channel_id.store(channel_id, std::memory_order_relaxed);
    page->members[i].joined.store(1, std::memory_order_release);
    page->member_count.fetch_add(1, std::memory_order_relaxed);
    return SessionLeaseResult::kOk;
  }
  return SessionLeaseResult::kFull;
}

SessionLeaseResult SessionLeaseLeave(SessionLeasePage* page, uint64_t token,
                                     uint32_t channel_id) {
  if (page == nullptr || !TokenOwns(*page, token)) {
    return SessionLeaseResult::kNotHeld;
  }
  for (uint32_t i = 0; i < kSessionLeaseMaxMembers; ++i) {
    if (page->members[i].joined.load(std::memory_order_acquire) != 1) continue;
    if (page->members[i].channel_id.load(std::memory_order_acquire) !=
        channel_id) {
      continue;
    }
    page->members[i].joined.store(0, std::memory_order_release);
    page->member_count.fetch_sub(1, std::memory_order_relaxed);
    return SessionLeaseResult::kOk;
  }
  return SessionLeaseResult::kNotHeld;
}

bool SessionLeaseGateMatches(const SessionLeasePage& page, uint32_t channel_id,
                             const ProducerGate& gate) {
  bool member = false;
  for (uint32_t i = 0; i < kSessionLeaseMaxMembers; ++i) {
    if (page.members[i].joined.load(std::memory_order_acquire) == 1 &&
        page.members[i].channel_id.load(std::memory_order_acquire) ==
            channel_id) {
      member = true;
      break;
    }
  }
  if (!member) return false;
  const uint64_t gate_owner = gate.owner_token.load(std::memory_order_acquire);
  if (gate_owner == kProducerGateUnheld) return true;
  const uint64_t holder_pid = page.owner_pid.load(std::memory_order_acquire);
  const uint64_t holder_start = page.owner_start.load(std::memory_order_acquire);
  if (holder_pid == 0) return false;
  return gate.owner_pid.load(std::memory_order_acquire) == holder_pid &&
         gate.owner_start.load(std::memory_order_acquire) == holder_start;
}

}  // namespace thyme
