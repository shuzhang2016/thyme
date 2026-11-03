#pragma once

// TRG-15: one shared page between trigger_center and its recorder process.
// Commands are a single slot (trigger / preempt / evict). Sample bytes stay
// in the channel mirror or big-frame ring; this page never carries a frame.
// The mapping is zeroed shared memory. Lock-free atomics are constructed
// with placement new before fork and are not destroyed.

#include <atomic>
#include <cstdint>

namespace thyme {
namespace recorder_ipc {

inline constexpr uint32_t kMagic = 0x31535054u;  // "TPS1"
inline constexpr uint32_t kOpTrigger = 1;
inline constexpr uint32_t kOpPreempt = 6;
inline constexpr uint32_t kOpEvict = 7;
inline constexpr uint32_t kFlagRealtime = 1u;
inline constexpr uint32_t kFlagHigh = 2u;
inline constexpr uint32_t kChannelBytes = 1024;

struct Page {
  std::atomic<uint32_t> magic{kMagic};
  std::atomic<uint32_t> ready{0};
  std::atomic<uint32_t> capture_active{0};
  std::atomic<uint32_t> disk_full{0};
  std::atomic<uint32_t> phase{0};
  std::atomic<uint64_t> pending_bytes{0};
  std::atomic<uint64_t> captures_done{0};
  std::atomic<uint64_t> frames_written{0};
  std::atomic<uint64_t> audit_drops{0};
  std::atomic<uint64_t> fsm_event_drops{0};

  std::atomic<uint64_t> cmd_seq{0};
  std::atomic<uint64_t> cmd_done{0};
  std::atomic<uint64_t> cmd_result{0};
  std::atomic<uint64_t> cmd_stamp_ns{0};
  uint32_t cmd_op = 0;
  uint32_t cmd_flags = 0;
  uint64_t cmd_trigger_id = 0;
  uint64_t cmd_before_ns = 0;
  uint64_t cmd_after_ns = 0;
  uint64_t cmd_t0_ns = 0;
  uint64_t cmd_evict_bytes = 0;
  uint32_t cmd_channel_bytes = 0;
  uint32_t cmd_reserved = 0;
  char cmd_channels[kChannelBytes] = {};

  // Absolute CLOCK_MONOTONIC stamps. wake_last_ns is the last trigger's
  // eventfd delay (poll return minus the parent's stamp). first_payload is
  // the drain thread's clock at the Tick that first wrote a frame.
  std::atomic<uint64_t> wake_last_ns{0};
  std::atomic<uint64_t> first_payload_mono_ns{0};
  std::atomic<uint64_t> trigger_hook_mono_ns{0};
};

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "recorder ipc atomics must be lock-free across processes");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "recorder ipc atomics must be lock-free across processes");

}  // namespace recorder_ipc
}  // namespace thyme
