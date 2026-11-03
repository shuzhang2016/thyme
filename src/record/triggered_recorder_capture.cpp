#include "triggered_recorder_internal.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>

namespace thyme {
namespace triggered {

namespace {

// RT-09: bounded re-sweep budget of HarvestPre's mirror-ring harvest. Each
// sweep retries only when the previous one observed new seqlock tears; a
// persistently torn (or killed-producer) slot still yields after this budget.
constexpr int kHarvestSweepRetries = 16;

}  // namespace

void TriggeredRecorder::DropOldestTriggered() {
  if (!queue_.empty()) {
    pending_bytes_ -= queue_.front().len;
    queue_.pop_front();
  }
  stats_.pending_evicted += 1;
  capture_evicted_ += 1;
}

void TriggeredRecorder::PushDescriptor(uint16_t source, bool pinned_slot,
                                       uint16_t struct_id, uint64_t index,
                                       uint64_t mono_ns, uint32_t len,
                                       uint32_t slot_seq) {
  if (len > cfg_.pending_bytes_quota) {
    // A single descriptor larger than the whole budget never enters.
    stats_.pending_evicted += 1;
    capture_evicted_ += 1;
    return;
  }
  // Shared quota (6.6): triggered capture has priority — evict from the
  // continuous bucket first, then the oldest entry of this trigger's queue.
  while (pending_bytes_ + cont_pending_bytes_ + len >
             cfg_.pending_bytes_quota &&
         !cont_queue_.empty()) {
    DropOldestContinuous();
  }
  while (pending_bytes_ + cont_pending_bytes_ + len >
             cfg_.pending_bytes_quota &&
         !queue_.empty()) {
    DropOldestTriggered();
  }
  // REC-20 descriptor-count bound: the ring may fill before the byte quota.
  // Evict the OLDEST with the same accounting as a quota eviction; the newest
  // descriptors are always the ones kept.
  if (queue_.capacity() != 0 && queue_.full()) DropOldestTriggered();
  SwapDescriptor d;
  d.source = source;
  d.pinned_slot = pinned_slot ? 1 : 0;
  d.struct_id = struct_id;
  d.index = index;
  d.mono_ns = mono_ns;
  d.len = len;
  d.slot_seq = slot_seq;
  if (!queue_.push_back(d)) {
    // Unsized ring (Init never ran): counted, never silently dropped.
    stats_.pending_evicted += 1;
    capture_evicted_ += 1;
    return;
  }
  pending_bytes_ += len;
  stats_.descriptors_queued += 1;
  capture_queued_ += 1;
}

void TriggeredRecorder::HarvestPre(uint64_t t0) {
  const uint64_t t_begin = t0 - before_ns_;
  for (std::size_t si = 0; si < sources_.size(); ++si) {
    SourceState& s = *sources_[si];
    if (!s.attached) continue;
    if (!TriggerIncluded(static_cast<uint16_t>(si))) continue;
    if (s.cfg.kind == SourceConfig::Kind::kMirrorRing) {
      const RecordMirrorRingPage* page = s.mirror.Page();
      for (uint32_t sid = 0; sid < s.struct_count; ++sid) {
        uint32_t decimate = trigger_decimate_[si];
        if (!SelectRecordStruct(trigger_rules_, s.cfg,
                                static_cast<uint16_t>(sid), &decimate)) {
          continue;
        }
        const uint32_t depth = page->RingDepth(static_cast<uint16_t>(sid));
        if (depth == 0) continue;
        std::vector<RecordMirrorRingSampleDesc> buf(depth);
        // A slot torn by an in-flight tap is skipped by HarvestWindow; retry
        // the sweep so the pre window is not silently short (bounded: a
        // persistently odd slot — killed producer — still yields).
        uint32_t n = 0;
        uint64_t torn_before = capture_torn_;
        // Index boundary: post_next[sid] is snapshotted in Trigger before this
        // harvest. The time window is widened to [t_begin, t_end] — samples
        // tapped between t0 and the snapshot (mono > t0, index < post_next)
        // are collected here; index >= post_next belongs to CollectPost and is
        // filtered out. Neither side drops or duplicates (the same boundary
        // semantics as the pinned_slot pre-walk).
        const uint64_t post_from = s.post_next[sid];
        for (int attempt = 0; attempt < kHarvestSweepRetries; ++attempt) {
          n = page->HarvestWindow(static_cast<uint16_t>(sid), t_begin,
                                  t0 + after_ns_, buf.data(), depth,
                                  &capture_torn_);
          if (capture_torn_ == torn_before) break;
          stats_.harvest_retries += 1;  // RT-09: only on the tear path
          torn_before = capture_torn_;
        }
        for (uint32_t i = 0; i < n; ++i) {
          if (buf[i].publish_index >= post_from) continue;  // belongs to post
          if (buf[i].publish_index % decimate != 0) {
            continue;
          }
          PushDescriptor(static_cast<uint16_t>(si), false,
                         static_cast<uint16_t>(sid), buf[i].publish_index,
                         buf[i].mono_ns, buf[i].payload_size,
                         buf[i].slot_seq);
        }
      }
    } else {
      // Big-frame pre window: walk back from the post cursor (snapped in
      // Trigger BEFORE this harvest — strict index partition: pre covers
      // [.., post_next), post covers [post_next, ..), so a frame committed
      // while the harvest runs is picked up by CollectPost instead of
      // falling into the gap between two head reads). Stop at the first
      // frame older than the window (mono_ns is monotonic per channel).
      // h264 with gop_ns looks that much further back, then keeps from the
      // last IDR at or before t_begin. No IDR in that slack keeps t_begin.
      // The walk collects newest-first; push oldest-first so the drain
      // queue keeps per-channel monotonic order.
      struct Held {
        SwapDescriptor d;
        uint32_t flags = 0;
      };
      std::vector<Held> rev;
      const uint64_t slack =
          (s.cfg.h264_stream && s.cfg.gop_ns > 0) ? s.cfg.gop_ns : 0;
      const uint64_t t_floor = t_begin > slack ? t_begin - slack : 0;
      const uint64_t h = s.post_next[0];
      const uint32_t count = s.frames.slot_count();
      const uint64_t oldest =
          h > count ? h - count : 0;
      for (uint64_t idx = h; idx-- > oldest;) {
        PinnedSlotReader::View v;
        PinnedSlotReader::ReadResult r = PinnedSlotReader::ReadResult::kTorn;
        for (int attempt = 0; attempt < 8; ++attempt) {
          r = s.frames.ReadFrame(idx, v, /*verify_crc=*/false);
          if (r != PinnedSlotReader::ReadResult::kTorn) break;
        }
        if (r == PinnedSlotReader::ReadResult::kOverrun) break;
        if (r == PinnedSlotReader::ReadResult::kNotPublished) continue;
        if (r == PinnedSlotReader::ReadResult::kTorn) {
          capture_torn_ += 1;
          continue;
        }
        // Index boundary (see the comment on h above): every frame with
        // idx < post_next belongs here, and the window filter uses
        // [t_begin, t_end] — frames committed between t0 and the post_next
        // snapshot (mono > t0) go to pre, later ones to post, with neither
        // dropped nor duplicated.
        if (v.mono_ns > t0 + after_ns_) continue;  // past the window's right edge
        if (v.mono_ns < t_floor) break;
        if (idx % trigger_decimate_[si] != 0) {
          continue;
        }
        Held held;
        held.d.source = static_cast<uint16_t>(si);
        held.d.pinned_slot = 1;
        held.d.struct_id = 0;
        held.d.index = idx;
        held.d.mono_ns = v.mono_ns;
        held.d.len = v.data_len;
        held.d.slot_seq = static_cast<uint32_t>(v.slot_seq);
        held.flags = v.flags;
        rev.push_back(held);
      }
      uint64_t start_mono = t_begin;
      if (slack > 0) {
        for (const Held& held : rev) {
          if (held.d.mono_ns > t_begin) continue;
          if ((held.flags & kH264FlagIsIdr) != 0) {
            start_mono = held.d.mono_ns;
            break;  // newest-first: first hit is the latest IDR
          }
        }
      }
      for (auto it = rev.rbegin(); it != rev.rend(); ++it) {
        if (it->d.mono_ns < start_mono) continue;
        PushDescriptor(it->d.source, true, it->d.struct_id, it->d.index,
                       it->d.mono_ns, it->d.len, it->d.slot_seq);
      }
    }
  }
}

uint64_t TriggeredRecorder::CollectPost() {
  uint64_t collected = 0;
  const uint64_t t_end = t0_ + after_ns_;
  for (std::size_t si = 0; si < sources_.size(); ++si) {
    SourceState& s = *sources_[si];
    if (!s.attached) continue;
    if (!TriggerIncluded(static_cast<uint16_t>(si))) continue;
    if (s.cfg.kind == SourceConfig::Kind::kMirrorRing) {
      const RecordMirrorRingPage* page = s.mirror.Page();
      for (uint32_t sid = 0; sid < s.struct_count; ++sid) {
        uint32_t decimate = trigger_decimate_[si];
        if (!SelectRecordStruct(trigger_rules_, s.cfg,
                                static_cast<uint16_t>(sid), &decimate)) {
          continue;
        }
        const uint64_t head = page->RingHead(static_cast<uint16_t>(sid));
        uint64_t& next = s.post_next[sid];
        while (next < head) {
          RecordMirrorRingSampleDesc d;
          if (!page->PeekRingSample(static_cast<uint16_t>(sid), next, d,
                                    &capture_torn_)) {
            const uint64_t h2 = page->RingHead(static_cast<uint16_t>(sid));
            if (h2 - next > page->RingDepth(static_cast<uint16_t>(sid))) {
              // Evicted before we could even queue it: a permanent hole.
              stats_.overrun_before_queue += 1;
              ++next;
              continue;
            }
            break;  // torn / in-flight tap: retry next tick
          }
          if (d.mono_ns > t_end) break;  // past the post window
          if (d.publish_index % decimate != 0) {
            ++next;
            continue;
          }
          PushDescriptor(static_cast<uint16_t>(si), false,
                         static_cast<uint16_t>(sid), d.publish_index,
                         d.mono_ns, d.payload_size, d.slot_seq);
          ++next;
          ++collected;
        }
      }
    } else {
      if (s.gop_post_done) continue;
      const uint64_t slack =
          (s.cfg.h264_stream && s.cfg.gop_ns > 0) ? s.cfg.gop_ns : 0;
      const uint64_t head = s.frames.head();
      uint64_t& next = s.post_next[0];
      while (next < head) {
        PinnedSlotReader::View v;
        const PinnedSlotReader::ReadResult r =
            s.frames.ReadFrame(next, v, /*verify_crc=*/false);
        if (r == PinnedSlotReader::ReadResult::kOk) {
          // Nominal post ends at t_end. h264 may keep the open GOP until
          // the next IDR, but not past t_end+gop_ns. That IDR starts the
          // following GOP and is left for a later window.
          if (v.mono_ns > t_end + slack) {
            if (slack > 0) s.gop_post_done = true;
            break;
          }
          if (slack > 0 && v.mono_ns > t_end &&
              (v.flags & kH264FlagIsIdr) != 0) {
            s.gop_post_done = true;
            break;
          }
          if (next % trigger_decimate_[si] != 0) {
            ++next;
            continue;
          }
          PushDescriptor(static_cast<uint16_t>(si), true, 0, next, v.mono_ns,
                         v.data_len, static_cast<uint32_t>(v.slot_seq));
          ++next;
          ++collected;
        } else if (r == PinnedSlotReader::ReadResult::kOverrun) {
          stats_.overrun_before_queue += 1;
          ++next;
        } else {
          // kTorn (mid-commit) or kNotPublished under a racing head: retry on
          // the next tick (FinishPost bounds the retry loop).
          break;
        }
      }
    }
  }
  return collected;
}

bool TriggeredRecorder::Trigger(uint64_t trigger_id, uint64_t before_ns,
                                uint64_t after_ns, uint64_t t0_mono_ns,
                                bool defer_settle, bool high_priority,
                                const std::vector<std::string>& channels) {
  if (phase_ != Phase::kArmed) {
    stats_.triggers_rejected += 1;
    return false;
  }
  if (disk_full_) {  // 6.5: host must run retention + ClearDiskFull first
    stats_.triggers_rejected += 1;
    return false;
  }
  if (!RefreshSessionLease()) {
    stats_.triggers_rejected += 1;
    return false;
  }
  // TRG-16: source and continuous rules share parsing and exclusion/decimate
  // resolution. Empty = all sources (backward compatible).
  trigger_rules_.clear();
  trigger_mask_.assign(sources_.size(), 0);
  trigger_decimate_.assign(sources_.size(), 1);
  uint32_t selected = 0;
  if (!channels.empty() &&
      !ParseChannelSelectors(channels, &trigger_rules_, nullptr)) {
    stats_.triggers_rejected += 1;
    return false;
  }
  for (std::size_t si = 0; si < sources_.size(); ++si) {
    const ChannelSelectorRule* rule =
        MatchChannelSelector(trigger_rules_, sources_[si]->cfg);
    if (rule != nullptr && sources_[si]->cfg.h264_stream &&
        rule->decimate > 1) {
      stats_.triggers_rejected += 1;
      return false;
    }
    if (channels.empty() || rule != nullptr) {
      trigger_mask_[si] = 1;
      trigger_decimate_[si] = rule == nullptr ? 1 : rule->decimate;
      ++selected;
    }
  }
  if (selected == 0) {
    stats_.triggers_rejected += 1;
    return false;
  }
  const uint64_t t0 = t0_mono_ns != 0 ? t0_mono_ns : MonoNs();
  trigger_id_ = trigger_id;
  t0_ = t0;
  before_ns_ = before_ns;
  after_ns_ = after_ns;
  capture_samples_ = 0;
  capture_frames_ = 0;
  capture_holes_ = 0;
  capture_evicted_ = 0;
  capture_torn_ = 0;
  capture_queued_ = 0;
  capture_flags_ = 0;
  defer_settle_ = defer_settle;
  flush_allowed_ = !defer_settle;  // realtime captures drain immediately
  high_priority_ = high_priority;
  // 6.3: rate shaping applies to idle-flushed (deferred) captures only.
  rate_limited_ = defer_settle && cfg_.io_rate_limit_bps > 0;
  bucket_.SetRate(rate_limited_ ? cfg_.io_rate_limit_bps : 0,
                  cfg_.io_burst_bytes);

  char leaf[64];
  std::snprintf(leaf, sizeof(leaf), "%llu_%llu",
                static_cast<unsigned long long>(trigger_id),
                static_cast<unsigned long long>(t0));
  // A crash-level package goes to the exclusive high/ subdir (6.4: low-priority
  // retention never deletes high/).
  pkg_dir_ = cfg_.out_dir + (high_priority_ ? "/high/" : "/") + leaf;
  capture_hashes_.Reset();
  if (!Mkdirs(pkg_dir_ + "/frames")) return false;
  last_package_dir_ = pkg_dir_;

  // meta.tbin: header + trigger entry + window record, kept open so Settle
  // can append the drain statistics record as the last entry.
  std::vector<ThymeStructDesc> registry;
  for (std::size_t si = 0; si < sources_.size(); ++si) {
    const auto& sp = sources_[si];
    if (sp->cfg.kind != SourceConfig::Kind::kMirrorRing || !sp->attached) {
      continue;
    }
    if (!TriggerIncluded(static_cast<uint16_t>(si))) continue;
    const RecordMirrorRingPage* page = sp->mirror.Page();
    for (uint32_t sid = 0; sid < sp->struct_count; ++sid) {
      if (!SelectRecordStruct(trigger_rules_, sp->cfg,
                              static_cast<uint16_t>(sid), nullptr)) {
        continue;
      }
      bool dup = false;
      for (const ThymeStructDesc& d : registry) {
        if (d.struct_id == page->struct_table[sid].struct_id) dup = true;
      }
      if (!dup) registry.push_back(page->struct_table[sid]);
    }
  }
  registry.push_back(
      detail::MakeDesc(kFrameIndexStructId, sizeof(FrameIndexRecord), "FrameIdx"));
  registry.push_back(
      detail::MakeDesc(kPayloadProfileStructId, sizeof(PayloadProfileRecord),
                       "PayProf"));
  registry.push_back(
      detail::MakeDesc(kMetaStructId, sizeof(TriggerMetaRecord), "TrigMeta"));
  meta_sink_ = std::make_unique<ThymeBinFileSink>(
      (pkg_dir_ + "/meta.tbin").c_str(), &capture_hashes_, "meta.tbin");
  if (!meta_sink_->ok()) return false;
  capture_registry_ = registry;
  meta_enc_ = std::make_unique<ThymeBinEncoder>(meta_sink_.get());
  if (!meta_enc_->WriteHeader(trigger_id, t0 - before_ns, registry.data(),
                              registry.size(), CaptureManifestBlock())) {
    return false;
  }
  if (!meta_enc_->WriteTrigger(trigger_id, t0, before_ns, after_ns)) {
    return false;
  }
  uint64_t first_hash = 0;
  for (std::size_t si = 0; si < sources_.size(); ++si) {
    const auto& sp = sources_[si];
    if (sp->cfg.kind == SourceConfig::Kind::kMirrorRing && sp->attached &&
        TriggerIncluded(static_cast<uint16_t>(si))) {
      first_hash = sp->pinned_hash;
      break;
    }
  }
  const TriggerMetaRecord meta{t0, before_ns, after_ns, first_hash};
  meta_rec_seq_ = 0;
  if (!meta_enc_->WriteData(meta_rec_seq_++, kMetaStructId, t0, 0, &meta,
                            sizeof(meta))) {
    return false;
  }
  CollectPayloadProfiles(false, &capture_profiles_);
  if (!WritePayloadProfileRows(meta_enc_.get(), t0, &meta_rec_seq_, false)) {
    return false;
  }

  // wal.log: opened at Trigger, sealed at Settle completion. A recorder crash
  // anywhere in between leaves it unsealed -> PackageComplete() == false.
  wal_ = FilePtr(std::fopen((pkg_dir_ + "/wal.log").c_str(), "wb"));
  if (!wal_) return false;
  capture_hashes_.Track("wal.log");
  const WalHeader wh{{'T', 'R', 'I', 'G', 'W', 'A', 'L', '1'}, trigger_id};
  if (detail::WritePackageBytes(wal_.get(), &wh, sizeof(wh), &capture_hashes_,
                                "wal.log") != sizeof(wh) ||
      !WriteClockAnchor(wal_.get(), &capture_hashes_, t0) ||
      !WriteLifecycleState(wal_.get(), &capture_hashes_,
                           PackageLifecycle::kArmed) ||
      !WriteLifecycleState(wal_.get(), &capture_hashes_,
                           PackageLifecycle::kCapturing)) {
    return false;
  }
  last_clock_anchor_ns_ = t0;
  CheckpointHashes(MonoNs(), true);

  // Swap cursors: pre harvest from the window, post continues from the heads.
  if (cfg_.test_hook_after_t0 != nullptr) {
    cfg_.test_hook_after_t0(cfg_.fault_ctx);  // test: publish into the pre/post handoff
  }
  // The post cursors on both sides are snapshotted before HarvestPre (a strict
  // index boundary: pre collects [.., post_next), post collects
  // [post_next, ..)) — samples/frames published between t0 and the snapshot,
  // or between the snapshot and the harvest, are collected by exactly one
  // side, never lost or duplicated.
  for (const auto& sp : sources_) sp->gop_post_done = false;
  for (std::size_t si = 0; si < sources_.size(); ++si) {
    const auto& sp = sources_[si];
    if (!sp->attached) continue;
    if (!TriggerIncluded(static_cast<uint16_t>(si))) continue;
    if (sp->cfg.kind == SourceConfig::Kind::kMirrorRing) {
      for (uint32_t sid = 0; sid < sp->struct_count; ++sid) {
        sp->post_next[sid] =
            sp->mirror.Page()->RingHead(static_cast<uint16_t>(sid));
      }
    } else {
      sp->post_next[0] = sp->frames.head();
    }
  }
  HarvestPre(t0);
  post_deadline_ = t0 + after_ns;
  phase_ = Phase::kPost;
  return true;
}

bool TriggeredRecorder::DescriptorLess(const SwapDescriptor& a,
                                       const SwapDescriptor& b) {
  if (a.mono_ns != b.mono_ns) return a.mono_ns < b.mono_ns;
  if (a.source != b.source) return a.source < b.source;
  if (a.pinned_slot != b.pinned_slot) return a.pinned_slot < b.pinned_slot;
  if (a.struct_id != b.struct_id) return a.struct_id < b.struct_id;
  return a.index < b.index;
}

void TriggeredRecorder::SortRingByMono(DescriptorRing& ring) {
  const uint32_t count = ring.size();
  if (count < 2) return;
  // Normalize the ring so the live range starts at slot 0, then sort it in
  // place. std::rotate is an index shuffle on the preallocated storage and
  // std::sort does not allocate, so REC-20's allocation-free drain path is
  // preserved; the cost is one O(n log n) pass over at most the ring capacity.
  if (ring.head != 0) {
    std::rotate(ring.slots.begin(),
                ring.slots.begin() + static_cast<std::ptrdiff_t>(ring.head),
                ring.slots.end());
    ring.head = 0;
  }
  std::sort(ring.slots.begin(),
            ring.slots.begin() + static_cast<std::ptrdiff_t>(count),
            DescriptorLess);
#ifndef NDEBUG
  // Invariant the drains rely on: the live range is (mono_ns, source, kind,
  // struct, index) non-decreasing. TODO-free debug check, O(n).
  for (uint32_t i = 1; i < count; ++i) {
    assert(!DescriptorLess(ring.slots[i], ring.slots[i - 1]));
  }
#endif
}

void TriggeredRecorder::SortQueueByMono() { SortRingByMono(queue_); }

void TriggeredRecorder::FinishPost() {
  SortQueueByMono();
  phase_ = Phase::kSettle;
}

bool TriggeredRecorder::GopPostOpen(uint64_t now) const {
  for (std::size_t si = 0; si < sources_.size(); ++si) {
    const SourceState& s = *sources_[si];
    if (!s.attached || !TriggerIncluded(static_cast<uint16_t>(si))) continue;
    if (!s.cfg.h264_stream || s.cfg.gop_ns == 0 || s.gop_post_done) continue;
    if (now >= t0_ + after_ns_ + s.cfg.gop_ns) continue;
    return true;
  }
  return false;
}

}  // namespace triggered
}  // namespace thyme
