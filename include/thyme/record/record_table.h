#pragma once

#include "thyme/record/record_schema.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/runtime/record_mirror.h"

#include <cstddef>
#include <cstdint>

namespace thyme {
enum class ThymeRecordPriority : uint8_t { kP0 = 0, kP1 = 1, kP2 = 2 };

// Project fills this via generated/record_table.h (GetThymeRecordTable).
struct ThymeRecordTable {
  std::size_t count = 0;
  uint32_t (*payload_size)(uint16_t struct_id) = nullptr;
  ThymeRecordPriority (*priority)(uint16_t struct_id) = nullptr;
  void (*fill_struct_descs)(ThymeStructDesc* out, std::size_t cap) = nullptr;
  // ABI-09/ABI-10: GEN-12 manifest identity (kRecordManifestHash/version)
  // plus the optional generated contract-layout lookup. A recorder that
  // forwards this into triggered::Config::record_manifest writes the manifest
  // block into meta.tbin and a field-level schema.json sidecar. Null (a
  // standalone host without codegen) keeps the pre-ABI-10 behaviour and an
  // honest "layout unavailable" sidecar.
  const schema::RecordManifest* manifest = nullptr;
};

inline bool ThymeRecordTableValid(const ThymeRecordTable& t) {
  return t.payload_size != nullptr && t.priority != nullptr &&
         t.fill_struct_descs != nullptr &&
         t.count <= kRecordMirrorMaxSlots;
}

}  // namespace thyme
