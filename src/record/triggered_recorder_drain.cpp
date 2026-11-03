#include "triggered_recorder_internal.h"

#include "thyme/runtime/log.h"

#include <cstdio>

namespace thyme {
namespace triggered {

bool TriggeredRecorder::WriteWalRow(uint8_t result, const SwapDescriptor& d) {
  const WalRow row{kWalOpDescriptor, result, d.struct_id, d.len, d.index,
                   d.mono_ns};
  return detail::WritePackageBytes(wal_.get(), &row, sizeof(row),
                                   &capture_hashes_, "wal.log") == sizeof(row);
}

uint64_t TriggeredRecorder::DrainOne(const SwapDescriptor& d, bool& fatal) {
  fatal = false;
  const bool faulted = cfg_.fault_inject_write != nullptr &&
                       cfg_.fault_inject_write(cfg_.fault_ctx);
  SourceState& s = *sources_[d.source];
  if (!d.pinned_slot) {
    if (!small_open_) {
      // small.tbin registry: deduped struct descs from all mirror pages.
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
          for (const ThymeStructDesc& e : registry) {
            if (e.struct_id == page->struct_table[sid].struct_id) dup = true;
          }
          if (!dup) registry.push_back(page->struct_table[sid]);
        }
      }
      small_sink_ = std::make_unique<ThymeBinFileSink>(
          (pkg_dir_ + "/small.tbin").c_str(), &capture_hashes_, "small.tbin");
      small_enc_ = std::make_unique<ThymeBinEncoder>(small_sink_.get());
      small_open_ = small_sink_->ok() &&
                    small_enc_->WriteHeader(trigger_id_, t0_ - before_ns_,
                                            registry.data(), registry.size(),
                                            CaptureManifestBlock());
      small_rec_seq_ = 0;
      if (!small_open_) {
        fatal = true;  // cannot even open the package file (ENOSPC/EROFS)
        return 0;
      }
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
                                        &capture_torn_) &&
        stable == d.slot_seq && mono == d.mono_ns && sz == d.len;
    if (ok && !faulted) {
      if (!small_enc_->WriteData(small_rec_seq_++, d.struct_id, mono, sseq,
                                 scratch_.data(), sz)) {
        fatal = true;
        return 0;
      }
      capture_samples_ += 1;
      stats_.samples_written += 1;
      if (!WriteWalRow(kWalOk, d)) fatal = true;
      return sz + 32;  // payload + record overhead approximation
    }
    // Readback hole (overwritten) or injected write fault on the data record:
    // a faulted data write must not become a gap — that would rewrite the
    // failure as if data were lost. Holes only for genuine overruns.
    if (ok && faulted) {
      fatal = true;
      return 0;
    }
    if (!small_enc_->WriteGap(d.struct_id, d.mono_ns, kGapReasonOverrun, 1)) {
      fatal = true;
      return 0;
    }
    capture_holes_ += 1;
    stats_.holes_written += 1;
    if (!WriteWalRow(kWalHole, d)) fatal = true;
    return 32;
  }

  // Big-frame descriptor: re-validate slot_seq (+ transport crc for
  // crc=frame channels, REC-21), then raw segment + index record into
  // frames/<channel>.{raw,index.tbin}.
  FrameOut& fo = frame_outs_[d.source];
  if (!fo.open) {
    const std::string leaf = s.cfg.channel_name.empty() ? "channel" : s.cfg.channel_name;
    const std::string base = pkg_dir_ + "/frames/" + leaf;
    fo.raw_path = "frames/" + leaf + ".raw";
    fo.raw = FilePtr(std::fopen((base + ".raw").c_str(), "wb"));
    if (fo.raw) capture_hashes_.Track(fo.raw_path, HashRawPayload(s.cfg));
    fo.idx_sink = std::make_unique<ThymeBinFileSink>(
        (base + ".index.tbin").c_str(), &capture_hashes_, "frames/" + leaf + ".index.tbin");
    fo.idx_enc = std::make_unique<ThymeBinEncoder>(fo.idx_sink.get());
    ThymeStructDesc idx_desc = detail::MakeDesc(kFrameIndexStructId, sizeof(FrameIndexRecord), "FrameIdx");
    ThymeStructDesc gap_desc = detail::MakeDesc(kFrameGapStructId, 0, "FrameGap");
    const ThymeStructDesc descs[2] = {idx_desc, gap_desc};
    fo.open = fo.raw != nullptr && fo.idx_sink->ok() &&
              fo.idx_enc->WriteHeader(trigger_id_, t0_ - before_ns_, descs, 2);
    fo.rec_seq = 0;
    fo.raw_off = 0;
    if (fo.open) {
      // REC-26: the .raw file identifies its channel on its own. Index offsets
      // are absolute, so this only shifts where payload bytes start.
      const detail::RawFileHeader rh = detail::MakeRawFileHeader(s.cfg);
      if (detail::WritePackageBytes(fo.raw.get(), &rh, sizeof(rh),
                                    &capture_hashes_, fo.raw_path) != sizeof(rh)) {
        fo.open = false;
      } else {
        fo.raw_off = sizeof(rh);
      }
    }
    if (!fo.open) {
      fatal = true;
      return 0;
    }
  }
  // Lease path — after pinning the slot, write from the view directly
  // (zero-copy, the writer cannot overwrite a pinned slot); a failed acquire
  // -> GAP.
  // REC-21: crc=frame channels verify the per-frame transport CRC at drain;
  // crc=package/off skip it (the REC-18 package manifest is the integrity
  // boundary). crc=off additionally records payload_crc=0 — no per-frame
  // anchor for `thyme-replay verify` to recompute against.
  const SourceConfig::CrcPolicy crc_policy = EffectiveCrcPolicy(s.cfg);
  const bool verify_crc = crc_policy == SourceConfig::CrcPolicy::kFrame;
  bool ok = false;
  {
    PinnedSlotReader::Lease lease =
        s.frames.Acquire(d.index, nullptr, verify_crc);
    if (lease.valid()) {
      const PinnedSlotReader::View& v = lease.view();
      ok = v.slot_seq == d.slot_seq && v.mono_ns == d.mono_ns &&
           v.data_len == d.len && (v.crc_ok || !verify_crc);
      if (ok && !faulted) {
        if (detail::WritePackageBytes(fo.raw.get(), v.payload, v.data_len,
                                      &capture_hashes_, fo.raw_path) != v.data_len) {
          fatal = true;
          return 0;
        }
        // payload_crc covers the bytes as written; the lease pinned the slot,
        // so the view equals what the producer committed. With the transport
        // CRC disabled (crc=package) the slot carries no CRC, so it is
        // computed here once for on-demand `verify` recompute; crc=off
        // records 0 (verify counts the frame as unchecked).
        const uint32_t payload_crc =
            crc_policy == SourceConfig::CrcPolicy::kOff
                ? 0
                : (v.crc32 != 0 ? v.crc32 : Crc32(v.payload, v.data_len));
        FrameIndexRecord idx{};
        idx.offset = fo.raw_off;
        idx.len = v.data_len;
        idx.flags = v.flags;
        idx.payload_crc = payload_crc;
        idx.producer_frame_index = v.frame_index;
        if (!fo.idx_enc->WriteData(fo.rec_seq++, kFrameIndexStructId,
                                   v.mono_ns, static_cast<uint32_t>(d.index),
                                   &idx, sizeof(idx))) {
          fatal = true;
          return 0;
        }
        fo.raw_off += v.data_len;
        capture_frames_ += 1;
        stats_.frames_written += 1;
        if (!WriteWalRow(kWalOk, d)) fatal = true;
        return v.data_len + 32;
      }
    }
  }
  // A failed acquire/verification -> GAP (falls through to the hole path
  // below); but an injected write failure has ENOSPC semantics: it is fatal
  // and must not masquerade as a data hole.
  if (ok && faulted) {
    fatal = true;
    return 0;
  }
  if (!fo.idx_enc->WriteGap(kFrameGapStructId, d.mono_ns, kGapReasonOverrun,
                            1)) {
    fatal = true;
    return 0;
  }
  capture_holes_ += 1;
  stats_.holes_written += 1;
  if (!WriteWalRow(kWalHole, d)) fatal = true;
  return 32;
}

void TriggeredRecorder::AbortCapture() {
  // 6.5 ENOSPC: the package is actively invalidated (abort row + rename), the
  // queue is dropped, and disk_full_ blocks new captures until the host has
  // run retention and called ClearDiskFull().
  queue_.clear();
  pending_bytes_ = 0;
  meta_sink_.reset();
  meta_enc_.reset();
  small_sink_.reset();
  small_enc_.reset();
  small_open_ = false;
  for (FrameOut& fo : frame_outs_) {
    fo = FrameOut{};  // drops any open .raw handle
  }
  InvalidatePackage(&wal_, /*terminal_rows_written=*/false, nullptr, 0,
                    PackageAbortReason::kWriteFailure, pkg_dir_,
                    TriggerPackageInfo(), capture_registry_, &capture_hashes_);
  stats_.disk_errors += 1;
  disk_full_ = true;
  phase_ = Phase::kArmed;
}

void TriggeredRecorder::DrainSettle() {
  if (defer_settle_ && !flush_allowed_) return;  // idle gate: host drives
  if (!RefreshSessionLease()) return;
  const uint64_t tick_start = MonoNs();  // T2 time slice
  uint32_t n = 0;
  while (!queue_.empty() && n < cfg_.settle_batch &&
         MonoNs() - tick_start < cfg_.drain_tick_budget_ns) {
    // 6.3 token bucket: idle-flushed captures spend budget per byte;
    // realtime/crash captures have an unlimited bucket (rate 0).
    if (rate_limited_ &&
        !bucket_.TryConsume(queue_.front().len + 32, MonoNs())) {
      break;
    }
    const SwapDescriptor d = queue_.front();
    queue_.pop_front();
    pending_bytes_ -= d.len;
    bool fatal = false;
    DrainOne(d, fatal);
    if (fatal) {
      AbortCapture();
      return;
    }
    ++n;
  }
  if (!queue_.empty()) return;

  // Queue drained: append the stats record, seal the wal, close everything.
  // Durability (6.1/6.5): a package counts as sealed only after every file
  // (meta/small/frames raw+index/wal) is fsync'ed AND the wal seal row was
  // written successfully; ANY failure actively invalidates the package
  // (abort row + .incomplete rename) — a seal row that merely reached the
  // page cache must never produce a "self-sealed" broken package.
  bool sealed_ok =
      WriteLifecycleState(wal_.get(), &capture_hashes_, PackageLifecycle::kSealing);
  if (cfg_.test_hook_after_sealing != nullptr) {
    cfg_.test_hook_after_sealing(cfg_.fault_ctx);
  }
  const int seal_fault = cfg_.fault_inject_seal != nullptr
                             ? cfg_.fault_inject_seal(cfg_.fault_ctx)
                             : 0;
  if (meta_enc_ != nullptr) {
    const CaptureStatsRecord csr{capture_queued_, capture_samples_,
                                 capture_frames_, capture_holes_,
                                 capture_evicted_, capture_torn_,
                                 capture_flags_};
    if (!meta_enc_->WriteData(meta_rec_seq_++, kMetaStructId, t0_, 1, &csr,
                              sizeof(csr))) {
      sealed_ok = false;
    }
    if (!meta_enc_->Finish()) sealed_ok = false;
    if (!meta_sink_->Sync()) sealed_ok = false;
  }
  meta_sink_.reset();
  meta_enc_.reset();
  if (small_enc_ != nullptr && !small_enc_->Finish()) sealed_ok = false;
  if (small_sink_ != nullptr && !small_sink_->Sync()) sealed_ok = false;
  small_sink_.reset();
  small_enc_.reset();
  small_open_ = false;
  for (FrameOut& fo : frame_outs_) {
    if (fo.raw) {
      if (seal_fault == 3 || !detail::SyncFile(fo.raw.get())) sealed_ok = false;
      fo.raw.reset();
    }
    if (fo.idx_enc != nullptr && !fo.idx_enc->Finish()) sealed_ok = false;
    if (fo.idx_sink != nullptr && !fo.idx_sink->Sync()) sealed_ok = false;
    fo = FrameOut{};
  }
  if (wal_ && !WriteClockAnchor(wal_.get(), &capture_hashes_, MonoNs())) {
    sealed_ok = false;
  }
  bool terminal_rows_written = false;
  WalRow terminal_rows[2]{
      {kWalOpLifecycle, 0, static_cast<uint16_t>(PackageLifecycle::kSealed), 0,
       detail::RealTimeNs(), MonoNs()},
      {kWalOpSeal, 0, 0, 0, 0, MonoNs()}};
  if (wal_) {
    // Defer hashing full terminal rows until the seal is known to survive. A
    // failed seal overwrites both rows with lifecycle-abort + legacy abort.
    if (seal_fault != 1) {  // 1: seal fwrite fails
      const std::size_t written =
          std::fwrite(terminal_rows, 1, sizeof(terminal_rows), wal_.get());
      terminal_rows_written = written == sizeof(terminal_rows);
      if (!terminal_rows_written && written != 0) {
        capture_hashes_.Append("wal.log", terminal_rows, written);
      }
    }
    if (!terminal_rows_written) sealed_ok = false;
    if (seal_fault == 2 || !detail::SyncFile(wal_.get())) {
      sealed_ok = false;  // 2: fsync fail
    }
  }
  // ABI-09: the schema sidecar describes the record contracts of this seal.
  // It is written before the integrity manifest so the manifest covers it; a
  // failure fails the seal (fail closed) exactly like a missing manifest.
  // Fault 4 injects exactly that failure path.
  if (sealed_ok &&
      (seal_fault == 4 ||
       !WriteSealedSchema(pkg_dir_, capture_registry_, &capture_hashes_))) {
    sealed_ok = false;
  }
  // SEC-03: a clean seal includes an integrity manifest over the now-final
  // file set (both wal terminal rows included). The wal stays open until the
  // manifest lands so a write failure can still overwrite them with aborts.
  if (sealed_ok) {
    CheckpointHashes(MonoNs(), true);
    integrity::PackageInfo pinfo = TriggerPackageInfo();
    pinfo.seal_time_ns = detail::RealTimeNs();
    std::string ierr;
    if (!integrity::WritePackageManifestFromHashes(
            pkg_dir_, pinfo, cfg_.signing_key_path, capture_hashes_, &ierr,
            "wal.log", terminal_rows,
            terminal_rows_written ? sizeof(terminal_rows) : std::size_t{0})) {
      THYME_LOG_ERROR("triggered_recorder: integrity manifest for %s failed: "
                      "%s",
                      pkg_dir_.c_str(), ierr.c_str());
      sealed_ok = false;
    }
  }
  stats_.torn_reads += capture_torn_;
  if (sealed_ok) {
    wal_.reset();
    stats_.captures_done += 1;
    if (!detail::SyncDir(pkg_dir_)) ++stats_.sync_errors;  // persist the directory entry
    // TRG-14: full seal completion is the SLO endpoint (durability steps all
    // done); the wal row above carries the slightly earlier journal commit.
    last_seal_trigger_id_ = trigger_id_;
    last_seal_mono_ns_ = MonoNs();
  } else {
    InvalidatePackage(&wal_, terminal_rows_written, terminal_rows, 2,
                      PackageAbortReason::kSealFailure, pkg_dir_,
                      TriggerPackageInfo(), capture_registry_,
                      &capture_hashes_);
    stats_.disk_errors += 1;
    disk_full_ = true;
  }
  phase_ = Phase::kArmed;
}

}  // namespace triggered
}  // namespace thyme
