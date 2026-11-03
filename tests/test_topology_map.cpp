// test_topology_map.cpp — REP-11 core mapping rules (ABI-11).
//
// Pure library test: no package, no SHM, no CLI. It pins the resolution
// contract used by thyme-replay:
//
//   (record_key/channel, contract_version, payload_size, layout_hash)
//
//   * keys survive a changed/reordered RecordStructId;
//   * every tuple difference is an explicit, named rejection;
//   * a zero layout_hash is "unknown", accepted but reported unverified;
//   * keyless legacy descriptors still map by struct_id;
//   * a duplicate target key is ambiguous, never first-match-wins.
//
// The layout hash itself is pinned to the ABI-11 recipe: identical field
// models compare equal, any field/offset/size/stride change does not.

#include "thyme/check.h"
#include "thyme/contracts/struct_desc.h"
#include "thyme/record/record_schema.h"
#include "thyme/replay/topology_map.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using namespace thyme;

namespace {

const schema::FieldLayout kFieldsA[] = {
    {"a", "uint64", 0, 8, 1, 8},
    {"b", "uint32", 8, 4, 1, 4},
};
const schema::ContractLayout kLayoutA = {"Pod", 16, 2, 8, kFieldsA};

const schema::FieldLayout kFieldsB[] = {
    {"a", "uint64", 0, 8, 1, 8},
    {"b", "uint32", 8, 4, 1, 4},
    {"c", "uint32", 12, 4, 1, 4},
};
const schema::ContractLayout kLayoutB = {"Pod", 16, 3, 8, kFieldsB};

ThymeStructDesc Desc(uint16_t id, const char* channel, const char* key,
                     uint32_t payload, uint64_t layout_hash,
                     uint16_t version = 1) {
  ThymeStructDesc d{};
  d.struct_id = id;
  d.contract_version = version;
  d.payload_size = payload;
  d.layout_hash = layout_hash;
  std::snprintf(d.name, sizeof(d.name), "%s", "Pod");
  SetThymeStructChannel(&d, channel);
  SetThymeStructRecordKey(&d, key);
  return d;
}

void TestLayoutHash() {
  const uint64_t a = schema::ComputeLayoutHash(kLayoutA);
  const uint64_t a2 = schema::ComputeLayoutHash(kLayoutA);
  const uint64_t b = schema::ComputeLayoutHash(kLayoutB);
  CHECK(a != 0);
  CHECK(a == a2);  // identical model -> identical hash across calls/builds
  CHECK(a != b);   // any field change changes the hash

  // Changing only an offset must change the hash too.
  schema::FieldLayout moved[] = {
      {"a", "uint64", 0, 8, 1, 8},
      {"b", "uint32", 8, 4, 1, 4},
  };
  moved[1].offset = 12;
  const schema::ContractLayout layout_moved = {"Pod", 16, 2, 8, moved};
  CHECK(schema::ComputeLayoutHash(layout_moved) != a);
}

void TestKeyedMapping() {
  const uint64_t h = schema::ComputeLayoutHash(kLayoutA);
  // Source topology: alpha=0, beta=1, removed=2.
  const ThymeStructDesc alpha = Desc(0, "alpha", "alpha", 52, h);
  const ThymeStructDesc beta = Desc(1, "beta", "beta", 32, h);
  const ThymeStructDesc removed = Desc(2, "removed", "removed", 8, h);
  // Target topology B: beta=0, alpha=1 (reordered), gamma=2 (added).
  const ThymeStructDesc tgt[] = {
      Desc(0, "beta", "beta", 32, h),
      Desc(1, "alpha", "alpha", 52, h),
      Desc(2, "gamma", "gamma", 16, h),
  };
  const std::size_t n = sizeof(tgt) / sizeof(tgt[0]);

  replay::TopologyMapping m;
  CHECK(replay::ResolveTopologyMapping(alpha, tgt, n, &m) ==
        replay::TopologyMapStatus::kMapped);
  CHECK(m.target_struct_id == 1);  // key, not source id 0
  CHECK(m.record_key == "alpha");
  CHECK(m.detail.find("verified") != std::string::npos);

  CHECK(replay::ResolveTopologyMapping(beta, tgt, n, &m) ==
        replay::TopologyMapStatus::kMapped);
  CHECK(m.target_struct_id == 0);

  CHECK(replay::ResolveTopologyMapping(removed, tgt, n, &m) ==
        replay::TopologyMapStatus::kMissing);
  CHECK(m.detail.find("removed") != std::string::npos);
}

void TestTupleMismatches() {
  const uint64_t h = schema::ComputeLayoutHash(kLayoutA);
  const ThymeStructDesc src = Desc(0, "alpha", "alpha", 52, h);
  replay::TopologyMapping m;

  {
    const ThymeStructDesc tgt[] = {
        Desc(5, "alpha", "alpha", 52, h, /*version=*/2)};
    CHECK(replay::ResolveTopologyMapping(src, tgt, 1, &m) ==
          replay::TopologyMapStatus::kVersionMismatch);
    CHECK(m.target_struct_id == 5);
    CHECK(m.detail.find("contract_version") != std::string::npos);
  }
  {
    const ThymeStructDesc tgt[] = {Desc(5, "alpha", "alpha", 48, h)};
    CHECK(replay::ResolveTopologyMapping(src, tgt, 1, &m) ==
          replay::TopologyMapStatus::kPayloadMismatch);
    CHECK(m.detail.find("payload_size") != std::string::npos);
  }
  {
    const ThymeStructDesc tgt[] = {
        Desc(5, "alpha", "alpha", 52, h ^ 0x1ULL)};
    CHECK(replay::ResolveTopologyMapping(src, tgt, 1, &m) ==
          replay::TopologyMapStatus::kLayoutMismatch);
    CHECK(m.detail.find("layout_hash") != std::string::npos);
  }
  {
    // Unknown target layout: key tuple otherwise matches, but the report
    // must say the layout could not be verified.
    const ThymeStructDesc tgt[] = {Desc(5, "alpha", "alpha", 52, 0)};
    CHECK(replay::ResolveTopologyMapping(src, tgt, 1, &m) ==
          replay::TopologyMapStatus::kMappedUnverified);
    CHECK(m.detail.find("unavailable") != std::string::npos);
  }
  {
    const ThymeStructDesc tgt[] = {
        Desc(0, "alpha", "alpha", 52, h),
        Desc(1, "alpha", "alpha", 52, h),
    };
    CHECK(replay::ResolveTopologyMapping(src, tgt, 2, &m) ==
          replay::TopologyMapStatus::kAmbiguous);
    CHECK(m.detail.find("matches 2") != std::string::npos);
  }
}

void TestChannelFallbackAndLegacy() {
  const uint64_t h = schema::ComputeLayoutHash(kLayoutA);
  // record_key absent -> channel is the key.
  ThymeStructDesc src = Desc(0, "alpha", "", 52, h);
  const ThymeStructDesc tgt[] = {Desc(7, "alpha", "", 52, h)};
  replay::TopologyMapping m;
  CHECK(replay::ResolveTopologyMapping(src, tgt, 1, &m) ==
        replay::TopologyMapStatus::kMapped);
  CHECK(m.target_struct_id == 7);

  // Legacy descriptor: no channel/key -> struct_id, even if the target has
  // keys (the pre-ABI-11 contract).
  ThymeStructDesc legacy{};
  legacy.struct_id = 1;
  legacy.contract_version = 1;
  legacy.payload_size = 32;
  const ThymeStructDesc tgt2[] = {
      Desc(0, "beta", "beta", 32, h),
      Desc(1, "alpha", "alpha", 52, h),
  };
  CHECK(replay::ResolveTopologyMapping(legacy, tgt2, 2, &m) ==
        replay::TopologyMapStatus::kMappedLegacy);
  CHECK(m.target_struct_id == 1);

  // Legacy id with no target is missing, not guessed by name.
  legacy.struct_id = 9;
  CHECK(replay::ResolveTopologyMapping(legacy, tgt2, 2, &m) ==
        replay::TopologyMapStatus::kMissing);
}

}  // namespace

int main() {
  TestLayoutHash();
  TestKeyedMapping();
  TestTupleMismatches();
  TestChannelFallbackAndLegacy();
  std::printf("test_topology_map: OK\n");
  return 0;
}
