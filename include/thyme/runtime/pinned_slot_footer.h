#pragma once

#include "thyme/runtime/pinned_slot_gate.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace thyme {

inline constexpr uint64_t kPinnedSlotFooterMagic =
    0x5448594D464F4F54ULL;  // "THYMFOOT"

// Header flag bits (PinnedSlotPageHeader::flags).
// REC-21: transport CRC is OFF by default for bulk channels (Create leaves
// this clear; the package-level integrity manifest is the recording integrity
// boundary). Producers that want per-frame transport CRC opt back in with
// SetCrcEnabled(true); readers handle both states, so the flag is not a
// mixed-peer ABI concern.
inline constexpr uint32_t kPinnedSlotFlagCrcEnabled = 1u;

struct alignas(64) PinnedSlotHeader {
  std::atomic<uint64_t> seq{0};  // seqlock: 0 = write in progress, else idx+1
  std::atomic<uint64_t> mono_ns{0};
  std::atomic<uint32_t> data_len{0};
  std::atomic<uint32_t> flags{0};  // producer-defined (e.g. h264 is_idr)
  std::atomic<uint32_t> crc32{0};  // over payload[0..data_len), 0 = disabled
  std::atomic<uint32_t> reserved{0};
};

// Slot layout of channel ABI v2 (kPinnedSlotStoreVersion); every slot starts
// at slot_stride (SlotStride()) and the payload/footer offsets below are
// computed from sizeof(PinnedSlotHeader) — a change here is a channel
// version bump (docs/reference/abi-compatibility.md).
static_assert(sizeof(PinnedSlotHeader) == 64,
              "slot header must fill exactly one cache line");
static_assert(offsetof(PinnedSlotHeader, mono_ns) == 8,
              "slot header field order is the v2 wire layout");
static_assert(offsetof(PinnedSlotHeader, data_len) == 16,
              "slot header field order is the v2 wire layout");
static_assert(offsetof(PinnedSlotHeader, crc32) == 24,
              "slot header field order is the v2 wire layout");
static_assert(offsetof(PinnedSlotHeader, reserved) == 28,
              "slot header tail is padding to the 64B cache line");
static_assert(std::is_standard_layout<PinnedSlotHeader>::value &&
                  std::is_trivially_copyable<PinnedSlotHeader>::value,
              "PinnedSlotHeader must be POD for SHM");

struct alignas(64) PinnedSlotFooter {
  std::atomic<uint64_t> magic{0};        // kPinnedSlotFooterMagic, written last
  std::atomic<uint64_t> frame_index{0};  // publish index the slot holds
  uint64_t reserved[6] = {};
};

// Footer offset inside a slot is sizeof(PinnedSlotHeader) +
// RoundUp64(slot_bytes) (SlotFooter()); readers locate it the same way.
static_assert(sizeof(PinnedSlotFooter) == 64,
              "slot footer must fill exactly one cache line");
static_assert(offsetof(PinnedSlotFooter, frame_index) == 8,
              "footer magic must stay the first (last-written) word");
static_assert(std::is_standard_layout<PinnedSlotFooter>::value &&
                  std::is_trivially_copyable<PinnedSlotFooter>::value,
              "PinnedSlotFooter must be POD for SHM");

}  // namespace thyme
