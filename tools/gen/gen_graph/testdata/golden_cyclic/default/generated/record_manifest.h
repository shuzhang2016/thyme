// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
// GEN-12 record manifest: the single ordered list of recorded channels.
// RecordStructId is the row index. Ordering rule: unique recorded
// channels, byte-wise ascending by channel name; a broadcast fan-out
// contributes one entry per channel. Reordering graph.yaml edges does
// not change ids. Adding/removing a recorded channel renumbers the
// rows that sort after it, which is an ABI change (ABI-10): the hash
// below and `gen_graph --check` make that visible, never silent.
#pragma once

#include <cstdint>

inline constexpr uint32_t kRecordManifestCount = 0;
inline constexpr uint64_t kRecordManifestHash = 0xcbf29ce484222325ull;

struct RecordManifestEntry {
  uint16_t id;
  const char* channel;
  const char* cpp_type;
  const char* producer_node;
};

inline constexpr RecordManifestEntry kRecordManifest[] = {
  {0, nullptr, nullptr, nullptr},
};

inline constexpr const char* kRecordManifestChannels[] = {nullptr};
