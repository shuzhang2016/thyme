#pragma once

// topology_map.h — REP-11: stable record-key mapping across topologies.
//
// A replay package records the topology it was produced under. RecordStructId
// is a *row index* into that build's sorted recorded-channel list, so the same
// numeric id means a different contract after a channel is added, removed or
// reordered. ABI-11 gives every descriptor a stable identity:
//
//   (record_key, contract_version, payload_size, layout_hash)
//
// This module resolves one package descriptor against a target registry (the
// struct table of a live RecordMirrorRingPage, or any generated descriptor
// array) without guessing:
//
//   * a keyed source matches the unique target descriptor with the same
//     record_key/channel and then verifies the contract tuple;
//   * a key match with a different contract_version/payload_size/layout_hash
//     is a hard incompatibility (never silently remapped);
//   * a layout_hash of 0 means "unknown" (a build without contract_layout.h):
//     the tuple is accepted but reported as unverified;
//   * a legacy descriptor (no channel/key) maps by struct_id, which is the
//     documented pre-ABI-11 behaviour, and is reported as such;
//   * no matching target is kMissing — the caller decides whether to skip it
//     (--allow-missing) or fail.
//
// The function is pure: no I/O, no SHM access, no allocation beyond the
// returned strings.

#include "thyme/contracts/struct_desc.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace thyme {
namespace replay {

enum class TopologyMapStatus : uint8_t {
  kMapped = 0,         // key matched; contract tuple verified
  kMappedUnverified,   // key matched; a layout_hash was unknown on one side
  kMappedLegacy,       // legacy source: matched by struct_id
  kMissing,            // no target descriptor for this key/id
  kAmbiguous,          // more than one target descriptor matches the key
  kVersionMismatch,    // key matched, contract_version differs
  kPayloadMismatch,    // key matched, payload_size differs
  kLayoutMismatch,     // key matched, both layout hashes known and differ
};

const char* TopologyMapStatusName(TopologyMapStatus status);

inline bool TopologyMapStatusMapped(TopologyMapStatus status) {
  return status == TopologyMapStatus::kMapped ||
         status == TopologyMapStatus::kMappedUnverified ||
         status == TopologyMapStatus::kMappedLegacy;
}

struct TopologyMapping {
  uint16_t source_struct_id = 0;
  uint16_t target_struct_id = 0;  // valid only when mapped
  std::string channel;            // source channel (diagnostics)
  std::string record_key;         // source canonical key
  uint16_t contract_version = 0;
  uint32_t payload_size = 0;
  uint64_t layout_hash = 0;
  TopologyMapStatus status = TopologyMapStatus::kMissing;
  std::string detail;  // human-readable reason for a non-mapped status
};

// Resolves one source descriptor against `target`/`target_count`.
// `out` must be non-null; it is always fully initialized.
TopologyMapStatus ResolveTopologyMapping(const ThymeStructDesc& source,
                                         const ThymeStructDesc* target,
                                         std::size_t target_count,
                                         TopologyMapping* out);

}  // namespace replay
}  // namespace thyme
