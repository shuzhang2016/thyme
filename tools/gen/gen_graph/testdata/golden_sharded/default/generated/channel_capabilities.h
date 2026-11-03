// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "thyme/runtime/channel_capability.h"

inline constexpr uint32_t kGraphChannelCapabilityVersion = 1;
inline constexpr uint64_t kGraphChannelCapabilityHash = 0xcf6971a0f0b13ea5ull;
inline constexpr std::size_t kGraphChannelCapabilityCount = 4;

struct GraphChannelCapability {
  const char* channel;
  const char* from;
  const char* to;
  const char* shm;
  thyme::ChannelTransportKind transport_kind;
  thyme::ChannelRetention retention;
  thyme::ChannelReaders readers;
  thyme::ChannelOverflow overflow;
  bool recordable;
  bool replay_gate;
  bool lease_required;
  bool hub_required;
  uint64_t max_wire_bytes;
  uint32_t slot_count;
};

inline constexpr GraphChannelCapability kGraphChannelCapabilities[] = {
  {"c0", "prod", "sink__s0", "", thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop, true, false, false, false, 0ULL, 0u},
  {"c1", "prod", "sink__s1", "", thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop, true, false, false, false, 0ULL, 0u},
  {"c2", "prod", "sink__s0", "", thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop, true, false, false, false, 0ULL, 0u},
  {"c3", "prod", "sink__s1", "", thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop, true, false, false, false, 0ULL, 0u},
};
