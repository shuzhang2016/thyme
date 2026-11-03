#include "thyme/app/mp_session_identity.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <pthread.h>

namespace thyme {
namespace app {
namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

void HashBytes(uint64_t* h, const void* data, std::size_t bytes) {
  const auto* p = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < bytes; ++i) {
    *h ^= static_cast<uint64_t>(p[i]);
    *h *= kFnvPrime;
  }
}

void HashU64(uint64_t* h, uint64_t v) { HashBytes(h, &v, sizeof(v)); }

void HashString(uint64_t* h, const char* s) {
  if (s == nullptr) {
    HashU64(h, 0);
    return;
  }
  const std::size_t n = std::strlen(s);
  HashU64(h, static_cast<uint64_t>(n));
  HashBytes(h, s, n);
}

// Process-local registry. Deliberately tiny and allocation-free: the SHM
// identity is only needed at start/attach time.
constexpr std::size_t kMaxEntries = 8;
constexpr std::size_t kPrefixCap = 40;

struct Entry {
  char prefix[kPrefixCap];
  uint64_t hash;
  bool used;
};

Entry g_entries[kMaxEntries];
std::size_t g_next = 0;
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

}  // namespace

uint64_t MpConfigHash(const MpSupervisorConfig& cfg) noexcept {
  uint64_t h = kFnvOffset;
  HashString(&h, cfg.shm_prefix_fmt);
  HashU64(&h, static_cast<uint64_t>(static_cast<uint32_t>(cfg.health_n)));
  HashU64(&h, static_cast<uint64_t>(cfg.processes.size()));
  for (const auto& p : cfg.processes) {
    HashString(&h, p.id);
    HashString(&h, p.role);
  }
  HashU64(&h, static_cast<uint64_t>(cfg.nodes.size()));
  for (const auto& n : cfg.nodes) {
    HashString(&h, n.id);
    HashString(&h, n.type);
    HashString(&h, n.process);
    HashU64(&h, static_cast<uint64_t>(static_cast<uint32_t>(n.hz)));
  }
  HashU64(&h, static_cast<uint64_t>(cfg.shm_channels.size()));
  for (const auto& c : cfg.shm_channels) {
    HashString(&h, c.suffix);
    HashU64(&h, static_cast<uint64_t>(static_cast<uint32_t>(c.kind)));
    HashString(&h, c.from_process);
    HashString(&h, c.to_process);
  }
  HashU64(&h, static_cast<uint64_t>(cfg.record_gates.size()));
  for (const auto& g : cfg.record_gates) {
    HashString(&h, g.process);
    HashU64(&h, static_cast<uint64_t>(g.slot));
  }
  HashU64(&h, static_cast<uint64_t>(cfg.slot_specs.size()));
  for (const auto& s : cfg.slot_specs) {
    HashU64(&h, static_cast<uint64_t>(s.region_bytes));
    HashU64(&h, static_cast<uint64_t>(s.payload_bytes));
    HashU64(&h, static_cast<uint64_t>(s.cell_stride));
    HashU64(&h, s.payload_layout_hash);
    HashU64(&h, s.feature_bits);
  }
  HashU64(&h, static_cast<uint64_t>(cfg.cross_gates.size()));
  for (const auto& g : cfg.cross_gates) {
    HashU64(&h, static_cast<uint64_t>(static_cast<uint32_t>(g.shm_kind)));
    HashU64(&h, static_cast<uint64_t>(g.counter_slot));
  }
  return h;
}

bool RegisterMpSessionIdentity(const char* prefix,
                               uint64_t config_hash) noexcept {
  if (prefix == nullptr || prefix[0] == '\0') return false;
  pthread_mutex_lock(&g_mutex);
  std::size_t slot = kMaxEntries;
  for (std::size_t i = 0; i < kMaxEntries; ++i) {
    if (g_entries[i].used &&
        std::strncmp(g_entries[i].prefix, prefix, kPrefixCap) == 0) {
      slot = i;
      break;
    }
    if (!g_entries[i].used && slot == kMaxEntries) slot = i;
  }
  if (slot == kMaxEntries) {
    slot = g_next;
    g_next = (g_next + 1) % kMaxEntries;
  }
  std::snprintf(g_entries[slot].prefix, kPrefixCap, "%s", prefix);
  g_entries[slot].hash = config_hash;
  g_entries[slot].used = true;
  pthread_mutex_unlock(&g_mutex);
  return true;
}

SessionIdentityCheck VerifyMpSessionIdentity(
    const char* prefix, uint64_t expected_config_hash) noexcept {
  if (prefix == nullptr || expected_config_hash == 0) {
    return SessionIdentityCheck::kUnregistered;
  }
  pthread_mutex_lock(&g_mutex);
  SessionIdentityCheck out = SessionIdentityCheck::kUnregistered;
  for (std::size_t i = 0; i < kMaxEntries; ++i) {
    if (!g_entries[i].used) continue;
    if (std::strncmp(g_entries[i].prefix, prefix, kPrefixCap) != 0) continue;
    out = g_entries[i].hash == expected_config_hash
              ? SessionIdentityCheck::kVerified
              : SessionIdentityCheck::kMismatch;
    break;
  }
  pthread_mutex_unlock(&g_mutex);
  return out;
}

}  // namespace app
}  // namespace thyme
