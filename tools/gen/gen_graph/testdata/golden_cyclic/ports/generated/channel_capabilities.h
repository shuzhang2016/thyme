// Code generated for the ports fixture graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "thyme/runtime/channel_capability.h"

inline constexpr uint32_t kGraphChannelCapabilityVersion = 1;
inline constexpr uint64_t kGraphChannelCapabilityHash = 0x5c5e64e17ea832a1ull;
inline constexpr std::size_t kGraphChannelCapabilityCount = 1;

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
  {"payload", "prod", "cons", "", thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kLatest, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kOverwrite, true, false, false, false, 0ULL, 0u},
};
