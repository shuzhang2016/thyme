#pragma once

// pinned_slot_injector.h — REP-04: replay big-frame payloads into a live
// PinnedSlotStore as a synthetic producer (roadmap §3.17).
//
// The package stores each frame twice: the FrameIndexRecord (offset/len/
// mono_ns/crc/flags) in frames/<channel>.index.tbin and the raw segment in
// frames/<channel>.raw. The injector copies the mapped .raw segment straight
// into the target channel's producer slot and publishes it with the recorded
// mono_ns:
//
//   Begin(ev.len) -> memcpy(target payload, ev.payload, ev.len)
//                 -> REP-12 header rewrite (ReplayHeaderPolicy, see
//                    header_policy.h): an embedded FrameHeader gets the
//                    replay-scheduled mono_ns, the replay session id and a
//                    renumbered sequence, and is re-sealed per ABI-12
//                 -> header->flags = ev.frame_flags
//                 -> Commit(ev.mono_ns)  (recomputes the slot CRC over the
//                    rewritten payload, or stores 0 when the channel has CRC
//                    disabled)
//
// Injection contract
// ------------------
// * The injector is the *only* writer of every target channel while it is
//   open (a replay player owns its targets; roadmap: "pinned_slot replay is a
//   fake producer and must own the target channel"). Begin() hands out the
//   producer slot, so a plain memcpy is correct here — AtomicPayloadCopyIn is
//   for copying under a concurrent writer, which does not exist on this side.
//   Readers synchronize on the slot seqlock exactly as they do for a real
//   producer.
// * The writer recomputes the slot CRC on Commit (when the channel has
//   kPinnedSlotFlagCrcEnabled). Independently, with verification enabled the
//   injector checks ev.payload_crc against Crc32(ev.payload, ev.len) *before*
//   writing; a mismatch is counted (crc_mismatches) and the frame is skipped,
//   never published as corrupt data. Events with payload_crc == 0 (recorder
//   CRC disabled) are not verified.
// * frame_flags (e.g. h264 IDR) are copied verbatim; they are the producer's
//   per-frame payload description, not reader state.
// * No per-event allocation: the channel table is built in Open() and every
//   Inject() is a bounds check, a memcpy and two atomics.
//
// Geometry
// --------
// The mapping is explicit (package channel id -> PinnedSlotWriter*); there is
// no name matching. Open() records the package's largest frame per channel
// (ReplayChannel::max_data_len, seeded by the reader's validation scan) next
// to the target geometry and flags a channel whose package frames cannot all
// fit (geometry_fits == false, stats().geometry_oversize_channels). Every
// event is still checked individually: an event larger than the target
// slot_bytes is counted (oversize) and skipped — the injector never calls
// Begin() with an oversize length and never truncates a frame. A skipped
// pinned slot is distinguished from oversize with PinnedSlotWriter::
// pending_skipped(), exactly as the BF-03 contract requires.
//
// Prefetch: deliberately not implemented. The reader's .raw payload pointers
// are mmap-stable, but a one-frame prefetch would have to hold a *target*
// slot across the next NextEvent() (the slot is not stable), which is the
// exact lifetime hazard BF-03/BF-11 guard against. The synchronous copy is
// bounded by slot_bytes.

#include "thyme/replay/header_policy.h"
#include "thyme/replay/inject_status.h"
#include "thyme/replay/replay_reader.h"
#include "thyme/runtime/pinned_slot_store.h"

#include <cstdint>
#include <string>
#include <vector>

namespace thyme {
namespace replay {

// Explicit channel table entry: package channel id -> target writer.
struct PinnedSlotTarget {
  uint32_t channel = 0;
  PinnedSlotWriter* writer = nullptr;
};

struct PinnedSlotStoreStats {
  uint32_t channel = 0;
  uint32_t slot_bytes = 0;    // target geometry
  uint32_t slot_count = 0;    // target geometry
  uint32_t max_frame_len = 0; // package geometry (0: channel has no frames)
  bool geometry_fits = true;  // max_frame_len <= slot_bytes
  uint64_t injected = 0;
  uint64_t bytes = 0;
  uint64_t skipped_pinned = 0;
  uint64_t oversize = 0;
  uint64_t crc_mismatches = 0;
  uint64_t sequence_errors = 0;  // non-increasing record_sequence on data
  uint64_t ignored = 0;          // GAP/trigger/session rows on this channel
  uint64_t gated = 0;            // REP-10: gate refused an inject write
  uint64_t headers_rewritten = 0;  // REP-12: embedded FrameHeader rewrites
  uint64_t last_mono_ns = 0;
};

struct PinnedSlotInjectorStats {
  uint64_t injected = 0;
  uint64_t bytes = 0;
  uint64_t skipped_pinned = 0;
  uint64_t oversize = 0;
  uint64_t crc_mismatches = 0;
  uint64_t sequence_errors = 0;
  uint64_t ignored = 0;
  uint64_t unmapped = 0;
  uint64_t bad_events = 0;
  uint64_t gated = 0;  // REP-10: inject writes refused by the gate
  uint64_t geometry_oversize_channels = 0;
  uint64_t headers_rewritten = 0;
  uint64_t last_mono_ns = 0;
};

class PinnedSlotInjector {
 public:
  PinnedSlotInjector();
  ~PinnedSlotInjector();
  PinnedSlotInjector(const PinnedSlotInjector&) = delete;
  PinnedSlotInjector& operator=(const PinnedSlotInjector&) = delete;

  // Resolves every target against an open package. Fails (kInvalidArgument,
  // error() carries the channel) when a target names a missing/non-big-frame
  // channel, a null writer, an unopened writer (slot geometry == 0), a
  // duplicate channel, or targets != nullptr && count > 0 without a package.
  // A package frame that does not fit the target geometry is NOT an Open
  // failure: it is reported per channel and counted per event.
  ReplayStatus Open(const ReplayPackage& pkg, const PinnedSlotTarget* targets,
                    uint32_t count);
  void Close();
  // REP-10: replay owns each target channel through the in-header producer
  // gate. A second injector (same or different process) that opens the same
  // target gets ReplayStatus::kBusy instead of sharing the writer. Call this
  // before Open(); --no-gate is the explicit escape hatch (tests/legacy
  // producers that cannot honour the gate).
  void SetGateEnabled(bool enabled) { gate_enabled_ = enabled; }
  bool gate_enabled() const { return gate_enabled_; }

  // Refresh the gate lease. The injector heartbeats on every Inject(); a host
  // that can sit in a long pause must call this from its own timer thread so
  // writers do not reclaim the gate while the replay is still alive.
  void Heartbeat();

  // Doubled-write observability, read from the target channel headers:
  //   gate_rejects — real-producer Begin()/Commit() calls blocked by the gate
  //   gate_steals  — stale/dead gates reclaimed by writers
  //   gate_aborted — producer commits aborted by a gate that appeared mid-frame
  uint64_t gate_rejects() const;
  uint64_t gate_steals() const;
  uint64_t gate_aborted() const;

  bool is_open() const { return open_; }

  void SetVerifyCrc(bool verify) { verify_crc_ = verify; }
  bool verify_crc() const { return verify_crc_; }

  // REP-12 header rewrite policy (see header_policy.h). Call before Open();
  // Open() resolves a zero session_id into a generated one. The effective
  // policy (resolved session id) is readable after Open.
  void SetHeaderPolicy(const ReplayHeaderPolicy& policy) { policy_ = policy; }
  const ReplayHeaderPolicy& header_policy() const { return policy_; }
  uint64_t session_id() const { return policy_.session_id; }

  // Injects one event. Safe for any ReplayEvent; rows that are not data on a
  // mapped channel return kIgnored. Never blocks and never retries: a pinned
  // target slot is counted and the next event proceeds.
  InjectStatus Inject(const ReplayEvent& ev);

  const PinnedSlotInjectorStats& stats() const { return stats_; }
  const std::vector<PinnedSlotStoreStats>& channels() const {
    return channels_;
  }
  const PinnedSlotStoreStats* FindChannel(uint32_t channel) const;
  const std::string& error() const { return error_; }

 private:
  struct Entry {
    uint32_t channel = 0;
    PinnedSlotWriter* writer = nullptr;
    uint64_t last_sequence = 0;
    uint32_t next_sequence = 0;  // REP-12 renumber counter (injection ordinal)
    bool has_data = false;
  };

  bool open_ = false;
  bool verify_crc_ = true;
  bool gate_enabled_ = true;  // REP-10
  uint64_t gate_token_ = 0;   // holder identity for this Open (0 = no gate)
  ReplayHeaderPolicy policy_{};  // effective policy (session id resolved)
  std::string error_;
  std::vector<Entry> entries_;
  std::vector<PinnedSlotStoreStats> channels_;
  PinnedSlotInjectorStats stats_;

  void ReleaseGates();
};

}  // namespace replay
}  // namespace thyme
