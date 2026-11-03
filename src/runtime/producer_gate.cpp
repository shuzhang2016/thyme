#include "thyme/runtime/producer_gate.h"

#include "thyme/record/file_ptr.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

namespace thyme {
namespace {

uint64_t Mix(uint64_t h, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (i * 8)) & 0xFFu;
    h *= 1099511628211ULL;
  }
  return h;
}

}  // namespace

uint64_t ProducerGateNowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t ProducerGateProcessStart(uint64_t pid) {
  if (pid == 0) return 0;
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%llu/stat",
                static_cast<unsigned long long>(pid));
  FilePtr f(std::fopen(path, "r"));
  if (f == nullptr) return 0;
  char buf[1024];
  const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f.get());
  if (n == 0) return 0;
  buf[n] = '\0';
  // The comm field may contain spaces and parentheses; fields after it start
  // after the LAST ')'. starttime is field 22, i.e. the 19th token there
  // (field 3 == token 0).
  const char* close = std::strrchr(buf, ')');
  if (close == nullptr) return 0;
  const char* p = close + 1;
  // Field 3 (right after comm) is the single-character state. Skip it, then
  // parse fields 4..22; starttime is field 22, the 19th numeric token.
  while (*p == ' ' || *p == '\t') ++p;
  if (*p == '\0') return 0;
  ++p;  // state
  for (int token = 0; token < 19; ++token) {
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '\0') return 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(p, &end, 10);
    if (end == p) return 0;
    if (token == 18) return static_cast<uint64_t>(v);
    p = end;
  }
  return 0;
}

bool ProducerGateProcessAlive(uint64_t pid, uint64_t start) {
  if (pid == 0) return false;
  if (start != 0) {
    const uint64_t actual = ProducerGateProcessStart(pid);
    if (actual != 0 && actual != start) return false;  // pid was reused
  }
  if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
  return errno == EPERM;
}

bool ProducerGateHolderLive(const ProducerGate& gate, uint64_t now_ns) {
  const uint64_t owner = gate.owner_token.load(std::memory_order_acquire);
  if (owner == kProducerGateUnheld) return false;
  const uint64_t heartbeat = gate.heartbeat_ns.load(std::memory_order_acquire);
  if (heartbeat == 0) return false;
  // A verified-dead holder is reclaimable immediately, even if its last
  // heartbeat is still inside the timeout. pid == 0 means the acquirer has
  // not published its identity yet, so the fresh heartbeat decides.
  const uint64_t pid = gate.owner_pid.load(std::memory_order_acquire);
  const uint64_t start = gate.owner_start.load(std::memory_order_acquire);
  if (pid != 0 && !ProducerGateProcessAlive(pid, start)) return false;
  uint64_t timeout = gate.timeout_ns.load(std::memory_order_acquire);
  if (timeout == 0) timeout = kProducerGateDefaultTimeoutNs;
  if (now_ns <= heartbeat) return true;
  return now_ns - heartbeat <= timeout;
}

GateResult ProducerGateAcquire(ProducerGate& gate, uint64_t token,
                               uint64_t now_ns, uint64_t timeout_ns) {
  if (token == kProducerGateUnheld || token == kProducerGateAcquiring) {
    return GateResult::kInvalidArgument;
  }
  if (timeout_ns == 0) timeout_ns = kProducerGateDefaultTimeoutNs;
  const uint64_t pid = static_cast<uint64_t>(getpid());
  const uint64_t start = ProducerGateProcessStart(pid);
  for (int attempt = 0; attempt < 64; ++attempt) {
    const uint64_t owner = gate.owner_token.load(std::memory_order_acquire);
    if (owner == token) {
      gate.heartbeat_ns.store(now_ns, std::memory_order_release);
      return GateResult::kOk;
    }
    if (owner != kProducerGateUnheld &&
        ProducerGateHolderLive(gate, now_ns)) {
      return GateResult::kBusy;
    }
    // Publish a provisional heartbeat before the owner CAS: a writer that
    // observes our token while the remaining owner fields are still being
    // stored must see a live lease, never a half-initialised dead one. Clear
    // the pid too, so a writer cannot mistake the previous holder's dead pid
    // for ours in that window.
    gate.heartbeat_ns.store(now_ns, std::memory_order_relaxed);
    gate.owner_pid.store(0, std::memory_order_relaxed);
    gate.owner_start.store(0, std::memory_order_relaxed);
    uint64_t expected = owner;
    if (gate.owner_token.compare_exchange_weak(
            expected, token, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      gate.owner_pid.store(pid, std::memory_order_relaxed);
      gate.owner_start.store(start, std::memory_order_relaxed);
      gate.timeout_ns.store(timeout_ns, std::memory_order_relaxed);
      gate.generation.fetch_add(1, std::memory_order_acq_rel);
      if (owner != kProducerGateUnheld) {
        gate.steals.fetch_add(1, std::memory_order_relaxed);
        return GateResult::kStolen;
      }
      return GateResult::kOk;
    }
  }
  return GateResult::kBusy;
}

GateResult ProducerGateRelease(ProducerGate& gate, uint64_t token) {
  if (token == kProducerGateUnheld || token == kProducerGateAcquiring) {
    return GateResult::kInvalidArgument;
  }
  uint64_t owner = gate.owner_token.load(std::memory_order_acquire);
  if (owner != token) return GateResult::kNotHeld;
  if (!gate.owner_token.compare_exchange_strong(
          owner, kProducerGateUnheld, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return GateResult::kNotHeld;  // stolen while we were releasing
  }
  gate.generation.fetch_add(1, std::memory_order_relaxed);
  gate.heartbeat_ns.store(0, std::memory_order_relaxed);
  gate.owner_pid.store(0, std::memory_order_relaxed);
  gate.owner_start.store(0, std::memory_order_relaxed);
  gate.timeout_ns.store(0, std::memory_order_relaxed);
  return GateResult::kOk;
}

bool ProducerGateHeartbeat(ProducerGate& gate, uint64_t token,
                           uint64_t now_ns) {
  if (token == kProducerGateUnheld || token == kProducerGateAcquiring) {
    return false;
  }
  if (gate.owner_token.load(std::memory_order_acquire) != token) return false;
  gate.heartbeat_ns.store(now_ns, std::memory_order_release);
  return true;
}

bool ProducerGateAllowsWrite(ProducerGate& gate, uint64_t writer_token,
                             uint64_t now_ns) {
  for (;;) {
    uint64_t owner = gate.owner_token.load(std::memory_order_acquire);
    if (owner == kProducerGateUnheld) return true;
    if (writer_token != kProducerGateUnheld && owner == writer_token) {
      return true;
    }
    if (ProducerGateHolderLive(gate, now_ns)) {
      gate.rejects.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    // Expired/dead holder: reclaim. A writer is not a gate owner, so it drops
    // the gate to unheld rather than claiming it.
    if (gate.owner_token.compare_exchange_weak(
            owner, kProducerGateUnheld, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      gate.steals.fetch_add(1, std::memory_order_relaxed);
      gate.heartbeat_ns.store(0, std::memory_order_relaxed);
      return true;
    }
  }
}

bool ProducerGateHeldByOther(const ProducerGate& gate, uint64_t writer_token,
                             uint64_t now_ns) {
  const uint64_t owner = gate.owner_token.load(std::memory_order_acquire);
  if (owner == kProducerGateUnheld) return false;
  if (writer_token != kProducerGateUnheld && owner == writer_token) {
    return false;
  }
  return ProducerGateHolderLive(gate, now_ns);
}

uint64_t ProducerGateNewToken() {
  static std::atomic<uint64_t> counter{0};
  const uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t h = 14695981039346656037ULL;
  h = Mix(h, static_cast<uint64_t>(getpid()));
  h = Mix(h, ProducerGateNowNs());
  h = Mix(h, seq);
  h = Mix(h, reinterpret_cast<uintptr_t>(&counter));
  if (h == kProducerGateUnheld || h == kProducerGateAcquiring) h ^= 0x9E3779B97F4A7C15ULL;
  return h;
}

}  // namespace thyme
