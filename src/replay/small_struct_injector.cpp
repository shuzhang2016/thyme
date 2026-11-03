#include "thyme/replay/small_struct_injector.h"

#include "thyme/runtime/pinned_slot_gate.h"

#include <cstring>
#include <string>
#include <utility>

namespace thyme {
namespace replay {

namespace {

const ThymeStructDesc* FindStruct(const ReplayChannel& ch, uint32_t struct_id) {
  for (const ThymeStructDesc& d : ch.registry) {
    if (d.struct_id == struct_id) {
      return &d;
    }
  }
  return nullptr;
}

}  // namespace

SmallStructInjector::SmallStructInjector() = default;

SmallStructInjector::~SmallStructInjector() = default;

ReplayStatus SmallStructInjector::Open(const ReplayPackage& pkg,
                                       const SmallStructTarget* targets,
                                       uint32_t count) {
  return OpenChannels(&pkg, nullptr, targets, count);
}

ReplayStatus SmallStructInjector::Open(const ReplayChannel& channel,
                                       const SmallStructTarget* targets,
                                       uint32_t count) {
  return OpenChannels(nullptr, &channel, targets, count);
}

ReplayStatus SmallStructInjector::OpenChannels(const ReplayPackage* pkg,
                                               const ReplayChannel* channel,
                                               const SmallStructTarget* targets,
                                               uint32_t count) {
  Close();
  if (pkg != nullptr && !pkg->is_open()) {
    error_ = "package is not open";
    return ReplayStatus::kInvalidArgument;
  }
  if (targets == nullptr && count != 0) {
    error_ = "null target table";
    return ReplayStatus::kInvalidArgument;
  }
  gate_token_ = gate_enabled_ ? ProducerGateNewToken() : 0;
  entries_.reserve(count);
  gates_.reserve(count);
  channels_.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const SmallStructTarget& t = targets[i];
    const ReplayChannel* ch =
        channel != nullptr ? channel : pkg->FindChannel(t.channel);
    if (ch == nullptr) {
      error_ =
          "channel " + std::to_string(t.channel) + " is not in the package";
      return FailOpen(ReplayStatus::kInvalidArgument);
    }
    if (ch->kind == ReplayChannelKind::kPinnedSlot) {
      error_ = "channel " + std::to_string(t.channel) + " (" + ch->name +
               ") is a big-frame channel";
      return FailOpen(ReplayStatus::kInvalidArgument);
    }
    if (channel != nullptr && t.channel != channel->id) {
      error_ = "target channel " + std::to_string(t.channel) +
               " does not match standalone channel " +
               std::to_string(channel->id);
      return FailOpen(ReplayStatus::kInvalidArgument);
    }
    const bool has_ring = t.ring != nullptr;
    const bool has_publish = t.publish != nullptr;
    if (has_ring == has_publish ||
        (has_ring && (!t.ring->Validate() ||
                      t.target_struct_id >= t.ring->struct_count())) ||
        (has_publish && t.payload_capacity == 0)) {
      error_ = "channel " + std::to_string(t.channel) +
               " must have exactly one valid target ring/struct or sink";
      return FailOpen(ReplayStatus::kInvalidArgument);
    }
    const ThymeStructDesc* desc = FindStruct(*ch, t.struct_id);
    if (desc == nullptr) {
      error_ = "struct " + std::to_string(t.struct_id) + " is not in channel " +
               std::to_string(t.channel) + " (" + ch->name + ") registry";
      return FailOpen(ReplayStatus::kInvalidArgument);
    }
    for (const Entry& e : entries_) {
      if (e.channel == t.channel && e.struct_id == t.struct_id) {
        error_ = "duplicate target for channel " + std::to_string(t.channel) +
                 " struct " + std::to_string(t.struct_id);
        return FailOpen(ReplayStatus::kInvalidArgument);
      }
    }
    if (has_ring && gate_enabled_ &&
        !HasGate(t.ring, t.target_struct_id)) {
      const GateResult gate = ProducerGateAcquire(
          *t.ring->producer_gate(t.target_struct_id), gate_token_,
          ProducerGateNowNs(), kProducerGateDefaultTimeoutNs);
      if (gate != GateResult::kOk && gate != GateResult::kStolen) {
        error_ = "target ring struct " + std::to_string(t.target_struct_id) +
                 " for channel " + std::to_string(t.channel) + " struct " +
                 std::to_string(t.struct_id) +
                 " is already gated by another replay holder";
        return FailOpen(ReplayStatus::kBusy);
      }
      gates_.push_back(GateEntry{t.ring, t.target_struct_id});
    }
    Entry e;
    e.channel = t.channel;
    e.struct_id = t.struct_id;
    e.target_struct_id = t.target_struct_id;
    e.ring = t.ring;
    e.publish = t.publish;
    e.publish_ctx = t.publish_ctx;
    e.capacity = has_ring
                     ? t.ring->ring_descs[t.target_struct_id].payload_capacity
                     : t.payload_capacity;
    entries_.push_back(e);

    SmallStructChannelStats cs;
    cs.channel = t.channel;
    cs.struct_id = t.struct_id;
    cs.target_struct_id = t.target_struct_id;
    cs.ring_capacity = e.capacity;
    cs.package_payload = desc->payload_size;
    cs.geometry_fits = desc->payload_size <= e.capacity;
    if (!cs.geometry_fits) {
      stats_.geometry_oversize_structs += 1;
    }
    channels_.push_back(cs);
  }
  open_ = true;
  if (policy_.session_id == 0) {
    // REP-12: resolve the replay session id once; it stays sticky across
    // Close/Open so a reused injector keeps one identity.
    policy_.session_id = GenerateReplaySessionId();
  }
  if (policy_.rewrite_embedded_frame) {
    // One scratch covers every possible Frame wire image (uint16 hdr.size).
    scratch_.resize(0xFFFFu);
  }
  error_.clear();
  return ReplayStatus::kOk;
}

ReplayStatus SmallStructInjector::FailOpen(ReplayStatus status) {
  ReleaseGates();
  gate_token_ = 0;
  entries_.clear();
  gates_.clear();
  scratch_.clear();
  channels_.clear();
  stats_ = SmallStructInjectorStats{};
  return status;
}

bool SmallStructInjector::HasGate(const RecordMirrorRingPage* ring,
                                  uint16_t target_struct_id) const {
  for (const GateEntry& gate : gates_) {
    if (gate.ring == ring && gate.target_struct_id == target_struct_id) {
      return true;
    }
  }
  return false;
}

void SmallStructInjector::ReleaseGates() {
  if (gate_token_ == 0) {
    return;
  }
  for (const GateEntry& entry : gates_) {
    ProducerGateRelease(*entry.ring->producer_gate(entry.target_struct_id),
                        gate_token_);
  }
}

void SmallStructInjector::Close() {
  ReleaseGates();
  gate_token_ = 0;
  open_ = false;
  entries_.clear();
  gates_.clear();
  scratch_.clear();
  channels_.clear();
  stats_ = SmallStructInjectorStats{};
  error_.clear();
}

void SmallStructInjector::Heartbeat() {
  if (gate_token_ == 0) {
    return;
  }
  const uint64_t now_ns = ProducerGateNowNs();
  for (const GateEntry& entry : gates_) {
    ProducerGateHeartbeat(*entry.ring->producer_gate(entry.target_struct_id),
                          gate_token_, now_ns);
  }
}

uint64_t SmallStructInjector::gate_rejects() const {
  uint64_t total = 0;
  for (const GateEntry& entry : gates_) {
    total += entry.ring->producer_gate(entry.target_struct_id)
                 ->rejects.load(std::memory_order_relaxed);
  }
  return total;
}

uint64_t SmallStructInjector::gate_steals() const {
  uint64_t total = 0;
  for (const GateEntry& entry : gates_) {
    total += entry.ring->producer_gate(entry.target_struct_id)
                 ->steals.load(std::memory_order_relaxed);
  }
  return total;
}

bool SmallStructInjector::HasTarget(uint32_t channel,
                                    uint32_t struct_id) const {
  for (const Entry& e : entries_) {
    if (e.channel == channel && e.struct_id == struct_id) {
      return true;
    }
  }
  return false;
}

InjectStatus SmallStructInjector::Inject(const ReplayEvent& ev) {
  if (!open_) {
    return InjectStatus::kNotOpen;
  }
  Entry* entry = nullptr;
  SmallStructChannelStats* cs = nullptr;
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].channel == ev.channel &&
        entries_[i].struct_id == ev.struct_id) {
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
  if (ev.len > entry->capacity) {
    cs->oversize += 1;
    stats_.oversize += 1;
    return InjectStatus::kOversize;
  }
  if (entry->has_data && ev.record_sequence <= entry->last_sequence) {
    cs->sequence_errors += 1;
    stats_.sequence_errors += 1;
  }
  entry->last_sequence = ev.record_sequence;
  entry->has_data = true;
  if (entry->ring != nullptr && gate_enabled_ &&
      !ProducerGateHeartbeat(
          *entry->ring->producer_gate(entry->target_struct_id), gate_token_,
          ProducerGateNowNs())) {
    cs->gated += 1;
    stats_.gated += 1;
    return InjectStatus::kGated;
  }
  // REP-12: default renumber per target (channel, struct) from 0 in injection
  // order; opt-in preserves the recorded producer sequence.
  const uint32_t sequence =
      policy_.renumber_sequence ? entry->next_sequence : ev.source_sequence;
  const uint8_t* payload = ev.payload;
  if (policy_.rewrite_embedded_frame &&
      DetectEmbeddedFrame(ev.payload, ev.len)) {
    // The recorded payload is a read-only mapping; rewrite the copy in the
    // Open-time scratch and publish from there (no allocation on this path).
    std::memcpy(scratch_.data(), ev.payload, ev.len);
    RewriteEmbeddedFrame(scratch_.data(), ev.len, policy_, ev.mono_ns,
                         sequence);
    payload = scratch_.data();
    cs->headers_rewritten += 1;
    stats_.headers_rewritten += 1;
  }
  bool published = false;
  if (entry->ring != nullptr) {
    // One copy, seqlock-published; a concurrent reader may see an odd slot and
    // skip the sample, which is how the ring's drop-oldest discipline already
    // behaves for a live producer.
    published = entry->ring->TryWriteRing(entry->target_struct_id, payload,
                                          ev.len, ev.mono_ns, sequence,
                                          gate_token_);
  } else {
    published = entry->publish(entry->publish_ctx, payload, ev.len, ev.mono_ns,
                               sequence, entry->target_struct_id);
  }
  if (!published) {
    if (entry->ring != nullptr && gate_enabled_ &&
        !ProducerGateHeartbeat(
            *entry->ring->producer_gate(entry->target_struct_id), gate_token_,
            ProducerGateNowNs())) {
      cs->gated += 1;
      stats_.gated += 1;
      return InjectStatus::kGated;
    }
    cs->write_failures += 1;
    stats_.write_failures += 1;
    return InjectStatus::kWriteFailed;
  }
  entry->next_sequence += 1;  // only on a published sample: no renumber gaps
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
