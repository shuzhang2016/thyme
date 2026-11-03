#pragma once

// pinned_slot_gate.h — REP-10: per-channel producer gate for replay.
//
// A replay owns a live channel as a synthetic producer. While it does, real
// producers (cloud/image/h264 drivers, demo producers) must not write that
// channel. The gate lives in the channel's own shared-memory header so the
// mechanism is in thyme core, not in every driver: every writer already uses
// PinnedSlotWriter::Begin/Commit, which consults the gate.
//
// Ownership is a lease, not a boolean:
//
//   * owner_token   unique 64-bit token (0 = ungated). A second replay gets
//                   kBusy instead of silently sharing the channel.
//   * owner_pid +   process identity so a writer can prove the holder is dead
//     owner_start   (pid-reuse safe) instead of waiting for the timeout.
//   * heartbeat_ns  monotonic time refreshed by the holder; a paused replay
//                   must keep a heartbeat thread running.
//   * timeout_ns    how long a stale heartbeat blocks writers.
//
// Recovery: if the holder crashes (or stops heartbeating), the next writer
// reclaims the gate in Begin() — immediately when /proc proves the pid is
// gone, otherwise after timeout_ns. A crashed replay therefore cannot leave a
// channel permanently gated.
//
// Writers that honour the gate see a distinct kGated result; blocked attempts
// are counted in rejects, and reclaims in steals. The replay reads rejects at
// the end of a run to report "a real producer tried to write during replay"
// (the double-write was prevented, not silently accepted).

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace thyme {

inline constexpr uint64_t kProducerGateUnheld = 0;
// Short-lived sentinel used by the acquire path while it publishes the owner
// fields. Writers treat it as gated.
inline constexpr uint64_t kProducerGateAcquiring = ~uint64_t{0};
inline constexpr uint64_t kProducerGateDefaultTimeoutNs = 2000000000ULL;
inline constexpr uint64_t kProducerGateHeartbeatIntervalNs = 250000000ULL;

enum class GateResult : uint8_t {
  kOk = 0,      // acquired (or released/heartbeat accepted)
  kBusy,        // another live holder owns the gate
  kStolen,      // acquired by reclaiming an expired/dead holder
  kNotHeld,     // release/heartbeat by a non-owner
  kInvalidArgument,
};

// Embedded in every gate-aware channel header. Zero-filled shared memory means
// owner_token == 0 == ungated; no explicit init is required.
struct alignas(64) ProducerGate {
  std::atomic<uint64_t> owner_token{kProducerGateUnheld};
  std::atomic<uint64_t> owner_pid{0};
  std::atomic<uint64_t> owner_start{0};  // /proc/<pid>/stat starttime token
  std::atomic<uint64_t> heartbeat_ns{0};
  std::atomic<uint64_t> timeout_ns{0};
  std::atomic<uint64_t> generation{0};  // bumped on acquire and release
  std::atomic<uint64_t> rejects{0};     // writer attempts blocked by the gate
  std::atomic<uint64_t> steals{0};      // stale/dead gates reclaimed
};

static_assert(sizeof(ProducerGate) == 64,
              "producer gate must fill exactly one cache line");
static_assert(std::is_standard_layout<ProducerGate>::value &&
                  std::is_trivially_copyable<ProducerGate>::value,
              "ProducerGate must be POD for SHM");

// Monotonic clock used for heartbeat/expiry (ns).
uint64_t ProducerGateNowNs();

// /proc/<pid>/stat starttime token (pid-reuse guard); 0 when unavailable.
uint64_t ProducerGateProcessStart(uint64_t pid);

// True when pid is alive and, if both start tokens are known, is the same
// process instance.
bool ProducerGateProcessAlive(uint64_t pid, uint64_t start);

// True while the gate is held by a live, non-expired holder. A fresh
// heartbeat alone is enough (owner fields may still be publishing); once the
// heartbeat is stale the pid/start pair decides.
bool ProducerGateHolderLive(const ProducerGate& gate, uint64_t now_ns);

// Acquire/heartbeat the gate for `token` (must be non-zero and not the
// sentinel). timeout_ns == 0 selects the default. Reclaims an expired or dead
// holder (kStolen). A live holder yields kBusy.
GateResult ProducerGateAcquire(ProducerGate& gate, uint64_t token,
                               uint64_t now_ns, uint64_t timeout_ns);

// Release only when `token` still owns the gate; a stolen gate is untouched.
GateResult ProducerGateRelease(ProducerGate& gate, uint64_t token);

// Refresh the lease. Returns false when the token no longer owns the gate.
bool ProducerGateHeartbeat(ProducerGate& gate, uint64_t token,
                           uint64_t now_ns);

// Writer-side check. Returns true when the write may proceed. A live holder
// increments rejects and blocks; an expired/dead holder is reclaimed
// (steals++). writer_token == 0 means "plain producer": any live holder
// blocks. The owner token itself is allowed.
bool ProducerGateAllowsWrite(ProducerGate& gate, uint64_t writer_token,
                             uint64_t now_ns);

// True when someone other than `writer_token` holds a live gate. Does not
// mutate counters (observability only).
bool ProducerGateHeldByOther(const ProducerGate& gate, uint64_t writer_token,
                             uint64_t now_ns);

// Unique non-zero token for a gate holder.
uint64_t ProducerGateNewToken();

}  // namespace thyme
