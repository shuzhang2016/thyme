#pragma once

// small_struct_injector.h — small-struct half of synchronized replay (REP-05;
// the generated `ReplayInject` path from REP-03 targets the same primitive).
//
// Standalone .tbin and package small.tbin/meta.tbin rows carry a struct_id
// from the file's own registry. Each mapped row goes either to a live
// RecordMirrorRingPage ring:
//
//   TryWriteRing(target_struct_id, payload, ev.len, ev.mono_ns, sequence,
//                gate_token)
//
// or to a synchronous generated publish sink with the same payload, mono_ns,
// sequence and target id. `payload`/`sequence` are the REP-12 policy outputs
// (header_policy.h): source_sequence is renumbered per target from 0 by
// default (renumber_sequence = false passes the recorded producer sequence
// through); a payload starting with a self-consistent FrameHeader is rewritten
// via a per-injector scratch buffer allocated in Open() (mono_ns / session_id /
// sequence + ABI-12 re-seal), so the recorded bytes are never touched. Ring
// publish_index is assigned by the ring itself and the page-level session_id
// belongs to the target; neither is imported from the recording.
//
// TryWriteRing performs the RT-19-safe AtomicPayloadCopyIn and the seqlock
// publish. The sink instead synchronously enters the generated pool/latest/
// NotifPipe/broadcast path and must consume the payload before returning.
//
// The mapping is explicit: (channel, struct_id) -> (ring or publish sink,
// target_struct_id). No name matching, no registry reinterpretation (that is
// REP-11 topology mapping). A struct_id present in the source but absent from
// the mapping is reported as kUnmapped by Inject and counted by the player —
// the mapping is never guessed.
//
// Geometry: Open() resolves the source payload size of each mapped struct
// (registry entry) against the target ring/sink payload capacity and flags a
// struct whose payload cannot fit. Every event is still checked before the
// write: an oversize payload is counted and skipped, never truncated.

#include <cstdint>
#include <string>
#include <vector>

#include "thyme/replay/header_policy.h"
#include "thyme/replay/inject_status.h"
#include "thyme/replay/replay_reader.h"
#include "thyme/runtime/record_mirror_ring.h"

namespace thyme {
namespace replay {

using SmallStructPublishFn = bool (*)(void* ctx, const void* payload,
                                      uint32_t size, uint64_t mono_ns,
                                      uint32_t source_sequence,
                                      uint16_t target_struct_id);

struct SmallStructTarget {
  uint32_t channel = 0;                  // source channel id (small/meta)
  uint16_t struct_id = 0;                // struct_id inside the source registry
  RecordMirrorRingPage* ring = nullptr;  // target page, RW-mapped
  uint16_t target_struct_id = 0;         // target ring/generated struct id
  // Synchronous alternative to ring. The callback must consume payload before
  // returning and must not retain its pointer. publish_ctx may be null.
  SmallStructPublishFn publish = nullptr;
  void* publish_ctx = nullptr;
  uint32_t payload_capacity = 0;
};

struct SmallStructChannelStats {
  uint32_t channel = 0;
  uint16_t struct_id = 0;
  uint16_t target_struct_id = 0;
  uint32_t ring_capacity = 0;    // target ring/sink payload capacity
  uint32_t package_payload = 0;  // source registry payload_size (ABI name kept)
  bool geometry_fits = true;
  uint64_t injected = 0;
  uint64_t bytes = 0;
  uint64_t oversize = 0;
  uint64_t write_failures = 0;  // ring/sink refused the publication
  uint64_t gated = 0;           // replay no longer owns the target struct gate
  uint64_t sequence_errors = 0;
  uint64_t ignored = 0;            // gaps/trigger rows on a mapped struct
  uint64_t headers_rewritten = 0;  // REP-12: embedded FrameHeader rewrites
  uint64_t last_mono_ns = 0;
};

struct SmallStructInjectorStats {
  uint64_t injected = 0;
  uint64_t bytes = 0;
  uint64_t oversize = 0;
  uint64_t write_failures = 0;
  uint64_t sequence_errors = 0;
  uint64_t ignored = 0;
  uint64_t unmapped = 0;
  uint64_t bad_events = 0;
  uint64_t gated = 0;
  uint64_t geometry_oversize_structs = 0;
  uint64_t headers_rewritten = 0;
  uint64_t last_mono_ns = 0;
};

class SmallStructInjector {
 public:
  SmallStructInjector();
  ~SmallStructInjector();
  SmallStructInjector(const SmallStructInjector&) = delete;
  SmallStructInjector& operator=(const SmallStructInjector&) = delete;

  // struct_id must exist in the source channel registry. Ring targets also
  // require target_struct_id in the page. Every target must select exactly one
  // valid ring or sink. kInvalidArgument (error()) otherwise, or for duplicate
  // (channel, struct_id) pairs. A live gate on the same target struct returns
  // kBusy; independent structs on one page may have different replay owners.
  ReplayStatus Open(const ReplayPackage& pkg, const SmallStructTarget* targets,
                    uint32_t count);
  // Standalone .tbin sources use this overload with their explicit local
  // channel id; package channel numbering is not consulted or changed.
  ReplayStatus Open(const ReplayChannel& channel,
                    const SmallStructTarget* targets, uint32_t count);
  void Close();
  bool is_open() const { return open_; }

  // Call before Open(). Disabling the gate is the explicit legacy escape
  // hatch; generated publish sinks never acquire a gate.
  void SetGateEnabled(bool enabled) { gate_enabled_ = enabled; }
  bool gate_enabled() const { return gate_enabled_; }

  // Refresh every acquired target-struct lease. Inject() also heartbeats its
  // target; a paused host must call this periodically.
  void Heartbeat();
  uint64_t gate_rejects() const;
  uint64_t gate_steals() const;

  // True when (channel, struct_id) has a target (used by the player to
  // distinguish "control row, ignored" from "unmapped data").
  bool HasTarget(uint32_t channel, uint32_t struct_id) const;

  // REP-12 header rewrite policy (see header_policy.h). Call before Open();
  // Open() resolves a zero session_id into a generated one.
  void SetHeaderPolicy(const ReplayHeaderPolicy& policy) { policy_ = policy; }
  const ReplayHeaderPolicy& header_policy() const { return policy_; }
  uint64_t session_id() const { return policy_.session_id; }

  InjectStatus Inject(const ReplayEvent& ev);

  const SmallStructInjectorStats& stats() const { return stats_; }
  const std::vector<SmallStructChannelStats>& channels() const {
    return channels_;
  }
  const std::string& error() const { return error_; }

 private:
  struct Entry {
    uint32_t channel = 0;
    uint16_t struct_id = 0;
    uint16_t target_struct_id = 0;
    RecordMirrorRingPage* ring = nullptr;
    SmallStructPublishFn publish = nullptr;
    void* publish_ctx = nullptr;
    uint32_t capacity = 0;
    uint64_t last_sequence = 0;
    uint32_t next_sequence = 0;  // REP-12 renumber counter (injection ordinal)
    bool has_data = false;
  };

  struct GateEntry {
    RecordMirrorRingPage* ring = nullptr;
    uint16_t target_struct_id = 0;
  };

  ReplayStatus OpenChannels(const ReplayPackage* pkg,
                            const ReplayChannel* channel,
                            const SmallStructTarget* targets, uint32_t count);
  ReplayStatus FailOpen(ReplayStatus status);
  void ReleaseGates();
  bool HasGate(const RecordMirrorRingPage* ring,
               uint16_t target_struct_id) const;

  bool open_ = false;
  bool gate_enabled_ = true;
  uint64_t gate_token_ = 0;
  ReplayHeaderPolicy policy_{};  // effective policy (session id resolved)
  std::string error_;
  std::vector<Entry> entries_;
  std::vector<GateEntry> gates_;
  // REP-12 scratch for the embedded-frame rewrite (the recorded payload is a
  // read-only mapping). A Frame wire image is at most 64 KB (uint16 hdr.size),
  // so one fixed buffer covers every frame; allocated in Open(), never on the
  // Inject path.
  std::vector<uint8_t> scratch_;
  std::vector<SmallStructChannelStats> channels_;
  SmallStructInjectorStats stats_;
};

}  // namespace replay
}  // namespace thyme
