#include "thyme/replay/pinned_slot_injector.h"

#include "thyme/contracts/frame_header.h"

#include <cstring>
#include <utility>

namespace thyme {
namespace replay {

const char* InjectStatusName(InjectStatus status) {
  switch (status) {
    case InjectStatus::kOk:
      return "ok";
    case InjectStatus::kIgnored:
      return "ignored";
    case InjectStatus::kSkippedPinned:
      return "skipped-pinned";
    case InjectStatus::kOversize:
      return "oversize";
    case InjectStatus::kUnmapped:
      return "unmapped";
    case InjectStatus::kBadEvent:
      return "bad-event";
    case InjectStatus::kCrcMismatch:
      return "crc-mismatch";
    case InjectStatus::kWriteFailed:
      return "write-failed";
    case InjectStatus::kGated:
      return "gated";
    case InjectStatus::kNotOpen:
      return "not-open";
  }
  return "unknown";
}

PinnedSlotInjector::PinnedSlotInjector() = default;

PinnedSlotInjector::~PinnedSlotInjector() = default;

ReplayStatus PinnedSlotInjector::Open(const ReplayPackage& pkg,
                                    const PinnedSlotTarget* targets,
                                    uint32_t count) {
  Close();
  if (!pkg.is_open()) {
    error_ = "package is not open";
    return ReplayStatus::kInvalidArgument;
  }
  gate_token_ = gate_enabled_ ? ProducerGateNewToken() : 0;
  if (targets == nullptr && count != 0) {
    error_ = "null target table";
    return ReplayStatus::kInvalidArgument;
  }
  entries_.reserve(count);
  channels_.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const PinnedSlotTarget& t = targets[i];
    const ReplayChannel* ch = pkg.FindChannel(t.channel);
    if (ch == nullptr) {
      error_ = "channel " + std::to_string(t.channel) + " is not in the package";
      ReleaseGates();
      entries_.clear();
      channels_.clear();
      return ReplayStatus::kInvalidArgument;
    }
    if (ch->kind != ReplayChannelKind::kPinnedSlot) {
      error_ = "channel " + std::to_string(t.channel) + " (" + ch->name +
               ") is not a big-frame channel";
      ReleaseGates();
      entries_.clear();
      channels_.clear();
      return ReplayStatus::kInvalidArgument;
    }
    if (t.writer == nullptr || t.writer->slot_bytes() == 0 ||
        t.writer->slot_count() == 0) {
      error_ = "channel " + std::to_string(t.channel) +
               " has no open target writer";
      ReleaseGates();
      entries_.clear();
      channels_.clear();
      return ReplayStatus::kInvalidArgument;
    }
    for (const Entry& e : entries_) {
      if (e.channel == t.channel) {
        error_ = "duplicate target for channel " + std::to_string(t.channel);
        ReleaseGates();
        entries_.clear();
        channels_.clear();
        return ReplayStatus::kInvalidArgument;
      }
    }
    if (gate_enabled_) {
      // REP-10: become the channel's only writer. A second replay (or an
      // injector sharing this writer) gets a distinct kBusy, never a shared
      // producer slot.
      const GateResult gate = t.writer->AcquireProducerGate(gate_token_);
      if (gate != GateResult::kOk && gate != GateResult::kStolen) {
        error_ = "channel " + std::to_string(t.channel) + " (" + ch->name +
                 ") is already gated by another replay holder";
        ReleaseGates();
        entries_.clear();
        channels_.clear();
        return ReplayStatus::kBusy;
      }
    }
    Entry e;
    e.channel = t.channel;
    e.writer = t.writer;
    entries_.push_back(e);

    PinnedSlotStoreStats cs;
    cs.channel = t.channel;
    cs.slot_bytes = t.writer->slot_bytes();
    cs.slot_count = t.writer->slot_count();
    cs.max_frame_len = ch->max_data_len;
    cs.geometry_fits = cs.max_frame_len <= cs.slot_bytes;
    if (!cs.geometry_fits) {
      stats_.geometry_oversize_channels += 1;
    }
    channels_.push_back(cs);
  }
  open_ = true;
  if (policy_.session_id == 0) {
    // REP-12: resolve the replay session id once; a resolved id stays sticky
    // across Close/Open so a reused injector keeps one identity.
    policy_.session_id = GenerateReplaySessionId();
  }
  error_.clear();
  return ReplayStatus::kOk;
}

void PinnedSlotInjector::Close() {
  ReleaseGates();
  gate_token_ = 0;
  open_ = false;
  entries_.clear();
  channels_.clear();
  stats_ = PinnedSlotInjectorStats{};
  error_.clear();
}

void PinnedSlotInjector::ReleaseGates() {
  for (Entry& e : entries_) {
    if (e.writer != nullptr) {
      e.writer->ReleaseProducerGate(gate_token_);
    }
  }
}

void PinnedSlotInjector::Heartbeat() {
  for (Entry& e : entries_) {
    if (e.writer != nullptr) {
      e.writer->HeartbeatProducerGate(gate_token_);
    }
  }
}

uint64_t PinnedSlotInjector::gate_rejects() const {
  uint64_t total = 0;
  for (const Entry& e : entries_) {
    if (e.writer != nullptr) total += e.writer->gate_rejects();
  }
  return total;
}

uint64_t PinnedSlotInjector::gate_steals() const {
  uint64_t total = 0;
  for (const Entry& e : entries_) {
    if (e.writer != nullptr) total += e.writer->gate_steals();
  }
  return total;
}

uint64_t PinnedSlotInjector::gate_aborted() const {
  uint64_t total = 0;
  for (const Entry& e : entries_) {
    if (e.writer != nullptr) total += e.writer->gate_aborted();
  }
  return total;
}

const PinnedSlotStoreStats* PinnedSlotInjector::FindChannel(
    uint32_t channel) const {
  for (const PinnedSlotStoreStats& cs : channels_) {
    if (cs.channel == channel) {
      return &cs;
    }
  }
  return nullptr;
}

InjectStatus PinnedSlotInjector::Inject(const ReplayEvent& ev) {
  if (!open_) {
    return InjectStatus::kNotOpen;
  }
  Entry* entry = nullptr;
  PinnedSlotStoreStats* cs = nullptr;
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].channel == ev.channel) {
      entry = &entries_[i];
      cs = &channels_[i];
      break;
    }
  }
  if (entry == nullptr) {
    stats_.unmapped += 1;
    return InjectStatus::kUnmapped;
  }
  if (ev.kind != ReplayKind::kData) {
    cs->ignored += 1;
    stats_.ignored += 1;
    return InjectStatus::kIgnored;
  }
  if (ev.payload == nullptr || ev.len == 0) {
    stats_.bad_events += 1;
    return InjectStatus::kBadEvent;
  }
  // Geometry is checked before Begin so an oversize frame is counted by the
  // injector and never truncates: the writer's Begin() would refuse it too
  // (dropped_oversize), but that would be the only record of the loss.
  if (ev.len > cs->slot_bytes) {
    cs->oversize += 1;
    stats_.oversize += 1;
    return InjectStatus::kOversize;
  }
  // The index record carries the CRC computed over the raw segment at record
  // time. 0 means the recording channel had CRC disabled; nothing to verify.
  if (verify_crc_ && ev.payload_crc != 0 &&
      Crc32(ev.payload, ev.len) != ev.payload_crc) {
    cs->crc_mismatches += 1;
    stats_.crc_mismatches += 1;
    return InjectStatus::kCrcMismatch;
  }
  if (entry->has_data && ev.record_sequence <= entry->last_sequence) {
    // The reader preserves file order within a source, so a per-channel
    // ordinal must increase for data rows. Count and keep going: the payload
    // is still the frame the package holds at this timeline position.
    cs->sequence_errors += 1;
    stats_.sequence_errors += 1;
  }
  entry->last_sequence = ev.record_sequence;
  entry->has_data = true;
  entry->writer->HeartbeatProducerGate();  // REP-10: keep the lease fresh

  PinnedSlotWriter::BeginView bv = entry->writer->Begin(ev.len);
  if (bv.payload == nullptr) {
    if (bv.status == PinnedSlotWriter::BeginStatus::kGated) {
      // REP-10: the injector no longer owns the gate (stolen after a missed
      // heartbeat). Never overwrite the producer that took it back.
      cs->gated += 1;
      stats_.gated += 1;
      return InjectStatus::kGated;
    }
    if (bv.status == PinnedSlotWriter::BeginStatus::kPinnedSkip ||
        entry->writer->pending_skipped()) {
      // BF-03: a reader holds a lease on the target slot; the writer skipped
      // it (head advanced, no publish). Count and move on — waiting would
      // deadlock a replay against a consumer that never releases.
      cs->skipped_pinned += 1;
      stats_.skipped_pinned += 1;
      return InjectStatus::kSkippedPinned;
    }
    // Defensive: the pre-check above and Begin() read the same slot_bytes,
    // but a concurrent reconfiguration of the target is not part of the
    // contract. Count as oversize, never as a silent drop.
    cs->oversize += 1;
    stats_.oversize += 1;
    return InjectStatus::kOversize;
  }
  // Plain memcpy: Begin() returns this producer's exclusive slot and the
  // injector is the channel's only writer (PinnedSlotWriter is single-producer).
  // AtomicPayloadCopyIn is the copy-under-concurrent-write primitive and
  // would add per-copy fences with no concurrent writer to protect against.
  std::memcpy(bv.payload, ev.payload, ev.len);
  if (policy_.rewrite_embedded_frame &&
      DetectEmbeddedFrame(bv.payload, ev.len)) {
    // REP-12: the recorded FrameHeader embedded in the payload still carries
    // the capture's time/session/sequence; rewrite it to this replay's
    // values and re-seal per ABI-12. Commit() below then computes the slot
    // CRC over the rewritten bytes. Detection ran on the slot copy, which is
    // byte-identical to the recorded payload at this point.
    RewriteEmbeddedFrame(bv.payload, ev.len, policy_, ev.mono_ns,
                         entry->next_sequence);
    cs->headers_rewritten += 1;
    stats_.headers_rewritten += 1;
  }
  bv.header->flags.store(ev.frame_flags, std::memory_order_relaxed);
  entry->writer->Commit(ev.mono_ns);
  entry->next_sequence += 1;  // wraps like any producer sequence

  cs->injected += 1;
  cs->bytes += ev.len;
  cs->last_mono_ns = ev.mono_ns;
  stats_.injected += 1;
  stats_.bytes += ev.len;
  stats_.last_mono_ns = ev.mono_ns;
  return InjectStatus::kOk;
}

}  // namespace replay
}  // namespace thyme
