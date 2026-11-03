#include "thyme/replay/topology_map.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace thyme {
namespace replay {
namespace {

bool TextEqual(const char* a, const char* b) {
  return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

// A target entry matches when the canonical keys agree, or when either the
// recorded channel or the explicit record_key agrees. Both sides are checked
// so a build that keeps a stable record_key across a channel rename still
// maps. A keyless target never matches by key.
bool KeyMatches(const ThymeStructDesc& source, const ThymeStructDesc& target) {
  if (!ThymeStructDescHasKey(source) || !ThymeStructDescHasKey(target)) {
    return false;
  }
  if (TextEqual(ThymeStructDescKey(source), ThymeStructDescKey(target))) {
    return true;
  }
  if (source.channel[0] != '\0' && TextEqual(source.channel, target.channel)) {
    return true;
  }
  if (source.record_key[0] != '\0' && target.record_key[0] != '\0' &&
      TextEqual(source.record_key, target.record_key)) {
    return true;
  }
  return false;
}

std::string Hex(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "0x%016llx",
                static_cast<unsigned long long>(v));
  return std::string(buf);
}

}  // namespace

const char* TopologyMapStatusName(TopologyMapStatus status) {
  switch (status) {
    case TopologyMapStatus::kMapped:
      return "mapped";
    case TopologyMapStatus::kMappedUnverified:
      return "mapped-unverified";
    case TopologyMapStatus::kMappedLegacy:
      return "mapped-legacy";
    case TopologyMapStatus::kMissing:
      return "missing";
    case TopologyMapStatus::kAmbiguous:
      return "ambiguous";
    case TopologyMapStatus::kVersionMismatch:
      return "version-mismatch";
    case TopologyMapStatus::kPayloadMismatch:
      return "payload-mismatch";
    case TopologyMapStatus::kLayoutMismatch:
      return "layout-mismatch";
  }
  return "unknown";
}

TopologyMapStatus ResolveTopologyMapping(const ThymeStructDesc& source,
                                         const ThymeStructDesc* target,
                                         std::size_t target_count,
                                         TopologyMapping* out) {
  if (out == nullptr) return TopologyMapStatus::kMissing;
  TopologyMapping m;
  m.source_struct_id = source.struct_id;
  m.channel = source.channel;
  m.record_key = ThymeStructDescKey(source);
  m.contract_version = source.contract_version;
  m.payload_size = source.payload_size;
  m.layout_hash = source.layout_hash;

  if (!ThymeStructDescHasKey(source)) {
    // Legacy descriptor: the pre-ABI-11 rule is struct_id. Keep it explicit
    // so a report can say why the id was trusted.
    if (target != nullptr) {
      for (std::size_t i = 0; i < target_count; ++i) {
        if (target[i].struct_id == source.struct_id) {
          m.target_struct_id = target[i].struct_id;
          m.status = TopologyMapStatus::kMappedLegacy;
          m.detail = "legacy descriptor: matched by struct_id (no record key)";
          *out = std::move(m);
          return out->status;
        }
      }
    }
    m.status = TopologyMapStatus::kMissing;
    m.detail = "legacy descriptor: no target with struct_id " +
               std::to_string(source.struct_id);
    *out = std::move(m);
    return out->status;
  }

  const ThymeStructDesc* match = nullptr;
  std::size_t matches = 0;
  if (target != nullptr) {
    for (std::size_t i = 0; i < target_count; ++i) {
      if (!KeyMatches(source, target[i])) continue;
      if (match == nullptr) match = &target[i];
      ++matches;
    }
  }
  if (matches == 0) {
    m.status = TopologyMapStatus::kMissing;
    m.detail = "no target channel for key '" + m.record_key + "'";
    *out = std::move(m);
    return out->status;
  }
  if (matches > 1) {
    m.status = TopologyMapStatus::kAmbiguous;
    m.detail = "key '" + m.record_key + "' matches " +
               std::to_string(matches) + " target descriptors";
    *out = std::move(m);
    return out->status;
  }

  m.target_struct_id = match->struct_id;
  if (match->contract_version != source.contract_version) {
    m.status = TopologyMapStatus::kVersionMismatch;
    m.detail = "key '" + m.record_key + "' contract_version " +
               std::to_string(source.contract_version) + " -> " +
               std::to_string(match->contract_version);
    *out = std::move(m);
    return out->status;
  }
  if (match->payload_size != source.payload_size) {
    m.status = TopologyMapStatus::kPayloadMismatch;
    m.detail = "key '" + m.record_key + "' payload_size " +
               std::to_string(source.payload_size) + " -> " +
               std::to_string(match->payload_size);
    *out = std::move(m);
    return out->status;
  }
  if (source.layout_hash != 0 && match->layout_hash != 0 &&
      source.layout_hash != match->layout_hash) {
    m.status = TopologyMapStatus::kLayoutMismatch;
    m.detail = "key '" + m.record_key + "' layout_hash " +
               Hex(source.layout_hash) + " -> " + Hex(match->layout_hash);
    *out = std::move(m);
    return out->status;
  }
  const bool verified = source.layout_hash != 0 && match->layout_hash != 0;
  m.status = verified ? TopologyMapStatus::kMapped
                      : TopologyMapStatus::kMappedUnverified;
  m.detail = verified ? "key tuple verified"
                      : "key tuple matched; layout_hash unavailable on " +
                            std::string(source.layout_hash == 0
                                            ? "package"
                                            : "target");
  *out = std::move(m);
  return out->status;
}

}  // namespace replay
}  // namespace thyme
