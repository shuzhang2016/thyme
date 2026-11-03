#include "triggered_recorder_internal.h"

#include "thyme/runtime/log.h"

#include <cerrno>
#include <ctime>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace thyme {
namespace triggered {

uint64_t TriggeredRecorder::MonoNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

bool TriggeredRecorder::ShmIdentity(const char* name, uint64_t& ino,
                                    uint64_t& size) {
  // Identity = inode + size. ctim is NOT usable: this kernel's tmpfs bumps
  // ctim when the producer first touches a fresh page (measured), which
  // spoofed re-attaches mid-capture. ino is monotonic on tmpfs (no immediate
  // reuse across unlink+create), and the region size is fixed at Create, so
  // (ino, size) is a stable lifetime fingerprint of the shm object.
  const int fd = shm_open(name, O_RDONLY, 0);
  if (fd < 0) return false;
  struct stat st {};
  const bool ok = fstat(fd, &st) == 0;
  close(fd);
  if (!ok) return false;
  ino = static_cast<uint64_t>(st.st_ino);
  size = static_cast<uint64_t>(st.st_size);
  return true;
}

bool TriggeredRecorder::Mkdirs(const std::string& dir) {
  if (dir.empty()) return false;
  std::string cur;
  if (dir[0] == '/') cur = "/";
  std::size_t pos = cur.size();
  while (pos <= dir.size()) {
    const std::size_t slash = dir.find('/', pos);
    const std::string part = dir.substr(0, slash);
    if (!part.empty() && part != "/") {
      if (mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    if (slash == std::string::npos) break;
    pos = slash + 1;
  }
  return true;
}

bool TriggeredRecorder::AttachSource(SourceState& s, bool initial) {
  uint64_t ino = 0;
  uint64_t size = 0;
  if (!ShmIdentity(s.cfg.shm_name.c_str(), ino, size)) return false;
  if (s.cfg.kind == SourceConfig::Kind::kMirrorRing) {
    // RingClient::Attach* unmaps any previous mapping first, so re-attach is
    // in place (the class is intentionally non-movable).
    if (!s.mirror.AttachReadOnly(s.cfg.shm_name.c_str(),
                                 cfg_.attach_timeout_ns,
                                 initial ? 0 : s.pinned_hash)) {
      return false;
    }
    const RecordMirrorRingPage* page = s.mirror.Page();
    if (page == nullptr || !page->Validate()) return false;
    s.struct_count = page->struct_count();
    if (initial) s.pinned_hash = page->config_hash();
  } else {
    s.frames.Close();  // Open() re-maps from scratch; clear stale header_
    if (!s.frames.Open(s.cfg.shm_name.c_str(), cfg_.attach_timeout_ns)) {
      return false;
    }
  }
  const uint32_t cursor_count =
      s.cfg.kind == SourceConfig::Kind::kMirrorRing ? s.struct_count : 1;
  s.post_next.assign(cursor_count, 0);
  s.cont_next.assign(s.struct_count, 0);
  s.cont_last_mono.assign(s.struct_count, 0);
  s.shm_ino = ino;
  s.shm_size = size;
  s.attached = true;
  s.ever_attached = true;
  return true;
}

bool TriggeredRecorder::Init(const Config& cfg) {
  Shutdown();
  init_lease_result_ = SessionLeaseResult::kInvalidArgument;
  if (cfg.out_dir.empty() || cfg.sources.empty()) return false;
  if (!Mkdirs(cfg.out_dir)) return false;
  if (!cfg.signing_key_path.empty()) {
    std::string key;
    std::string kerr;
    if (!integrity::LoadSigningKey(cfg.signing_key_path, &key, &kerr)) {
      THYME_LOG_ERROR("triggered_recorder: %s", kerr.c_str());
      return false;
    }
  }
  cfg_ = cfg;
  if (static_cast<uint8_t>(cfg_.manifest_mode) >
      static_cast<uint8_t>(integrity::ManifestMode::kMixed)) {
    return false;
  }
  for (SourceConfig& source : cfg_.sources) {
    if (source.kind == SourceConfig::Kind::kMirrorRing &&
        (source.manifest == SourceConfig::ManifestPolicy::kMetadata ||
         source.manifest == SourceConfig::ManifestPolicy::kOff)) {
      return false;
    }
    if (source.payload_profile.empty()) {
      if (source.payload_profile_version != 0 ||
          !source.payload_profile_params.empty() ||
          source.payload_profile_digest != 0) {
        return false;
      }
    } else {
      if (source.kind != SourceConfig::Kind::kPinnedSlot) return false;
      if (source.payload_profile.size() >= schema::kMaxProfileBytes) {
        return false;
      }
      source.payload_profile_digest =
          schema::HashProfileParams(source.payload_profile_params);
    }
  }
  integrity_mode_ = cfg_.manifest_mode;
  if (integrity_mode_ != integrity::ManifestMode::kOff) {
    bool raw_full = false;
    bool raw_metadata = false;
    bool raw_off = false;
    for (const SourceConfig& source : cfg_.sources) {
      if (source.kind != SourceConfig::Kind::kPinnedSlot) continue;
      switch (EffectiveManifestMode(source, cfg_.manifest_mode)) {
        case integrity::ManifestMode::kFull: raw_full = true; break;
        case integrity::ManifestMode::kMetadata: raw_metadata = true; break;
        case integrity::ManifestMode::kOff: raw_off = true; break;
        case integrity::ManifestMode::kMixed: return false;
      }
    }
    if (raw_off || (cfg_.manifest_mode == integrity::ManifestMode::kFull && raw_metadata) ||
        (cfg_.manifest_mode == integrity::ManifestMode::kMetadata && raw_full)) {
      integrity_mode_ = integrity::ManifestMode::kMixed;
    }
  }
  // ABI-10: pin the GEN-12 manifest identity once. A host without codegen
  // (record_manifest == nullptr or version 0) keeps the pre-ABI-10 format:
  // no manifest block, and the sidecar declares the identity unavailable.
  has_manifest_block_ = cfg_.record_manifest != nullptr &&
                        cfg_.record_manifest->version != 0;
  if (has_manifest_block_) {
    manifest_block_ = MakeThymeBinManifestBlock(
        cfg_.record_manifest->hash, cfg_.record_manifest->version);
  }
  // REC-20: descriptor rings are fixed-capacity and sized once here; enqueue
  // and dequeue are index updates only. Both buckets share the same bound.
  const uint32_t ring_cap = DescriptorRingCapacity(cfg_.pending_bytes_quota);
  queue_.Reset(ring_cap);
  cont_queue_.Reset(ring_cap);
  sources_.clear();
  frame_outs_.clear();
  for (const SourceConfig& sc : cfg_.sources) {
    auto s = std::make_unique<SourceState>();
    s->cfg = sc;
    if (!AttachSource(*s, true)) {
      Shutdown();
      return false;
    }
    sources_.push_back(std::move(s));
  }
  // REC-20: the drain staging scratch is sized once to the largest payload any
  // attached source can produce, so the settle/continuous drain never resizes
  // (allocates) it per descriptor. Re-attach on a larger geometry still falls
  // back to the lazy resize below.
  uint32_t max_payload = 0;
  for (const auto& sp : sources_) {
    if (sp->cfg.kind == SourceConfig::Kind::kMirrorRing) {
      const RecordMirrorRingPage* page = sp->mirror.Page();
      for (uint32_t sid = 0; sid < sp->struct_count; ++sid) {
        const uint32_t cap = page->ring_descs[sid].payload_capacity;
        if (cap > max_payload) max_payload = cap;
      }
    } else {
      const uint32_t cap = sp->frames.slot_bytes();
      if (cap > max_payload) max_payload = cap;
    }
  }
  scratch_.assign(max_payload, 0);
  frame_outs_.resize(sources_.size());
  cont_frame_outs_.resize(sources_.size());
  char lease_name[64];
  if (!SessionLeaseNameForRoot(cfg_.out_dir.c_str(), lease_name,
                               sizeof(lease_name))) {
    Shutdown();
    return false;
  }
  uint64_t token = 0;
  const SessionLeaseResult held = SessionLeaseAcquire(
      &session_lease_, lease_name, SessionRole::kRecorder,
      SessionLeaseRootId(cfg_.out_dir.c_str()), MonoNs(), 0, &token);
  init_lease_result_ = held;
  if (held != SessionLeaseResult::kOk && held != SessionLeaseResult::kStolen) {
    Shutdown();
    return false;
  }
  session_lease_token_ = token;
  phase_ = Phase::kArmed;
  return true;
}

bool TriggeredRecorder::RefreshSessionLease() {
  if (session_lease_token_ == 0 || session_lease_.data() == nullptr) {
    return false;
  }
  return SessionLeaseHeartbeat(
      static_cast<SessionLeasePage*>(session_lease_.data()),
      session_lease_token_, MonoNs());
}

void TriggeredRecorder::Shutdown() {
  if (cont_armed_) DisarmContinuous();  // finalize and seal the current slice
  cont_meta_sink_.reset();
  cont_meta_enc_.reset();
  cont_small_sink_.reset();
  cont_small_enc_.reset();
  cont_frame_outs_.clear();  // closes any still-open .raw handles
  cont_wal_.reset();
  // unsealed: package stays incomplete by design
  wal_.reset();
  frame_outs_.clear();  // closes any still-open .raw handles
  small_sink_.reset();
  small_enc_.reset();
  meta_sink_.reset();
  meta_enc_.reset();
  sources_.clear();
  queue_.clear();
  cont_queue_.clear();
  cont_queue_dirty_ = false;
  cont_small_last_mono_ = 0;
  pending_bytes_ = 0;
  phase_ = Phase::kArmed;
  if (session_lease_token_ != 0 && session_lease_.data() != nullptr) {
    SessionLeaseRelease(static_cast<SessionLeasePage*>(session_lease_.data()),
                        session_lease_token_);
  }
  session_lease_token_ = 0;
  session_lease_.Unmap();
}

TriggeredRecorder::~TriggeredRecorder() { Shutdown(); }

TriggeredRecorder::TriggeredRecorder() = default;

void TriggeredRecorder::ScanSources(uint64_t now) {
  if (now - last_scan_ns_ < cfg_.reattach_scan_ns) return;
  last_scan_ns_ = now;
  // A capture in flight keeps the old generation's mappings; re-attach is an
  // Armed-phase operation (the drain re-validation turns any mid-capture
  // replacement into holes instead of corrupt reads).
  if (phase_ != Phase::kArmed) return;
  for (std::size_t si = 0; si < sources_.size(); ++si) {
    SourceState& s = *sources_[si];
    uint64_t ino = 0;
    uint64_t size = 0;
    if (!ShmIdentity(s.cfg.shm_name.c_str(), ino, size)) {
      continue;  // producer gone (or not yet up): keep the old mapping
    }
    if (s.attached && ino == s.shm_ino && size == s.shm_size) continue;
    const bool was_attached = s.ever_attached;
    if (AttachSource(s, !was_attached)) {
      if (was_attached) {
        stats_.session_reattaches += 1;
        // The continuous cursors point at the previous generation's page;
        // after re-attaching they continue from the new generation's current
        // head (no history replay; the gap is visible in the slice mono).
        if (cont_armed_ && ContIncluded(static_cast<uint16_t>(si))) {
          if (s.cfg.kind == SourceConfig::Kind::kMirrorRing) {
            for (uint32_t sid = 0; sid < s.struct_count; ++sid) {
              s.cont_next[sid] =
                  s.mirror.Page()->RingHead(static_cast<uint16_t>(sid));
            }
          } else {
            s.frames.SeekToHead();
            s.cont_bf_drops = s.frames.drop_count();
          }
        }
      }
    } else if (was_attached) {
      stats_.source_errors += 1;
      s.attached = false;
    }
  }
}

void TriggeredRecorder::AllowFlush() {
  flush_allowed_ = true;  // sticky for the active capture
}

bool TriggeredRecorder::Preempt() {
  if (phase_ == Phase::kArmed) return false;
  capture_flags_ |= kCaptureFlagTruncated;
  if (phase_ == Phase::kPost) FinishPost();
  // Preemption forces realtime (6.3): bypass the idle gate AND lift the token
  // bucket.
  // Draining a rate-limited queue without this would spin the caller at 100%
  // CPU until the bucket refilled. The queue is quota-bounded, so after
  // lifting the limit the synchronous loop below finishes in a bounded number
  // of settle batches at disk speed — no yield needed.
  flush_allowed_ = true;
  if (rate_limited_) {
    rate_limited_ = false;
    bucket_.SetRate(0, cfg_.io_burst_bytes);
  }
  while (phase_ == Phase::kSettle) DrainSettle();
  return true;
}

uint64_t TriggeredRecorder::EvictContinuous(uint64_t bytes) {
  uint64_t freed = 0;
  while (freed < bytes && !cont_queue_.empty()) {
    freed += cont_queue_.front().len;
    DropOldestContinuous();
  }
  return freed;
}

uint64_t TriggeredRecorder::EvictOldest(uint64_t bytes) {
  // Evict from the continuous bucket first (6.6: triggered packages are kept
  // in preference), then from the trigger queue.
  uint64_t freed = EvictContinuous(bytes);
  while (freed < bytes && !queue_.empty()) {
    freed += queue_.front().len;
    DropOldestTriggered();
  }
  return freed;
}

bool TriggeredRecorder::ready() const {
  for (const auto& sp : sources_) {
    if (!sp->attached) return false;
  }
  return !sources_.empty();
}

void TriggeredRecorder::Tick() {
  if (session_lease_token_ != 0) RefreshSessionLease();
  const uint64_t now = MonoNs();
  ScanSources(now);
  if (phase_ == Phase::kPost) {
    const uint64_t collected = CollectPost();
    // h264 with gop_ns stays in post until the closing IDR or the slack
    // elapses, even after the nominal post deadline.
    if (!GopPostOpen(now) &&
        now >= post_deadline_ + cfg_.post_grace_ns && collected == 0) {
      FinishPost();
    }
  } else if (phase_ == Phase::kSettle) {
    DrainSettle();
  }
  if (cont_armed_) TickContinuous(now);  // continuous and triggered coexist
  WriteClockAnchors(now, false);
  CheckpointHashes(now, false);
}

}  // namespace triggered
}  // namespace thyme
