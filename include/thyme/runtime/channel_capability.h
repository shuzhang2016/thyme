#pragma once

#include <cstdint>

namespace thyme {

enum class ChannelTransportKind : uint8_t {
  kFrame = 0,
  kPinnedSlot = 1,
};

enum class ChannelRetention : uint8_t {
  kLatest = 0,
  kQueue = 1,
};

enum class ChannelReaders : uint8_t {
  kOne = 0,
  kMany = 1,
};

enum class ChannelOverflow : uint8_t {
  kOverwrite = 0,
  kDrop = 1,
  // Producer-visible backpressure or failure; never an unbounded hot-path wait.
  kReject = 2,
};

struct ChannelCapabilityInfo {
  ChannelTransportKind transport_kind = ChannelTransportKind::kFrame;
  ChannelRetention retention = ChannelRetention::kLatest;
  ChannelReaders readers = ChannelReaders::kOne;
  ChannelOverflow overflow = ChannelOverflow::kOverwrite;
  bool recordable = false;
  bool replay_gate = false;
  bool lease_required = false;
  bool hub_required = false;
  uint64_t max_wire_bytes = 0;  // 0 = supplied by the typed backend at compile time
  uint32_t slot_count = 0;      // PinnedSlot only; 0 = not applicable
};

inline const char* ChannelTransportKindName(ChannelTransportKind value) {
  return value == ChannelTransportKind::kPinnedSlot ? "pinned_slot" : "frame";
}

inline const char* ChannelRetentionName(ChannelRetention value) {
  return value == ChannelRetention::kQueue ? "queue" : "latest";
}

inline const char* ChannelReadersName(ChannelReaders value) {
  return value == ChannelReaders::kMany ? "many" : "one";
}

inline const char* ChannelOverflowName(ChannelOverflow value) {
  switch (value) {
    case ChannelOverflow::kOverwrite: return "overwrite";
    case ChannelOverflow::kDrop: return "drop";
    case ChannelOverflow::kReject: return "reject";
  }
  return "overwrite";
}

}  // namespace thyme
