#include "emit.h"

#include "emit_internal.h"

#include "rate.h"
#include "thyme/contracts/frame_header.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace gen_emit;

namespace gen_emit {

const char* RecordPriorityHeuristic(const std::string& cpp_type) {
  if (cpp_type.find("Safety") != std::string::npos ||
      cpp_type.find("CanOut") != std::string::npos ||
      cpp_type == "ArControlSlot") {
    return "RecordPriority::kP0";
  }
  return "RecordPriority::kP1";
}

const char* RecordPriorityForEdge(const Edge& e) {
  if (e.record_priority == "p0") return "RecordPriority::kP0";
  if (e.record_priority == "p2") return "RecordPriority::kP2";
  if (e.record_priority == "p1") return "RecordPriority::kP1";
  return RecordPriorityHeuristic(e.cpp_type);
}

const char* RecordPriorityLabel(const Edge& e) {
  if (e.record_priority == "p0") return "P0";
  if (e.record_priority == "p2") return "P2";
  if (e.record_priority == "p1") return "P1";
  const char* tok = RecordPriorityHeuristic(e.cpp_type);
  return std::string(tok).find("kP0") != std::string::npos ? "P0" : "P1";
}

}  // namespace gen_emit

// GEN-12: generated/record_manifest.h is the single ordered recorded-channel
// list. It is emitted for every graph (count 0 included) so `gen_graph --check`
// surfaces any change to the recorded set, including "all record: false".
constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

bool EmitRecordManifest(const std::string& tmp_dir, const Graph& /*g*/,
                        const TemplateCatalog& cat,
                        const EdgeManifest& m) {
  uint64_t hash = kFnvOffsetBasis;
  auto mix = [&hash](const std::string& s) {
    for (const char c : s) {
      hash ^= static_cast<unsigned char>(c);
      hash *= kFnvPrime;
    }
    hash ^= '\n';
    hash *= kFnvPrime;
  };
  for (std::size_t i = 0; i < m.recorded.size(); ++i) {
    mix(std::to_string(i) + ":" + m.recorded[i].edge->channel + ":" +
        m.recorded[i].edge->cpp_type);
  }
  for (const auto& row : m.bulk) {
    const Edge& edge = *row.edge;
    mix("bulk:" + edge.channel + ":" + edge.record_kind + ":" +
        edge.record_shm + ":" + std::to_string(edge.bitrate_kbps) + ":" +
        std::to_string(edge.avg_frame_bytes));
  }
  char hash_buf[32];
  std::snprintf(hash_buf, sizeof(hash_buf), "0x%016llxull",
                static_cast<unsigned long long>(hash));

  std::ostringstream os;
  os << BannerHeader(cat);
  os << "// GEN-12 record manifest: the single ordered list of recorded channels.\n";
  os << "// RecordStructId is the row index. Ordering rule: unique recorded\n";
  os << "// channels, byte-wise ascending by channel name; a broadcast fan-out\n";
  os << "// contributes one entry per channel. Reordering graph.yaml edges does\n";
  os << "// not change ids. Adding/removing a recorded channel renumbers the\n";
  os << "// rows that sort after it, which is an ABI change (ABI-10): the hash\n";
  os << "// below and `gen_graph --check` make that visible, never silent.\n";
  os << "#pragma once\n\n";
  os << "#include <cstdint>\n\n";
  os << "inline constexpr uint32_t kRecordManifestCount = "
     << m.recorded.size() << ";\n";
  os << "inline constexpr uint64_t kRecordManifestHash = " << hash_buf << ";\n\n";
  os << "struct RecordManifestEntry {\n";
  os << "  uint16_t id;\n";
  os << "  const char* channel;\n";
  os << "  const char* cpp_type;\n";
  os << "  const char* producer_node;\n";
  os << "};\n\n";
  os << "inline constexpr RecordManifestEntry kRecordManifest[] = {\n";
  if (m.recorded.empty()) {
    os << "  {0, nullptr, nullptr, nullptr},\n";
  } else {
    for (std::size_t i = 0; i < m.recorded.size(); ++i) {
      const auto& r = m.recorded[i];
      os << "  {" << i << ", \"" << r.edge->channel << "\", \""
         << r.edge->cpp_type << "\", \"" << r.edge->from << "\"},\n";
    }
  }
  os << "};\n\n";
  os << "inline constexpr const char* kRecordManifestChannels[] = {";
  if (m.recorded.empty()) {
    os << "nullptr";
  } else {
    for (std::size_t i = 0; i < m.recorded.size(); ++i) {
      os << (i == 0 ? "\n" : ",\n") << "  \"" << m.recorded[i].edge->channel
         << "\"";
    }
  }
  os << "};\n";
  if (!m.bulk.empty()) {
    os << "\n// Bulk recorder sources. These names are not RecordStructIds.\n";
    os << "inline constexpr const char* kRecordBulkChannels[] = {\n";
    for (std::size_t i = 0; i < m.bulk.size(); ++i) {
      os << "  \"" << m.bulk[i].edge->channel << "\",\n";
    }
    os << "};\n";
  }
  return WriteFile(fs::path(tmp_dir) / "record_manifest.h", os.str());
}

const LayoutFieldCat* FindLayoutField(const ContractLayoutCat& layout,
                                      const char* name) {
  for (const LayoutFieldCat& field : layout.fields) {
    if (field.name == name) return &field;
  }
  return nullptr;
}

bool FieldIs(const LayoutFieldCat* field, const char* type, uint32_t size) {
  return field != nullptr && field->type == type && field->count == 1 &&
         field->size == size;
}

// OPS-13: [chassis] comes from the catalog extra plus the contract layout.
// Offsets are into the recorded Frame<T> sample, not the bare payload and
// not the idle detector's 0/4 defaults. A missing fact omits the section.
bool AppendChassisSection(std::ostringstream& os, const Graph& g,
                          const TemplateCatalog& cat, const EdgeManifest& m) {
  const ExtraBufCat* chassis = nullptr;
  for (const ExtraBufCat& extra : cat.extras) {
    if (extra.name != "chassis") continue;
    if (chassis != nullptr) {
      std::fprintf(stderr, "EmitTriggerBase: duplicate extras.txt chassis buffer\n");
      return false;
    }
    chassis = &extra;
  }
  if (chassis == nullptr) return true;

  std::vector<std::size_t> hits;
  for (std::size_t i = 0; i < m.recorded.size(); ++i) {
    if (m.recorded[i].edge->cpp_type == chassis->cpp_type) hits.push_back(i);
  }
  if (hits.empty()) {
    os << "# chassis section omitted: " << chassis->cpp_type
       << " is not a recorded mirror struct.\n";
    return true;
  }
  if (hits.size() != 1) {
    std::fprintf(stderr,
                 "EmitTriggerBase: chassis cpp_type %s is recorded on more "
                 "than one channel\n",
                 chassis->cpp_type.c_str());
    return false;
  }
  const ContractLayoutCat* layout = FindContractLayout(cat, chassis->cpp_type);
  if (layout == nullptr) {
    os << "# chassis section omitted: no contract layout for "
       << chassis->cpp_type << ".\n";
    return true;
  }
  const LayoutFieldCat* epb = FindLayoutField(*layout, "park_brk_status");
  const LayoutFieldCat* speed = FindLayoutField(*layout, "vehicle_speed");
  if (!FieldIs(epb, "uint8", 1) || !FieldIs(speed, "float32", 4)) {
    os << "# chassis section omitted: " << chassis->cpp_type
       << " has no uint8 park_brk_status and float32 vehicle_speed.\n";
    return true;
  }
  if (layout->align == 0 || (layout->align & (layout->align - 1)) != 0) {
    std::fprintf(stderr, "EmitTriggerBase: chassis layout align %u is not a "
                         "power of two\n",
                 layout->align);
    return false;
  }
  if (epb->offset + epb->size > layout->payload_bytes ||
      speed->offset + speed->size > layout->payload_bytes) {
    std::fprintf(stderr, "EmitTriggerBase: chassis field offset exceeds "
                         "payload of %s\n",
                 chassis->cpp_type.c_str());
    return false;
  }
  const uint32_t header = static_cast<uint32_t>(sizeof(thyme::FrameHeader));
  const uint32_t start = (header + layout->align - 1u) / layout->align * layout->align;
  os << "# Offsets are into the recorded Frame sample.\n";
  os << "[chassis]\n";
  os << "page_shm = " << g.shm_prefix << "_rec\n";
  os << "struct_id = " << hits[0] << "\n";
  os << "epb_offset = " << (start + epb->offset) << "\n";
  os << "speed_offset = " << (start + speed->offset) << "\n";
  return true;
}

std::string BulkRecordShm(const Graph& g, const Edge& edge) {
  if (!edge.record_shm.empty()) return edge.record_shm;
  if (!edge.shm.empty()) return g.shm_prefix + edge.shm;
  return "/" + edge.channel;
}

bool AppendBulkSections(std::ostringstream& os, const Graph& g,
                        const EdgeManifest& m) {
  if (m.bulk.empty()) return true;
  RateInfo rates;
  std::vector<std::string> warnings;
  (void)DeriveRates(g, &rates, &warnings);
  os << "# Bulk sources are not mirror structs. gop_s is not a graph field.\n";
  for (const auto& row : m.bulk) {
    const Edge& edge = *row.edge;
    os << "[channel." << BulkRecordShm(g, edge) << "]\n";
    os << "name = " << edge.channel << "\n";
    if (edge.record_kind == "h264") {
      os << "kind = h264_stream\n";
      os << "bitrate_kbps = " << edge.bitrate_kbps << "\n";
      continue;
    }
    os << "kind = pinned_slot\n";
    os << "avg_frame_bytes = " << edge.avg_frame_bytes << "\n";
    const auto rate = rates.edge_producer_rate.find(
        edge.from + "->" + edge.to + ":" + edge.channel);
    if (edge.feedback || rate == rates.edge_producer_rate.end() ||
        rate->second <= 0) {
      os << "# rate_hz omitted: graph rate is unresolved.\n";
    } else {
      os << "rate_hz = " << rate->second << "\n";
    }
  }
  return true;
}

bool EmitTriggerBase(const std::string& tmp_dir, const Graph& g,
                     const TemplateCatalog& cat, const EdgeManifest& m) {
  std::ostringstream os;
  os << "# Code generated from graph.yaml. DO NOT EDIT.\n";
  os << "# Mirror records aggregate into one source. record_kind pinned_slot or "
        "h264 is a separate source. gop_s is not a graph field.\n";
  if (m.recorded.empty()) {
    if (!AppendBulkSections(os, g, m)) return false;
    if (!AppendChassisSection(os, g, cat, m)) return false;
    return WriteFile(fs::path(tmp_dir) / "trigger.base.ini", os.str());
  }

  os << "# One record mirror aggregates all manifest structs; never emit one source per struct.\n";
  os << "# The default supervisor mirror name retains %pid and must match --record-shm at deployment.\n";
  os << "[channel." << g.shm_prefix << "_rec]\n";
  os << "kind = mirror_ring\n";
  os << "record_channels = ";
  for (std::size_t i = 0; i < m.recorded.size(); ++i) {
    os << (i == 0 ? "" : ",") << m.recorded[i].edge->channel;
  }
  os << "\n";

  RateInfo rates;
  std::vector<std::string> warnings;
  (void)DeriveRates(g, &rates, &warnings);
  uint64_t total_rate = 0;
  uint64_t total_bytes_per_second = 0;
  bool complete = true;
  for (const auto& row : m.recorded) {
    const Edge& edge = *row.edge;
    const ContractSizeCat* size = FindContractSize(cat, edge.cpp_type);
    const auto rate = rates.edge_producer_rate.find(
        edge.from + "->" + edge.to + ":" + edge.channel);
    if (size == nullptr || edge.feedback || rate == rates.edge_producer_rate.end() ||
        rate->second <= 0) {
      complete = false;
      break;
    }
    total_rate += static_cast<uint64_t>(rate->second);
    total_bytes_per_second += static_cast<uint64_t>(rate->second) *
                              (sizeof(thyme::FrameHeader) + size->bytes);
  }
  if (complete) {
    const uint64_t average = (total_bytes_per_second + total_rate - 1) / total_rate;
    os << "# Aggregate rate and ceiling of the weighted mean framed wire size.\n";
    os << "rate_hz = " << total_rate << "\n";
    os << "avg_frame_bytes = " << average << "\n";
  } else {
    os << "# rate_hz/avg_frame_bytes omitted: rate or contract size metadata is incomplete.\n";
  }
  if (!AppendBulkSections(os, g, m)) return false;
  if (!AppendChassisSection(os, g, cat, m)) return false;
  return WriteFile(fs::path(tmp_dir) / "trigger.base.ini", os.str());
}

bool EmitRecordRegistry(const std::string& tmp_dir, const Graph& g,
                        const TemplateCatalog& cat,
                        const EdgeManifest& m) {
  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n\n";
  if (!m.recorded.empty()) {
    os << "#include \"" << cat.contracts_include << "\"\n";
  }
  os << "#include \"generated/graph_ir.h\"\n";
  os << "#include \"generated/record_manifest.h\"\n";
  os << "#include \"thyme/contracts/abi.h\"\n";
  os << "#include \"thyme/record/thyme_bin.h\"\n";
  os << "#include \"thyme/runtime/record_mirror_ring.h\"\n\n";
  os << "enum class RecordStructId : uint16_t {\n";
  for (std::size_t i = 0; i < m.recorded.size(); ++i) {
    os << "  k" << PascalCase(m.recorded[i].edge->channel) << " = " << i
       << ",\n";
  }
  os << "};\n\n";
  os << "inline constexpr uint32_t kRecordRegistryCount = "
     << m.recorded.size() << ";\n\n";
  os << "static_assert(kRecordRegistryCount == kRecordManifestCount,\n"
     << "              \"record registry and record manifest diverged\");\n\n";
  os << "// Edge samples carry the RecordStructId in one byte (perf log);\n";
  os << "// 0xFF is reserved as the sampler's no-id sentinel.\n";
  os << "static_assert(kRecordRegistryCount <= 255,\n"
     << "              \"RecordStructId must fit the perf-log u8 edge id (0xFF reserved)\");\n";
  // GEN-08: the record layer is bounded by ABI ceilings, not just by the
  // u8 perf-log id. thyme_bin / RecordMirror arrays are sized by
  // kAbiMaxStructCount (64) and each mirror slot by kAbiMaxPayloadBytes
  // (512 KB); exceeding either must fail the build, not corrupt a recording.
  os << "static_assert(kRecordRegistryCount <= thyme::kAbiMaxStructCount,\n"
     << "              \"record registry exceeds kAbiMaxStructCount (thyme_bin/mirror tables)\");\n";
  {
    std::set<std::string> emitted_type;
    for (const auto& r : m.recorded) {
      if (!emitted_type.insert(r.edge->cpp_type).second) continue;
      os << "static_assert(sizeof(" << Framed(r.edge->cpp_type)
         << ") <= thyme::kAbiMaxPayloadBytes,\n"
         << "              \"record contract " << r.edge->cpp_type
         << ": framed sample exceeds the 512 KB record payload ceiling\");\n";
    }
  }
  os << "\n";
  os << "enum class RecordPriority : uint8_t { kP0 = 0, kP1 = 1, kP2 = 2 };\n\n";
  os << "struct RecordStructMeta {\n";
  os << "  RecordStructId id;\n";
  os << "  const char* channel;\n";
  os << "  const char* cpp_type;\n";
  os << "  const char* producer_node;\n";
  os << "  const char* producer_process;\n";
  os << "  RecordPriority priority;\n";
  os << "  uint16_t contract_version;\n";
  os << "  uint32_t payload_size;\n";
  os << "};\n\n";
  os << "inline constexpr RecordStructMeta kRecordRegistry[] = {\n";
  for (std::size_t i = 0; i < m.recorded.size(); ++i) {
    const auto& r = m.recorded[i];
    os << "  { RecordStructId::k" << PascalCase(r.edge->channel) << ", \""
       << r.edge->channel << "\", \"" << r.edge->cpp_type << "\", \""
       << r.edge->from << "\", \"" << r.from_process << "\", "
        << r.record_priority << ", 1, sizeof(" << Framed(r.edge->cpp_type)
       << ") },\n";
  }
  os << "};\n\n";
  // Depth one preserves the generated app's existing latest-value tap semantics.
  // Trigger-window sizing can be introduced separately without changing ids.
  os << "inline constexpr thyme::RecordMirrorRingSpec kRecordMirrorRingSpecs[] = {\n";
  for (const auto& r : m.recorded) {
    os << "  {sizeof(" << Framed(r.edge->cpp_type) << "), 1},\n";
  }
  os << "};\n\n";
  const auto& gates = m.record_gates;
  os << "static_assert(kGraphRecordGateCount <= kRecordRegistryCount,\n"
     << "              \"record gates exceed registry\");\n";
  for (const auto& gate : gates) {
    os << "static_assert(" << gate.second
       << "u < kRecordRegistryCount, \"record gate slot for " << gate.first
       << " out of range\");\n";
  }
  os << "\ninline bool RecordTapOwnedByTransport(RecordStructId id) {\n";
  os << "  switch (id) {\n";
  for (std::size_t i = 0; i < m.recorded.size(); ++i) {
    const Edge& e = *m.recorded[i].edge;
    if (!EdgeUsesEventPipe(g, e) && !IsBroadcastDelivery(e)) continue;
    os << "    case RecordStructId::k" << PascalCase(e.channel) << ": return true;\n";
  }
  os << "    default: return false;\n";
  os << "  }\n";
  os << "}\n\n";
  os << "inline void RecordTapMirror(thyme::RecordMirrorRingPage* page, RecordStructId id,\n";
  os << "                            const void* pod, uint32_t size,\n";
  os << "                            uint64_t mono_ns, uint32_t source_sequence) {\n";
  os << "  if (page == nullptr || RecordTapOwnedByTransport(id)) {\n";
  os << "    return;\n";
  os << "  }\n";
  os << "  (void)page->TryWriteRing(static_cast<uint16_t>(id), pod, size, mono_ns,\n";
  os << "                           source_sequence);\n";
  os << "}\n";
  return WriteFile(fs::path(tmp_dir) / "record_registry.h", os.str());
}

bool EmitRecordTable(const std::string& tmp_dir, const Graph& /*g*/,
                     const TemplateCatalog& cat,
                     const EdgeManifest& m) {
  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n\n";
  os << "#include \"generated/record_registry.h\"\n";
  os << "#include \"thyme/record/record_schema.h\"\n";
  os << "#include \"thyme/record/record_table.h\"\n";
  if (!cat.contracts_layout_include.empty()) {
    os << "#include \"" << cat.contracts_layout_include << "\"\n";
  }
  os << "#include <cstdio>\n";
  os << "#include <cstdlib>\n\n";
  // ABI-09/ABI-10: the record manifest identity of this build. The hash is
  // GEN-12's kRecordManifestHash (the sorted recorded-channel list); the
  // contract lookup comes from the gen_contracts field-layout table when the
  // template declares one. A recorder forwards this into
  // triggered::Config::record_manifest; packages then carry the manifest
  // block and a field-level schema.json sidecar.
  os << "inline const thyme::schema::RecordManifest kThymeRecordManifest = {\n";
  os << "    kRecordManifestHash,\n";
  os << "    thyme::schema::kManifestVersion,\n";
  if (!cat.contracts_layout_include.empty()) {
    os << "    &thyme_contract_layout::FindContractLayout};\n\n";
  } else {
    os << "    nullptr};  // no contracts_layout.txt: field layout unavailable\n\n";
  }
  os << "inline uint32_t ThymeRecordPayloadSize(uint16_t sid) {\n";
  if (m.recorded.empty()) {
    os << "  (void)sid;\n  return 0;\n";
  } else {
    os << "  return sid < kRecordRegistryCount ? kRecordRegistry[sid].payload_size : 0;\n";
  }
  os << "}\n\n";
  os << "inline thyme::ThymeRecordPriority ThymeRecordPriorityOf(uint16_t sid) {\n";
  if (m.recorded.empty()) {
    os << "  (void)sid;\n  return thyme::ThymeRecordPriority::kP1;\n";
  } else {
    os << "  if (sid >= kRecordRegistryCount) return thyme::ThymeRecordPriority::kP1;\n";
    os << "  switch (kRecordRegistry[sid].priority) {\n";
    os << "    case RecordPriority::kP0: return thyme::ThymeRecordPriority::kP0;\n";
    os << "    case RecordPriority::kP2: return thyme::ThymeRecordPriority::kP2;\n";
    os << "    default: return thyme::ThymeRecordPriority::kP1;\n";
    os << "  }\n";
  }
  os << "}\n\n";
  os << "inline void ThymeRecordFillStructDescs(thyme::ThymeStructDesc* out, std::size_t cap) {\n";
  if (m.recorded.empty()) {
    os << "  (void)out;\n  (void)cap;\n";
  } else {
    os << "  if (cap < kRecordRegistryCount) {\n";
    os << "    std::fprintf(stderr,\n";
    os << "                 \"ThymeRecordFillStructDescs: cap %zu < kRecordRegistryCount %u\\n\",\n";
    os << "                 cap, kRecordRegistryCount);\n";
    os << "    std::abort();\n";
    os << "  }\n";
    os << "  for (std::size_t i = 0; i < kRecordRegistryCount; ++i) {\n";
    os << "    out[i].struct_id = static_cast<uint16_t>(i);\n";
    os << "    out[i].contract_version = kRecordRegistry[i].contract_version;\n";
    os << "    out[i].payload_size = kRecordRegistry[i].payload_size;\n";
    os << "    std::snprintf(out[i].name, sizeof(out[i].name), \"%s\",\n";
    os << "                  kRecordRegistry[i].cpp_type);\n";
    os << "    // ABI-11: stable record identity. The layout hash comes from the\n";
    os << "    // generated contract model (static_assert-bound to the real\n";
    os << "    // offsetof/sizeof); it stays 0 when this host has no layout table.\n";
    os << "    const thyme::schema::ContractLayout* layout =\n";
    os << "        kThymeRecordManifest.contract != nullptr\n";
    os << "            ? kThymeRecordManifest.contract(kRecordRegistry[i].cpp_type)\n";
    os << "            : nullptr;\n";
    os << "    out[i].layout_hash = layout != nullptr\n";
    os << "                             ? thyme::schema::ComputeLayoutHash(*layout)\n";
    os << "                             : thyme::schema::kLayoutHashNone;\n";
    os << "    std::snprintf(out[i].channel, sizeof(out[i].channel), \"%s\",\n";
    os << "                  kRecordRegistry[i].channel);\n";
    os << "    std::snprintf(out[i].record_key, sizeof(out[i].record_key), \"%s\",\n";
    os << "                  kRecordRegistry[i].channel);\n";
    os << "  }\n";
  }
  os << "}\n\n";
  os << "inline thyme::ThymeRecordTable GetThymeRecordTable() {\n";
  os << "  return thyme::ThymeRecordTable{\n";
  os << "      kRecordRegistryCount,\n";
  os << "      &ThymeRecordPayloadSize,\n";
  os << "      &ThymeRecordPriorityOf,\n";
  os << "      &ThymeRecordFillStructDescs,\n";
  os << "      &kThymeRecordManifest,\n";
  os << "  };\n";
  os << "}\n\n";
  os << "inline uint64_t ThymeRecordMirrorRingRequiredBytes() {\n";
  os << "  thyme::ThymeStructDesc descs[thyme::kRecordMirrorRingMaxStructs];\n";
  os << "  ThymeRecordFillStructDescs(descs, thyme::kRecordMirrorRingMaxStructs);\n";
  os << "  return thyme::RecordMirrorRingPage::RequiredBytes(\n";
  os << "      descs, kRecordMirrorRingSpecs, kRecordRegistryCount);\n";
  os << "}\n\n";
  os << "inline uint64_t ThymeRecordMirrorRingConfigHash() {\n";
  os << "  thyme::ThymeStructDesc descs[thyme::kRecordMirrorRingMaxStructs];\n";
  os << "  ThymeRecordFillStructDescs(descs, thyme::kRecordMirrorRingMaxStructs);\n";
  os << "  return thyme::RecordMirrorRingPage::HashConfig(\n";
  os << "      descs, kRecordMirrorRingSpecs, kRecordRegistryCount);\n";
  os << "}\n";
  return WriteFile(fs::path(tmp_dir) / "record_table.h", os.str());
}


bool EmitRecordTapHelpers(const std::string& tmp_dir, const Graph& /*g*/,
                          const TemplateCatalog& cat,
                          const EdgeManifest& m) {
  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n\n";
  os << "#include \"generated/record_registry.h\"\n";
  os << "#include \"support/pipeline_support.h\"\n\n";
  {
    std::set<std::string> seen_channel;
    for (const auto& r : m.recorded) {
      if (!seen_channel.insert(r.edge->channel).second) continue;
      os << "inline void RecordTap" << PascalCase(r.edge->channel)
         << "(thyme::RecordMirrorRingPage* page, const "
         << Framed(r.edge->cpp_type) << "& v) {\n";
      os << "  RecordTapMirror(page, RecordStructId::k"
         << PascalCase(r.edge->channel)
         << ", &v, sizeof(v), NowMonoNs(), v.hdr.sequence);\n";
      os << "}\n\n";
    }
  }
  return WriteFile(fs::path(tmp_dir) / "record_tap_helpers.h", os.str());
}

bool EmitGraphRecordCheck(const std::string& tmp_dir, const Graph& /*g*/,
                          const TemplateCatalog& cat,
                          const EdgeManifest& m) {
  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n\n";
  os << "#include \"generated/record_registry.h\"\n\n";
  os << "static_assert(kRecordRegistryCount == " << m.recorded.size()
     << ",\n              \"record registry must cover every recorded channel\");\n";
  return WriteFile(fs::path(tmp_dir) / "graph_record_check.h", os.str());
}

