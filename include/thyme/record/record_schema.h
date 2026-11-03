#pragma once

// record_schema.h — ABI-09 self-describing package sidecar + ABI-10 record
// manifest identity.
//
// Every capture package carries `<pkg>/schema.json`, written at seal (and, for
// an invalidated/crashed package, over the surviving file set together with
// the REC-18 partial integrity manifest). The sidecar describes the record
// contract of the build that wrote the package, so an offline tool can decode
// the payloads **without compiling thyme or the generated contract headers**:
//
//   format            "thyme-record-schema" (never localized, never versioned
//                     by schema reshaping — only `schema_version` moves)
//   schema_version    3 (v1/v2 sidecars are still readable)
//   partial           false for a sealed package; true for `.incomplete`
//   record_manifest   GEN-12 manifest identity of the recorded channel set:
//                       hash     "0x<16 hex>" or null (host has no codegen)
//                       version  ABI-10 manifest version (1) or null
//                       channels ABI-11 key table, ordered by struct_id:
//                                {struct_id, channel, record_key,
//                                 contract_version, payload_size,
//                                 layout_hash}; empty when the descriptors
//                                are legacy (no generated channel keys)
//   frame_header      40-byte FrameHeader layout (fields, offsets, sizes)
//   records[]         one entry per ThymeStructDesc of the package registry:
//                       struct_id, name (contract cpp_type), contract_version,
//                       payload_size (as declared in the package registry),
//                       channel/record_key/layout_hash (ABI-11; null on a
//                       legacy descriptor),
//                       frame_header_bytes (36; the header that precedes a
//                       framed record payload — internal control records such
//                       as FrameIdx/TrigMeta are bare and have no layout),
//                       payload_offset (byte offset of the bare contract
//                       inside the recorded payload: 0 when the producer
//                       records the bare contract, align_up(36, align) when
//                       it records sizeof(Frame<T>) — Frame is not packed, so
//                       alignment padding can sit between the header and the
//                       payload), contract_bytes (bare payload),
//                       layout ("known" | "unavailable"), fields[] (null when
//                       unavailable)
//   payload_profiles[] REC-27 (schema_version 3): one entry per recorded
//                       big-frame channel {channel, profile|null,
//                       profile_version, params_digest|null}. Core stores the
//                       opaque identity only; it never parses the parameter
//                       string. v1/v2 sidecars omit the array.
//
// Field offsets are relative to their own struct start. For a record the
// contract starts at `payload_offset` inside the recorded payload, so the
// absolute offset of a field is `payload_offset + field.offset`. (Frame is
// not packed: `sizeof(Frame<T>)` can exceed `sizeof(FrameHeader) +
// sizeof(T)` by the alignment padding before the payload member.)
//
// Honesty rules (what the recorder can and cannot know):
//   * struct_id / name / contract_version / payload_size come from the
//     package's own ThymeStructDesc registry (BFS: the mirror page), so they
//     are always real.
//   * Field-level layout is only emitted when the host supplies the generated
//     contract layout table (`schema::RecordManifest::contract`). The numbers
//     come from the ABI-03 generator model and are static_assert-bound to the
//     compiler's offsetof/sizeof in the generated header, so they cannot
//     drift from the wire layout. Without that table the record is emitted
//     with `"layout": "unavailable"` and `"fields": null` — a tool must then
//     fall back to raw bytes. Never guess a field layout.
//   * ABI-11 channel/record keys come from ThymeStructDesc (the generated
//     record table sets them). A legacy descriptor has none, so the sidecar
//     emits an empty channels array and null per-record keys instead of
//     inventing names.
//
// The writer never runs on the record hot path: it is called at seal /
// invalidation, after the data files are final and before the integrity
// manifest so the sidecar is covered by the package hash chain.

#include "thyme/record/package_hash.h"
#include "thyme/record/thyme_bin.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace thyme {
namespace schema {

inline constexpr char kFileName[] = "schema.json";
inline constexpr char kFormat[] = "thyme-record-schema";
// Version history:
//   1 — ABI-09: record_manifest.channels was null (no record key in the
//       runtime registry) and record entries had no channel/key/layout_hash.
//   2 — ABI-11: record_manifest.channels carries the sorted channel keys and
//       every record entry carries channel/record_key/layout_hash. Readers
//       still accept v1 sidecars (legacy packages).
//   3 — REC-27: payload_profiles[] names each big-frame channel's opaque
//       payload decoder identity (profile / version / params_digest). v1/v2
//       sidecars stay readable and report an empty profile list.
inline constexpr uint32_t kVersion = 3;
inline constexpr uint32_t kVersionMin = 1;
inline constexpr uint32_t kMaxProfileBytes = 56;

// ABI-10: version of the record-manifest identity carried by packages, the
// sidecar and perf logs. A change to the manifest ordering/hash algorithm
// bumps this, and readers must refuse to mix versions.
inline constexpr uint16_t kManifestVersion = 1;

// Bounds: the package registry itself is bounded by kThymeBinMaxStructCount
// (64); the reader ceiling is wider so a standalone schema (e.g. an extracted
// sidecar used as a contract dictionary) can be read too, while a corrupt or
// hostile file is rejected before it can grow without limit.
inline constexpr uint32_t kMaxStructs = 4096;
inline constexpr uint32_t kMaxFields = 256;
inline constexpr uint32_t kMaxStringBytes = 256;
inline constexpr uint64_t kMaxFileBytes = 16ull * 1024 * 1024;

// Field layout in the ABI-03 model. `stride` includes the element type's own
// trailing padding, so an array occupies `count * stride` bytes.
struct FieldLayout {
  const char* name;
  const char* type;  // datatypes.json spelling (uint32, float32, nested name)
  uint32_t offset;
  uint32_t size;    // count * stride
  uint32_t count;
  uint32_t stride;
};

// One contract's bare payload layout (FrameHeader excluded). Emitted by
// gen_contracts into contract_layout.h. `align` is alignof(the contract): with
// a non-packed Frame<T> the payload starts at align_up(sizeof(FrameHeader),
// align) and sizeof(Frame<T>) is that offset plus payload_bytes, rounded up to
// the same alignment. The schema writer checks this against the package
// registry before it trusts the table.
struct ContractLayout {
  const char* cpp_type;
  uint32_t payload_bytes;
  uint16_t field_count;
  uint16_t align;
  const FieldLayout* fields;
};

// ABI-11: 0 means "layout unknown" (a build without contract_layout.h).
// Non-zero values are comparable across builds: ComputeLayoutHash() mixes
// the contract type, payload_bytes/field_count/align and every field's
// name/type/offset/size/count/stride with FNV-1a, so the same field model
// always yields the same number and any layout change yields a different one.
// constexpr form so generated code can pin the identity of a LatestSlot page
// at compile time from the generated contract_layout.h table. The out-of-line
// ComputeLayoutHash() below keeps the long-standing linkable symbol for
// existing consumers.
inline constexpr uint64_t kLayoutHashNone = 0;
constexpr uint64_t ComputeLayoutHashConstexpr(const ContractLayout& layout) {
  // FNV-1a 64 over a length-free, order-stable field model. Strings are
  // terminated by 0xFF (which cannot appear as a byte of a decimal number) so
  // ("ab","c") and ("a","bc") do not collide; integers are mixed little-endian
  // byte by byte. The exact recipe is ABI: changing it changes every
  // layout_hash and therefore the documented ABI-11 value.
  uint64_t h = 14695981039346656037ULL;
  auto mix_byte = [&h](uint8_t b) {
    h ^= b;
    h *= 1099511628211ULL;
  };
  auto mix_u32 = [&mix_byte](uint32_t v) {
    for (int i = 0; i < 4; ++i) mix_byte(static_cast<uint8_t>(v >> (i * 8)));
  };
  auto mix_str = [&mix_byte](const char* s) {
    if (s != nullptr) {
      for (const char* p = s; *p != '\0'; ++p) {
        mix_byte(static_cast<uint8_t>(*p));
      }
    }
    mix_byte(0xFF);
  };
  mix_str(layout.cpp_type);
  mix_u32(layout.payload_bytes);
  mix_u32(layout.field_count);
  mix_u32(layout.align);
  const uint32_t n =
      layout.fields == nullptr ? 0 : static_cast<uint32_t>(layout.field_count);
  for (uint32_t i = 0; i < n; ++i) {
    const FieldLayout& f = layout.fields[i];
    mix_str(f.name);
    mix_str(f.type);
    mix_u32(f.offset);
    mix_u32(f.size);
    mix_u32(f.count);
    mix_u32(f.stride);
  }
  // 0 is reserved for "layout unknown"; an actual FNV collision with 0 is
  // remapped so it can never be mistaken for an unavailable layout.
  return h == kLayoutHashNone ? 1 : h;
}

// Out-of-line ABI-stable entry point: same recipe as
// ComputeLayoutHashConstexpr(). Kept as a separately linkable symbol for
// consumers built against earlier headers.
uint64_t ComputeLayoutHash(const ContractLayout& layout);

// GEN-12 manifest identity + optional contract layout lookup, supplied by the
// generated record table (`GetThymeRecordTable().manifest`). A host without
// codegen leaves it null: packages then carry no manifest block and the
// sidecar declares the manifest and the field layout as unavailable.
struct RecordManifest {
  uint64_t hash = 0;
  uint16_t version = kManifestVersion;
  const ContractLayout* (*contract)(const char* cpp_type) = nullptr;
};

enum class LayoutState : uint8_t { kKnown, kUnavailable };

struct FieldDesc {
  std::string name;
  std::string type;
  uint32_t offset = 0;
  uint32_t size = 0;
  uint32_t count = 0;
  uint32_t stride = 0;
};

struct RecordDesc {
  uint16_t struct_id = 0;
  uint16_t contract_version = 0;
  uint32_t payload_size = 0;
  std::string name;
  // ABI-11 stable identity (empty / 0 on a legacy descriptor).
  std::string channel;
  std::string record_key;
  uint64_t layout_hash = 0;
  LayoutState layout = LayoutState::kUnavailable;
  uint32_t payload_offset = 0;  // contract start inside the payload
  uint32_t contract_bytes = 0;  // bare payload bytes (0 when unavailable)
  std::vector<FieldDesc> fields;
};

// ABI-11: one entry of record_manifest.channels. The array is ordered by
// struct_id (which is also the GEN-12 manifest order: byte-wise channel
// name). `record_key` is the canonical topology key; `channel` is the
// recorded name.
struct ChannelDesc {
  uint16_t struct_id = 0;
  std::string channel;
  std::string record_key;
  uint16_t contract_version = 0;
  uint32_t payload_size = 0;
  uint64_t layout_hash = 0;
};

// REC-27: one big-frame channel's payload identity. `profile` empty means
// the package honestly has no decoder name (`"profile": null` in JSON);
// `params_digest` 0 means no parameter string was supplied.
struct PayloadProfileDesc {
  std::string channel;
  std::string profile;
  uint32_t version = 0;
  uint64_t params_digest = 0;
};

struct PackageSchema {
  uint32_t version = 0;
  bool partial = false;
  bool manifest_declared = false;
  uint64_t manifest_hash = 0;
  uint16_t manifest_version = 0;
  uint32_t frame_header_bytes = 0;
  std::vector<FieldDesc> frame_header_fields;
  std::vector<ChannelDesc> channels;
  std::vector<RecordDesc> records;
  std::vector<PayloadProfileDesc> payload_profiles;
};

enum class ReadState : uint8_t {
  kOk,       // parsed
  kAbsent,   // no schema.json (pre-ABI-09 package) — legacy, not an error
  kInvalid,  // present but unparseable/inconsistent/tampered
};

const char* ReadStateName(ReadState state);

// Writes `<package_dir>/schema.json` atomically (temp file + fsync + rename +
// directory fsync). `manifest` may be null. Returns false with *err on any I/O
// failure; the caller (recorder) then treats the seal as failed.
bool WritePackageSchema(const std::string& package_dir,
                        const ThymeStructDesc* descs, std::size_t count,
                        const RecordManifest* manifest, bool partial,
                        std::string* err,
                        integrity::PackageHashState* hashes = nullptr,
                        const PayloadProfileDesc* profiles = nullptr,
                        std::size_t profile_count = 0);

// Path-addressed form (tests, tools that seal a package elsewhere).
bool WriteSchemaFile(const std::string& path, const ThymeStructDesc* descs,
                     std::size_t count, const RecordManifest* manifest,
                     bool partial, std::string* err,
                     const PayloadProfileDesc* profiles = nullptr,
                     std::size_t profile_count = 0);

// FNV-1a 64 of the profile parameter string. Empty input is 0 (no params);
// a real hash that collides with 0 is remapped to 1 so 0 stays "none".
uint64_t HashProfileParams(const std::string& params);

// Reads and strictly validates a sidecar. A directory argument reads
// `<dir>/schema.json`. Unknown keys, duplicate keys, unknown enum states,
// out-of-range numbers and trailing bytes are rejected (a schema that this
// build does not fully understand is invalid, never partially trusted).
ReadState ReadPackageSchema(const std::string& package_dir, PackageSchema* out,
                            std::string* err);
ReadState ReadSchemaFile(const std::string& path, PackageSchema* out,
                         std::string* err);

// FrameHeader layout of this build (36 bytes; offsets from the real struct).
const FieldDesc* FrameHeaderFields(std::size_t* count);

bool ParseManifestHash(const std::string& text, uint64_t* out);
std::string FormatManifestHash(uint64_t hash);

// Human-readable dump (thyme_probe schema / thyme-replay info).
std::string RenderSchemaText(const PackageSchema& schema);

}  // namespace schema
}  // namespace thyme
