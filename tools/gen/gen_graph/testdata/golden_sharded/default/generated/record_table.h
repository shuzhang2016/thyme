// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/record_registry.h"
#include "thyme/record/record_schema.h"
#include "thyme/record/record_table.h"
#include <cstdio>
#include <cstdlib>

inline const thyme::schema::RecordManifest kThymeRecordManifest = {
    kRecordManifestHash,
    thyme::schema::kManifestVersion,
    nullptr};  // no contracts_layout.txt: field layout unavailable

inline uint32_t ThymeRecordPayloadSize(uint16_t sid) {
  return sid < kRecordRegistryCount ? kRecordRegistry[sid].payload_size : 0;
}

inline thyme::ThymeRecordPriority ThymeRecordPriorityOf(uint16_t sid) {
  if (sid >= kRecordRegistryCount) return thyme::ThymeRecordPriority::kP1;
  switch (kRecordRegistry[sid].priority) {
    case RecordPriority::kP0: return thyme::ThymeRecordPriority::kP0;
    case RecordPriority::kP2: return thyme::ThymeRecordPriority::kP2;
    default: return thyme::ThymeRecordPriority::kP1;
  }
}

inline void ThymeRecordFillStructDescs(thyme::ThymeStructDesc* out, std::size_t cap) {
  if (cap < kRecordRegistryCount) {
    std::fprintf(stderr,
                 "ThymeRecordFillStructDescs: cap %zu < kRecordRegistryCount %u\n",
                 cap, kRecordRegistryCount);
    std::abort();
  }
  for (std::size_t i = 0; i < kRecordRegistryCount; ++i) {
    out[i].struct_id = static_cast<uint16_t>(i);
    out[i].contract_version = kRecordRegistry[i].contract_version;
    out[i].payload_size = kRecordRegistry[i].payload_size;
    std::snprintf(out[i].name, sizeof(out[i].name), "%s",
                  kRecordRegistry[i].cpp_type);
    // ABI-11: stable record identity. The layout hash comes from the
    // generated contract model (static_assert-bound to the real
    // offsetof/sizeof); it stays 0 when this host has no layout table.
    const thyme::schema::ContractLayout* layout =
        kThymeRecordManifest.contract != nullptr
            ? kThymeRecordManifest.contract(kRecordRegistry[i].cpp_type)
            : nullptr;
    out[i].layout_hash = layout != nullptr
                             ? thyme::schema::ComputeLayoutHash(*layout)
                             : thyme::schema::kLayoutHashNone;
    std::snprintf(out[i].channel, sizeof(out[i].channel), "%s",
                  kRecordRegistry[i].channel);
    std::snprintf(out[i].record_key, sizeof(out[i].record_key), "%s",
                  kRecordRegistry[i].channel);
  }
}

inline thyme::ThymeRecordTable GetThymeRecordTable() {
  return thyme::ThymeRecordTable{
      kRecordRegistryCount,
      &ThymeRecordPayloadSize,
      &ThymeRecordPriorityOf,
      &ThymeRecordFillStructDescs,
      &kThymeRecordManifest,
  };
}

inline uint64_t ThymeRecordMirrorRingRequiredBytes() {
  thyme::ThymeStructDesc descs[thyme::kRecordMirrorRingMaxStructs];
  ThymeRecordFillStructDescs(descs, thyme::kRecordMirrorRingMaxStructs);
  return thyme::RecordMirrorRingPage::RequiredBytes(
      descs, kRecordMirrorRingSpecs, kRecordRegistryCount);
}

inline uint64_t ThymeRecordMirrorRingConfigHash() {
  thyme::ThymeStructDesc descs[thyme::kRecordMirrorRingMaxStructs];
  ThymeRecordFillStructDescs(descs, thyme::kRecordMirrorRingMaxStructs);
  return thyme::RecordMirrorRingPage::HashConfig(
      descs, kRecordMirrorRingSpecs, kRecordRegistryCount);
}
