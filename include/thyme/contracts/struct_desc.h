#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace thyme {

// ABI-11 stable record identity: a descriptor is no longer identified by its
// row index alone. `channel` is the GEN-12 recorded-channel name, `record_key`
// is the canonical cross-build key (generated code currently mirrors the
// channel), and `layout_hash` pins the contract field layout. A replay tool
// resolves an old file's struct_id against a new topology by
// (key, contract_version, payload_size, layout_hash), never by raw id.
//
// Registry entry version history (docs/reference/abi-compatibility.md):
//   1 — pre-ABI-11 entry, 40 bytes: id/contract_version/payload_size/name
//   2 — ABI-11, adds layout_hash + channel + record_key (this struct)
// The version is implied by the container: .thyme.bin file version < 4 and
// mirror pages below their ABI-11 versions carry v1 entries; the current
// writer/reader only produce/consume version 2.
inline constexpr uint16_t kThymeStructDescVersion = 2;
inline constexpr uint16_t kThymeStructDescLegacyVersion = 1;
inline constexpr uint32_t kThymeStructDescLegacySize = 40;
inline constexpr std::size_t kThymeStructChannelBytes = 32;
inline constexpr std::size_t kThymeStructKeyBytes = 48;

// Registry entry describing one recorded struct (shared by the .thyme.bin
// header and the SHM mirror page; base of the record layer).
struct ThymeStructDesc {
  uint16_t struct_id = 0;
  // Contract version the payload was produced with. Metadata: consumers
  // decide whether they understand it. 0 = unset and rejected by the
  // .thyme.bin reader; 1 is the only version any writer emits today.
  uint16_t contract_version = 1;
  uint32_t payload_size = 0;
  char name[32]{};
  // ABI-11: stable identity of the contract layout (FNV-1a over the ABI-03
  // field model). 0 = unknown (a host without contract_layout.h); two
  // non-zero values that differ mean the layouts are incompatible.
  uint64_t layout_hash = 0;
  // ABI-11: recorded channel name (GEN-12 manifest). Empty on a legacy
  // descriptor; a consumer then falls back to struct_id.
  char channel[kThymeStructChannelBytes]{};
  // ABI-11: canonical record key used for topology mapping. Generated code
  // sets it to `channel`; empty falls back to `channel`.
  char record_key[kThymeStructKeyBytes]{};
};

// This entry is embedded in .thyme.bin and in both mirror pages
// (RecordMirrorPage v3, RecordMirrorRingPage v4) and is read back by
// out-of-process probes, so its size/field order is a wire ABI: changing it
// requires bumping both file and page versions.
static_assert(sizeof(ThymeStructDesc) == 128,
              "ThymeStructDesc is the record-layer wire entry (128 bytes)");
static_assert(offsetof(ThymeStructDesc, contract_version) == 2,
              "struct descriptor field order is wire ABI");
static_assert(offsetof(ThymeStructDesc, payload_size) == 4,
              "struct descriptor field order is wire ABI");
static_assert(offsetof(ThymeStructDesc, name) == 8,
              "struct name follows the scalar header");
static_assert(offsetof(ThymeStructDesc, layout_hash) == 40,
              "ABI-11 layout hash follows the 32-byte contract name");
static_assert(offsetof(ThymeStructDesc, channel) == 48,
              "ABI-11 channel follows the layout hash");
static_assert(offsetof(ThymeStructDesc, record_key) == 80,
              "ABI-11 record key follows the channel");
static_assert(std::is_standard_layout<ThymeStructDesc>::value,
              "ThymeStructDesc is copied as bytes into files and SHM");

// Bounded text copy for the char[] fields above (always NUL-terminated).
inline void SetThymeStructText(char* dst, std::size_t cap, const char* src) {
  if (dst == nullptr || cap == 0) return;
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  std::size_t n = 0;
  while (n + 1 < cap && src[n] != '\0') {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = '\0';
}

inline void SetThymeStructChannel(ThymeStructDesc* d, const char* channel) {
  if (d == nullptr) return;
  SetThymeStructText(d->channel, sizeof(d->channel), channel);
}

inline void SetThymeStructRecordKey(ThymeStructDesc* d, const char* key) {
  if (d == nullptr) return;
  SetThymeStructText(d->record_key, sizeof(d->record_key), key);
}

// Canonical key: the generated record_key when present, otherwise the
// channel name. Empty means "legacy descriptor, map by struct_id".
inline const char* ThymeStructDescKey(const ThymeStructDesc& d) {
  return d.record_key[0] != '\0' ? d.record_key : d.channel;
}

inline bool ThymeStructDescHasKey(const ThymeStructDesc& d) {
  return ThymeStructDescKey(d)[0] != '\0';
}

}  // namespace thyme
