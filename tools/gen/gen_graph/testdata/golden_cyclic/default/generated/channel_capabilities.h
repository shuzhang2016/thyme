// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "thyme/runtime/channel_capability.h"

inline constexpr uint32_t kGraphChannelCapabilityVersion = 1;
inline constexpr uint64_t kGraphChannelCapabilityHash = 0xcbf29ce484222325ull;
inline constexpr std::size_t kGraphChannelCapabilityCount = 0;

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
  {nullptr, nullptr, nullptr, nullptr, thyme::ChannelTransportKind::kFrame,
   thyme::ChannelRetention::kLatest, thyme::ChannelReaders::kOne,
   thyme::ChannelOverflow::kOverwrite, false, false, false, false, 0, 0},
};
