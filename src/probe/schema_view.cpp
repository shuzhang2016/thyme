#include "thyme/probe/schema_view.h"

#include <cstdint>
#include <cstdio>
#include <sys/stat.h>

namespace thyme {
namespace probe {

namespace {

void AppendJsonString(std::string* out, const std::string& s) {
  out->push_back('"');
  for (const char c : s) {
    switch (c) {
      case '"':
        *out += "\\\"";
        break;
      case '\\':
        *out += "\\\\";
        break;
      case '\n':
        *out += "\\n";
        break;
      case '\r':
        *out += "\\r";
        break;
      case '\t':
        *out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x",
                        static_cast<unsigned int>(
                            static_cast<unsigned char>(c)));
          *out += buf;
        } else {
          out->push_back(c);
        }
        break;
    }
  }
  out->push_back('"');
}

void AppendJsonFields(std::string* out, const std::vector<schema::FieldDesc>& fs,
                      uint32_t base) {
  *out += "[";
  for (std::size_t i = 0; i < fs.size(); ++i) {
    const schema::FieldDesc& f = fs[i];
    if (i != 0) *out += ",";
    *out += "{\"name\":";
    AppendJsonString(out, f.name);
    *out += ",\"type\":";
    AppendJsonString(out, f.type);
    *out += ",\"offset\":" + std::to_string(base + f.offset);
    *out += ",\"offset_in_struct\":" + std::to_string(f.offset);
    *out += ",\"size\":" + std::to_string(f.size);
    *out += ",\"count\":" + std::to_string(f.count);
    *out += ",\"stride\":" + std::to_string(f.stride) + "}";
  }
  *out += "]";
}

}  // namespace

SchemaView LoadSchemaView(const std::string& path) {
  SchemaView view;
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    view.path = path;
    view.state = schema::ReadState::kAbsent;
    view.error = "no such file or directory";
    return view;
  }
  const bool dir = S_ISDIR(st.st_mode);
  const std::string file = dir ? path + "/" + schema::kFileName : path;
  view.path = file;
  view.state = schema::ReadSchemaFile(file, &view.schema, &view.error);
  return view;
}

std::string RenderSchemaViewText(const SchemaView& view) {
  if (view.state == schema::ReadState::kAbsent) {
    return "schema : absent (" + view.path +
           "); pre-ABI-09 package: decode payloads as raw bytes\n";
  }
  if (view.state == schema::ReadState::kInvalid) {
    return "schema : INVALID " + view.error + "\n";
  }
  std::string out = "file   : " + view.path + "\n";
  out += schema::RenderSchemaText(view.schema);
  return out;
}

std::string RenderSchemaViewJson(const SchemaView& view) {
  std::string out = "{\"tool\":\"thyme_probe\",\"command\":\"schema\",\"file\":";
  AppendJsonString(&out, view.path);
  out += ",\"state\":";
  AppendJsonString(&out, schema::ReadStateName(view.state));
  out += ",\"ok\":";
  out += view.state == schema::ReadState::kOk ? "true" : "false";
  out += ",\"error\":";
  if (view.error.empty()) {
    out += "null";
  } else {
    AppendJsonString(&out, view.error);
  }
  if (view.state == schema::ReadState::kOk) {
    const schema::PackageSchema& sc = view.schema;
    out += ",\"schema_version\":" + std::to_string(sc.version);
    out += ",\"partial\":";
    out += sc.partial ? "true" : "false";
    out += ",\"record_manifest\":";
    if (sc.manifest_declared) {
      out += "{\"hash\":";
      AppendJsonString(&out, schema::FormatManifestHash(sc.manifest_hash));
      out += ",\"version\":" + std::to_string(sc.manifest_version);
      out += ",\"channels\":[";
      for (std::size_t i = 0; i < sc.channels.size(); ++i) {
        const schema::ChannelDesc& c = sc.channels[i];
        if (i != 0) out += ",";
        out += "{\"struct_id\":" + std::to_string(c.struct_id);
        out += ",\"channel\":";
        AppendJsonString(&out, c.channel);
        out += ",\"record_key\":";
        AppendJsonString(&out, c.record_key);
        out += ",\"contract_version\":" + std::to_string(c.contract_version);
        out += ",\"payload_size\":" + std::to_string(c.payload_size);
        out += ",\"layout_hash\":";
        if (c.layout_hash == schema::kLayoutHashNone) {
          out += "null";
        } else {
          AppendJsonString(&out, schema::FormatManifestHash(c.layout_hash));
        }
        out += "}";
      }
      out += "]}";
    } else {
      out += "null";
    }
    out += ",\"frame_header\":{\"bytes\":" +
           std::to_string(sc.frame_header_bytes) + ",\"fields\":";
    AppendJsonFields(&out, sc.frame_header_fields, 0);
    out += "},\"records\":[";
    for (std::size_t i = 0; i < sc.records.size(); ++i) {
      const schema::RecordDesc& r = sc.records[i];
      if (i != 0) out += ",";
      out += "{\"struct_id\":" + std::to_string(r.struct_id);
      out += ",\"name\":";
      AppendJsonString(&out, r.name);
      out += ",\"contract_version\":" + std::to_string(r.contract_version);
      out += ",\"payload_size\":" + std::to_string(r.payload_size);
      out += ",\"channel\":";
      if (r.channel.empty()) {
        out += "null";
      } else {
        AppendJsonString(&out, r.channel);
      }
      out += ",\"record_key\":";
      if (r.record_key.empty()) {
        out += "null";
      } else {
        AppendJsonString(&out, r.record_key);
      }
      out += ",\"layout_hash\":";
      if (r.layout_hash == schema::kLayoutHashNone) {
        out += "null";
      } else {
        AppendJsonString(&out, schema::FormatManifestHash(r.layout_hash));
      }
      out += ",\"layout\":";
      AppendJsonString(&out, r.layout == schema::LayoutState::kKnown
                                 ? "known"
                                 : "unavailable");
      out += ",\"payload_offset\":" + std::to_string(r.payload_offset);
      out += ",\"contract_bytes\":" + std::to_string(r.contract_bytes);
      out += ",\"fields\":";
      AppendJsonFields(&out, r.fields, r.payload_offset);
      out += "}";
    }
    out += "],\"payload_profiles\":[";
    for (std::size_t i = 0; i < sc.payload_profiles.size(); ++i) {
      const schema::PayloadProfileDesc& p = sc.payload_profiles[i];
      if (i != 0) out += ",";
      out += "{\"channel\":";
      AppendJsonString(&out, p.channel);
      out += ",\"profile\":";
      if (p.profile.empty()) {
        out += "null";
      } else {
        AppendJsonString(&out, p.profile);
      }
      out += ",\"profile_version\":" + std::to_string(p.version);
      out += ",\"params_digest\":";
      if (p.params_digest == 0) {
        out += "null";
      } else {
        AppendJsonString(&out, schema::FormatManifestHash(p.params_digest));
      }
      out += "}";
    }
    out += "]";
  }
  out += "}";
  return out;
}

}  // namespace probe
}  // namespace thyme
