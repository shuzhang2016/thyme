#include "thyme/replay/replay_player.h"

#include "thyme/replay/replay_timeline.h"
#include "thyme/replay/topology_map.h"

#include <ctime>
#include <utility>

namespace thyme {
namespace replay {

ReplayPlayer::ReplayPlayer() = default;

ReplayPlayer::~ReplayPlayer() = default;

uint64_t ReplayPlayer::NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

ReplayStatus ReplayPlayer::Open(const std::string& dir,
                                const ReplayPlayerConfig& cfg) {
  Close();
  if (cfg.skip_overlaps && cfg.stitch_timeline) {
    error_ = "skip_overlaps and stitch_timeline are mutually exclusive";
    return ReplayStatus::kInvalidArgument;
  }
  const ReplayStatus st = session_.Open(dir);
  if (st != ReplayStatus::kOk) {
    error_ = std::string("session open failed: ") + ReplayStatusName(st);
    return st;
  }
  const ReplayPackage& pkg = session_.package();
  // REP-12: one replay session id shared by both injectors.
  header_policy_ = cfg.header_policy;
  if (header_policy_.session_id == 0) {
    header_policy_.session_id = GenerateReplaySessionId();
  }
  pinned_slots_.SetVerifyCrc(cfg.verify_crc);
  pinned_slots_.SetHeaderPolicy(header_policy_);
  const ReplayStatus bst = pinned_slots_.Open(
      pkg, cfg.pinned_slots.data(), static_cast<uint32_t>(cfg.pinned_slots.size()));
  if (bst != ReplayStatus::kOk) {
    error_ = "big-frame mapping: " + pinned_slots_.error();
    session_.Close();
    return bst;
  }
  small_.SetHeaderPolicy(header_policy_);
  const ReplayStatus sst = small_.Open(pkg, cfg.small_structs.data(),
                                       static_cast<uint32_t>(
                                           cfg.small_structs.size()));
  if (sst != ReplayStatus::kOk) {
    error_ = "small-struct mapping: " + small_.error();
    pinned_slots_.Close();
    session_.Close();
    return sst;
  }
  channels_.assign(pkg.channels().size(), PlayerChannelStats{});
  for (std::size_t i = 0; i < channels_.size(); ++i) {
    channels_[i].channel = pkg.channels()[i].id;
    channels_[i].kind = pkg.channels()[i].kind;
  }
  contract_channels_ = pkg.channels();
  stats_ = PlayerStats{};
  skip_overlaps_ = cfg.skip_overlaps;
  skip_join_overlap_ = false;
  join_previous_last_ns_ = 0;
  stitch_timeline_ = cfg.stitch_timeline;
  timeline_offset_ns_ = 0;
  has_event_ = false;
  anchored_ = false;
  reanchor_.store(true, std::memory_order_relaxed);
  open_ = true;
  error_.clear();
  return ReplayStatus::kOk;
}

ReplayStatus ValidateNextPackageContract(
    const ReplayPackage& pkg,
    const std::vector<ReplayChannel>& contract_channels, std::string* error) {
  if (pkg.channels().size() != contract_channels.size()) {
    *error = "next package channel count differs from the first package";
    return ReplayStatus::kManifestMismatch;
  }
  for (std::size_t i = 0; i < pkg.channels().size(); ++i) {
    const ReplayChannel& next = pkg.channels()[i];
    const ReplayChannel& first = contract_channels[i];
    if (next.id != first.id || next.kind != first.kind) {
      *error = "next package channel id/kind differs at channel " +
               std::to_string(i);
      return ReplayStatus::kManifestMismatch;
    }
    if (next.registry.size() != first.registry.size()) {
      *error = "next package registry size differs at channel " +
               std::to_string(next.id);
      return ReplayStatus::kManifestMismatch;
    }
    for (const ThymeStructDesc& source : next.registry) {
      TopologyMapping mapping;
      const TopologyMapStatus status = ResolveTopologyMapping(
          source, first.registry.data(), first.registry.size(), &mapping);
      if (!TopologyMapStatusMapped(status) ||
          mapping.target_struct_id != source.struct_id) {
        *error = "next package channel " + std::to_string(next.id) +
                 " struct " + std::to_string(source.struct_id) + ": " +
                 TopologyMapStatusName(status) + " " + mapping.detail;
        return ReplayStatus::kManifestMismatch;
      }
      const ThymeStructDesc* target = nullptr;
      for (const ThymeStructDesc& desc : first.registry) {
        if (desc.struct_id == mapping.target_struct_id) {
          target = &desc;
          break;
        }
      }
      if (target == nullptr ||
          source.contract_version != target->contract_version ||
          source.payload_size != target->payload_size ||
          (source.layout_hash != 0 && target->layout_hash != 0 &&
           source.layout_hash != target->layout_hash)) {
        *error = "next package channel " + std::to_string(next.id) +
                 " struct " + std::to_string(source.struct_id) +
                 " has an incompatible contract";
        return ReplayStatus::kManifestMismatch;
      }
    }
  }
  return ReplayStatus::kOk;
}

ReplayStatus ReplayPlayer::ValidateNextPackage(const ReplayPackage& pkg) {
  return ValidateNextPackageContract(pkg, contract_channels_, &error_);
}

ReplayStatus ReplayPlayer::OpenNextPackage(const std::string& dir,
                                           bool count_join,
                                           int64_t timeline_offset_ns) {
  if (!open_) {
    return ReplayStatus::kInvalidArgument;
  }
  if (session_.stopped()) {
    error_ = "cannot continue a stopped replay";
    return ReplayStatus::kStopped;
  }
  ReplayPackage next;
  ReplayStatus st = next.Open(dir);
  if (st != ReplayStatus::kOk) {
    error_ = "next package open failed: " + next.error();
    return st;
  }
  st = ValidateNextPackage(next);
  if (st != ReplayStatus::kOk) {
    return st;
  }
  uint64_t shifted_ns = 0;
  if (stitch_timeline_ &&
      (!ShiftReplayTimestamp(next.first_mono_ns(), timeline_offset_ns,
                             &shifted_ns) ||
       !ShiftReplayTimestamp(next.last_mono_ns(), timeline_offset_ns,
                             &shifted_ns))) {
    error_ = "next package stitch offset overflows mono_ns";
    return ReplayStatus::kInvalidArgument;
  }

  const double rate = session_.rate();
  const bool as_fast = session_.as_fast();
  const bool paused = session_.paused();
  const bool had_event = has_event_;
  const uint64_t previous_last_ns = stats_.last_event_ns;
  const uint64_t previous_source_last_ns = session_.package().last_mono_ns();
  st = session_.Open(dir);
  if (st != ReplayStatus::kOk) {
    open_ = false;
    error_ = "next session open failed: " + session_.package().error();
    return st;
  }
  (void)session_.SetRate(rate);
  session_.AsFast(as_fast);
  if (paused) {
    session_.Pause();
  }
  skip_join_overlap_ = count_join && skip_overlaps_ && had_event;
  join_previous_last_ns_ = previous_last_ns;
  if (count_join) {
    stats_.slice_joins += 1;
    const uint64_t join_previous_ns =
        stitch_timeline_ ? previous_source_last_ns : previous_last_ns;
    if (had_event && next.first_mono_ns() > join_previous_ns) {
      stats_.join_holes += 1;
    }
  }
  timeline_offset_ns_ = stitch_timeline_ ? timeline_offset_ns : 0;
  has_event_ = had_event;
  anchored_ = false;
  reanchor_.store(true, std::memory_order_relaxed);
  error_.clear();
  return ReplayStatus::kOk;
}

void ReplayPlayer::Close() {
  session_.Close();
  pinned_slots_.Close();
  small_.Close();
  contract_channels_.clear();
  channels_.clear();
  stats_ = PlayerStats{};
  header_policy_ = ReplayHeaderPolicy{};
  skip_overlaps_ = false;
  skip_join_overlap_ = false;
  join_previous_last_ns_ = 0;
  stitch_timeline_ = false;
  timeline_offset_ns_ = 0;
  has_event_ = false;
  anchored_ = false;
  open_ = false;
  error_.clear();
}

ReplayStatus ReplayPlayer::Dispatch(const ReplayEvent& ev) {
  if (ev.channel >= channels_.size()) {
    // The reader guarantees dense channel ids; a violation is a package
    // integrity problem, not a mapping problem.
    stats_.failed += 1;
    error_ = "event channel out of range";
    return ReplayStatus::kCorrupt;
  }
  PlayerChannelStats& cs = channels_[ev.channel];
  if (stats_.events == 0) {
    stats_.first_event_ns = ev.mono_ns;
  }
  stats_.events += 1;
  cs.events += 1;
  if (has_event_ && ev.mono_ns < stats_.last_event_ns) {
    stats_.mono_regressions += 1;
  }
  has_event_ = true;
  stats_.last_event_ns = ev.mono_ns;
  cs.last_event_ns = ev.mono_ns;

  InjectStatus is = InjectStatus::kIgnored;
  switch (cs.kind) {
    case ReplayChannelKind::kPinnedSlot:
      is = pinned_slots_.Inject(ev);
      break;
    case ReplayChannelKind::kSmall:
      is = small_.Inject(ev);
      break;
    case ReplayChannelKind::kMeta:
      // meta.tbin rows describe the capture (trigger, stats); they only have
      // a target when the caller explicitly maps that struct to a ring.
      is = small_.HasTarget(ev.channel, ev.struct_id) ? small_.Inject(ev)
                                                      : InjectStatus::kIgnored;
      break;
  }
  switch (is) {
    case InjectStatus::kOk:
      cs.injected += 1;
      cs.bytes += ev.len;
      stats_.injected += 1;
      break;
    case InjectStatus::kIgnored:
      cs.ignored += 1;
      stats_.ignored += 1;
      break;
    case InjectStatus::kSkippedPinned:
    case InjectStatus::kOversize:
    case InjectStatus::kCrcMismatch:
    case InjectStatus::kWriteFailed:
      // Expected degradations: counted, the stream continues.
      cs.skipped += 1;
      stats_.skipped += 1;
      break;
    case InjectStatus::kUnmapped:
    case InjectStatus::kBadEvent:
    case InjectStatus::kGated:
    case InjectStatus::kNotOpen:
      cs.skipped += 1;
      stats_.failed += 1;
      break;
  }
  return ReplayStatus::kOk;
}

ReplayStatus ReplayPlayer::Deliver(const ReplayEvent& ev) {
  ReplayEvent shifted = ev;
  if (stitch_timeline_ &&
      !ShiftReplayTimestamp(ev.mono_ns, timeline_offset_ns_, &shifted.mono_ns)) {
    error_ = "stitch event timestamp overflows mono_ns";
    return ReplayStatus::kInvalidArgument;
  }
  if (skip_join_overlap_) {
    if (shifted.mono_ns < join_previous_last_ns_) {
      stats_.join_duplicates += 1;
      return ReplayStatus::kOk;
    }
    skip_join_overlap_ = false;
  }
  NoteDispatchTime(shifted);
  return Dispatch(shifted);
}

void ReplayPlayer::NoteDispatchTime(const ReplayEvent& ev) {
  const uint64_t now = NowNs();
  const double rate = session_.rate();
  if (reanchor_.exchange(false, std::memory_order_relaxed) || !anchored_ ||
      session_.as_fast() || rate != anchor_rate_) {
    anchored_ = true;
    anchor_mono_ns_ = ev.mono_ns;
    anchor_wall_ns_ = now;
    anchor_rate_ = rate;
    stats_.last_skew_ns = 0;
    return;
  }
  // The event was due at anchor_wall + (mono delta)/rate; a positive
  // difference is dispatch lag (pause windows are excluded by re-anchoring).
  const double due_ns =
      static_cast<double>(anchor_wall_ns_) +
      static_cast<double>(ev.mono_ns - anchor_mono_ns_) / rate;
  int64_t lag = static_cast<int64_t>(static_cast<double>(now) - due_ns);
  if (lag < 0) {
    lag = 0;
  }
  stats_.last_skew_ns = lag;
  if (lag > 0 && static_cast<uint64_t>(lag) > stats_.max_skew_ns) {
    stats_.max_skew_ns = static_cast<uint64_t>(lag);
  }
}

ReplayStatus ReplayPlayer::Pump() {
  if (!open_) {
    return ReplayStatus::kInvalidArgument;
  }
  ReplayEvent ev;
  const ReplayStatus st = session_.NextEvent(&ev);
  if (st != ReplayStatus::kOk) {
    return st;
  }
  return Deliver(ev);
}

ReplayStatus ReplayPlayer::Run() {
  for (;;) {
    const ReplayStatus st = Pump();
    if (st == ReplayStatus::kOk) {
      continue;
    }
    if (st == ReplayStatus::kEnd) {
      return ReplayStatus::kOk;
    }
    return st;
  }
}

ReplayStatus ReplayPlayer::Step() {
  if (!open_) {
    return ReplayStatus::kInvalidArgument;
  }
  ReplayEvent ev;
  const ReplayStatus st = session_.Step(&ev);
  if (st != ReplayStatus::kOk) {
    return st;
  }
  const ReplayStatus dst = Deliver(ev);
  // Stepping leaves the session paused; the next paced event restarts the
  // skew clock at its own timestamp.
  reanchor_.store(true, std::memory_order_relaxed);
  return dst;
}

ReplayStatus ReplayPlayer::Seek(uint64_t mono_ns) {
  uint64_t source_ns = mono_ns;
  if (stitch_timeline_ &&
      !UnshiftReplayTimestamp(mono_ns, timeline_offset_ns_, &source_ns)) {
    if (timeline_offset_ns_ > 0 &&
        mono_ns < static_cast<uint64_t>(timeline_offset_ns_)) {
      source_ns = 0;
    } else {
      error_ = "stitch seek target is outside the source timestamp range";
      return ReplayStatus::kInvalidArgument;
    }
  }
  const ReplayStatus st = session_.Seek(source_ns);
  if (st != ReplayStatus::kOk) {
    return st;
  }
  has_event_ = false;  // a backwards seek must not count as a regression
  skip_join_overlap_ = false;
  anchored_ = false;
  reanchor_.store(true, std::memory_order_relaxed);
  return ReplayStatus::kOk;
}

ReplayStatus ReplayPlayer::SetRate(double rate) {
  const ReplayStatus st = session_.SetRate(rate);
  if (st == ReplayStatus::kOk) {
    anchored_ = false;
    reanchor_.store(true, std::memory_order_relaxed);
  }
  return st;
}

void ReplayPlayer::AsFast(bool fast) {
  session_.AsFast(fast);
  anchored_ = false;
  reanchor_.store(true, std::memory_order_relaxed);
}

void ReplayPlayer::Heartbeat() {
  pinned_slots_.Heartbeat();
  small_.Heartbeat();
}

void ReplayPlayer::Pause() {
  session_.Pause();
  Heartbeat();
  anchored_ = false;
  reanchor_.store(true, std::memory_order_relaxed);
}

void ReplayPlayer::Resume() {
  session_.Resume();
  anchored_ = false;
  reanchor_.store(true, std::memory_order_relaxed);
}

void ReplayPlayer::Stop() {
  session_.Stop();
}

}  // namespace replay
}  // namespace thyme
