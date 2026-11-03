#pragma once

// replay_player.h — REP-05: synchronized replay of a capture package into a
// live system.
//
// The player owns one ReplaySession (REP-02) and the injectors that write the
// events back into live targets:
//
//   ReplaySession::NextEvent()  ->  dispatch by ReplayChannel::kind
//                                    big frame  -> PinnedSlotInjector (REP-04)
//                                    small/meta -> SmallStructInjector
//
// Synchronization is by construction: ReplayPackage already merges every
// package source on one mono_ns timeline and ReplaySession paces that single
// stream. The player injects each event synchronously before asking for the
// next one, so:
//
//   * injection order is non-decreasing in mono_ns across channels
//     (stats().mono_regressions counts any violation — it must stay 0);
//   * all events sharing a mono_ns are injected back to back;
//   * cross-channel dispatch skew is bounded by the pacing quantum: an event
//     due at timeline t is returned by the session no earlier than t/rate and
//     is injected immediately (one payload copy). The player measures the
//     positive lag against the pacing clock in stats().last_skew_ns /
//     max_skew_ns. In as-fast mode the "quantum" is one copy + one reader
//     step; in wall-clock mode ReplaySession slices its wait at 10 ms, so the
//     lag stays below one 10 ms slice plus the copy cost.
//
// Modes: wall-clock via SetRate() (1.0 = recorded speed), as-fast via
// AsFast(true), single step via Step() (one event, session stays paused), and
// pause/resume. Stop() may be called from any thread and terminates a
// blocked Run()/Pump(); it is the only way out of a paused player besides
// Resume().
//
// Threading: Open/Close/Pump/Run/Step/Seek belong to one playback thread;
// Pause/Resume/SetRate/AsFast/Stop/Heartbeat may be called from any thread. The
// player does not create a thread — the host owns the loop (Run() is a
// convenience).

#include "thyme/replay/pinned_slot_injector.h"
#include "thyme/replay/replay_session.h"
#include "thyme/replay/small_struct_injector.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace thyme {
namespace replay {

// Validates a continuation package against the first package's channel and
// registry contract without opening targets or injecting events.
ReplayStatus ValidateNextPackageContract(
    const ReplayPackage& pkg,
    const std::vector<ReplayChannel>& contract_channels, std::string* error);

struct ReplayPlayerConfig {
  std::vector<PinnedSlotTarget> pinned_slots;
  std::vector<SmallStructTarget> small_structs;
  // Verify FrameIndexRecord.payload_crc before injecting a big frame (see
  // PinnedSlotInjector). Default on.
  bool verify_crc = true;
  // REP-12 header rewrite policy (see header_policy.h), applied to both
  // injectors. session_id == 0 makes Open() generate ONE id shared by every
  // injector, so the whole replay carries a single session identity.
  ReplayHeaderPolicy header_policy;
  // Absolute multi-package replay drops records older than the previous
  // slice's last event at a join. Single-package Open() defaults to false.
  bool skip_overlaps = false;
  // Shift continuation packages onto a gap-free stitched timeline. The
  // playlist supplies the offset computed by BuildReplayStitchPlan().
  bool stitch_timeline = false;
};

struct PlayerChannelStats {
  uint32_t channel = 0;
  ReplayChannelKind kind = ReplayChannelKind::kMeta;
  uint64_t events = 0;    // every timeline event on this channel
  uint64_t injected = 0;
  uint64_t skipped = 0;   // pinned / oversize / crc mismatch / write failure
  uint64_t ignored = 0;   // gap/trigger/session rows (no payload to inject)
  uint64_t bytes = 0;
  uint64_t last_event_ns = 0;
};

struct PlayerStats {
  uint64_t events = 0;  // dispatched events (join duplicates excluded)
  uint64_t injected = 0;
  uint64_t skipped = 0;
  uint64_t ignored = 0;
  uint64_t failed = 0;  // unmapped target or unusable event
  uint64_t mono_regressions = 0;  // must stay 0 (timeline is ordered)
  uint64_t slice_joins = 0;
  uint64_t join_duplicates = 0;
  uint64_t join_holes = 0;
  uint64_t first_event_ns = 0;
  uint64_t last_event_ns = 0;
  int64_t last_skew_ns = 0;   // dispatch lag against the pacing clock
  uint64_t max_skew_ns = 0;   // largest positive lag observed
};

class ReplayPlayer {
 public:
  ReplayPlayer();
  ~ReplayPlayer();
  ReplayPlayer(const ReplayPlayer&) = delete;
  ReplayPlayer& operator=(const ReplayPlayer&) = delete;

  // Opens the package and resolves every mapping. A mapping that names a
  // missing channel/struct or points at an unopened target fails the Open
  // (the session is closed again). A package that simply has channels the
  // caller did not map is not an error: those events are counted as unmapped
  // while running.
  ReplayStatus Open(const std::string& dir, const ReplayPlayerConfig& cfg);
  // Continues on the same injectors and producer gates. The next package is
  // fully opened and checked against the first package's channel/registry
  // contract before the session is replaced; incompatibility injects nothing.
  ReplayStatus OpenNextPackage(const std::string& dir, bool count_join = true,
                               int64_t timeline_offset_ns = 0);
  void Close();
  bool is_open() const { return open_; }

  // Call before Open(). The same setting applies to big-frame channels and
  // small-struct ring targets; publish sinks do not have a producer gate.
  void SetGateEnabled(bool enabled) {
    pinned_slots_.SetGateEnabled(enabled);
    small_.SetGateEnabled(enabled);
  }

  // Refresh both injectors' leases. The player does not create a thread, so a
  // host must call this periodically throughout a long pause or step wait.
  void Heartbeat();

  // Pulls one event through the session (honouring pacing/pause) and injects
  // it. Returns kOk, kEnd at the end of the timeline, kStopped after Stop(),
  // or the reader/session error.
  ReplayStatus Pump();

  // Pump() until the end of the timeline. Returns kOk at kEnd, kStopped after
  // Stop(), otherwise the first hard error.
  ReplayStatus Run();

  // Delivers and injects exactly one event; the session stays paused.
  ReplayStatus Step();

  ReplayStatus Seek(uint64_t mono_ns);
  ReplayStatus SetRate(double rate);
  void AsFast(bool fast);
  void Pause();
  void Resume();
  void Stop();

  bool paused() const { return session_.paused(); }
  bool as_fast() const { return session_.as_fast(); }
  double rate() const { return session_.rate(); }
  ReplayStatus status() const { return session_.status(); }
  ReplaySessionState state() const { return session_.state(); }
  uint64_t delivered_events() const { return session_.delivered_events(); }

  const PlayerStats& stats() const { return stats_; }
  const std::vector<PlayerChannelStats>& channels() const { return channels_; }
  const PinnedSlotInjector& pinned_slot_injector() const { return pinned_slots_; }
  const SmallStructInjector& small_struct_injector() const { return small_; }
  const ReplaySession& session() const { return session_; }
  // REP-12: the session id stamped into rewritten embedded FrameHeaders
  // (resolved at Open; 0 while closed).
  uint64_t replay_session_id() const { return header_policy_.session_id; }
  const ReplayHeaderPolicy& header_policy() const { return header_policy_; }
  const std::string& error() const { return error_; }

 private:
  ReplayStatus Dispatch(const ReplayEvent& ev);
  ReplayStatus Deliver(const ReplayEvent& ev);
  ReplayStatus ValidateNextPackage(const ReplayPackage& pkg);
  void NoteDispatchTime(const ReplayEvent& ev);
  static uint64_t NowNs();

  ReplaySession session_;
  PinnedSlotInjector pinned_slots_;
  SmallStructInjector small_;
  bool open_ = false;
  std::string error_;
  ReplayHeaderPolicy header_policy_;  // REP-12: resolved at Open
  std::vector<ReplayChannel> contract_channels_;
  std::vector<PlayerChannelStats> channels_;
  PlayerStats stats_;
  bool skip_overlaps_ = false;
  bool skip_join_overlap_ = false;
  uint64_t join_previous_last_ns_ = 0;
  bool stitch_timeline_ = false;
  int64_t timeline_offset_ns_ = 0;
  bool has_event_ = false;  // ordering baseline (reset by Seek)
  // Pacing-clock anchor for the skew metric (observability only). Bumped by
  // Pause/Resume/SetRate/AsFast/Seek so pause time is not reported as lag.
  std::atomic<bool> reanchor_{true};
  bool anchored_ = false;
  uint64_t anchor_mono_ns_ = 0;
  uint64_t anchor_wall_ns_ = 0;
  double anchor_rate_ = 1.0;
};

}  // namespace replay
}  // namespace thyme
