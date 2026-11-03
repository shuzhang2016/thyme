#include "emit.h"

#include "emit_internal.h"

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

bool EmitReplayInject(const std::string& tmp_dir, const Graph& g,
                      const TemplateCatalog& cat, const EdgeManifest& m) {
  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n\n";
  os << "#include \"generated/wiring.h\"\n";
  os << "#include \"generated/record_registry.h\"\n\n";
  os << "#include \"thyme/app/replay_mode.h\"\n";
  os << "#include \"thyme/runtime/notif_pipe.h\"\n\n";
  os << "#include <cstring>\n\n";
  os << "inline bool ReplayInjectOne(Pipeline& p, uint16_t struct_id,\n";
  os << "                            const void* data, uint32_t size) {\n";
  os << "  switch (struct_id) {\n";
  for (std::size_t i = 0; i < m.recorded.size(); ++i) {
    const auto& r = m.recorded[i];
    const Edge& e = *r.edge;
    if (IsBroadcastDelivery(e)) {
      os << "    case " << i << ": {\n";
      os << "      if (size != sizeof(" << Framed(e.cpp_type) << ")) return false;\n";
      os << "      " << Framed(e.cpp_type) << " tmp{};\n";
      os << "      std::memcpy(&tmp, data, size);\n";
      os << "      p." << SpBroadcastTransportPrefix << BroadcastRingMember(e)
         << ".Push(tmp);\n";
      os << "      return true;\n";
      os << "    }\n";
      continue;
    }
    if (r.to_event) {
      // Event-node input: inject by value into the NotifPipe transport.
      os << "    case " << i << ": {\n";
      os << "      if (size != sizeof(" << Framed(e.cpp_type) << ")) return false;\n";
      os << "      " << Framed(e.cpp_type) << " tmp{};\n";
      os << "      std::memcpy(&tmp, data, size);\n";
      if (IsLocalFrameHubEdge(g, cat, e)) {
        LocalFrameHub hub;
        if (!FindLocalFrameHub(g, cat, e, &hub)) return false;
        os << "      return p." << SpEventTransportPrefix << LocalFrameHubMember(hub)
           << ".input()";
      } else {
        os << "      return p." << SpEventTransportPrefix << r.event_pipe_member;
      }
      os << ".PushOrWait(tmp, thyme::app::replay_mode::MakePushWaitGate());\n";
      os << "    }\n";
      continue;
    }
    if (!r.pool_found) continue;
    os << "    case " << i << ": {\n";
      os << "      if (size != sizeof(" << Framed(e.cpp_type) << ")) return false;\n";
      os << "      auto& pool = p." << r.pool_prefix << "_pool;\n";
    os << "      auto& ring = p." << r.pool_prefix << "_ring;\n";
    os << "      uint32_t idx = pool.acquire();\n";
    os << "      if (idx == " << e.pool << "::kInvalidIndex) return false;\n";
    os << "      std::memcpy(&pool.at(idx), data, size);\n";
    os << "      pool.publish(idx);\n";
    os << "      const uint32_t gen = pool.generation(idx);\n";
    os << "      uint32_t dropped_gen = 0;\n";
    os << "      uint32_t dropped = ring.push_latest(idx, gen, &dropped_gen);\n";
    os << "      if (dropped != 0xFFFFFFFFu) pool.release(dropped, dropped_gen);\n";
    os << "      return true;\n";
    os << "    }\n";
  }
  os << "    default: return false;\n";
  os << "  }\n";
  os << "}\n";
  return WriteFile(fs::path(tmp_dir) / "replay_inject.h", os.str());
}

bool HintSkipsReplayBody(const TemplateCatalog& cat, const std::string& node_type) {
  const IntegrationHintCat* h = FindIntegrationHint(cat, node_type);
  if (h == nullptr) return false;
  return h->checklist.find("replay_mode::Active()") != std::string::npos ||
         h->checklist.find("thyme::app::replay_mode::Active()") !=
             std::string::npos;
}

bool EmitReplayStages(const std::string& tmp_dir, const Graph& g,
                      const TemplateCatalog& cat, const EdgeManifest& m) {
  std::set<std::string> default_pause_producers;
  std::set<std::string> data_producers;
  for (const auto& r : m.recorded) {
    data_producers.insert(r.edge->from);
    data_producers.insert(r.edge->to);
  }
  for (const auto& n : g.nodes) {
    if (HintSkipsReplayBody(cat, n.type)) default_pause_producers.insert(n.id);
  }

  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n\n";
  os << "#include \"generated/record_registry.h\"\n";
  os << "#include \"thyme/app/replay_mode.h\"\n\n";
  os << "#include <cstring>\n\n";
  os << "inline bool ReplayStageIncludes(uint16_t struct_id, const char* stage) {\n";
  os << "  if (stage == nullptr) return false;\n";
  os << "  switch (struct_id) {\n";
  for (std::size_t i = 0; i < m.recorded.size(); ++i) {
    const auto& r = m.recorded[i];
    os << "    case " << i << ": return std::strcmp(stage, \"" << r.edge->to
       << "\") == 0;\n";
  }
  os << "    default: return false;\n";
  os << "  }\n";
  os << "}\n\n";

  os << "inline bool ReplayDefaultPausedProducer(const char* producer_node) {\n";
  os << "  if (producer_node == nullptr) return false;\n";
  if (default_pause_producers.empty()) {
    os << "  return false;\n";
  } else {
    os << "  return ";
    bool first = true;
    for (const auto& id : default_pause_producers) {
      if (!first) os << " || ";
      first = false;
      os << "std::strcmp(producer_node, \"" << id << "\") == 0";
    }
    os << ";\n";
  }
  os << "}\n\n";

  os << "inline bool ReplayIsDataProducer(const char* producer_node) {\n";
  os << "  if (producer_node == nullptr) return false;\n";
  if (data_producers.empty()) {
    os << "  return false;\n";
  } else {
    os << "  return ";
    bool first = true;
    for (const auto& id : data_producers) {
      if (!first) os << " ||\n       ";
      first = false;
      os << "std::strcmp(producer_node, \"" << id << "\") == 0";
    }
    os << ";\n";
  }
  os << "}\n\n";

  os << "inline bool ReplayProducerPausedForInjectStage(const char* producer_node,\n";
  os << "                                                   const char* stage) {\n";
  os << "  if (producer_node == nullptr || stage == nullptr) return false;\n";
  os << "  for (uint16_t sid = 0; sid < kRecordRegistryCount; ++sid) {\n";
  os << "    if (!ReplayStageIncludes(sid, stage)) continue;\n";
  os << "    if (std::strcmp(kRecordRegistry[sid].producer_node, producer_node) == 0) {\n";
  os << "      return true;\n";
  os << "    }\n";
  os << "  }\n";
  os << "  return false;\n";
  os << "}\n\n";

  os << "inline bool ReplayProducerShouldPause(const char* producer_node) {\n";
  os << "  if (thyme::app::replay_mode::Paused()) return true;\n";
  os << "  if (!thyme::app::replay_mode::Active()) return false;\n";
  os << "  if (thyme::app::replay_mode::FullInject()) "
        "return ReplayIsDataProducer(producer_node);\n";
  os << "  const char* stage = thyme::app::replay_mode::InjectStage();\n";
  os << "  if (stage != nullptr) {\n";
  os << "    return ReplayProducerPausedForInjectStage(producer_node, stage);\n";
  os << "  }\n";
  os << "  return ReplayDefaultPausedProducer(producer_node);\n";
  os << "}\n\n";

  os << "inline bool ReplayDefaultInjectAllowed(uint16_t struct_id) {\n";
  os << "  if (struct_id >= kRecordRegistryCount) return false;\n";
  os << "  if (thyme::app::replay_mode::FullInject()) return true;\n";
  os << "  return ReplayDefaultPausedProducer(\n";
  os << "      kRecordRegistry[struct_id].producer_node);\n";
  os << "}\n";

  return WriteFile(fs::path(tmp_dir) / "replay_stages.h", os.str());
}

