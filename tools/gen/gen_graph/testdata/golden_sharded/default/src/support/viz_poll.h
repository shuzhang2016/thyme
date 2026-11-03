#pragma once

#include "generated/record_registry.h"
#include "generated/viz_ser.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/viz/foxglove_ws.h"
#include "thyme/viz/ws_util.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace viz_poll {

struct SlotCursor {
  uint64_t last_head[kRecordRegistryCount]{};
};

struct Stats {
  std::size_t messages_sent = 0;
  std::size_t slots_polled = 0;
};

// Resolve whitelist channel names to registry indices. Empty whitelist = all slots.
// Unknown names → false and optional err message.
bool ResolveChannelFilter(const std::vector<std::string>& whitelist,
                          bool enabled[kRecordRegistryCount],
                          std::string* err = nullptr);

std::vector<thyme::FoxgloveChannelDesc> BuildChannels(
    const std::vector<std::string>* whitelist = nullptr);

// Poll mirror once; publish subscribed slot updates. Returns messages sent.
// enabled=nullptr means all slots; otherwise only enabled[sid] slots are polled.
std::size_t PollOnce(const thyme::RecordMirrorRingPage& page, SlotCursor& cur,
                     thyme::FoxgloveWsServer& server, Stats* stats = nullptr,
                     const bool* enabled = nullptr);

}  // namespace viz_poll
