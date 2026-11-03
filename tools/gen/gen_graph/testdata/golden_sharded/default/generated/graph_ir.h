// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once
#include <cstddef>
#include <cstdint>

#ifndef THYME_GRAPH_HAS_RECORD
#define THYME_GRAPH_HAS_RECORD 1
#endif

inline constexpr int kGraphHealthN = 3;
inline constexpr const char* kGraphShmPrefixFmt = "/c9_%d";
inline constexpr uint64_t kGraphHash = 0x713491718c52efcbULL;
static_assert(kGraphHash != 0, "generated graph hash must be non-zero");

enum class GraphShmKind : int { None = 0 };

struct GraphProcess {
  const char* id;
  const char* role;
};
inline constexpr GraphProcess kGraphProcesses[] = {
  {"main", "main"} };

struct GraphNode {
  const char* id;
  const char* type;
  const char* process;
  int hz;
};
inline constexpr GraphNode kGraphNodes[] = {
  {"prod", "producer", "main", 50},
  {"sink__s0", "sink", "main", 50},
  {"sink__s1", "sink", "main", 50} };
inline constexpr std::size_t kGraphNodesCount = 3;

#ifndef THYME_GRAPH_HAS_EVENT_NODES
#define THYME_GRAPH_HAS_EVENT_NODES 1
#endif

inline constexpr bool kGraphNodeIsEvent[] = {false, true, true};

struct GraphShmChannel {
  const char* suffix;
  const char* channel;
  GraphShmKind kind;
  const char* from_id;
  const char* to_id;
  const char* from_process;
  const char* to_process;
  const char* delivery;
};
inline constexpr GraphShmChannel kGraphShmChannels[] = { };

struct GraphRecordGate {
  const char* process;
  uint16_t slot;
};
inline constexpr std::size_t kGraphRecordGateCount = 1;
inline constexpr GraphRecordGate kGraphRecordGates[] = {
  {"main", 0} };
