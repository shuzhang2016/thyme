// Code generated for the ports fixture graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "dummy/contracts/stub_types.h"
#include "generated/graph_ir.h"
#include "generated/record_manifest.h"
#include "thyme/contracts/abi.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/runtime/record_mirror_ring.h"

enum class RecordStructId : uint16_t {
  kPayload = 0,
};

inline constexpr uint32_t kRecordRegistryCount = 1;

static_assert(kRecordRegistryCount == kRecordManifestCount,
              "record registry and record manifest diverged");

// Edge samples carry the RecordStructId in one byte (perf log);
// 0xFF is reserved as the sampler's no-id sentinel.
static_assert(kRecordRegistryCount <= 255,
              "RecordStructId must fit the perf-log u8 edge id (0xFF reserved)");
static_assert(kRecordRegistryCount <= thyme::kAbiMaxStructCount,
              "record registry exceeds kAbiMaxStructCount (thyme_bin/mirror tables)");
static_assert(sizeof(thyme::Frame<Foo>) <= thyme::kAbiMaxPayloadBytes,
              "record contract Foo: framed sample exceeds the 512 KB record payload ceiling");

enum class RecordPriority : uint8_t { kP0 = 0, kP1 = 1, kP2 = 2 };

struct RecordStructMeta {
  RecordStructId id;
  const char* channel;
  const char* cpp_type;
  const char* producer_node;
  const char* producer_process;
  RecordPriority priority;
  uint16_t contract_version;
  uint32_t payload_size;
};

inline constexpr RecordStructMeta kRecordRegistry[] = {
  { RecordStructId::kPayload, "payload", "Foo", "prod", "main", RecordPriority::kP1, 1, sizeof(thyme::Frame<Foo>) },
};

inline constexpr thyme::RecordMirrorRingSpec kRecordMirrorRingSpecs[] = {
  {sizeof(thyme::Frame<Foo>), 1},
};

static_assert(kGraphRecordGateCount <= kRecordRegistryCount,
              "record gates exceed registry");
static_assert(0u < kRecordRegistryCount, "record gate slot for main out of range");

inline bool RecordTapOwnedByTransport(RecordStructId id) {
  switch (id) {
    default: return false;
  }
}

inline void RecordTapMirror(thyme::RecordMirrorRingPage* page, RecordStructId id,
                            const void* pod, uint32_t size,
                            uint64_t mono_ns, uint32_t source_sequence) {
  if (page == nullptr || RecordTapOwnedByTransport(id)) {
    return;
  }
  (void)page->TryWriteRing(static_cast<uint16_t>(id), pod, size, mono_ns,
                           source_sequence);
}
