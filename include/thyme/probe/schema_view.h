#pragma once

// schema_view.h — ABI-09 read-only view of a package's record schema.
//
// `thyme_probe schema <pkg|schema.json>` is the tool-side half of ABI-09: it
// decodes the sidecar written by the recorder without compiling thyme's
// generated contract headers (or even the contract types), which is exactly
// the contract the sidecar exists for. It never touches SHM and never writes.
//
// States are explicit:
//   * kOk      — parsed; text/JSON render the manifest identity and the
//                per-record field layout (or "layout unavailable");
//   * kAbsent  — no sidecar (pre-ABI-09 package): the command exits non-zero,
//                because "no schema" is not the same as "empty schema";
//   * kInvalid — present but unparseable/tampered: exit non-zero with the
//                parser's reason; nothing is partially trusted.
//
// Field offsets in the render are absolute inside the recorded payload
// (schema::RecordDesc::payload_offset + field.offset), so a consumer can
// slice the payload bytes directly.

#include "thyme/record/record_schema.h"

#include <string>

namespace thyme {
namespace probe {

struct SchemaView {
  std::string path;  // resolved schema.json path (or the requested dir)
  schema::ReadState state = schema::ReadState::kAbsent;
  std::string error;
  schema::PackageSchema schema;
};

// `path` may be a package directory or a schema.json file.
SchemaView LoadSchemaView(const std::string& path);

std::string RenderSchemaViewText(const SchemaView& view);
std::string RenderSchemaViewJson(const SchemaView& view);

}  // namespace probe
}  // namespace thyme
