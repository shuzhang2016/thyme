// replay_runner.cpp — REP-08 playback: one REP-02 ReplaySession driving the
// REP-04/REP-05 injectors over explicitly mapped targets.
//
// Why a session and not ReplayPlayer::Run()? A player dispatches each event
// before the host sees it, but the CLI has to stop BEFORE injecting the first
// event past --to and it has to account unmapped channels as skips (the
// player counts them as failures). The session + injectors are exactly the
// REP-03/REP-04 primitives the player itself uses, in the same order, so the
// injector-level guarantees (per-frame commit, no truncation) are unchanged.
// Stop() is ReplaySession::Stop(), which is what ReplayPlayer::Stop() calls.

#include "replay_cli.h"

#include "replay_status.h"

#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/replay/pinned_slot_injector.h"
#include "thyme/replay/replay_player.h"
#include "thyme/replay/replay_playlist.h"
#include "thyme/replay/small_struct_injector.h"
#include "thyme/replay/topology_map.h"
#include "thyme/runtime/pinned_slot_store.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <poll.h>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace thyme {
namespace replaycli {

namespace {

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

bool DecimalU64(const std::string& text, uint64_t* out) {
  if (text.empty() || text[0] == '-' || text[0] == '+') {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0') {
    return false;
  }
  *out = static_cast<uint64_t>(v);
  return true;
}

const char* KindName(replay::ReplayChannelKind kind) {
  switch (kind) {
    case replay::ReplayChannelKind::kMeta:
      return "meta";
    case replay::ReplayChannelKind::kSmall:
      return "small";
    case replay::ReplayChannelKind::kPinnedSlot:
      return "pinned_slot";
  }
  return "unknown";
}

void AddU64(uint64_t a, uint64_t b, uint64_t* out) {
  *out = (b > std::numeric_limits<uint64_t>::max() - a) ? std::numeric_limits<uint64_t>::max()
                                                        : a + b;
}

// Resolves a target's channel text: exact package channel name first, then
// "#<id>" or a bare decimal id.
const replay::ReplayChannel* ResolveChannel(const replay::ReplayPackage& pkg,
                                            const std::string& text,
                                            std::string* err) {
  for (const replay::ReplayChannel& c : pkg.channels()) {
    if (c.name == text) {
      return &c;
    }
  }
  std::string t = text;
  if (!t.empty() && t[0] == '#') {
    t = t.substr(1);
  }
  uint64_t id = 0;
  if (DecimalU64(t, &id) && id < pkg.channels().size()) {
    return &pkg.channels()[id];
  }
  *err = "channel '" + text + "' is not in the package";
  return nullptr;
}

struct PendingPinnedSlot {
  const replay::ReplayChannel* channel = nullptr;
  TargetSpec spec;
  std::unique_ptr<PinnedSlotWriter> writer;
  uint32_t slot_bytes = 0;
  uint32_t slot_count = 0;
  bool created = false;  // this run owns the channel (safe to unlink)
};

struct PendingSmall {
  const replay::ReplayChannel* channel = nullptr;
  TargetSpec spec;
  std::unique_ptr<record_mirror::RingClient> ring;
  // REP-11: resolved (package struct -> target struct) pairs. The explicit
  // form has exactly one; the auto-map form has one per package struct the
  // target topology provides.
  struct Mapping {
    uint16_t struct_id = 0;
    uint16_t target_struct_id = 0;
  };
  std::vector<Mapping> mappings;
};

// One row of the `--check-compat` / `--dry-run` mapping table.
struct MappingRow {
  uint32_t channel = 0;
  std::string channel_name;
  std::string ring;
  uint16_t source_struct_id = 0;
  uint16_t target_struct_id = 0;
  std::string key;
  std::string status;
  std::string detail;
  bool mapped = false;
};

class Runner {
 public:
  Runner(const PlayOptions& opt, PlayControl* control, std::ostream& out,
         PlaySummary* summary)
      : opt_(opt), control_(control), out_(out), summary_(summary) {}

  ~Runner() {
    smalls_.Close();
    pinned_slots_.Close();
    CleanupTargets();
  }

  int Run();

 private:
  void PrintWarning(const std::string& text);
  bool ResolveTargets(std::string* err);
  bool CreateTargets(std::string* err);
  bool CheckUnmapped(std::string* err);
  void CleanupTargets();
  void Account(const replay::ReplayEvent& ev, replay::InjectStatus status,
               bool mapped);
  void PrintPlanHeader();
  void PrintMappingTable() const;
  void PrintMappingJson() const;
  void PrintSummaryText() const;
  void PrintSummaryJson() const;
  // REP-08: live/final status document (see replay_status.h).
  StatusSnapshot BuildSnapshot(const char* state) const;
  void PublishStatus(const char* state, bool force);
  bool ApplyControlRequests();
  replay::ReplayStatus OpenInitialPackage();
  replay::ReplayStatus OpenNextPackage(std::size_t index, bool count_join);
  replay::ReplayStatus SeekTo(uint64_t mono_ns);

  const PlayOptions& opt_;
  PlayControl* control_;
  std::ostream& out_;
  PlaySummary* summary_;

  replay::ReplaySession session_;
  std::vector<replay::ReplayPlaylistSlice> slices_;
  std::vector<replay::ReplayChannel> contract_channels_;
  std::size_t current_slice_ = 0;
  bool has_order_baseline_ = false;
  bool skip_join_overlap_ = false;
  uint64_t join_previous_last_ns_ = 0;
  replay::PinnedSlotInjector pinned_slots_;
  replay::SmallStructInjector smalls_;
  std::vector<PendingPinnedSlot> big_targets_;
  std::vector<PendingSmall> small_targets_;
  std::vector<replay::PinnedSlotTarget> big_table_;
  std::vector<replay::SmallStructTarget> small_table_;
  std::vector<bool> big_mapped_;
  std::vector<bool> small_mapped_;  // channel has at least one struct target
  std::vector<MappingRow> mapping_rows_;
  bool targets_ready_ = false;
  bool success_ = false;

  StatusFile status_;
  std::string run_state_;  // last published state name
  double effective_rate_ = 1.0;
  uint64_t run_from_ns_ = 0;
  uint64_t run_to_ns_ = 0;
  bool has_window_ = false;
  uint64_t position_ns_ = 0;
  uint64_t setup_ns_ = 0;
  uint64_t run_begin_ns_ = 0;
  replay::ReplayStatus control_failure_ = replay::ReplayStatus::kOk;
};

void Runner::PrintWarning(const std::string& text) {
  summary_->warnings.push_back(text);
  if (!opt_.json) {
    out_ << "warning: " << text << '\n';
  }
}

void Runner::CleanupTargets() {
  if (targets_ready_ || success_) {
    // Playback ran (or setup finished): the targets are left in place. A
    // big-frame channel stays mapped by name for consumers; ring pages are
    // owned by their creator and only detached.
    for (PendingSmall& p : small_targets_) {
      if (p.ring) {
        p.ring->Shutdown(false);
      }
    }
    return;
  }
  // Setup failed: unlink channels this run created (we own them and never
  // handed a target to a consumer).
  for (PendingPinnedSlot& p : big_targets_) {
    if (p.writer) {
      p.writer->Close();
      if (p.created) {
        ShmRegion::UnlinkName(p.spec.shm_name.c_str());
        ShmRegion::UnlinkName(
            PinnedSlotWriter::LeaseShmName(p.spec.shm_name.c_str()).c_str());
      }
    }
  }
  for (PendingSmall& p : small_targets_) {
    if (p.ring) {
      p.ring->Shutdown(false);
    }
  }
}

bool Runner::ResolveTargets(std::string* err) {
  const replay::ReplayPackage& pkg = session_.package();
  const std::size_t n = pkg.channels().size();
  big_mapped_.assign(n, false);
  small_mapped_.assign(n, false);
  summary_->channels.assign(n, PlayChannelSummary{});
  for (std::size_t i = 0; i < n; ++i) {
    PlayChannelSummary& cs = summary_->channels[i];
    cs.id = pkg.channels()[i].id;
    cs.name = pkg.channels()[i].name;
    cs.kind = KindName(pkg.channels()[i].kind);
  }

  for (const TargetSpec& spec : opt_.pinned_slots) {
    const replay::ReplayChannel* ch = ResolveChannel(pkg, spec.channel, err);
    if (ch == nullptr) {
      return false;
    }
    if (ch->kind != replay::ReplayChannelKind::kPinnedSlot) {
      *err = "channel " + ch->name + " (" + KindName(ch->kind) +
             ") is not a big-frame channel";
      return false;
    }
    if (big_mapped_[ch->id]) {
      *err = "duplicate --pinned_slot target for channel " + ch->name;
      return false;
    }
    PendingPinnedSlot p;
    p.channel = ch;
    p.spec = spec;
    p.slot_bytes = spec.slot_bytes;
    if (p.slot_bytes == 0) {
      p.slot_bytes = std::max<uint32_t>(
          64u, PinnedSlotWriter::RoundUp64(ch->max_data_len));
    } else {
      p.slot_bytes = PinnedSlotWriter::RoundUp64(p.slot_bytes);
      if (p.slot_bytes < ch->max_data_len) {
        *err = "channel " + ch->name + ": target slot_bytes " +
               std::to_string(p.slot_bytes) + " < package max frame " +
               std::to_string(ch->max_data_len);
        return false;
      }
    }
    p.slot_count = opt_.pinned_slot_slots;
    if (!opt_.force) {
      // A name that already maps a valid channel is treated as live: the
      // writer would unlink the name and leave the old producer/consumers on
      // a detached segment. REP-10 adds the second line of defence in
      // PinnedSlotWriter::Create(), which refuses a live producer gate even with
      // --force; this probe keeps the polite "use --force" message for an
      // ungated live channel.
      PinnedSlotReader probe;
      if (probe.Open(spec.shm_name.c_str(), 5000000ULL,
                     /*lease_read_only=*/true)) {
        *err = "big-frame channel '" + spec.shm_name +
               "' already exists; refusing to replace it (use --force)";
        return false;
      }
    }
    big_mapped_[ch->id] = true;
    big_targets_.push_back(std::move(p));
  }

  for (const TargetSpec& spec : opt_.smalls) {
    const replay::ReplayChannel* ch = ResolveChannel(pkg, spec.channel, err);
    if (ch == nullptr) {
      return false;
    }
    if (ch->kind == replay::ReplayChannelKind::kPinnedSlot) {
      *err = "channel " + ch->name +
             " is a big-frame channel; use --pinned_slot for it";
      return false;
    }
    for (const PendingSmall& p : small_targets_) {
      if (p.channel->id != ch->id) continue;
      if (spec.auto_map || p.spec.auto_map ||
          p.spec.struct_id == spec.struct_id) {
        *err = "duplicate --small target for channel " + ch->name +
               (spec.auto_map || p.spec.auto_map
                    ? " (the auto-map form already covers every struct)"
                    : " struct " + std::to_string(spec.struct_id));
        return false;
      }
    }
    PendingSmall p;
    p.channel = ch;
    p.spec = spec;
    p.ring = std::make_unique<record_mirror::RingClient>();
    const bool attached =
        opt_.dry_run
            ? p.ring->AttachReadOnly(spec.shm_name.c_str(), 200000000ULL)
            : p.ring->AttachReadWrite(spec.shm_name.c_str(), 200000000ULL);
    if (!attached) {
      *err = "cannot attach ring '" + spec.shm_name +
             (opt_.dry_run ? "' read-only (is the target page running?)"
                           : "' read-write (is the target page running?)");
      return false;
    }
    const RecordMirrorRingPage* page = p.ring->Page();
    if (page == nullptr || !page->Validate()) {
      *err = "ring '" + spec.shm_name + "' is not a valid ring page";
      return false;
    }

    auto find_desc = [ch](uint16_t sid) -> const ThymeStructDesc* {
      for (const ThymeStructDesc& d : ch->registry) {
        if (d.struct_id == sid) return &d;
      }
      return nullptr;
    };

    if (spec.auto_map) {
      // REP-11: resolve every package struct against the target descriptors
      // by (key, contract_version, payload_size, layout_hash). The target id
      // is the one the key resolves to, not a caller-supplied number.
      for (const ThymeStructDesc& src : ch->registry) {
        replay::TopologyMapping m;
        const replay::TopologyMapStatus status = replay::ResolveTopologyMapping(
            src, page->struct_table, page->struct_count(), &m);
        mapping_rows_.push_back(
            MappingRow{ch->id, ch->name, spec.shm_name, src.struct_id,
                       replay::TopologyMapStatusMapped(status) ? m.target_struct_id
                                                               : uint16_t{0},
                       m.record_key, replay::TopologyMapStatusName(status),
                       m.detail, replay::TopologyMapStatusMapped(status)});
        if (!replay::TopologyMapStatusMapped(status)) {
          if (status == replay::TopologyMapStatus::kMissing &&
              opt_.allow_missing) {
            summary_->mappings_missing += 1;
            PrintWarning("channel " + ch->name + " struct " +
                         std::to_string(src.struct_id) +
                         " has no target (" + m.detail +
                         "); skipped by --allow-missing");
            continue;
          }
          summary_->mapping_conflicts += 1;
          *err = "topology mapping failed for channel " + ch->name +
                 " struct " + std::to_string(src.struct_id) + " (key '" +
                 m.record_key + "'): " + m.detail;
          if (status == replay::TopologyMapStatus::kMissing) {
            *err += " (use --allow-missing to skip it)";
          }
          return false;
        }
        if (m.target_struct_id >= page->struct_count()) {
          *err = "channel " + ch->name + " struct " +
                 std::to_string(src.struct_id) + " maps to target struct " +
                 std::to_string(m.target_struct_id) +
                 " outside the ring (struct_count " +
                 std::to_string(page->struct_count()) + ")";
          return false;
        }
        const uint32_t capacity =
            page->ring_descs[m.target_struct_id].payload_capacity;
        if (src.payload_size > capacity) {
          *err = "channel " + ch->name + " struct " +
                 std::to_string(src.struct_id) + " payload " +
                 std::to_string(src.payload_size) +
                 " B exceeds ring slot capacity " + std::to_string(capacity) +
                 " B";
          return false;
        }
        if (status == replay::TopologyMapStatus::kMappedLegacy) {
          summary_->mappings_legacy += 1;
        } else {
          summary_->mappings_mapped += 1;
        }
        p.mappings.push_back(PendingSmall::Mapping{src.struct_id,
                                                   m.target_struct_id});
      }
    } else {
      const ThymeStructDesc* src = find_desc(spec.struct_id);
      if (src == nullptr) {
        *err = "struct " + std::to_string(spec.struct_id) +
               " is not in channel " + ch->name + " registry";
        return false;
      }
      if (spec.target_struct_id >= page->struct_count()) {
        *err = "target struct " + std::to_string(spec.target_struct_id) +
               " does not exist in ring '" + spec.shm_name +
               "' (struct_count " + std::to_string(page->struct_count()) +
               ")";
        return false;
      }
      const uint32_t capacity =
          page->ring_descs[spec.target_struct_id].payload_capacity;
      if (src->payload_size > capacity) {
        *err = "channel " + ch->name + " struct " +
               std::to_string(spec.struct_id) + " payload " +
               std::to_string(src->payload_size) +
               " B exceeds ring slot capacity " + std::to_string(capacity) +
               " B";
        return false;
      }
      mapping_rows_.push_back(
          MappingRow{ch->id, ch->name, spec.shm_name, spec.struct_id,
                     spec.target_struct_id, ThymeStructDescKey(*src),
                     "explicit", "caller-supplied struct_id mapping", true});
      summary_->mappings_mapped += 1;
      p.mappings.push_back(PendingSmall::Mapping{spec.struct_id,
                                                 spec.target_struct_id});
    }
    if (!p.mappings.empty()) {
      small_mapped_[ch->id] = true;
    }
    small_targets_.push_back(std::move(p));
  }
  return true;
}

bool Runner::CreateTargets(std::string* err) {
  const replay::ReplayPackage& pkg = session_.package();
  for (PendingPinnedSlot& p : big_targets_) {
    p.writer = std::make_unique<PinnedSlotWriter>();
    bool attached = false;
    if (opt_.force && p.writer->Open(p.spec.shm_name.c_str(), 200000000ULL)) {
      if (p.writer->slot_bytes() < p.channel->max_data_len ||
          p.writer->slot_count() == 0) {
        *err = "big-frame channel '" + p.spec.shm_name +
               "' has incompatible geometry";
        return false;
      }
      attached = true;
      p.slot_bytes = p.writer->slot_bytes();
      p.slot_count = p.writer->slot_count();
    }
    // REP-10: --force may rebuild an incompatible stale channel, but Create()
    // still refuses a live replay gate (returning kGated) so two replays can
    // never own the same target.
    if (!attached &&
        !p.writer->Create(p.spec.shm_name.c_str(), p.slot_bytes, p.slot_count,
                          0, kPinnedSlotLeaseDefaultStealTimeoutNs,
                          opt_.force)) {
      *err = "cannot create big-frame channel '" + p.spec.shm_name +
             "': " + p.writer->last_error();
      return false;
    }
    p.created = !attached;
    // Replay targets keep the per-frame transport CRC on (REC-21 override):
    // play is a verification/debug path — consumers of the injected stream
    // and the REP-12 header re-seal expect a slot CRC over the final bytes.
    p.writer->SetCrcEnabled(true);
    big_table_.push_back(replay::PinnedSlotTarget{p.channel->id, p.writer.get()});
    if (!opt_.json) {
      out_ << "target  : channel " << p.channel->id << " (" << p.channel->name
           << ", pinned_slot) -> " << p.spec.shm_name << "  " << p.slot_bytes
           << " B x " << p.slot_count << '\n';
    }
  }
  for (PendingSmall& p : small_targets_) {
    for (const PendingSmall::Mapping& m : p.mappings) {
      replay::SmallStructTarget st;
      st.channel = p.channel->id;
      st.struct_id = m.struct_id;
      st.ring = p.ring->Page();
      st.target_struct_id = m.target_struct_id;
      small_table_.push_back(st);
      if (!opt_.json) {
        out_ << "target  : channel " << p.channel->id << " ("
             << p.channel->name << ", " << KindName(p.channel->kind)
             << ") struct " << m.struct_id << " -> " << p.spec.shm_name << ":"
             << m.target_struct_id << '\n';
      }
    }
  }
  replay::ReplayHeaderPolicy header_policy;
  header_policy.renumber_sequence = !opt_.preserve_sequences;
  pinned_slots_.SetHeaderPolicy(header_policy);
  smalls_.SetHeaderPolicy(header_policy);
  pinned_slots_.SetGateEnabled(opt_.gate);
  smalls_.SetGateEnabled(opt_.gate);
  if (!opt_.gate) {
    PrintWarning(
        "--no-gate: real producers are NOT blocked during replay "
        "(double-write detection disabled)");
  }
  const replay::ReplayStatus bst = pinned_slots_.Open(
      pkg, big_table_.data(), static_cast<uint32_t>(big_table_.size()));
  if (bst != replay::ReplayStatus::kOk) {
    *err = "big-frame mapping: " + pinned_slots_.error();
    return false;
  }
  pinned_slots_.SetVerifyCrc(opt_.verify_crc);
  const replay::ReplayStatus sst = smalls_.Open(
      pkg, small_table_.data(), static_cast<uint32_t>(small_table_.size()));
  if (sst != replay::ReplayStatus::kOk) {
    *err = "small-struct mapping: " + smalls_.error();
    return false;
  }
  targets_ready_ = true;
  return true;
}

bool Runner::CheckUnmapped(std::string* err) {
  const replay::ReplayPackage& pkg = session_.package();
  std::string required;
  for (const replay::ReplayChannel& ch : pkg.channels()) {
    if (ch.kind == replay::ReplayChannelKind::kMeta || ch.data_count == 0) {
      continue;  // meta is informational; an empty channel has nothing to skip
    }
    const bool mapped = ch.kind == replay::ReplayChannelKind::kPinnedSlot
                            ? big_mapped_[ch.id]
                            : small_mapped_[ch.id];
    if (mapped) {
      continue;
    }
    std::ostringstream msg;
    msg << "channel " << ch.id << " (" << ch.name << ", "
        << KindName(ch.kind) << ") has no target: " << ch.data_count
        << " data event(s) will be skipped";
    PrintWarning(msg.str());
    if (!required.empty()) {
      required += ", ";
    }
    required += ch.name;
  }
  if (opt_.strict_targets && !required.empty()) {
    *err = "--strict-targets: no target for " + required;
    return false;
  }
  return true;
}

void Runner::Account(const replay::ReplayEvent& ev,
                     replay::InjectStatus status, bool mapped) {
  if (ev.channel >= summary_->channels.size()) {
    summary_->failed += 1;
    return;
  }
  PlayChannelSummary& cs = summary_->channels[ev.channel];
  cs.events += 1;
  cs.last_mono_ns = ev.mono_ns;
  if (ev.kind != replay::ReplayKind::kData) {
    cs.ignored += 1;
    summary_->ignored += 1;
    return;
  }
  if (!mapped) {
    cs.skipped += 1;
    cs.unmapped += 1;
    summary_->skipped += 1;
    summary_->unmapped += 1;
    return;
  }
  switch (status) {
    case replay::InjectStatus::kOk:
      cs.injected += 1;
      cs.bytes += ev.len;
      summary_->injected += 1;
      break;
    case replay::InjectStatus::kIgnored:
      cs.ignored += 1;
      summary_->ignored += 1;
      break;
    case replay::InjectStatus::kSkippedPinned:
    case replay::InjectStatus::kOversize:
    case replay::InjectStatus::kCrcMismatch:
    case replay::InjectStatus::kWriteFailed:
      cs.skipped += 1;
      summary_->skipped += 1;
      break;
    case replay::InjectStatus::kUnmapped:
      cs.skipped += 1;
      cs.unmapped += 1;
      summary_->skipped += 1;
      summary_->unmapped += 1;
      break;
    case replay::InjectStatus::kBadEvent:
    case replay::InjectStatus::kGated:
    case replay::InjectStatus::kNotOpen:
      cs.failed += 1;
      summary_->failed += 1;
      break;
  }
}

void Runner::PrintPlanHeader() {
  out_ << "thyme-replay: play " << opt_.package_dir << '\n';
  out_ << "package : channels=" << session_.package().channels().size()
       << "  mono_ns=" << slices_[current_slice_].timeline_first_ns << ".."
       << slices_[current_slice_].timeline_last_ns << '\n';
}

void Runner::PrintMappingTable() const {
  std::size_t mapped = 0;
  std::size_t legacy = 0;
  std::size_t missing = 0;
  std::size_t conflicts = 0;
  for (const MappingRow& r : mapping_rows_) {
    if (r.status == "mapped" || r.status == "mapped-unverified" ||
        r.status == "explicit") {
      ++mapped;
    } else if (r.status == "mapped-legacy") {
      ++legacy;
    } else if (r.status == "missing") {
      ++missing;
    } else {
      ++conflicts;
    }
  }
  out_ << "topology: " << mapping_rows_.size() << " mapping(s)"
       << "  mapped=" << (mapped + legacy) << " legacy=" << legacy
       << " missing=" << missing << " conflicts=" << conflicts << '\n';
  out_ << std::left << std::setw(4) << "CH" << ' ' << std::setw(16)
       << "CHANNEL" << ' ' << std::setw(6) << "SRC" << ' ' << std::setw(6)
       << "TGT" << ' ' << std::setw(20) << "KEY" << ' ' << std::setw(18)
       << "STATUS" << "DETAIL\n";
  for (const MappingRow& r : mapping_rows_) {
    out_ << std::left << std::setw(4) << r.channel << ' ' << std::setw(16)
         << r.channel_name << ' ' << std::setw(6) << r.source_struct_id << ' '
         << std::setw(6)
         << (r.mapped ? std::to_string(r.target_struct_id) : std::string("-"))
         << ' ' << std::setw(20) << r.key << ' ' << std::setw(18) << r.status
         << r.detail << '\n';
  }
}

void Runner::PrintMappingJson() const {
  std::ostringstream o;
  auto esc = [](const std::string& in) {
    std::string r;
    for (const char c : in) {
      if (c == '"' || c == '\\') {
        r += '\\';
        r += c;
      } else if (c == '\n') {
        r += "\\n";
      } else {
        r += c;
      }
    }
    return r;
  };
  auto quoted = [&esc](const std::string& v) {
    return std::string("\"") + esc(v) + "\"";
  };
  o << "{\"tool\":\"thyme-replay\",\"command\":\""
    << (opt_.check_compat ? "check-compat" : "dry-run") << "\",\"package\":"
    << quoted(opt_.package_dir) << ",\"ok\":true,\"mappings\":[";
  for (std::size_t i = 0; i < mapping_rows_.size(); ++i) {
    const MappingRow& r = mapping_rows_[i];
    if (i != 0) o << ',';
    o << "{\"channel\":" << r.channel << ",\"channel_name\":"
      << quoted(r.channel_name) << ",\"ring\":" << quoted(r.ring)
      << ",\"source_struct_id\":" << r.source_struct_id
      << ",\"target_struct_id\":"
      << (r.mapped ? std::to_string(r.target_struct_id) : std::string("null"))
      << ",\"key\":" << quoted(r.key) << ",\"status\":" << quoted(r.status)
      << ",\"detail\":" << quoted(r.detail) << '}';
  }
  o << "]}";
  out_ << o.str() << '\n';
}

void Runner::PrintSummaryText() const {
  const PlaySummary& s = *summary_;
  out_ << "playback: " << s.mode << "  timeline=" << s.timeline
       << "  rate=" << std::fixed << std::setprecision(3) << s.rate
       << "  events=" << s.events << " injected=" << s.injected
       << " skipped=" << s.skipped << " ignored=" << s.ignored
       << " failed=" << s.failed << " unmapped=" << s.unmapped << '\n';
  out_ << "joins   : " << s.slice_joins << "  duplicates=" << s.join_duplicates
       << "  holes=" << s.join_holes << '\n';
  if (s.stopped) {
    out_ << "stop    : requested (SIGINT/SIGTERM or 'q')\n";
  }
  if (s.window_closed) {
    out_ << "window  : closed by --to before the next event\n";
  }
  if (s.step_done) {
    out_ << "step    : delivered exactly one event\n";
  }
  out_ << "timing  : setup=" << FormatDurationNs(s.setup_ns)
       << " play=" << FormatDurationNs(s.play_ns)
       << " span="
       << FormatDurationNs(s.last_event_ns >= s.first_event_ns
                               ? s.last_event_ns - s.first_event_ns
                               : 0)
       << "\n";
  if (s.mono_regressions != 0) {
    out_ << "order   : " << s.mono_regressions
         << " mono_ns regression(s) (timeline is not monotonic)\n";
  }
  out_ << "result  : " << (s.ok ? "OK" : "FAILED")
       << "  state=" << s.state;
  if (!s.ok && !s.error.empty()) {
    out_ << " (" << s.error << ')';
  }
  out_ << '\n';
  if (!s.status_file.empty()) {
    out_ << "status  : " << s.status_file << '\n';
  }
  out_ << std::left << std::setw(3) << "ID" << ' ' << std::setw(14) << "NAME"
       << ' ' << std::setw(8) << "KIND" << std::right << std::setw(8)
       << "EVENTS" << std::setw(10) << "INJECTED" << std::setw(9) << "SKIPPED"
       << std::setw(9) << "IGNORED" << std::setw(8) << "FAILED"
       << std::setw(10) << "UNMAPPED" << std::setw(12) << "BYTES" << '\n';
  for (const PlayChannelSummary& c : s.channels) {
    out_ << std::left << std::setw(3) << c.id << ' ' << std::setw(14) << c.name
         << ' ' << std::setw(8) << c.kind << std::right << std::setw(8)
         << c.events << std::setw(10) << c.injected << std::setw(9) << c.skipped
         << std::setw(9) << c.ignored << std::setw(8) << c.failed
         << std::setw(10) << c.unmapped << std::setw(12)
         << FormatBytes(c.bytes) << '\n';
  }
}

void Runner::PrintSummaryJson() const {
  const PlaySummary& s = *summary_;
  std::ostringstream o;
  auto esc = [](const std::string& in) {
    std::string r;
    for (const char c : in) {
      if (c == '"' || c == '\\') {
        r += '\\';
        r += c;
      } else if (c == '\n') {
        r += "\\n";
      } else if (c == '\r') {
        r += "\\r";
      } else if (c == '\t') {
        r += "\\t";
      } else {
        r += c;
      }
    }
    return r;
  };
  auto quoted = [&esc](const std::string& v) {
    return std::string("\"") + esc(v) + "\"";
  };
  o << "{\"tool\":\"thyme-replay\",\"command\":\"play\",\"package\":"
    << quoted(opt_.package_dir) << ",\"ok\":" << (s.ok ? "true" : "false")
    << ",\"exit_code\":" << s.exit_code << ",\"error\":"
    << (s.error.empty() ? std::string("null") : quoted(s.error))
    << ",\"state\":" << quoted(s.state)
    << ",\"mode\":" << quoted(s.mode)
    << ",\"timeline\":" << quoted(s.timeline)
    << ",\"rate\":" << std::fixed << std::setprecision(3) << s.rate
    << ",\"status_file\":"
    << (s.status_file.empty() ? std::string("null") : quoted(s.status_file))
    << ",\"stopped\":" << (s.stopped ? "true" : "false")
    << ",\"window_closed\":" << (s.window_closed ? "true" : "false")
    << ",\"step_done\":" << (s.step_done ? "true" : "false")
    << ",\"events\":" << s.events << ",\"injected\":" << s.injected
    << ",\"skipped\":" << s.skipped << ",\"ignored\":" << s.ignored
    << ",\"failed\":" << s.failed << ",\"unmapped\":" << s.unmapped
    << ",\"first_mono_ns\":" << s.first_event_ns
    << ",\"last_mono_ns\":" << s.last_event_ns
    << ",\"mono_regressions\":" << s.mono_regressions
    << ",\"slice_joins\":" << s.slice_joins
    << ",\"join_duplicates\":" << s.join_duplicates
    << ",\"join_holes\":" << s.join_holes
    << ",\"gate_rejects\":" << s.gate_rejects
    << ",\"gate_steals\":" << s.gate_steals
    << ",\"setup_ns\":" << s.setup_ns << ",\"play_ns\":" << s.play_ns
    << ",\"warnings\":[";
  for (std::size_t i = 0; i < s.warnings.size(); ++i) {
    if (i != 0) {
      o << ',';
    }
    o << quoted(s.warnings[i]);
  }
  o << "],\"mappings\":[";
  for (std::size_t i = 0; i < mapping_rows_.size(); ++i) {
    const MappingRow& r = mapping_rows_[i];
    if (i != 0) o << ',';
    o << "{\"channel\":" << r.channel << ",\"channel_name\":"
      << quoted(r.channel_name) << ",\"ring\":" << quoted(r.ring)
      << ",\"source_struct_id\":" << r.source_struct_id
      << ",\"target_struct_id\":"
      << (r.mapped ? std::to_string(r.target_struct_id) : std::string("null"))
      << ",\"key\":" << quoted(r.key) << ",\"status\":" << quoted(r.status)
      << ",\"detail\":" << quoted(r.detail) << '}';
  }
  o << "],\"mapping_counts\":{\"mapped\":" << s.mappings_mapped
    << ",\"legacy\":" << s.mappings_legacy
    << ",\"missing\":" << s.mappings_missing
    << ",\"conflicts\":" << s.mapping_conflicts << "},\"channels\":[";
  for (std::size_t i = 0; i < s.channels.size(); ++i) {
    const PlayChannelSummary& c = s.channels[i];
    if (i != 0) {
      o << ',';
    }
    o << "{\"id\":" << c.id << ",\"name\":" << quoted(c.name)
      << ",\"kind\":" << quoted(c.kind) << ",\"events\":" << c.events
      << ",\"injected\":" << c.injected << ",\"skipped\":" << c.skipped
      << ",\"ignored\":" << c.ignored << ",\"failed\":" << c.failed
      << ",\"unmapped\":" << c.unmapped << ",\"bytes\":" << c.bytes
      << ",\"last_mono_ns\":" << c.last_mono_ns << '}';
  }
  o << "]}";
  out_ << o.str() << '\n';
}

StatusSnapshot Runner::BuildSnapshot(const char* state) const {
  StatusSnapshot s;
  s.state = state;
  s.package = opt_.package_dir;
  s.mode = summary_->mode;
  s.timeline = summary_->timeline;
  s.rate = effective_rate_;
  s.pid = static_cast<uint64_t>(getpid());
  s.events = summary_->events;
  s.injected = summary_->injected;
  s.skipped = summary_->skipped;
  s.ignored = summary_->ignored;
  s.failed = summary_->failed;
  s.unmapped = summary_->unmapped;
  s.position_mono_ns = position_ns_;
  s.first_event_ns = summary_->first_event_ns;
  s.last_event_ns = summary_->last_event_ns;
  s.package_first_mono_ns = slices_[current_slice_].timeline_first_ns;
  s.package_last_mono_ns = slices_[current_slice_].timeline_last_ns;
  s.slice_joins = summary_->slice_joins;
  s.join_duplicates = summary_->join_duplicates;
  s.join_holes = summary_->join_holes;
  s.has_window = has_window_;
  s.from_ns = run_from_ns_;
  s.to_ns = run_to_ns_;
  s.setup_ns = setup_ns_;
  s.elapsed_ns =
      run_begin_ns_ == 0 ? 0 : NowNs() - run_begin_ns_;
  s.stopped = summary_->stopped;
  s.window_closed = summary_->window_closed;
  s.step_done = summary_->step_done;
  s.error = summary_->error;
  s.warnings = summary_->warnings;
  s.channels.reserve(summary_->channels.size());
  for (const PlayChannelSummary& c : summary_->channels) {
    StatusChannel sc;
    sc.id = c.id;
    sc.name = c.name;
    sc.kind = c.kind;
    sc.events = c.events;
    sc.injected = c.injected;
    sc.skipped = c.skipped;
    sc.ignored = c.ignored;
    sc.failed = c.failed;
    sc.unmapped = c.unmapped;
    sc.bytes = c.bytes;
    sc.last_mono_ns = c.last_mono_ns;
    s.channels.push_back(std::move(sc));
  }
  return s;
}

void Runner::PublishStatus(const char* state, bool force) {
  if (!status_.enabled()) {
    return;
  }
  if (!force && !status_.Due(state)) {
    return;  // avoid building the snapshot on the playback fast path
  }
  run_state_ = state;
  status_.Publish(BuildSnapshot(state), force);
}

replay::ReplayStatus Runner::OpenInitialPackage() {
  struct stat meta_st {};
  const bool explicit_list = !opt_.package_dirs.empty();
  const bool is_package =
      stat((opt_.package_dir + "/meta.tbin").c_str(), &meta_st) == 0 &&
      S_ISREG(meta_st.st_mode);
  if (!explicit_list && is_package) {
    const replay::ReplayStatus st = session_.Open(opt_.package_dir);
    if (st == replay::ReplayStatus::kOk) {
      replay::ReplayPlaylistSlice slice;
      slice.dir = opt_.package_dir;
      slice.first_mono_ns = session_.package().first_mono_ns();
      slice.last_mono_ns = session_.package().last_mono_ns();
      slice.timeline_first_ns = slice.first_mono_ns;
      slice.timeline_last_ns = slice.last_mono_ns;
      slice.has_events = true;
      slices_.push_back(std::move(slice));
    }
    return st;
  }

  replay::ReplayPlayerConfig preflight_cfg;
  preflight_cfg.stitch_timeline = opt_.timeline == "stitch";
  replay::ReplayPlaylist preflight;
  const replay::ReplayStatus st =
      explicit_list ? preflight.Open(opt_.package_dirs, preflight_cfg)
                    : preflight.Open(opt_.package_dir, preflight_cfg);
  if (st != replay::ReplayStatus::kOk) {
    summary_->error = preflight.error();
    return st;
  }
  slices_ = preflight.slices();
  return session_.Open(slices_.front().dir);
}

replay::ReplayStatus Runner::OpenNextPackage(std::size_t index,
                                             bool count_join) {
  if (index >= slices_.size()) {
    return replay::ReplayStatus::kInvalidArgument;
  }
  replay::ReplayPackage next;
  replay::ReplayStatus st = next.Open(slices_[index].dir);
  if (st != replay::ReplayStatus::kOk) {
    summary_->error = "next package open failed: " + next.error();
    return st;
  }
  st = replay::ValidateNextPackageContract(next, contract_channels_,
                                           &summary_->error);
  if (st != replay::ReplayStatus::kOk) {
    return st;
  }

  const double rate = session_.rate();
  const bool as_fast = session_.as_fast();
  const bool paused = session_.paused();
  const bool had_event = has_order_baseline_;
  const uint64_t previous_last_ns = summary_->last_event_ns;
  const uint64_t previous_source_last_ns = session_.package().last_mono_ns();
  st = session_.Open(slices_[index].dir);
  if (st != replay::ReplayStatus::kOk) {
    summary_->error = "next session open failed: " + session_.package().error();
    return st;
  }
  (void)session_.SetRate(rate);
  session_.AsFast(as_fast);
  if (paused) {
    session_.Pause();
  }
  if (control_ != nullptr && control_->stop_requested()) {
    session_.Stop();
  }
  skip_join_overlap_ = count_join && had_event && opt_.timeline != "stitch";
  join_previous_last_ns_ = previous_last_ns;
  if (count_join) {
    summary_->slice_joins += 1;
    const uint64_t join_previous_ns = opt_.timeline == "stitch"
                                          ? previous_source_last_ns
                                          : previous_last_ns;
    if (had_event && next.first_mono_ns() > join_previous_ns) {
      summary_->join_holes += 1;
    }
  }
  current_slice_ = index;
  return replay::ReplayStatus::kOk;
}

replay::ReplayStatus Runner::SeekTo(uint64_t mono_ns) {
  std::size_t target = current_slice_;
  if (slices_.size() > 1) {
    target = slices_.size() - 1;
    for (std::size_t i = 0; i < slices_.size(); ++i) {
      if (mono_ns <= slices_[i].timeline_last_ns) {
        target = i;
        break;
      }
    }
  }
  if (target != current_slice_) {
    const replay::ReplayStatus st = OpenNextPackage(target, false);
    if (st != replay::ReplayStatus::kOk) {
      return st;
    }
  }
  uint64_t source_ns = mono_ns;
  if (opt_.timeline == "stitch" &&
      !replay::UnshiftReplayTimestamp(
          mono_ns, slices_[current_slice_].offset_ns, &source_ns)) {
    const int64_t offset_ns = slices_[current_slice_].offset_ns;
    if (offset_ns > 0 && mono_ns < static_cast<uint64_t>(offset_ns)) {
      source_ns = 0;
    } else {
      summary_->error =
          "stitch seek target is outside the source timestamp range";
      return replay::ReplayStatus::kInvalidArgument;
    }
  }
  const replay::ReplayStatus st = session_.Seek(source_ns);
  if (st == replay::ReplayStatus::kOk) {
    if (slices_.size() > 1) {
      has_order_baseline_ = false;
    }
    skip_join_overlap_ = false;
    position_ns_ = mono_ns;
  }
  return st;
}

bool Runner::ApplyControlRequests() {
  if (control_ == nullptr) {
    return true;
  }
  double rate = 0.0;
  bool has_rate = false;
  uint64_t seek_ns = 0;
  bool has_seek = false;
  control_->TakeRequests(&rate, &has_rate, &seek_ns, &has_seek);
  if (has_rate && rate > 0.0) {
    if (session_.SetRate(rate) == replay::ReplayStatus::kOk) {
      effective_rate_ = rate;
      summary_->rate = rate;
    }
  }
  if (has_seek) {
    const replay::ReplayStatus st = SeekTo(seek_ns);
    if (st != replay::ReplayStatus::kOk) {
      if (summary_->error.empty()) {
        summary_->error = std::string("seek failed: ") +
                          replay::ReplayStatusName(st) + ": " +
                          session_.package().error();
      }
      control_failure_ = st;
      return false;
    }
    // A seek starts a new pacing segment: no accumulated event state leaks
    // into the snapshot's position if the new target is past the window.
  }
  return true;
}

int Runner::Run() {
  const uint64_t setup_begin = NowNs();
  summary_->mode =
      opt_.step_once ? "step" : opt_.as_fast ? "as-fast" : "paced";
  summary_->rate = opt_.rate;
  summary_->timeline = opt_.timeline;
  summary_->state = "failed";
  effective_rate_ = opt_.rate;
  const replay::ReplayStatus open_st = OpenInitialPackage();
  if (open_st != replay::ReplayStatus::kOk) {
    if (summary_->error.empty()) {
      summary_->error = std::string(replay::ReplayStatusName(open_st)) + ": " +
                        session_.package().error();
    }
    summary_->exit_code = kExitFailure;
    if (opt_.json) {
      PrintSummaryJson();
    } else {
      out_ << "thyme-replay: cannot open package " << opt_.package_dir << ": "
           << summary_->error << '\n';
    }
    return kExitFailure;
  }
  contract_channels_ = session_.package().channels();
  // ABI-10: refuse to inject under a different recorded-channel assignment.
  if (!CheckManifestIdentity(session_.package().manifest_declared(),
                             session_.package().manifest_hash(),
                             opt_.have_expect_manifest_hash,
                             opt_.expect_manifest_hash, &summary_->error)) {
    summary_->exit_code = kExitFailure;
    if (opt_.json) {
      PrintSummaryJson();
    } else {
      out_ << "thyme-replay: " << summary_->error << '\n';
    }
    return kExitFailure;
  }
  if (!opt_.json) {
    PrintPlanHeader();
  }
  if (!(opt_.rate > 0.0)) {
    summary_->error = "--rate must be > 0";
    summary_->exit_code = kExitUsage;
    if (opt_.json) {
      PrintSummaryJson();
    } else {
      out_ << "thyme-replay: " << summary_->error << '\n';
    }
    return kExitUsage;
  }
  if (opt_.start_paused && control_ == nullptr) {
    summary_->error = "--pause needs a control surface";
    summary_->exit_code = kExitUsage;
    if (opt_.json) {
      PrintSummaryJson();
    } else {
      out_ << "thyme-replay: " << summary_->error << '\n';
    }
    return kExitUsage;
  }
  std::string err;
  if (!ResolveTargets(&err)) {
    summary_->error = err;
    summary_->exit_code = kExitTargets;
    if (opt_.json) {
      PrintSummaryJson();
    } else {
      out_ << "thyme-replay: target setup failed: " << err << '\n';
    }
    return kExitTargets;
  }
  if (opt_.dry_run) {
    // REP-11: --check-compat / --dry-run resolve the topology and report it
    // without creating, attaching or injecting anything.
    if (!CheckUnmapped(&err)) {
      summary_->error = err;
      summary_->exit_code = kExitTargets;
      if (opt_.json) {
        PrintSummaryJson();
      } else {
        out_ << "thyme-replay: " << err << '\n';
      }
      return kExitTargets;
    }
    summary_->ok = true;
    summary_->exit_code = kExitOk;
    summary_->state = opt_.check_compat ? "check-compat" : "dry-run";
    if (opt_.json) {
      PrintMappingJson();
    } else {
      PrintMappingTable();
      out_ << "result  : OK  (no targets created, no events injected)\n";
    }
    return kExitOk;
  }
  if (!CheckUnmapped(&err)) {
    summary_->error = err;
    summary_->exit_code = kExitTargets;
    if (opt_.json) {
      PrintSummaryJson();
    } else {
      out_ << "thyme-replay: " << err << '\n';
    }
    return kExitTargets;
  }
  if (!CreateTargets(&err)) {
    summary_->error = err;
    summary_->exit_code = kExitTargets;
    if (opt_.json) {
      PrintSummaryJson();
    } else {
      out_ << "thyme-replay: target setup failed: " << err << '\n';
    }
    return kExitTargets;
  }
  const replay::ReplayStatus rate_st = session_.SetRate(opt_.rate);
  if (rate_st != replay::ReplayStatus::kOk) {
    summary_->error = "invalid rate";
    summary_->exit_code = kExitUsage;
    return kExitUsage;
  }
  if (opt_.as_fast) {
    session_.AsFast(true);
  }
  summary_->mode =
      opt_.step_once ? "step" : opt_.as_fast ? "as-fast" : "paced";
  summary_->rate = opt_.rate;
  effective_rate_ = opt_.rate;

  const uint64_t pkg_first = slices_[current_slice_].timeline_first_ns;
  bool has_start = false;
  uint64_t start_ns = pkg_first;
  const TimeSpec* start_spec = nullptr;
  if (opt_.has_from) {
    start_spec = &opt_.from;
  } else if (opt_.has_seek) {
    start_spec = &opt_.seek;
  }
  if (start_spec != nullptr) {
    has_start = true;
    if (start_spec->absolute) {
      start_ns = start_spec->ns;
    } else {
      AddU64(pkg_first, start_spec->ns, &start_ns);
    }
  }
  bool has_to = opt_.has_to;
  uint64_t to_ns = 0;
  if (has_to) {
    if (opt_.to.absolute) {
      to_ns = opt_.to.ns;
    } else {
      AddU64(pkg_first, opt_.to.ns, &to_ns);
    }
  }
  if (has_start) {
    const replay::ReplayStatus seek_st = SeekTo(start_ns);
    if (seek_st != replay::ReplayStatus::kOk) {
      if (summary_->error.empty()) {
        summary_->error = std::string("seek failed: ") +
                          replay::ReplayStatusName(seek_st) + ": " +
                          session_.package().error();
      }
      summary_->exit_code = kExitFailure;
      if (opt_.json) {
        PrintSummaryJson();
      } else {
        out_ << "thyme-replay: " << summary_->error << '\n';
      }
      return kExitFailure;
    }
  }
  if (opt_.start_paused) {
    control_->RequestPause();
  }
  if (opt_.step_once && control_ != nullptr) {
    control_->RequestResume();  // step implies an immediate delivery
  }
  if (!opt_.json) {
    const char* mode = opt_.step_once ? "step"
                       : opt_.as_fast ? "as-fast"
                                      : "paced";
    out_ << "start   : " << mode
         << (has_start ? "  from mono_ns " + std::to_string(start_ns) : "")
         << (has_to ? "  to " + std::to_string(to_ns) + " (inclusive)" : "")
         << (opt_.start_paused ? "  PAUSED" : "") << '\n';
  }
  has_window_ = has_start || has_to;
  run_from_ns_ = has_start ? start_ns : 0;
  run_to_ns_ = to_ns;
  position_ns_ = has_start ? start_ns : pkg_first;

  if (control_ != nullptr) {
    control_->Attach(&session_);
  }

  // REP-08: the status document is opened before any event is delivered, so a
  // bad path fails the run up front instead of silently losing observability.
  if (!opt_.status_file.empty()) {
    std::string serr;
    if (!status_.Open(opt_.status_file, opt_.status_interval_ms, &serr)) {
      summary_->error = serr;
      summary_->exit_code = kExitUsage;
      if (opt_.json) {
        PrintSummaryJson();
      } else {
        out_ << "thyme-replay: " << serr << '\n';
      }
      return kExitUsage;
    }
    summary_->status_file = opt_.status_file;
  }

  // Interactive command thread (tty only; pause mode). Polling keeps it
  // joinable: no detached thread is left reading stdin.
  std::atomic<bool> tty_done{false};
  std::thread tty_thread;
  if (opt_.start_paused && control_ != nullptr && isatty(STDIN_FILENO) != 0) {
    tty_thread = std::thread([&, this, pkg_first] {
      while (!tty_done.load()) {
        struct pollfd pfd {};
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        const int rc = poll(&pfd, 1, 100);
        if (rc <= 0) {
          continue;
        }
        std::string line;
        if (!std::getline(std::cin, line)) {
          control_->RequestStop();
          break;
        }
        if (line == "q" || line == "quit") {
          control_->RequestStop();
          break;
        }
        if (line.rfind("rate ", 0) == 0) {
          char* end = nullptr;
          const double r = std::strtod(line.c_str() + 5, &end);
          if (end != line.c_str() + 5 && *end == '\0' && r > 0.0) {
            control_->RequestRate(r);
          }
          continue;
        }
        if (line.rfind("seek ", 0) == 0) {
          TimeSpec ts;
          std::string perr;
          if (ParseTimeSpec(line.substr(5), &ts, &perr)) {
            uint64_t target = ts.ns;
            if (!ts.absolute) {
              AddU64(pkg_first, ts.ns, &target);
            }
            control_->RequestSeek(target);
          }
          continue;
        }
        if (line == "p") {
          control_->RequestPause();
        } else if (line == "s" || line == "step") {
          control_->RequestStep();
        } else {
          control_->RequestResume();
        }
      }
    });
  }

  const uint64_t setup_ns = NowNs() - setup_begin;
  summary_->setup_ns = setup_ns;
  setup_ns_ = setup_ns;
  const uint64_t play_begin = NowNs();
  run_begin_ns_ = play_begin;

  // REP-08: apply controls queued before playback started, then publish the
  // first snapshot. A pre-queued seek behaves exactly like --seek (it is
  // applied before any event is delivered).
  if (!ApplyControlRequests()) {
    summary_->exit_code = kExitFailure;
    if (opt_.json) {
      PrintSummaryJson();
    } else {
      out_ << "thyme-replay: " << summary_->error << '\n';
    }
    return kExitFailure;
  }
  if (status_.enabled()) {
    PublishStatus(control_ != nullptr && control_->paused() ? "paused"
                                                            : "running",
                  true);
    if (!status_.error().empty()) {
      summary_->error = status_.error();
      summary_->exit_code = kExitFailure;
      if (opt_.json) {
        PrintSummaryJson();
      } else {
        out_ << "thyme-replay: " << summary_->error << '\n';
      }
      return kExitFailure;
    }
  }

  // REP-10: keep the producer gate lease fresh while replay is alive, including
  // long pauses (the session may sit in NextEvent() for minutes). Without this
  // a legitimate pause would look like a dead replay to writers.
  std::atomic<bool> gate_heartbeat_done{false};
  std::thread gate_heartbeat;
  if (opt_.gate && (!big_table_.empty() || !small_table_.empty())) {
    gate_heartbeat = std::thread([this, &gate_heartbeat_done] {
      while (!gate_heartbeat_done.load(std::memory_order_relaxed)) {
        pinned_slots_.Heartbeat();
        smalls_.Heartbeat();
        std::this_thread::sleep_for(
            std::chrono::nanoseconds(thyme::kProducerGateHeartbeatIntervalNs));
      }
    });
  }

  bool last_was_step = false;
  bool stop_seen = false;
  replay::ReplayStatus failure = replay::ReplayStatus::kOk;
  replay::ReplayEvent ev;

  for (;;) {
    if (control_ != nullptr) {
      // Publish before blocking in WaitForTurn(), so a paused run is visible
      // to `thyme-replay status` while it waits.
      PublishStatus(control_->paused() ? "paused" : "running", false);
      const ControlTurn turn = control_->WaitForTurn();
      if (turn == ControlTurn::kStop) {
        stop_seen = true;
        break;
      }
      if (turn == ControlTurn::kRetry) {
        if (!ApplyControlRequests()) {
          failure = control_failure_;
          break;
        }
        PublishStatus(control_->paused() ? "paused" : "running", true);
        continue;
      }
      const bool step_now = turn == ControlTurn::kStep;
      replay::ReplayStatus st;
      if (step_now) {
        st = session_.Step(&ev);
        last_was_step = true;
      } else {
        if (last_was_step) {
          session_.Resume();
          last_was_step = false;
        }
        st = session_.NextEvent(&ev);
      }
      if (st == replay::ReplayStatus::kStopped) {
        stop_seen = true;
        break;
      }
      if (st == replay::ReplayStatus::kEnd) {
        if (current_slice_ + 1 >= slices_.size()) {
          break;
        }
        const replay::ReplayStatus next =
            OpenNextPackage(current_slice_ + 1, true);
        if (next != replay::ReplayStatus::kOk) {
          failure = next;
          break;
        }
        continue;
      }
      if (st != replay::ReplayStatus::kOk) {
        failure = st;
        break;
      }
    } else {
      const replay::ReplayStatus st = session_.NextEvent(&ev);
      if (st == replay::ReplayStatus::kEnd) {
        if (current_slice_ + 1 >= slices_.size()) {
          break;
        }
        const replay::ReplayStatus next =
            OpenNextPackage(current_slice_ + 1, true);
        if (next != replay::ReplayStatus::kOk) {
          failure = next;
          break;
        }
        continue;
      }
      if (st == replay::ReplayStatus::kStopped) {
        stop_seen = true;
        break;
      }
      if (st != replay::ReplayStatus::kOk) {
        failure = st;
        break;
      }
    }

    if (opt_.timeline == "stitch" &&
        !replay::ShiftReplayTimestamp(ev.mono_ns,
                                      slices_[current_slice_].offset_ns,
                                      &ev.mono_ns)) {
      failure = replay::ReplayStatus::kInvalidArgument;
      summary_->error = "stitch event timestamp overflows mono_ns";
      break;
    }
    if (skip_join_overlap_) {
      if (ev.mono_ns < join_previous_last_ns_) {
        summary_->join_duplicates += 1;
        continue;
      }
      skip_join_overlap_ = false;
    }
    if (has_to && ev.mono_ns > to_ns) {
      summary_->window_closed = true;
      break;  // the out-of-window event is never injected
    }
    if (summary_->events == 0) {
      summary_->first_event_ns = ev.mono_ns;
    } else if (has_order_baseline_ && ev.mono_ns < summary_->last_event_ns) {
      summary_->mono_regressions += 1;
    }
    has_order_baseline_ = true;
    summary_->events += 1;
    summary_->last_event_ns = ev.mono_ns;

    if (ev.channel >= session_.package().channels().size()) {
      summary_->failed += 1;
      continue;
    }
    const replay::ReplayChannel& ch =
        session_.package().channels()[ev.channel];
    bool mapped = false;
    replay::InjectStatus inject = replay::InjectStatus::kIgnored;
    if (ev.kind == replay::ReplayKind::kData) {
      if (ch.kind == replay::ReplayChannelKind::kPinnedSlot) {
        mapped = big_mapped_[ev.channel];
        if (mapped) {
          inject = pinned_slots_.Inject(ev);
        }
      } else if (ch.kind == replay::ReplayChannelKind::kSmall) {
        mapped = smalls_.HasTarget(ev.channel, ev.struct_id);
        if (mapped) {
          inject = smalls_.Inject(ev);
        }
      } else {  // meta: only injected when explicitly mapped
        mapped = smalls_.HasTarget(ev.channel, ev.struct_id);
        if (mapped) {
          inject = smalls_.Inject(ev);
        } else {
          // Meta rows are informational: counted as ignored, never as a
          // skip or a failure.
          inject = replay::InjectStatus::kIgnored;
          mapped = true;
        }
      }
    }
    Account(ev, inject, mapped);
    position_ns_ = ev.mono_ns;
    // Throttled by --status-interval-ms; no I/O on the fast path otherwise.
    PublishStatus(control_ != nullptr && control_->paused() ? "paused"
                                                            : "running",
                  false);
    if (opt_.step_once) {
      summary_->step_done = true;
      if (control_ != nullptr) {
        control_->RequestStop();
      }
      break;
    }
  }

  if (failure != replay::ReplayStatus::kOk && summary_->error.empty()) {
    summary_->error = std::string(replay::ReplayStatusName(failure)) + ": " +
                      session_.package().error();
  }
  summary_->play_ns = NowNs() - play_begin;
  summary_->stopped = stop_seen;
  if (control_ != nullptr) {
    control_->Detach();
  }
  tty_done.store(true);
  if (tty_thread.joinable()) {
    tty_thread.join();
  }
  gate_heartbeat_done.store(true, std::memory_order_relaxed);
  if (gate_heartbeat.joinable()) {
    gate_heartbeat.join();
  }

  // REP-10: surface double-write attempts. A blocked producer means the gate
  // did its job (the write never reached the slot); report it so an operator
  // knows the real driver is still running. Steals mean a dead/expired replay
  // lease was reclaimed — expected after a crash, not after a clean run.
  summary_->gate_rejects =
      pinned_slots_.gate_rejects() + smalls_.gate_rejects();
  summary_->gate_steals = pinned_slots_.gate_steals() + smalls_.gate_steals();
  if (summary_->gate_rejects != 0) {
    PrintWarning(std::to_string(summary_->gate_rejects) +
                 " real-producer write(s) were blocked by the replay gate "
                 "(double-write prevented)");
  }
  if (summary_->gate_steals != 0) {
    PrintWarning(std::to_string(summary_->gate_steals) +
                 " stale replay gate lease(s) reclaimed by writers");
  }
  // Release both gate types deterministically before returning. The target
  // channels themselves stay in place.
  smalls_.Close();
  pinned_slots_.Close();

  if (!summary_->error.empty()) {
    summary_->ok = false;
    summary_->exit_code = kExitFailure;
  } else if (summary_->failed != 0) {
    // Unusable events / injector not open are hard failures.
    summary_->ok = false;
    summary_->exit_code = kExitFailure;
    summary_->error = "injection failures: " + std::to_string(summary_->failed);
  } else {
    summary_->ok = true;
    summary_->exit_code = kExitOk;
  }
  success_ = true;
  summary_->rate = effective_rate_;
  summary_->state = !summary_->ok ? "failed"
                    : summary_->stopped ? "stopped"
                                        : "finished";
  // REP-08: the status document's last snapshot is the final summary, so a
  // reader that polls during the run does not need a second command/format.
  if (status_.enabled()) {
    PublishStatus(summary_->state.c_str(), true);
    if (!status_.error().empty()) {
      PrintWarning("status-file: " + status_.error());
    }
  }
  if (opt_.json) {
    PrintSummaryJson();
  } else {
    PrintSummaryText();
  }
  return summary_->exit_code;
}

}  // namespace

PlayControl::PlayControl() = default;
PlayControl::~PlayControl() = default;

void PlayControl::RequestPause() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    paused_ = true;
  }
  cv_.notify_all();
}

void PlayControl::RequestResume() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    paused_ = false;
  }
  cv_.notify_all();
}

void PlayControl::RequestStep() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    paused_ = true;
    steps_ += 1;
  }
  cv_.notify_all();
}

void PlayControl::RequestStop() {
  stop_.store(true, std::memory_order_relaxed);
  replay::ReplaySession* s = session_.load(std::memory_order_acquire);
  if (s != nullptr) {
    s->Stop();
  }
  cv_.notify_all();
}

void PlayControl::RequestRate(double rate) {
  if (!(rate > 0.0)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    rate_ = rate;
    has_rate_ = true;
  }
  cv_.notify_all();
}

void PlayControl::RequestSeek(uint64_t mono_ns) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    seek_ns_ = mono_ns;
    has_seek_ = true;
  }
  cv_.notify_all();
}

bool PlayControl::paused() const {
  std::lock_guard<std::mutex> lock(mu_);
  return paused_;
}

void PlayControl::TakeRequests(double* rate, bool* has_rate, uint64_t* seek_ns,
                               bool* has_seek) {
  std::lock_guard<std::mutex> lock(mu_);
  *has_rate = has_rate_;
  if (has_rate_) {
    *rate = rate_;
    has_rate_ = false;
  }
  *has_seek = has_seek_;
  if (has_seek_) {
    *seek_ns = seek_ns_;
    has_seek_ = false;
  }
}

void PlayControl::Attach(replay::ReplaySession* session) {
  session_.store(session, std::memory_order_release);
  if (stop_.load(std::memory_order_relaxed) && session != nullptr) {
    session->Stop();
  }
}

void PlayControl::Detach() {
  session_.store(nullptr, std::memory_order_release);
}

ControlTurn PlayControl::WaitForTurn() {
  std::unique_lock<std::mutex> lock(mu_);
  cv_.wait(lock, [this] {
    return stop_.load(std::memory_order_relaxed) || !paused_ || steps_ > 0 ||
           has_rate_ || has_seek_;
  });
  if (stop_.load(std::memory_order_relaxed)) {
    return ControlTurn::kStop;
  }
  if (has_rate_ || has_seek_) {
    return ControlTurn::kRetry;
  }
  if (steps_ > 0) {
    --steps_;
    return ControlTurn::kStep;
  }
  return ControlTurn::kProceed;
}

int RunPlay(const PlayOptions& options, PlayControl* control, std::ostream& out,
            PlaySummary* summary) {
  PlaySummary local;
  PlaySummary* s = summary != nullptr ? summary : &local;
  *s = PlaySummary{};
  Runner runner(options, control, out, s);
  return runner.Run();
}

}  // namespace replaycli
}  // namespace thyme
