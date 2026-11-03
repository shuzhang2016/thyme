#include "emit_internal.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace gen_emit {
namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

void Mix(uint64_t* hash, const std::string& text) {
  for (const char c : text) {
    *hash ^= static_cast<unsigned char>(c);
    *hash *= kFnvPrime;
  }
  *hash ^= '\n';
  *hash *= kFnvPrime;
}

uint64_t CapabilityHash(const EdgeManifest& m) {
  uint64_t hash = kFnvOffset;
  for (const auto& row : m.edges) {
    const Edge& e = *row.edge;
    const auto& c = row.capability;
    Mix(&hash, e.channel + "|" + e.from + "|" + e.to + "|" + e.shm);
    Mix(&hash, std::string(thyme::ChannelTransportKindName(c.transport_kind)) + "|" +
                   thyme::ChannelRetentionName(c.retention) + "|" +
                   thyme::ChannelReadersName(c.readers) + "|" +
                   thyme::ChannelOverflowName(c.overflow));
    Mix(&hash, std::to_string(c.recordable) + "|" + std::to_string(c.replay_gate) +
                   "|" + std::to_string(c.lease_required) + "|" +
                   std::to_string(c.hub_required) + "|" +
                   std::to_string(c.max_wire_bytes) + "|" +
                   std::to_string(c.slot_count));
  }
  return hash;
}

std::string HashLiteral(uint64_t hash) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%016llxull",
                static_cast<unsigned long long>(hash));
  return buf;
}

std::string Json(const std::string& value) {
  std::string out = "\"";
  for (const char c : value) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

const char* Bool(bool value) { return value ? "true" : "false"; }

}  // namespace

bool EmitChannelCapabilities(const std::string& tmp_dir, const Graph& g,
                             const TemplateCatalog& cat, const EdgeManifest& m) {
  (void)g;
  const uint64_t hash = CapabilityHash(m);
  std::ostringstream h;
  h << BannerHeader(cat);
  h << "#pragma once\n\n#include \"thyme/runtime/channel_capability.h\"\n\n";
  h << "inline constexpr uint32_t kGraphChannelCapabilityVersion = 1;\n";
  h << "inline constexpr uint64_t kGraphChannelCapabilityHash = "
    << HashLiteral(hash) << ";\n";
  h << "inline constexpr std::size_t kGraphChannelCapabilityCount = "
    << m.edges.size() << ";\n\n";
  h << "struct GraphChannelCapability {\n"
       "  const char* channel;\n  const char* from;\n  const char* to;\n"
       "  const char* shm;\n  thyme::ChannelTransportKind transport_kind;\n"
       "  thyme::ChannelRetention retention;\n  thyme::ChannelReaders readers;\n"
       "  thyme::ChannelOverflow overflow;\n  bool recordable;\n"
       "  bool replay_gate;\n  bool lease_required;\n  bool hub_required;\n"
       "  uint64_t max_wire_bytes;\n  uint32_t slot_count;\n};\n\n";
  h << "inline constexpr GraphChannelCapability kGraphChannelCapabilities[] = {\n";
  if (m.edges.empty()) {
    h << "  {nullptr, nullptr, nullptr, nullptr, thyme::ChannelTransportKind::kFrame,\n"
         "   thyme::ChannelRetention::kLatest, thyme::ChannelReaders::kOne,\n"
         "   thyme::ChannelOverflow::kOverwrite, false, false, false, false, 0, 0},\n";
  }
  for (const auto& row : m.edges) {
    const Edge& e = *row.edge;
    const auto& c = row.capability;
    h << "  {\"" << e.channel << "\", \"" << e.from << "\", \"" << e.to
      << "\", \"" << e.shm << "\", thyme::"
      << (c.transport_kind == thyme::ChannelTransportKind::kPinnedSlot
              ? "ChannelTransportKind::kPinnedSlot"
              : "ChannelTransportKind::kFrame")
      << ", thyme::" << (c.retention == thyme::ChannelRetention::kQueue
                             ? "ChannelRetention::kQueue"
                             : "ChannelRetention::kLatest")
      << ", thyme::" << (c.readers == thyme::ChannelReaders::kMany
                             ? "ChannelReaders::kMany"
                             : "ChannelReaders::kOne")
      << ", thyme::";
    switch (c.overflow) {
      case thyme::ChannelOverflow::kOverwrite: h << "ChannelOverflow::kOverwrite"; break;
      case thyme::ChannelOverflow::kDrop: h << "ChannelOverflow::kDrop"; break;
      case thyme::ChannelOverflow::kReject: h << "ChannelOverflow::kReject"; break;
    }
    h << ", " << Bool(c.recordable) << ", " << Bool(c.replay_gate) << ", "
      << Bool(c.lease_required) << ", " << Bool(c.hub_required) << ", "
      << c.max_wire_bytes << "ULL, " << c.slot_count << "u},\n";
  }
  h << "};\n";

  std::ostringstream j;
  j << "{\n  \"version\": 1,\n  \"hash\": \"" << HashLiteral(hash).substr(0, 18)
    << "\",\n  \"channels\": [\n";
  for (std::size_t i = 0; i < m.edges.size(); ++i) {
    const auto& row = m.edges[i];
    const Edge& e = *row.edge;
    const auto& c = row.capability;
    j << "    {\"channel\": " << Json(e.channel) << ", \"from\": " << Json(e.from)
      << ", \"to\": " << Json(e.to) << ", \"shm\": " << Json(e.shm)
      << ", \"transport_kind\": " << Json(thyme::ChannelTransportKindName(c.transport_kind))
      << ", \"retention\": " << Json(thyme::ChannelRetentionName(c.retention))
      << ", \"readers\": " << Json(thyme::ChannelReadersName(c.readers))
      << ", \"overflow\": " << Json(thyme::ChannelOverflowName(c.overflow))
      << ", \"recordable\": " << Bool(c.recordable)
      << ", \"replay_gate\": " << Bool(c.replay_gate)
      << ", \"lease_required\": " << Bool(c.lease_required)
      << ", \"hub_required\": " << Bool(c.hub_required)
      << ", \"max_wire_bytes\": " << c.max_wire_bytes
      << ", \"slot_count\": " << c.slot_count << "}"
      << (i + 1 == m.edges.size() ? "\n" : ",\n");
  }
  j << "  ]\n}\n";

  return WriteFile(fs::path(tmp_dir) / "channel_capabilities.h", h.str()) &&
         WriteFile(fs::path(tmp_dir) / "channel_capabilities.json", j.str());
}

}  // namespace gen_emit
