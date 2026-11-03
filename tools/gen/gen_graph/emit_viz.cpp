#include "emit.h"

#include "emit_internal.h"

#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace gen_emit;

namespace {

const char* kGenericSchema =
    "{\"type\":\"object\",\"properties\":{\"mono_ns\":{\"type\":\"integer\"},"
    "\"source_sequence\":{\"type\":\"integer\"},"
    "\"payload_b64\":{\"type\":\"string\"}}}";

// Escape a string for embedding inside a C++ string literal (" and \).
std::string CppEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

// Emit generated/viz_ser.h — the slot -> (schema, serializer) dispatch the
// viz bridge (viz_poll.cpp) uses to publish decoded fields instead of a raw
// base64 envelope.
//
// Two modes, chosen by cat.contracts_json_include (GEN-04: explicit template
// config via contracts_json.txt — the generator no longer probes the project
// tree to guess whether the header exists):
//   * set   (project codegen'd from datatypes.json) -> per-slot dispatch to
//     thyme_contracts_json::ToJson / kSchema_<Type> (structured field JSON, or
//     the opaque envelope for `payload` blob types — both defined by
//     gen_contracts).
//   * empty (no contracts_json.txt) -> generic envelope fallback (base64 of
//     the wire bytes, timing sourced from the packed FrameHeader), matching
//     the historical viz_bridge behavior for projects without datatypes.json
//     (e.g. dummy).
bool EmitVizSer(const std::string& tmp_dir, const Graph& /*g*/,
                const TemplateCatalog& cat, const EdgeManifest& m) {
  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n\n";
  os << "#include \"generated/record_registry.h\"\n";
  os << "#include <cstddef>\n";
  os << "#include <cstdint>\n";
  os << "#include <string>\n";

  const std::string& json_inc = cat.contracts_json_include;
  const bool contracts_present = !json_inc.empty();

  if (contracts_present) {
    os << "#include \"" << json_inc << "\"\n";
    os << "#include <cstring>\n";
  } else {
    os << "#include \"thyme/contracts/frame_header.h\"\n";
    os << "\nnamespace {\n";
    os << "inline void VizB64(std::string& out, const uint8_t* p, std::size_t n) {\n";
    os << "  static const char kTab[] = \"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/\";\n";
    os << "  for (std::size_t i = 0; i < n; i += 3) {\n";
    os << "    const uint32_t a = p[i];\n";
    os << "    const uint32_t b = (i + 1 < n) ? p[i + 1] : 0;\n";
    os << "    const uint32_t c = (i + 2 < n) ? p[i + 2] : 0;\n";
    os << "    const uint32_t x = (a << 16) | (b << 8) | c;\n";
    os << "    out += kTab[(x >> 18) & 63];\n";
    os << "    out += kTab[(x >> 12) & 63];\n";
    os << "    out += (i + 1 < n) ? kTab[(x >> 6) & 63] : '=';\n";
    os << "    out += (i + 2 < n) ? kTab[x & 63] : '=';\n";
    os << "  }\n";
    os << "}\n";
    os << "}  // namespace\n";
  }
  os << "\n";

  // ---- VizSchemaSlot ----
  os << "inline const char* VizSchemaSlot(uint16_t sid) {\n";
  if (!contracts_present) {
    os << "  (void)sid;\n";
    os << "  return \"" << CppEscape(kGenericSchema) << "\";\n";
  } else {
    os << "  switch (sid) {\n";
    for (std::size_t i = 0; i < m.recorded.size(); ++i) {
      os << "    case " << i << ": return thyme_contracts_json::kSchema_"
         << m.recorded[i].edge->cpp_type << ";\n";
    }
    os << "    default: return \"{}\";\n";
    os << "  }\n";
  }
  os << "}\n\n";

  // ---- VizEncodeSlot ----
  os << "inline bool VizEncodeSlot(uint16_t sid, const uint8_t* bytes, "
        "std::size_t n, std::string* out) {\n";
  os << "  if (out == nullptr || bytes == nullptr) return false;\n";
  if (!contracts_present) {
    os << "  (void)sid;\n";
    os << "  const thyme::FrameHeader* h = reinterpret_cast<const thyme::FrameHeader*>(bytes);\n";
    os << "  out->append(\"{\\\"mono_ns\\\":\");\n";
    os << "  out->append(std::to_string(h->mono_ns));\n";
    os << "  out->append(\",\\\"source_sequence\\\":\");\n";
    os << "  out->append(std::to_string(h->sequence));\n";
    os << "  out->append(\",\\\"payload_b64\\\":\\\"\");\n";
    os << "  VizB64(*out, bytes, n);\n";
    os << "  out->append(\"\\\"}\");\n";
    os << "  return true;\n";
  } else {
    os << "  switch (sid) {\n";
    for (std::size_t i = 0; i < m.recorded.size(); ++i) {
      const std::string& t = m.recorded[i].edge->cpp_type;
      os << "    case " << i << ": {\n";
      os << "      " << Framed(t) << " v{};\n";
      os << "      std::memcpy(&v, bytes, n < sizeof(" << Framed(t)
         << ") ? n : sizeof(" << Framed(t) << "));\n";
      os << "      thyme_contracts_json::ToJson(v, *out);\n";
      os << "      return true;\n";
      os << "    }\n";
    }
    os << "    default: return false;\n";
    os << "  }\n";
  }
  os << "}\n";

  return WriteFile(fs::path(tmp_dir) / "viz_ser.h", os.str());
}
