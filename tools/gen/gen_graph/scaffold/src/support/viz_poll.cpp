#include "support/viz_poll.h"

#include <cstdio>
#include <vector>

namespace viz_poll {

namespace {

bool ChannelMatches(const char* name, const std::string& want) {
  return want == name;
}

}  // namespace

bool ResolveChannelFilter(const std::vector<std::string>& whitelist,
                          bool enabled[kRecordRegistryCount],
                          std::string* err) {
  for (std::size_t i = 0; i < kRecordRegistryCount; ++i) {
    enabled[i] = whitelist.empty();
  }
  if (whitelist.empty()) return true;

  for (const std::string& want : whitelist) {
    bool found = false;
    for (std::size_t i = 0; i < kRecordRegistryCount; ++i) {
      if (ChannelMatches(kRecordRegistry[i].channel, want)) {
        enabled[i] = true;
        found = true;
        break;
      }
    }
    if (!found) {
      if (err != nullptr) {
        *err = "unknown viz channel: " + want;
      }
      return false;
    }
  }
  return true;
}

std::vector<thyme::FoxgloveChannelDesc> BuildChannels(
    const std::vector<std::string>* whitelist) {
  bool enabled[kRecordRegistryCount]{};
  if (whitelist != nullptr && !ResolveChannelFilter(*whitelist, enabled)) {
    return {};
  }
  if (whitelist == nullptr || whitelist->empty()) {
    for (std::size_t i = 0; i < kRecordRegistryCount; ++i) enabled[i] = true;
  }

  std::vector<thyme::FoxgloveChannelDesc> out;
  for (std::size_t i = 0; i < kRecordRegistryCount; ++i) {
    if (!enabled[i]) continue;
    thyme::FoxgloveChannelDesc c;
    c.id = static_cast<uint32_t>(i + 1);
    char topic[128];
    std::snprintf(topic, sizeof(topic), "/thyme/%s", kRecordRegistry[i].channel);
    c.topic = topic;
    c.schema_name = kRecordRegistry[i].cpp_type;
    c.schema_json = VizSchemaSlot(static_cast<uint16_t>(i));
    out.push_back(std::move(c));
  }
  return out;
}

std::size_t PollOnce(const thyme::RecordMirrorRingPage& page, SlotCursor& cur,
                     thyme::FoxgloveWsServer& server, Stats* stats,
                     const bool* enabled) {
  if (!page.Validate() || !server.HasSubscribers()) return 0;
  const uint32_t n = page.struct_count();
  std::size_t sent = 0;
  for (uint32_t sid = 0; sid < n && sid < kRecordRegistryCount; ++sid) {
    if (enabled != nullptr && !enabled[sid]) continue;
    if (stats != nullptr) ++stats->slots_polled;
    const uint64_t head = page.RingHead(static_cast<uint16_t>(sid));
    if (head == 0 || head == cur.last_head[sid]) continue;

    const uint32_t psz = kRecordRegistry[sid].payload_size;
    std::vector<uint8_t> buf(psz);
    uint32_t rsz = 0;
    uint64_t mono = 0;
    uint32_t src = 0;
    if (!page.ReadRingSample(static_cast<uint16_t>(sid), head - 1, buf.data(),
                             static_cast<uint32_t>(buf.size()), rsz, mono, src) ||
        rsz != psz) {
      continue;
    }
    std::string json;
    if (!VizEncodeSlot(static_cast<uint16_t>(sid), buf.data(), rsz, &json)) {
      continue;
    }
    const uint32_t channel_id = static_cast<uint32_t>(sid + 1);
    if (server.PublishJson(channel_id, mono, json)) {
      cur.last_head[sid] = head;
      ++sent;
      if (stats != nullptr) ++stats->messages_sent;
    }
  }
  return sent;
}

}  // namespace viz_poll
