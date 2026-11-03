// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/record_registry.h"
#include "thyme/app/replay_mode.h"

#include <cstring>

inline bool ReplayStageIncludes(uint16_t struct_id, const char* stage) {
  if (stage == nullptr) return false;
  switch (struct_id) {
    case 0: return std::strcmp(stage, "sink__s0") == 0;
    case 1: return std::strcmp(stage, "sink__s1") == 0;
    case 2: return std::strcmp(stage, "sink__s0") == 0;
    case 3: return std::strcmp(stage, "sink__s1") == 0;
    default: return false;
  }
}

inline bool ReplayDefaultPausedProducer(const char* producer_node) {
  if (producer_node == nullptr) return false;
  return false;
}

inline bool ReplayIsDataProducer(const char* producer_node) {
  if (producer_node == nullptr) return false;
  return std::strcmp(producer_node, "prod") == 0 ||
       std::strcmp(producer_node, "sink__s0") == 0 ||
       std::strcmp(producer_node, "sink__s1") == 0;
}

inline bool ReplayProducerPausedForInjectStage(const char* producer_node,
                                                   const char* stage) {
  if (producer_node == nullptr || stage == nullptr) return false;
  for (uint16_t sid = 0; sid < kRecordRegistryCount; ++sid) {
    if (!ReplayStageIncludes(sid, stage)) continue;
    if (std::strcmp(kRecordRegistry[sid].producer_node, producer_node) == 0) {
      return true;
    }
  }
  return false;
}

inline bool ReplayProducerShouldPause(const char* producer_node) {
  if (thyme::app::replay_mode::Paused()) return true;
  if (!thyme::app::replay_mode::Active()) return false;
  if (thyme::app::replay_mode::FullInject()) return ReplayIsDataProducer(producer_node);
  const char* stage = thyme::app::replay_mode::InjectStage();
  if (stage != nullptr) {
    return ReplayProducerPausedForInjectStage(producer_node, stage);
  }
  return ReplayDefaultPausedProducer(producer_node);
}

inline bool ReplayDefaultInjectAllowed(uint16_t struct_id) {
  if (struct_id >= kRecordRegistryCount) return false;
  if (thyme::app::replay_mode::FullInject()) return true;
  return ReplayDefaultPausedProducer(
      kRecordRegistry[struct_id].producer_node);
}
