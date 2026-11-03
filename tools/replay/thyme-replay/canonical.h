#pragma once

// REP-15: schema-based protobuf and CDR codecs for offline export.
// The .tbin bytes stay raw. Callers pass a known contract layout; this file
// does not guess a field table and does not run on the record/replay hot path.

#include "self_desc.h"

#include "thyme/record/record_schema.h"

#include <cstdint>
#include <string>
#include <vector>

namespace thyme {
namespace canonical {

inline constexpr uint16_t kVersion = 1;

enum class Encoding : uint8_t { kProtobuf = 1, kCdr = 2 };

// schema.json versions this converter understands. Anything else is rejected.
bool SupportedSchemaVersion(uint32_t version);

const schema::RecordDesc* FindRecord(const schema::PackageSchema& schema,
                                    uint32_t struct_id);

// Encode one known record. `fields` on decode is a map in schema order.
// optional fields are absent (null). Unknown types and versions fail closed.
bool Encode(Encoding enc, const schema::PackageSchema& schema,
            const schema::RecordDesc& rec, const uint8_t* payload, uint32_t len,
            std::string* out, std::string* err);
bool Decode(Encoding enc, const schema::PackageSchema& schema,
            const schema::RecordDesc& rec, const uint8_t* data, std::size_t len,
            selfdesc::Value* fields, std::string* err);

// Human-readable schemas. Protobuf text is for protoc; the MCAP schema record
// carries RenderFileDescriptorSet instead. CDR text is Foxglove ros2msg.
std::string RenderProto(const schema::PackageSchema& schema,
                        const schema::RecordDesc& rec);
std::string RenderProtoSet(
    const schema::PackageSchema& schema,
    const std::vector<const schema::RecordDesc*>& recs);
std::string RenderMsg(const schema::PackageSchema& schema,
                      const schema::RecordDesc& rec);
bool RenderFileDescriptorSet(const schema::PackageSchema& schema,
                             const schema::RecordDesc& rec, std::string* out,
                             std::string* err);

}  // namespace canonical
}  // namespace thyme
