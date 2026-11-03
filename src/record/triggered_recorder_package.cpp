#include "triggered_recorder_internal.h"

#include "thyme/runtime/log.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace thyme {
namespace triggered {

// REC-18 helpers: manifest identity of the in-flight capture/slice.
integrity::PackageInfo TriggeredRecorder::TriggerPackageInfo() const {
  integrity::PackageInfo pinfo;
  pinfo.kind = integrity::PackageKind::kTrigger;
  pinfo.session_id = trigger_id_;  // meta.tbin header session id
  pinfo.trigger_id = trigger_id_;
  pinfo.t0_ns = t0_;
  pinfo.before_ns = before_ns_;
  pinfo.after_ns = after_ns_;
  ApplyManifestPolicy(&pinfo);
  return pinfo;
}

integrity::PackageInfo TriggeredRecorder::SlicePackageInfo(
    uint64_t end_ns) const {
  integrity::PackageInfo pinfo;
  pinfo.kind = integrity::PackageKind::kContinuous;
  pinfo.session_id = kContinuousWalMark | cont_seq_;
  pinfo.slice_seq = cont_seq_;
  pinfo.begin_ns = cont_slice_begin_ns_;
  pinfo.end_ns = end_ns;
  ApplyManifestPolicy(&pinfo);
  return pinfo;
}

void TriggeredRecorder::ApplyManifestPolicy(integrity::PackageInfo* info) const {
  info->manifest_mode = integrity_mode_;
  for (const SourceConfig& source : cfg_.sources) {
    if (source.kind != SourceConfig::Kind::kPinnedSlot) continue;
    const integrity::ManifestMode mode = EffectiveManifestMode(source, cfg_.manifest_mode);
    info->bulk_modes.emplace_back(source.channel_name.empty() ? "channel" : source.channel_name,
                                  mode);
  }
}

bool TriggeredRecorder::WriteClockAnchor(
    std::FILE* wal, integrity::PackageHashState* hashes, uint64_t mono_ns) {
  if (wal == nullptr) return false;
  const WalRow row{kWalOpClockAnchor, kWalOk, 0, 0, mono_ns,
                   detail::RealTimeNs()};
  return detail::WritePackageBytes(wal, &row, sizeof(row), hashes, "wal.log") ==
         sizeof(row);
}

bool TriggeredRecorder::WriteLifecycleState(
    std::FILE* wal, integrity::PackageHashState* hashes,
    PackageLifecycle state, PackageAbortReason reason) {
  if (wal == nullptr) return false;
  const WalRow row{kWalOpLifecycle,
                   static_cast<uint8_t>(reason),
                   static_cast<uint16_t>(state),
                   0,
                   detail::RealTimeNs(),
                   MonoNs()};
  if (detail::WritePackageBytes(wal, &row, sizeof(row), hashes, "wal.log") !=
      sizeof(row)) {
    return false;
  }
  return detail::SyncFile(wal);
}

void TriggeredRecorder::WriteClockAnchors(uint64_t now, bool force) {
  if (!force && now - last_clock_anchor_ns_ < 1000000000ull) return;
  bool ok = true;
  if (wal_ != nullptr) {
    ok = WriteClockAnchor(wal_.get(), &capture_hashes_, now) && ok;
  }
  if (cont_wal_ != nullptr) {
    ok = WriteClockAnchor(cont_wal_.get(), &cont_hashes_, now) && ok;
  }
  if (!ok) {
    ++stats_.disk_errors;
    disk_full_ = true;
    THYME_LOG_WARN_RL("triggered_recorder: clock anchor write failed");
  }
  last_clock_anchor_ns_ = now;
}

void TriggeredRecorder::CheckpointHashes(uint64_t now, bool force) {
  if (!force && now - last_checkpoint_ns_ < 1000000000ull) return;
  if (wal_ != nullptr) {
    const std::string path =
        pkg_dir_ + "/" + integrity::PackageHashState::kCheckpointName;
    if (!capture_hashes_.SaveCheckpoint(pkg_dir_,
                                        TriggerPackageInfo().session_id)) {
      THYME_LOG_WARN_RL("triggered_recorder: hash checkpoint failed for %s",
                        path.c_str());
    }
  }
  if (cont_wal_ != nullptr) {
    const std::string path =
        cont_dir_ + "/" + integrity::PackageHashState::kCheckpointName;
    if (!cont_hashes_.SaveCheckpoint(cont_dir_,
                                     SlicePackageInfo(now).session_id)) {
      THYME_LOG_WARN_RL("triggered_recorder: hash checkpoint failed for %s",
                        path.c_str());
    }
  }
  last_checkpoint_ns_ = now;
}

bool TriggeredRecorder::HashRawPayload(const SourceConfig& source) const {
  return cfg_.manifest_mode != integrity::ManifestMode::kOff &&
         EffectiveManifestMode(source, cfg_.manifest_mode) == integrity::ManifestMode::kFull;
}

void TriggeredRecorder::InvalidatePackage(
    FilePtr* wal_pp, bool terminal_rows_written,
    const WalRow* overwritten_rows, uint32_t overwritten_row_count,
    PackageAbortReason reason, const std::string& dir,
    integrity::PackageInfo info,
    const std::vector<ThymeStructDesc>& registry,
    integrity::PackageHashState* hashes) {
  std::FILE* wal = wal_pp->get();
  if (wal != nullptr) {
    WalRow abort_rows[2]{
        {kWalOpLifecycle, static_cast<uint8_t>(reason),
         static_cast<uint16_t>(PackageLifecycle::kAborted), 0,
         detail::RealTimeNs(), MonoNs()},
        {kWalOpAbort, 0, 0, 0, 0, 0}};
    if (terminal_rows_written) {
      // Terminal rows were deliberately not hashed yet: the normal outcome is
      // an in-place abort overwrite. If seeking fails, the abort is appended
      // instead, so account for the terminal bytes that remain before it.
      const std::size_t terminal_bytes = overwritten_row_count * sizeof(WalRow);
      if (std::fseek(wal, -static_cast<long>(terminal_bytes), SEEK_END) != 0 &&
          overwritten_rows != nullptr) {
        hashes->Append("wal.log", overwritten_rows, terminal_bytes);
      }
    }
    const bool abort_written =
        detail::WritePackageBytes(wal, abort_rows, sizeof(abort_rows), hashes,
                                  "wal.log") == sizeof(abort_rows);
    if (!abort_written || !detail::SyncFile(wal)) {
      ++stats_.sync_errors;  // abort rows did not reach stable storage
    }
    wal_pp->reset();
  }
  const std::string renamed = dir + ".incomplete";
  const bool renamed_ok = ::rename(dir.c_str(), renamed.c_str()) == 0;
  if (!renamed_ok) {
    THYME_LOG_ERROR("triggered_recorder: failed to invalidate package %s",
                    dir.c_str());
  }
  if (!detail::SyncParentDir(renamed)) ++stats_.sync_errors;  // persist the rename
  // REC-18: the surviving files of an invalidated package are still forensic
  // evidence — finalize their incrementally computed hashes in a partial
  // manifest. Best-effort: a failure leaves the package without a manifest.
  const std::string& target = renamed_ok ? renamed : dir;
  // ABI-09: the surviving file set still describes its record contracts, so
  // the partial schema is written before the partial manifest covers it.
  WritePartialSchema(target, registry, hashes);
  info.partial = true;
  info.seal_time_ns = detail::RealTimeNs();
  std::string merr;
  if (!integrity::WritePackageManifestFromHashes(
          target, info, cfg_.signing_key_path, *hashes, &merr)) {
    THYME_LOG_WARN("triggered_recorder: partial manifest for %s failed: %s",
                   target.c_str(), merr.c_str());
  }
}

void TriggeredRecorder::CollectPayloadProfiles(
    bool continuous, std::vector<schema::PayloadProfileDesc>* out) const {
  out->clear();
  for (std::size_t i = 0; i < sources_.size(); ++i) {
    const SourceState& s = *sources_[i];
    if (s.cfg.kind != SourceConfig::Kind::kPinnedSlot) continue;
    if (continuous) {
      if (!ContIncluded(static_cast<uint16_t>(i))) continue;
    } else if (!TriggerIncluded(static_cast<uint16_t>(i))) {
      continue;
    }
    schema::PayloadProfileDesc d;
    d.channel = s.cfg.channel_name.empty() ? "channel" : s.cfg.channel_name;
    d.profile = s.cfg.payload_profile;
    d.version = s.cfg.payload_profile_version;
    d.params_digest = s.cfg.payload_profile_digest;
    out->push_back(std::move(d));
  }
}

bool TriggeredRecorder::WritePayloadProfileRows(ThymeBinEncoder* enc,
                                                uint64_t mono_ns,
                                                uint64_t* rec_seq,
                                                bool continuous) {
  if (enc == nullptr || rec_seq == nullptr) return false;
  const auto& profiles = continuous ? cont_profiles_ : capture_profiles_;
  for (const schema::PayloadProfileDesc& d : profiles) {
    SourceConfig cfg;
    cfg.kind = SourceConfig::Kind::kPinnedSlot;
    cfg.channel_name = d.channel;
    cfg.payload_profile = d.profile;
    cfg.payload_profile_version = d.version;
    cfg.payload_profile_digest = d.params_digest;
    const PayloadProfileRecord row = detail::MakePayloadProfileRecord(cfg);
    if (!enc->WriteData((*rec_seq)++, kPayloadProfileStructId, mono_ns, 3, &row,
                        sizeof(row))) {
      return false;
    }
  }
  return true;
}

bool TriggeredRecorder::WriteSealedSchema(
    const std::string& dir, const std::vector<ThymeStructDesc>& registry,
    integrity::PackageHashState* hashes) {
  const auto& profiles =
      &registry == &cont_registry_ ? cont_profiles_ : capture_profiles_;
  std::string err;
  if (!schema::WritePackageSchema(dir, registry.data(), registry.size(),
                                  cfg_.record_manifest, /*partial=*/false, &err,
                                  hashes, profiles.data(), profiles.size())) {
    THYME_LOG_ERROR("triggered_recorder: schema sidecar for %s failed: %s",
                    dir.c_str(), err.c_str());
    return false;
  }
  return true;
}

void TriggeredRecorder::WritePartialSchema(
    const std::string& dir, const std::vector<ThymeStructDesc>& registry,
    integrity::PackageHashState* hashes) {
  if (registry.empty()) return;  // no capture state to describe
  const auto& profiles =
      &registry == &cont_registry_ ? cont_profiles_ : capture_profiles_;
  std::string err;
  if (!schema::WritePackageSchema(dir, registry.data(), registry.size(),
                                  cfg_.record_manifest, /*partial=*/true, &err,
                                  hashes, profiles.data(), profiles.size())) {
    THYME_LOG_WARN("triggered_recorder: partial schema for %s failed: %s",
                   dir.c_str(), err.c_str());
  }
}

const char* PackageLifecycleName(PackageLifecycle state) {
  switch (state) {
    case PackageLifecycle::kUnknown: return "unknown";
    case PackageLifecycle::kArmed: return "armed";
    case PackageLifecycle::kCapturing: return "capturing";
    case PackageLifecycle::kSealing: return "sealing";
    case PackageLifecycle::kSealed: return "sealed";
    case PackageLifecycle::kAborted: return "aborted";
  }
  return "unknown";
}

const char* PackageAbortReasonName(PackageAbortReason reason) {
  switch (reason) {
    case PackageAbortReason::kNone: return "none";
    case PackageAbortReason::kUnspecified: return "unspecified";
    case PackageAbortReason::kWriteFailure: return "write_failure";
    case PackageAbortReason::kSealFailure: return "seal_failure";
    case PackageAbortReason::kCrashRecovery: return "crash_recovery";
  }
  return "unspecified";
}

namespace {

uint64_t LifecycleMonoNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

bool ValidLifecycleTransition(PackageLifecycle from, PackageLifecycle to) {
  if (from == PackageLifecycle::kUnknown) {
    return to == PackageLifecycle::kArmed || to == PackageLifecycle::kAborted;
  }
  if (to == PackageLifecycle::kAborted) {
    // sealed -> aborted is the recovery trace left by a crash between the two
    // terminal rows; any abort still wins over the earlier seal attempt.
    return from != PackageLifecycle::kAborted;
  }
  return (from == PackageLifecycle::kArmed && to == PackageLifecycle::kCapturing) ||
         (from == PackageLifecycle::kCapturing && to == PackageLifecycle::kSealing) ||
         (from == PackageLifecycle::kSealing && to == PackageLifecycle::kSealed);
}

}  // namespace

bool ReadPackageLifecycle(const std::string& package_dir,
                          PackageLifecycleState* out) {
  if (out == nullptr) return false;
  *out = PackageLifecycleState{};
  FilePtr f(std::fopen((package_dir + "/wal.log").c_str(), "rb"));
  if (!f) return false;
  struct stat st {};
  if (fstat(fileno(f.get()), &st) != 0 ||
      st.st_size < static_cast<off_t>(sizeof(WalHeader)) ||
      (st.st_size - static_cast<off_t>(sizeof(WalHeader))) %
              static_cast<off_t>(sizeof(WalRow)) !=
          0) {
    return false;
  }
  WalHeader wh{};
  if (std::fread(&wh, sizeof(wh), 1, f.get()) != 1 ||
      std::memcmp(wh.magic, kWalMagic, sizeof(kWalMagic)) != 0) {
    return false;
  }
  out->session_id = wh.trigger_id;
  bool has_lifecycle = false;
  bool aborted = false;
  uint8_t last_op = kWalOpDescriptor;
  WalRow row{};
  while (std::fread(&row, sizeof(row), 1, f.get()) == 1) {
    last_op = row.op;
    if (row.op == kWalOpAbort) aborted = true;
    if (row.op != kWalOpLifecycle) continue;
    if (row.struct_id < static_cast<uint16_t>(PackageLifecycle::kArmed) ||
        row.struct_id > static_cast<uint16_t>(PackageLifecycle::kAborted) ||
        row.len != 0) {
      *out = PackageLifecycleState{};
      return false;
    }
    const auto state = static_cast<PackageLifecycle>(row.struct_id);
    const auto reason = static_cast<PackageAbortReason>(row.result);
    const bool valid_reason =
        state == PackageLifecycle::kAborted
            ? reason >= PackageAbortReason::kUnspecified &&
                  reason <= PackageAbortReason::kCrashRecovery
            : reason == PackageAbortReason::kNone;
    if (!valid_reason || !ValidLifecycleTransition(out->state, state)) {
      *out = PackageLifecycleState{};
      return false;
    }
    has_lifecycle = true;
    out->state = state;
    out->abort_reason = reason;
    out->mono_ns = row.mono_ns;
    out->realtime_ns = row.index;
  }
  if (std::ferror(f.get()) != 0) {
    *out = PackageLifecycleState{};
    return false;
  }
  const bool legacy_sealed = !has_lifecycle && last_op == kWalOpSeal;
  if (has_lifecycle) {
    if ((out->state != PackageLifecycle::kSealed && last_op == kWalOpSeal) ||
        (aborted && out->state != PackageLifecycle::kAborted)) {
      *out = PackageLifecycleState{};
      return false;
    }
    if (out->state == PackageLifecycle::kSealed && last_op != kWalOpSeal) {
      // A crash between the 24-byte lifecycle and legacy seal rows is still a
      // sealing package. Recovery can therefore append a valid terminal abort.
      out->state = PackageLifecycle::kSealing;
    }
  } else {
    out->legacy = true;
    out->state = aborted ? PackageLifecycle::kAborted
                         : (legacy_sealed ? PackageLifecycle::kSealed
                                          : PackageLifecycle::kCapturing);
    out->abort_reason = aborted ? PackageAbortReason::kUnspecified
                                : PackageAbortReason::kNone;
  }
  return true;
}

bool AbortPackageAfterCrash(const std::string& package_dir) {
  PackageLifecycleState state;
  if (!ReadPackageLifecycle(package_dir, &state)) return false;
  if (state.state == PackageLifecycle::kSealed ||
      state.state == PackageLifecycle::kAborted) {
    return true;
  }
  FilePtr wal(std::fopen((package_dir + "/wal.log").c_str(), "ab"));
  if (!wal) return false;
  const WalRow rows[2]{
      {kWalOpLifecycle, static_cast<uint8_t>(PackageAbortReason::kCrashRecovery),
       static_cast<uint16_t>(PackageLifecycle::kAborted), 0,
       detail::RealTimeNs(), LifecycleMonoNs()},
      {kWalOpAbort, 0, 0, 0, 0, 0}};
  return std::fwrite(rows, sizeof(rows), 1, wal.get()) == 1 &&
         detail::SyncFile(wal.get());
}

// static
bool TriggeredRecorder::PackageComplete(const std::string& package_dir) {
  // Directories already invalidated by the engine (seal-path failure) are
  // never complete, regardless of journal content.
  if (package_dir.size() >= 11 &&
      package_dir.compare(package_dir.size() - 11, 11, ".incomplete") == 0) {
    return false;
  }
  PackageLifecycleState state;
  return ReadPackageLifecycle(package_dir, &state) &&
         state.state == PackageLifecycle::kSealed;
}

}  // namespace triggered
}  // namespace thyme
