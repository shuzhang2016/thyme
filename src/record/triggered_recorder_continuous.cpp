#include "triggered_recorder_internal.h"

#include "thyme/runtime/log.h"

#include <cassert>
#include <cstdio>

namespace thyme {
namespace triggered {

// --------------------------------------------------------------------------
// Continuous mode (design 6.6): same sources, same drain pipeline, infinite
// window, rolling sealed slices. Big-frame channels are excluded by default
// (except when whitelisted).
// --------------------------------------------------------------------------

void TriggeredRecorder::DropOldestContinuous() {
  if (!cont_queue_.empty()) {
    cont_pending_bytes_ -= cont_queue_.front().len;
    cont_queue_.pop_front();
  }
  cont_stats_.queue_evicted += 1;
  cont_slice_evicted_ += 1;
}

void TriggeredRecorder::PushContinuous(uint16_t source, bool pinned_slot,
                                       uint16_t struct_id, uint64_t index,
                                       uint64_t mono_ns, uint32_t len,
                                       uint32_t slot_seq) {
  // Shared quota: over the limit, evict the oldest entry of this bucket first
  // (continuous data is the most expendable; the trigger bucket is never
  // squeezed by continuous traffic).
  while (pending_bytes_ + cont_pending_bytes_ + len >
             cfg_.pending_bytes_quota &&
         !cont_queue_.empty()) {
    DropOldestContinuous();
  }
  if (pending_bytes_ + cont_pending_bytes_ + len > cfg_.pending_bytes_quota) {
    cont_stats_.queue_evicted += 1;  // single entry over quota: not enqueued
    cont_slice_evicted_ += 1;
    return;
  }
  // REC-20 descriptor-count bound: same oldest-first policy as the triggered
  // ring, counted in ContinuousStats::queue_evicted.
  if (cont_queue_.capacity() != 0 && cont_queue_.full()) {
    DropOldestContinuous();
  }
  SwapDescriptor d;
  d.source = source;
  d.pinned_slot = pinned_slot ? 1 : 0;
  d.struct_id = struct_id;
  d.index = index;
  d.mono_ns = mono_ns;
  d.len = len;
  d.slot_seq = slot_seq;
  if (!cont_queue_.push_back(d)) {
    cont_stats_.queue_evicted += 1;  // unsized ring: counted, not silent
    cont_slice_evicted_ += 1;
    return;
  }
  cont_pending_bytes_ += len;
  // REP-07 follow-up: this descriptor joined the pending window; the next
  // drain pass must re-order the window before it writes (struct-major
  // collection would otherwise reach small.tbin unchanged).
  cont_queue_dirty_ = true;
}

void TriggeredRecorder::SortContinuousQueue() {
  if (!cont_queue_dirty_) return;
  SortRingByMono(cont_queue_);
  cont_queue_dirty_ = false;
}

// static
bool TriggeredRecorder::ParseContinuousChannels(
    const std::vector<std::string>& entries,
    std::vector<ContinuousChannelRule>* out, std::string* err) {
  return ParseChannelSelectors(entries, out, err);
}

const ContinuousChannelRule* TriggeredRecorder::ContRule(
    uint16_t source_idx) const {
  return MatchChannelSelector(cont_rules_, sources_[source_idx]->cfg);
}

bool TriggeredRecorder::ContIncluded(uint16_t source_idx) const {
  return ContRule(source_idx) != nullptr;
}

bool TriggeredRecorder::ContHasH264() const {
  for (std::size_t i = 0; i < sources_.size(); ++i) {
    const SourceState& s = *sources_[i];
    if (s.attached && s.cfg.kind == SourceConfig::Kind::kPinnedSlot &&
        s.cfg.h264_stream && ContIncluded(static_cast<uint16_t>(i))) {
      return true;
    }
  }
  return false;
}


bool TriggeredRecorder::ArmContinuous(const ContinuousConfig& cfg) {
  if (cont_armed_) return false;
  cont_cfg_ = cfg;
  if (cont_cfg_.channels.empty()) cont_cfg_.channels = {"small_struct:*"};
  if (cont_cfg_.slice_bytes == 0) cont_cfg_.slice_bytes = 1;
  if (!ParseContinuousChannels(cont_cfg_.channels, &cont_rules_, nullptr)) {
    return false;
  }
  uint32_t selected = 0;
  for (const auto& source : sources_) {
    const ChannelSelectorRule* rule =
        MatchChannelSelector(cont_rules_, source->cfg);
    if (rule == nullptr) {
      continue;
    }
    if (source->cfg.h264_stream && rule->decimate > 1) {
      return false;
    }
    ++selected;
  }
  if (selected == 0) {
    return false;
  }
  cont_stats_ = ContinuousStats{};
  cont_seq_ = 0;
  cont_queue_.clear();
  cont_queue_dirty_ = false;
  cont_pending_bytes_ = 0;
  cont_bucket_.SetRate(cfg_.io_rate_limit_bps, cfg_.io_burst_bytes);
  cont_gate_open_ = !cont_cfg_.wait_idle;
  cont_roll_pending_ = false;
  // Cursors start at the current heads: no backfill of history. Big-frame
  // channels use streaming follow cursors (no descriptor quota use); mirror
  // sources use PeekRingSample cursors.
  for (std::size_t i = 0; i < sources_.size(); ++i) {
    SourceState& s = *sources_[i];
    if (!s.attached || !ContIncluded(static_cast<uint16_t>(i))) continue;
    if (s.cfg.kind == SourceConfig::Kind::kMirrorRing) {
      s.cont_next.assign(s.struct_count, 0);
      s.cont_last_mono.assign(s.struct_count, 0);
      for (uint32_t sid = 0; sid < s.struct_count; ++sid) {
        s.cont_next[sid] =
            s.mirror.Page()->RingHead(static_cast<uint16_t>(sid));
      }
    } else {
      s.frames.SeekToHead();
      s.cont_bf_drops = s.frames.drop_count();
    }
  }
  cont_armed_ = true;
  OpenSlice(MonoNs());
  return true;
}

void TriggeredRecorder::DisarmContinuous() {
  if (!cont_armed_) return;
  // Drain what is queued/available (bounded by the data itself), then seal.
  StreamPinnedSlots(MonoNs());
  while (!cont_queue_.empty()) DrainContinuous(MonoNs());
  CloseSlice(MonoNs(), cont_last_frame_mono_);
  cont_armed_ = false;
}

void TriggeredRecorder::OpenSlice(uint64_t now) {
  char leaf[96];
  std::snprintf(leaf, sizeof(leaf), "continuous_%llu_%llu",
                static_cast<unsigned long long>(cont_seq_),
                static_cast<unsigned long long>(now));
  cont_dir_ = cfg_.out_dir + "/" + leaf;
  cont_hashes_.Reset();
  cont_slice_begin_ns_ = now;
  cont_slice_bytes_ = 0;
  cont_slice_descriptors_ = 0;
  cont_slice_evicted_ = 0;
  // New file, new order: the watermark only guards one small.tbin's
  // monotonicity (rows of the previous slice live in another package).
  cont_small_last_mono_ = 0;
  if (!Mkdirs(cont_dir_ + "/frames")) {
    disk_full_ = true;  // 6.5: continuous capture obeys ENOSPC semantics too
    return;
  }
  // meta.tbin: header + registry (+ ContinuousMetaRecord at slice close).
  std::vector<ThymeStructDesc> registry;
  for (std::size_t i = 0; i < sources_.size(); ++i) {
    SourceState& s = *sources_[i];
    if (!s.attached || !ContIncluded(static_cast<uint16_t>(i))) continue;
    if (s.cfg.kind != SourceConfig::Kind::kMirrorRing) continue;
    const RecordMirrorRingPage* page = s.mirror.Page();
    for (uint32_t sid = 0; sid < s.struct_count; ++sid) {
      if (!SelectRecordStruct(cont_rules_, s.cfg, static_cast<uint16_t>(sid),
                              nullptr)) {
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
      detail::MakeDesc(kMetaStructId, sizeof(ContinuousMetaRecord), "ContMeta"));
  cont_registry_ = registry;
  cont_meta_sink_ = std::make_unique<ThymeBinFileSink>(
      (cont_dir_ + "/meta.tbin").c_str(), &cont_hashes_, "meta.tbin");
  cont_meta_enc_ = std::make_unique<ThymeBinEncoder>(cont_meta_sink_.get());
  if (!cont_meta_sink_->ok() ||
      !cont_meta_enc_->WriteHeader(kContinuousWalMark | cont_seq_, now,
                                   registry.data(), registry.size(),
                                   CaptureManifestBlock())) {
    disk_full_ = true;
    return;
  }
  CollectPayloadProfiles(true, &cont_profiles_);
  uint64_t profile_seq = 4;
  if (!WritePayloadProfileRows(cont_meta_enc_.get(), now, &profile_seq, true)) {
    disk_full_ = true;
    return;
  }
  cont_wal_ = FilePtr(std::fopen((cont_dir_ + "/wal.log").c_str(), "wb"));
  if (!cont_wal_) {
    disk_full_ = true;
    return;
  }
  cont_hashes_.Track("wal.log");
  const WalHeader wh{{'T', 'R', 'I', 'G', 'W', 'A', 'L', '1'},
                     kContinuousWalMark | cont_seq_};
  if (detail::WritePackageBytes(cont_wal_.get(), &wh, sizeof(wh), &cont_hashes_,
                                "wal.log") != sizeof(wh) ||
      !WriteClockAnchor(cont_wal_.get(), &cont_hashes_, now) ||
      !WriteLifecycleState(cont_wal_.get(), &cont_hashes_,
                           PackageLifecycle::kArmed) ||
      !WriteLifecycleState(cont_wal_.get(), &cont_hashes_,
                           PackageLifecycle::kCapturing)) {
    disk_full_ = true;
  }
  last_clock_anchor_ns_ = now;
  CheckpointHashes(now, true);
  cont_small_open_ = false;
  for (ContFrameOut& fo : cont_frame_outs_) fo = ContFrameOut{};
}

bool TriggeredRecorder::CloseSlice(uint64_t now, uint64_t cut_detail) {
  if (cont_dir_.empty()) return false;
  // Same durability rule as triggered packages: fsync every file before the
  // wal seal row; any failure actively invalidates the slice (abort row +
  // .incomplete rename), never a self-sealed broken package.
  bool sealed_ok =
      WriteLifecycleState(cont_wal_.get(), &cont_hashes_, PackageLifecycle::kSealing);
  if (cfg_.test_hook_after_sealing != nullptr) {
    cfg_.test_hook_after_sealing(cfg_.fault_ctx);
  }
  const int seal_fault = cfg_.fault_inject_seal != nullptr
                             ? cfg_.fault_inject_seal(cfg_.fault_ctx)
                             : 0;
  if (cont_meta_enc_ != nullptr) {
    // detail: the GOP-aligned cut point (mono_ns of the IDR frame); 0 for
    // other rolls.
    const ContinuousMetaRecord cm{cont_seq_, cont_slice_begin_ns_, now,
                                  cont_stats_.samples_written,
                                  cont_stats_.frames_written,
                                  cont_stats_.holes_written,
                                  cont_slice_evicted_, cut_detail};
    // Note: counters in the meta record are cumulative; the slice-local view
    // is derivable from consecutive slices. Write with source_sequence = 2.
    if (!cont_meta_enc_->WriteData(2, kMetaStructId, now, 2, &cm,
                                   sizeof(cm))) {
      sealed_ok = false;
    }
    const CaptureStatsRecord csr{cont_slice_descriptors_,
                                 cont_stats_.samples_written,
                                 cont_stats_.frames_written,
                                 cont_stats_.holes_written,
                                 cont_slice_evicted_,
                                 cont_stats_.torn_reads, 0};
    if (!cont_meta_enc_->WriteData(3, kMetaStructId, now, 1, &csr,
                                   sizeof(csr))) {
      sealed_ok = false;
    }
    if (!cont_meta_enc_->Finish()) sealed_ok = false;
    if (!cont_meta_sink_->Sync()) sealed_ok = false;
  }
  cont_meta_sink_.reset();
  cont_meta_enc_.reset();
  if (cont_small_enc_ != nullptr && !cont_small_enc_->Finish()) {
    sealed_ok = false;
  }
  if (cont_small_sink_ != nullptr && !cont_small_sink_->Sync()) {
    sealed_ok = false;
  }
  cont_small_sink_.reset();
  cont_small_enc_.reset();
  cont_small_open_ = false;
  for (ContFrameOut& fo : cont_frame_outs_) {
    if (fo.raw) {
      if (!detail::SyncFile(fo.raw.get())) sealed_ok = false;
      fo.raw.reset();
    }
    if (fo.idx_enc != nullptr && !fo.idx_enc->Finish()) sealed_ok = false;
    if (fo.idx_sink != nullptr && !fo.idx_sink->Sync()) sealed_ok = false;
    fo = ContFrameOut{};
  }
  if (cont_wal_ && !WriteClockAnchor(cont_wal_.get(), &cont_hashes_, MonoNs())) {
    sealed_ok = false;
  }
  bool terminal_rows_written = false;
  WalRow terminal_rows[2]{
      {kWalOpLifecycle, 0, static_cast<uint16_t>(PackageLifecycle::kSealed), 0,
       detail::RealTimeNs(), MonoNs()},
      {kWalOpSeal, 0, 0, 0, 0, MonoNs()}};
  if (cont_wal_) {
    if (seal_fault != 1) {
      const std::size_t written =
          std::fwrite(terminal_rows, 1, sizeof(terminal_rows), cont_wal_.get());
      terminal_rows_written = written == sizeof(terminal_rows);
      if (!terminal_rows_written && written != 0) {
        cont_hashes_.Append("wal.log", terminal_rows, written);
      }
    }
    if (!terminal_rows_written) sealed_ok = false;
    if (seal_fault == 2 || !detail::SyncFile(cont_wal_.get())) sealed_ok = false;
  }
  // ABI-09: same sidecar rule as triggered packages (fault 4 = write fails).
  if (sealed_ok &&
      (seal_fault == 4 ||
       !WriteSealedSchema(cont_dir_, cont_registry_, &cont_hashes_))) {
    sealed_ok = false;
  }
  // SEC-03: same manifest rule as triggered packages. wal.log is hashed with
  // both terminal rows; the file stays open until the manifest is durable.
  if (sealed_ok) {
    CheckpointHashes(now, true);
    integrity::PackageInfo pinfo = SlicePackageInfo(now);
    pinfo.seal_time_ns = detail::RealTimeNs();
    std::string ierr;
    if (!integrity::WritePackageManifestFromHashes(
            cont_dir_, pinfo, cfg_.signing_key_path, cont_hashes_, &ierr,
            "wal.log", terminal_rows,
            terminal_rows_written ? sizeof(terminal_rows) : std::size_t{0})) {
      THYME_LOG_ERROR("triggered_recorder: integrity manifest for %s failed: "
                      "%s",
                      cont_dir_.c_str(), ierr.c_str());
      sealed_ok = false;
    }
  }
  if (sealed_ok) {
    cont_wal_.reset();
    cont_stats_.slices_done += 1;
    if (!detail::SyncDir(cont_dir_)) ++stats_.sync_errors;  // persist the dir entry
  } else {
    // Same invalidation semantics as a triggered package; continuous mode also
    // stops (re-arm after host retention).
    InvalidatePackage(&cont_wal_, terminal_rows_written, terminal_rows, 2,
                      PackageAbortReason::kSealFailure, cont_dir_,
                      SlicePackageInfo(now), cont_registry_, &cont_hashes_);
    stats_.disk_errors += 1;
    disk_full_ = true;
    cont_armed_ = false;
    cont_dir_.clear();
  }
  return sealed_ok;
}

void TriggeredRecorder::RollSlice(uint64_t now, uint64_t cut_detail) {
  // If CloseSlice fails, stop rolling (the invalidation already disarmed
  // continuous capture) and leave no orphan directory.
  if (!CloseSlice(now, cut_detail)) return;
  ++cont_seq_;
  OpenSlice(now);
  cont_roll_pending_ = false;
}

void TriggeredRecorder::TickContinuous(uint64_t now) {
  if (disk_full_) return;  // 6.5: continue only after retention + ClearDiskFull
  if (!RefreshSessionLease()) return;
  CollectContinuous();
  if (cont_gate_open_ || !cont_cfg_.wait_idle) {
    // Stream big frames first (no descriptor quota use), then drain the
    // small-struct queue.
    StreamPinnedSlots(now);
    if (!cont_queue_.empty()) DrainContinuous(now);
  }
  // Time-based roll (slice_s): when the limit is reached, set roll_pending; an
  // h264 channel waits for the next IDR to cut (see
  // DrainContinuous/StreamPinnedSlots), otherwise the cut is immediate.
  if (cont_cfg_.slice_ns > 0 &&
      now - cont_slice_begin_ns_ >= cont_cfg_.slice_ns &&
      !cont_roll_pending_) {
    cont_roll_pending_ = true;
    cont_roll_since_ns_ = now;
  }
  if (cont_roll_pending_ && !ContHasH264() && cont_queue_.empty()) {
    RollSlice(now, 0);  // no h264: cut at the boundary immediately
  }
}

void TriggeredRecorder::CollectContinuous() {
  for (std::size_t si = 0; si < sources_.size(); ++si) {
    SourceState& s = *sources_[si];
    const ContinuousChannelRule* rule = ContRule(static_cast<uint16_t>(si));
    if (!s.attached || rule == nullptr) {
      continue;
    }
    if (s.cfg.kind == SourceConfig::Kind::kMirrorRing) {
      const RecordMirrorRingPage* page = s.mirror.Page();
      for (uint32_t sid = 0; sid < s.struct_count; ++sid) {
        uint32_t decimate = rule->decimate;
        if (!SelectRecordStruct(cont_rules_, s.cfg, static_cast<uint16_t>(sid),
                                &decimate)) {
          continue;
        }
        const uint64_t head = page->RingHead(static_cast<uint16_t>(sid));
        uint64_t& next = s.cont_next[sid];
        while (next < head) {
          if (next % decimate != 0) {
            ++next;
            continue;
          }
          RecordMirrorRingSampleDesc d;
          if (!page->PeekRingSample(static_cast<uint16_t>(sid), next, d,
                                    &cont_stats_.torn_reads)) {
            const uint64_t h2 = page->RingHead(static_cast<uint16_t>(sid));
            if (h2 - next > page->RingDepth(static_cast<uint16_t>(sid))) {
              // Evicted in continuous mode: a hole under an infinite window,
              // written as a GAP by the drain side. PushContinuous is used
              // there so the fixed ring's full-ring eviction policy is shared.
              // The evicted sample's own mono_ns is gone; stamp the hole with
              // this struct's last collected mono (0 before the first sample)
              // so the GAP row stays inside the slice's mono order.
              const uint64_t hole_mono = s.cont_last_mono[sid];
              PushContinuous(static_cast<uint16_t>(si), false,
                             static_cast<uint16_t>(sid), next, hole_mono, 0, 0);
              ++next;
              continue;
            }
            break;  // torn / in-flight: retry next tick
          }
          PushContinuous(static_cast<uint16_t>(si), false,
                         static_cast<uint16_t>(sid), d.publish_index,
                         d.mono_ns, d.payload_size, d.slot_seq);
          s.cont_last_mono[sid] = d.mono_ns;
          ++next;
        }
      }
    }
    // Whitelisted big-frame channels do not enter the descriptor queue (full-
    // rate big frames would blow the quota); StreamPinnedSlots drains them in
    // real time with a follow cursor.
  }
}

// Streaming big-frame drain: every Tick reads new frames with a TryRead follow
// cursor and writes them straight to disk (through the rate bucket); lagging
// gap-skips are counted in pinned_slot_drops; a GOP-aligned roll waits for the
// next IDR.
void TriggeredRecorder::StreamPinnedSlots(uint64_t now) {
  const uint64_t tick_start = MonoNs();  // T2 time slice
  for (std::size_t si = 0; si < sources_.size(); ++si) {
    SourceState& s = *sources_[si];
    if (!s.attached || s.cfg.kind != SourceConfig::Kind::kPinnedSlot) continue;
    const ContinuousChannelRule* rule = ContRule(static_cast<uint16_t>(si));
    if (rule == nullptr) continue;
    PinnedSlotReader::View v;
    while (s.frames.TryRead(v)) {
      if (MonoNs() - tick_start >= cfg_.drain_tick_budget_ns) return;
      if (v.frame_index % rule->decimate != 0) continue;  // decimated
      if (cont_roll_pending_) {
        const bool gop_cut = !s.cfg.h264_stream ||
                             (v.flags & kH264FlagIsIdr) != 0;
        if (gop_cut) {
          // h264: the new slice starts at that IDR; point cloud: cut at the
          // frame boundary.
          RollSlice(now, s.cfg.h264_stream ? v.mono_ns : 0);
        } else if (now - cont_roll_since_ns_ > cfg_.gop_roll_timeout_ns) {
          RollSlice(now, 0);  // forced cut as a safety net (counted in roll_forced)
          cont_stats_.roll_forced += 1;
        }
      }
      if (!cont_bucket_.TryConsume(v.data_len + 32, now)) return;  // rate-limited: pause
      ContFrameOut& fo = cont_frame_outs_[si];
      if (!fo.open) {
        const std::string leaf = s.cfg.channel_name.empty() ? "channel" : s.cfg.channel_name;
        const std::string base = cont_dir_ + "/frames/" + leaf;
        fo.raw_path = "frames/" + leaf + ".raw";
        fo.raw = FilePtr(std::fopen((base + ".raw").c_str(), "wb"));
        if (fo.raw) cont_hashes_.Track(fo.raw_path, HashRawPayload(s.cfg));
        fo.idx_sink = std::make_unique<ThymeBinFileSink>(
            (base + ".index.tbin").c_str(), &cont_hashes_, "frames/" + leaf + ".index.tbin");
        fo.idx_enc = std::make_unique<ThymeBinEncoder>(fo.idx_sink.get());
        ThymeStructDesc idx_desc =
            detail::MakeDesc(kFrameIndexStructId, sizeof(FrameIndexRecord), "FrameIdx");
        ThymeStructDesc gap_desc = detail::MakeDesc(kFrameGapStructId, 0, "FrameGap");
        const ThymeStructDesc descs[2] = {idx_desc, gap_desc};
        fo.open =
            fo.raw != nullptr && fo.idx_sink->ok() &&
            fo.idx_enc->WriteHeader(kContinuousWalMark | cont_seq_, cont_slice_begin_ns_, descs, 2);
        fo.rec_seq = 0;
        fo.raw_off = 0;
        if (fo.open) {
          // REC-26: same file-level header as the triggered path; index
          // offsets become absolute including it.
          const detail::RawFileHeader rh = detail::MakeRawFileHeader(s.cfg);
          if (detail::WritePackageBytes(fo.raw.get(), &rh, sizeof(rh),
                                        &cont_hashes_, fo.raw_path) != sizeof(rh)) {
            fo.open = false;
          } else {
            fo.raw_off = sizeof(rh);
          }
        }
        if (!fo.open) {
          stats_.disk_errors += 1;
          disk_full_ = true;
          cont_armed_ = false;
          return;
        }
      }
      // Lease path — the pin covers the whole write sequence: fault check /
      // fwrite / CRC / index row all inside the lease scope (the same pattern
      // as DrainOne), so the writer can never overwrite the pinned slot and
      // .raw cannot contain a torn frame. A failed acquire -> GAP index row.
      // Only crc=frame channels verify the transport CRC at drain; package/off
      // skip it (the package manifest is the safety net), and off records an
      // index payload_crc of 0.
      const SourceConfig::CrcPolicy crc_policy = EffectiveCrcPolicy(s.cfg);
      {
        PinnedSlotReader::ReadResult rr = PinnedSlotReader::ReadResult::kTorn;
        PinnedSlotReader::Lease lease =
            s.frames.Acquire(v.frame_index, &rr,
                             crc_policy == SourceConfig::CrcPolicy::kFrame);
        if (!lease.valid()) {
          cont_stats_.torn_reads += 1;
          if (!fo.idx_enc->WriteGap(kFrameGapStructId, v.mono_ns,
                                    kGapReasonOverrun, 1)) {
            stats_.disk_errors += 1;
            disk_full_ = true;
            cont_armed_ = false;
            return;
          }
          continue;
        }
        const PinnedSlotReader::View& lv = lease.view();
        const bool faulted = cfg_.fault_inject_write != nullptr &&
                             cfg_.fault_inject_write(cfg_.fault_ctx);
        const std::size_t raw_written =
            faulted ? 0
                    : detail::WritePackageBytes(fo.raw.get(), lv.payload, lv.data_len,
                                                &cont_hashes_, fo.raw_path);
        if (raw_written != lv.data_len) {
          stats_.disk_errors += 1;
          disk_full_ = true;
          cont_armed_ = false;
          return;
        }
        // payload_crc covers the bytes as written; the lease pinned the slot,
        // so the view equals what the producer committed. crc=off records 0
        // (no per-frame anchor; REC-21); a disabled transport CRC (package)
        // is recomputed here so `verify` can re-check on demand.
        const uint32_t payload_crc =
            crc_policy == SourceConfig::CrcPolicy::kOff
                ? 0
                : (lv.crc32 != 0 ? lv.crc32 : Crc32(lv.payload, lv.data_len));
        FrameIndexRecord idx{};
        idx.offset = fo.raw_off;
        idx.len = lv.data_len;
        idx.flags = lv.flags;
        idx.payload_crc = payload_crc;
        idx.producer_frame_index = lv.frame_index;
        if (!fo.idx_enc->WriteData(fo.rec_seq++, kFrameIndexStructId,
                                   lv.mono_ns,
                                   static_cast<uint32_t>(lv.frame_index),
                                   &idx, sizeof(idx))) {
          stats_.disk_errors += 1;
          disk_full_ = true;
          cont_armed_ = false;
          return;
        }
        fo.raw_off += lv.data_len;
        cont_stats_.frames_written += 1;
      }
      cont_slice_bytes_ += v.data_len + 32;
      cont_stats_.bytes_written += v.data_len + 32;
      cont_slice_descriptors_ += 1;
      cont_last_frame_mono_ = v.mono_ns;
      if (!cont_roll_pending_ &&
          cont_slice_bytes_ >= cont_cfg_.slice_bytes) {
        cont_roll_pending_ = true;  // byte limit reached: h264 waits for the next IDR
        cont_roll_since_ns_ = now;
      }
    }
    // Lag settlement: book the gap-skip difference (overwrites while the
    // wait_idle gate is held go through this path too).
    const uint64_t drops = s.frames.drop_count();
    if (drops > s.cont_bf_drops) {
      cont_stats_.pinned_slot_drops += drops - s.cont_bf_drops;
      s.cont_bf_drops = drops;
    }
  }
}

uint64_t TriggeredRecorder::DrainContOne(const SwapDescriptor& d,
                                         bool& fatal) {
  fatal = false;
  const bool faulted = cfg_.fault_inject_write != nullptr &&
                       cfg_.fault_inject_write(cfg_.fault_ctx);
  const auto write_wal = [this](const WalRow& row) {
    return detail::WritePackageBytes(cont_wal_.get(), &row, sizeof(row),
                                     &cont_hashes_, "wal.log") == sizeof(row);
  };
  SourceState& s = *sources_[d.source];
  if (!d.pinned_slot) {
    if (!cont_small_open_) {
      cont_small_sink_ = std::make_unique<ThymeBinFileSink>(
          (cont_dir_ + "/small.tbin").c_str(), &cont_hashes_, "small.tbin");
      cont_small_enc_ =
          std::make_unique<ThymeBinEncoder>(cont_small_sink_.get());
      // Registry mirrors the slice meta.tbin's mirror section.
      std::vector<ThymeStructDesc> registry;
      for (std::size_t i = 0; i < sources_.size(); ++i) {
        SourceState& os = *sources_[i];
        if (!os.attached || !ContIncluded(static_cast<uint16_t>(i))) continue;
        if (os.cfg.kind != SourceConfig::Kind::kMirrorRing) continue;
        const RecordMirrorRingPage* page = os.mirror.Page();
        for (uint32_t sid = 0; sid < os.struct_count; ++sid) {
          if (!SelectRecordStruct(cont_rules_, os.cfg,
                                  static_cast<uint16_t>(sid), nullptr)) {
            continue;
          }
          bool dup = false;
          for (const ThymeStructDesc& e : registry) {
            if (e.struct_id == page->struct_table[sid].struct_id) dup = true;
          }
          if (!dup) registry.push_back(page->struct_table[sid]);
        }
      }
      cont_small_open_ =
          cont_small_sink_->ok() &&
          cont_small_enc_->WriteHeader(kContinuousWalMark | cont_seq_,
                                       cont_slice_begin_ns_, registry.data(),
                                       registry.size(),
                                       CaptureManifestBlock());
      cont_small_rec_seq_ = 0;
      if (!cont_small_open_) {
        fatal = true;
        return 0;
      }
    }
    // REP-07 follow-up: strict per-tick monotonic cut. A descriptor can still
    // fall behind the watermark when a producer stamps a sample, is preempted
    // before publishing it, and a later-mono sample of another struct is
    // drained first (the tick-boundary case a per-window sort cannot see).
    // Appending it would make small.tbin regress — breaking ReplayPackage's
    // per-source order (and the footer index check at Open) — so it is
    // demoted to a GAP at the watermark and counted: the loss is visible in
    // the timeline, never a silent contract violation.
    const bool late = d.mono_ns < cont_small_last_mono_;
    const uint64_t write_mono = late ? cont_small_last_mono_ : d.mono_ns;
    if (faulted && d.len != 0) {  // overrun hole marker (or injected fault)
      fatal = true;  // injected write failure on a real sample
      return 0;
    }
    if (d.len == 0) {  // overrun hole marker
      if (!cont_small_enc_->WriteGap(d.struct_id, write_mono,
                                     kGapReasonOverrun, 1)) {
        fatal = true;
        return 0;
      }
      cont_small_last_mono_ = write_mono;
      cont_stats_.holes_written += 1;
      const WalRow row{kWalOpDescriptor, kWalHole, d.struct_id, d.len,
                       d.index, write_mono};
      if (!write_wal(row)) fatal = true;
      return 32;
    }
    if (late) {
      cont_stats_.out_of_order += 1;
      if (!cont_small_enc_->WriteGap(d.struct_id, write_mono,
                                     kGapReasonOverrun, 1)) {
        fatal = true;
        return 0;
      }
      cont_stats_.holes_written += 1;
      const WalRow row{kWalOpDescriptor, kWalHole, d.struct_id, d.len,
                       d.index, write_mono};
      if (!write_wal(row)) fatal = true;
      return 32;
    }
    if (scratch_.size() < d.len) scratch_.resize(d.len);
    uint32_t sz = 0;
    uint64_t mono = 0;
    uint32_t sseq = 0;
    uint32_t stable = 0;
    const bool ok =
        s.attached &&
        s.mirror.Page()->ReadRingSample(d.struct_id, d.index, scratch_.data(),
                                        d.len, sz, mono, sseq, &stable,
                                        &cont_stats_.torn_reads) &&
        stable == d.slot_seq && mono == d.mono_ns && sz == d.len;
    if (ok) {
      assert(mono >= cont_small_last_mono_);  // ordering invariant
      if (!cont_small_enc_->WriteData(cont_small_rec_seq_++, d.struct_id,
                                      mono, sseq, scratch_.data(), sz)) {
        fatal = true;
        return 0;
      }
      cont_small_last_mono_ = mono;
      cont_stats_.samples_written += 1;
      const WalRow row{kWalOpDescriptor, kWalOk, d.struct_id, d.len, d.index,
                       d.mono_ns};
      if (!write_wal(row)) fatal = true;
      return sz + 32;
    }
    if (!cont_small_enc_->WriteGap(d.struct_id, write_mono, kGapReasonOverrun,
                                   1)) {
      fatal = true;
      return 0;
    }
    cont_small_last_mono_ = write_mono;
    cont_stats_.holes_written += 1;
    const WalRow row{kWalOpDescriptor, kWalHole, d.struct_id, d.len, d.index,
                     write_mono};
    if (!write_wal(row)) fatal = true;
    return 32;
  }

  // Big frames no longer enter the descriptor queue (StreamPinnedSlots drains
  // them in streaming mode), so this cannot happen.
  fatal = true;
  return 0;
}

void TriggeredRecorder::DrainContinuous(uint64_t now) {
  // REP-07 follow-up: order the whole pending window (this tick's collected
  // descriptors plus any bounded backlog) before writing anything. Every
  // drain pass starts here, so small.tbin is written in non-decreasing
  // mono_ns; the window is the fixed descriptor ring, never a slice copy.
  SortContinuousQueue();
  const uint64_t tick_start = MonoNs();  // T2 time slice
  uint32_t n = 0;
  while (!cont_queue_.empty() && n < cfg_.settle_batch &&
         MonoNs() - tick_start < cfg_.drain_tick_budget_ns) {
    // Continuous realtime rate limit (6.3/6.6): always through the token
    // bucket (rate 0 = unlimited).
    if (!cont_bucket_.TryConsume(cont_queue_.front().len + 32, now)) break;
    const SwapDescriptor d = cont_queue_.front();
    cont_queue_.pop_front();
    cont_pending_bytes_ -= d.len;
    bool fatal = false;
    const uint64_t written = DrainContOne(d, fatal);
    if (fatal) {
      // 6.5: actively invalidate the current slice (abort row + rename) and
      // wait for host retention + ClearDiskFull before re-arming.
      cont_queue_.clear();
      cont_queue_dirty_ = false;
      cont_pending_bytes_ = 0;
      cont_meta_sink_.reset();
      cont_meta_enc_.reset();
      cont_small_sink_.reset();
      cont_small_enc_.reset();
      cont_small_open_ = false;
      for (ContFrameOut& fo : cont_frame_outs_) {
        fo = ContFrameOut{};  // drops any open .raw handle
      }
      InvalidatePackage(&cont_wal_, /*terminal_rows_written=*/false, nullptr, 0,
                        PackageAbortReason::kWriteFailure, cont_dir_,
                        SlicePackageInfo(MonoNs()), cont_registry_,
                        &cont_hashes_);
      cont_dir_.clear();
      stats_.disk_errors += 1;
      disk_full_ = true;
      cont_armed_ = false;  // stop continuous capture; re-arm after the host handles it
      return;
    }
    cont_slice_bytes_ += written;
    cont_stats_.bytes_written += written;
    cont_slice_descriptors_ += 1;
    ++n;
    if (cont_slice_bytes_ >= cont_cfg_.slice_bytes && !cont_roll_pending_) {
      // Byte limit reached: an h264 channel waits for the next IDR (cut inside
      // StreamPinnedSlots), otherwise cut immediately.
      if (ContHasH264()) {
        cont_roll_pending_ = true;
        cont_roll_since_ns_ = now;
      } else {
        RollSlice(now, 0);
        if (disk_full_) return;
      }
    }
  }
}

}  // namespace triggered
}  // namespace thyme
