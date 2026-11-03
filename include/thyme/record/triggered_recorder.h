#pragma once

// triggered_recorder.h — triggered-acquisition capture engine (P2, design
// doc 3.2 recorder row / 4.4 zero-copy swap / 6.1 package format). Library
// component only: no trigger_center, no priority/mute, no idle detection —
// the host process (the future recorder daemon) drives Tick() and Trigger().
//
// Pipeline per trigger:
//   Armed --Trigger(t0, before, after)--> Post --(post window elapsed,
//   continuous harvest complete)--> Settle --(queue drained, package
//   sealed)--> Armed
//
// Sources (attached read-only at Init, re-attached on session/recreation
// change detected by shm identity):
//   * mirror ring pages (RecordMirrorRingPage via RingClient): pre window
//     via HarvestWindow(t0-before, t0), post window via per-struct
//     PeekRingSample continuation cursors;
//   * PinnedSlotStores: pre window via head() + ReadFrame lookback, post via
//     frame-index continuation cursor.
//
// Swap descriptors are header-only ({source, kind, struct_id, index, mono_ns,
// len, slot_seq}) — small-struct payload bytes are copied only at Settle,
// after a seqlock/slot_seq + crc re-validation; big-frame payload is written
// from a lease-pinned slot view (reader pin -> writer skip, BF-06), so the
// bytes on disk equal what the producer committed. Slots overwritten before
// the drain (or not acquirable) become GAP records
// (hole markers, reason kGapReasonOverrun). Both descriptor queues are
// fixed-capacity rings whose storage is sized once in Init() and never grows:
// enqueue/dequeue are index updates only (REC-20). Descriptors are
// byte-accounted against Config::pending_bytes_quota: on overflow the OLDEST
// descriptors are evicted and counted (Stats::pending_evicted). When the
// descriptor-count bound is reached before the byte quota, the OLDEST
// descriptor is evicted through the same counters — never a silent drop.
//
// Package layout (design doc 6.1, single capture per directory):
//   <out_dir>/<trigger_id>_<t0>/
//   ├── meta.tbin    — thyme_bin: header + TRIGGER entry (t0, before, after)
//   │                  + TrigMeta + per-channel PayProf (REC-27) + stats
//   ├── small.tbin   — small-struct samples (+ GAP holes), drained in
//   │                  ascending mono_ns order across structs (REP-07), so
//   │                  every source file the replay reader merges is monotonic
//   │                  (triggered: one sort of the queue before drain;
//   │                  continuous: the pending window is re-sorted per drain
//   │                  pass; see SortRingByMono)
//   ├── frames/<channel>.raw + <channel>.index.tbin  — big-frame segments
//   └── wal.log      — drain journal; sealed on completion
//                      (PackageComplete() == seal present)
//
// Instantiable, no global state (thyme convention).

#include "thyme/record/file_ptr.h"
#include "thyme/record/package_integrity.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/record_schema.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/token_bucket.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/session_lease.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace thyme {
namespace triggered {

// thyme_bin struct_ids used inside capture packages.
inline constexpr uint16_t kFrameIndexStructId = 60;
inline constexpr uint16_t kFrameGapStructId = 61;
inline constexpr uint16_t kPayloadProfileStructId = 62;
inline constexpr uint16_t kMetaStructId = 63;
inline constexpr uint32_t kPayloadProfileFlagHasProfile = 1u;
// GAP reasons written by the drain.
inline constexpr uint16_t kGapReasonOverrun = 1;  // slot overwritten pre-drain

// Mirrors record_control::RecorderPhase (kept as a local enum so this header
// does not depend on the control socket component).
enum class Phase : uint8_t { kArmed = 0, kPost = 1, kSettle = 2 };

struct SourceConfig {
  enum class Kind : uint8_t { kMirrorRing = 0, kPinnedSlot = 1 };
  Kind kind = Kind::kMirrorRing;
  std::string shm_name;      // "/thyme_recr_<pid>" or "/h264_<camera>" etc.
  std::string channel_name;  // kPinnedSlot only: frames/<channel_name>.* naming
  // OPS-13: names of an aggregated mirror, in page struct-index order.
  // Empty means the page is one source. A trigger/continuous selector may
  // name one entry; only that struct is harvested.
  std::vector<std::string> record_channels;
  // Recording parameters (chapters 7 channels list item; used for continuous
  // mode rate accounting, 0 by default).
  bool h264_stream = false;  // kind=h264_stream: GOP-aligned slices, no decimation
  double rate_hz = 0;
  uint64_t avg_frame_bytes = 0;
  uint32_t bitrate_kbps = 0;   // h264_stream
  // OPS-13: h264 trigger-window slack. 0 keeps [t0-before, t0+after].
  // Pre start walks back to the last IDR, at most this far earlier. Post
  // keeps frames after t0+after until the next IDR, and never past this.
  uint64_t gop_ns = 0;
  // REC-21 per-channel CRC policy ([channel.*] crc = package|frame|off):
  //   kFrame   — per-frame CRC verified at drain (pinned_slot) / per-record CRC
  //              kept (mirror small structs; .tbin records are always CRC'd);
  //   kPackage — per-frame transport CRC off, drain skips per-frame
  //              verification; the REC-18 package manifest is the integrity
  //              boundary. The drain still records a payload_crc per frame
  //              index row so `thyme-replay verify` can recompute on demand;
  //   kOff     — like kPackage but the frame index payload_crc is 0: no
  //              per-frame anchor at all, package manifest only.
  // kDefault resolves by kind (EffectiveCrcPolicy): bulk pinned_slot/h264
  // channels default to kPackage, small-struct mirror rings to kFrame.
  enum class CrcPolicy : uint8_t {
    kDefault = 0,
    kPackage = 1,
    kFrame = 2,
    kOff = 3,
  };
  CrcPolicy crc = CrcPolicy::kDefault;
  enum class ManifestPolicy : uint8_t {
    kDefault = 0,
    kFull = 1,
    kMetadata = 2,
    kOff = 3,
  };
  // REC-23: bulk-only override of [security].manifest. Small-struct/control
  // channels reject metadata/off overrides.
  ManifestPolicy manifest = ManifestPolicy::kDefault;
  // REC-27: opaque big-frame payload identity. Core stores the identifier,
  // version and a digest of the parameter string; it never parses params.
  // Empty profile means "no decoder name" and is the default.
  std::string payload_profile;
  uint32_t payload_profile_version = 0;
  std::string payload_profile_params;
  uint64_t payload_profile_digest = 0;
};

struct Config {
  std::vector<SourceConfig> sources;
  std::string out_dir;  // package root; created if missing
  // SEC-03: raw HMAC-SHA256 key file for signed evidence manifests. Empty =
  // unsigned manifests. A configured but unreadable/empty key fails Init and
  // any seal (fail closed: no silent unsigned downgrade).
  std::string signing_key_path;
  integrity::ManifestMode manifest_mode = integrity::ManifestMode::kFull;
  // Byte budget for the pending swap-descriptor queue (sum of payload lens).
  // Overflow evicts the OLDEST descriptors (they become unaccounted holes,
  // counted in Stats::pending_evicted). The same quota also sizes each
  // descriptor ring — see DescriptorRingCapacity().
  uint64_t pending_bytes_quota = 64ull * 1024 * 1024;
  uint64_t reattach_scan_ns = 200000000ull;  // session/replacement scan period
  uint64_t attach_timeout_ns = 2000000000ull;
  // ABI-09/ABI-10: GEN-12 record-manifest identity + contract layout lookup
  // from the host's generated record table (`GetThymeRecordTable().manifest`).
  // When set, every package carries the CRC-protected manifest block in
  // meta.tbin and a field-level schema.json sidecar; when null the package
  // keeps the pre-ABI-10 file format and the sidecar reports the layout as
  // unavailable (never a guessed layout). The pointer is borrowed for the
  // recorder's lifetime. This is a producer/decode ABI boundary: a package
  // written under one manifest must not be decoded as if it were another.
  const schema::RecordManifest* record_manifest = nullptr;
  // After the post deadline, keep sweeping until a full sweep collects
  // nothing new AND this grace has elapsed (covers the MonoNs-stamp to
  // ring-head-store window inside the producer's tap — a preempted thread can
  // sit there for more than a millisecond on a loaded box).
  uint64_t post_grace_ns = 20000000ull;  // 20 ms (was 2 ms: too tight, R4)
  uint32_t settle_batch = 256;  // descriptors drained per Tick
  // T2: drain time slice per Tick — a long settle must not starve the
  // trigger socket. The batch count is the primary bound; this wall-clock
  // budget caps the per-Tick cost of big frames.
  uint64_t drain_tick_budget_ns = 5000000ull;
  // 6.3 rate shaping, applied to idle-flushed (deferred) captures only;
  // realtime/crash captures drain unthrottled. 0 = unlimited.
  uint64_t io_rate_limit_bps = 0;
  uint64_t io_burst_bytes = 4ull * 1024 * 1024;
  // ARCH-03: continuous-mode slice roll is GOP-aligned when the channel is an
  // h264 stream; if the next IDR does not arrive within this bound the slice
  // is cut anyway (counted in ContinuousStats::roll_forced). The bound is a
  // host policy (slice cadence vs GOP loss), not a recorder mechanism.
  uint64_t gop_roll_timeout_ns = 5000000000ull;  // 5 s
  // Fault injection (tests): when set and it returns true, drain writes fail
  // as if ENOSPC — the capture is aborted unsealed and disk_full_ set.
  void* fault_ctx = nullptr;
  bool (*fault_inject_write)(void* ctx) = nullptr;
  // Seal-path fault injection (tests): 0 = ok; 1 = wal seal fwrite fails;
  // 2 = wal fsync fails after the seal row reached the page cache;
  // 3 = a data-file fsync fails; 4 = the ABI-09 schema sidecar write fails.
  // Any non-zero value must end in package invalidation (abort row +
  // .incomplete rename), never a self-sealed broken package.
  int (*fault_inject_seal)(void* ctx) = nullptr;
  // Test hook (P2-a regression): invoked inside Trigger() after the t0 stamp
  // and BEFORE the post-cursor snapshot + HarvestPre — lets a test publish a
  // sample/frame exactly into the pre/post handoff window. Uses fault_ctx.
  void (*test_hook_after_t0)(void* ctx) = nullptr;
  // Crash-recovery regression: invoked after the durable sealing transition,
  // before package finalization. Uses fault_ctx.
  void (*test_hook_after_sealing)(void* ctx) = nullptr;
};

// thyme_bin index record for one raw frame segment (frames/<channel>.index.tbin
// rows, design doc 6.1). payload_crc is CRC32 over the raw segment as
// written (R2): replay/verification recomputes from the .raw bytes and a
// mismatch degrades the frame to a GAP report with its offset.
// REC-28: v2 adds the producer's publish index. v1 records ended at 24 bytes
// (offset/len/flags/payload_crc plus the 4-byte tail padding this struct had);
// the first 20 bytes are unchanged, so a v2 reader can memcpy a v1 row and
// leave producer_frame_index at 0. `reserved` makes that old padding explicit.
struct FrameIndexRecord {
  uint64_t offset;
  uint32_t len;
  uint32_t flags;
  uint32_t payload_crc;
  uint32_t reserved;              // v1 tail padding; always 0
  uint64_t producer_frame_index;  // v2: PinnedSlotReader::View::frame_index, 0 on v1
};
static_assert(sizeof(FrameIndexRecord) == 32,
              "FrameIdx v2 is 32 bytes (v1 rows are 24 and stay readable)");
static_assert(offsetof(FrameIndexRecord, producer_frame_index) == 24,
              "producer frame index starts where v1 rows ended");

struct Stats {
  uint64_t captures_done = 0;
  uint64_t triggers_rejected = 0;  // Trigger while not Armed
  uint64_t descriptors_queued = 0;
  uint64_t pending_evicted = 0;    // queue-quota evictions (oldest first)
  uint64_t overrun_before_queue = 0;  // samples evicted from shm before the
                                      // post sweep could even queue them
  uint64_t samples_written = 0;    // small-struct data records
  uint64_t frames_written = 0;     // big-frame raw segments + index records
  uint64_t holes_written = 0;      // GAP records (overwritten before drain)
  uint64_t torn_reads = 0;         // seqlock tears observed while harvesting
  // RT-09: pre-window harvest re-sweeps (bounded by kHarvestSweepRetries).
  // Incremented only when a sweep observed a new tear, so the normal path is
  // free.
  uint64_t harvest_retries = 0;
  uint64_t session_reattaches = 0;  // source re-attach after shm replacement
  uint64_t source_errors = 0;       // re-attach failures (e.g. config drift)
  uint64_t disk_errors = 0;         // drain write failures (6.5 ENOSPC path)
  // ARCH-05: durability steps that failed (abort-row write/fsync and the
  // directory fsync at seal/invalidate). The package is still sealed by its
  // wal row; this counter keeps a failed write/fsync from being invisible.
  uint64_t sync_errors = 0;
};

// Window metadata record (kMetaStructId) — ThymeBinReader kTrigger entries
// expose pre/post roll since the API-gap fix, this record additionally pins
// the per-source config hashes for drift forensics.
struct TriggerMetaRecord {
  uint64_t t0;
  uint64_t before_ns;
  uint64_t after_ns;
  uint64_t config_hash;  // first mirror source's hash (0 if none)
};

// REC-27: one meta.tbin row per recorded big-frame channel. Written once at
// channel open (Trigger / OpenSlice), never on the per-frame path. `profile`
// empty and flags=0 mean the package has no decoder name.
struct PayloadProfileRecord {
  char channel[32];
  char profile[56];
  uint32_t profile_version;
  uint32_t flags;
  uint64_t params_digest;
};
static_assert(sizeof(PayloadProfileRecord) == 104,
              "PayloadProfileRecord is 104 bytes");

// Drain statistics record, appended to meta.tbin as the last entry.
struct CaptureStatsRecord {
  uint64_t descriptors;
  uint64_t samples_written;
  uint64_t frames_written;
  uint64_t holes_written;
  uint64_t pending_evicted;
  uint64_t torn_reads;
  uint64_t flags;  // kCaptureFlag*
};
// CaptureStatsRecord::flags bit0: the capture was truncated by a higher-
// priority preemption (post window abandoned, queued data sealed as-is).
inline constexpr uint64_t kCaptureFlagTruncated = 1ull << 0;

// ---- continuous mode (design 6.6): same pipeline, infinite window ----

// meta.tbin record (kMetaStructId, source_sequence = 2) marking a
// continuous-mode slice package; written at slice close. detail: for
// GOP-aligned rolls, the mono_ns of the IDR frame the cut was aligned to
// (0 for byte/time-boundary cuts and non-h264 channels).
struct ContinuousMetaRecord {
  uint64_t slice_seq;
  uint64_t begin_mono_ns;
  uint64_t end_mono_ns;
  uint64_t samples;
  uint64_t frames;
  uint64_t holes;
  uint64_t evicted;  // quota evictions from the continuous queue this slice
  uint64_t detail;
};

// Shared source/continuous channels whitelist/exclude/decimate rules:
//   "small_struct:*"                 all mirror sources (continuous default)
//   "small_struct:<glob>"            the matching mirror sources
//   "pinned_slot:<glob>@dec=N"          the matching big-frame channels, record
//                                     1 frame every N
//   "<name-or-glob>@dec=N"           no kind prefix, matches name and shm name
//   A leading '!' marks an exclusion and exclusions win, e.g.
//   "!small_struct:/thyme_recr_1234".
// A specific include overrides the decimate of a '*' include; h264_stream
// cannot be decimated (GOP).
struct ChannelSelectorRule {
  bool exclude = false;
  bool has_kind = false;
  bool pinned_slot = false;
  std::string selector;
  uint32_t decimate = 1;
};
using ContinuousChannelRule = ChannelSelectorRule;

struct ContinuousConfig {
  uint64_t slice_bytes = 512ull * 1024 * 1024;  // slice_mb
  uint64_t slice_ns = 0;   // slice_s; 0 = do not roll by time
  bool wait_idle = false;  // true: hang on the same idle gate (driven by
                           // SetContinuousGate). Note: wait_idle does not
                           // combine well with big frames — frames are
                           // overwritten while the gate is held and counted in
                           // ContinuousStats::pinned_slot_drops.
  // Shared selector list (see ChannelSelectorRule); empty = {"small_struct:*"}.
  std::vector<std::string> channels;
};

struct ContinuousStats {
  uint64_t slices_done = 0;
  uint64_t samples_written = 0;
  uint64_t frames_written = 0;
  uint64_t holes_written = 0;
  uint64_t queue_evicted = 0;   // continuous-queue evictions under shared quota
  uint64_t torn_reads = 0;
  uint64_t bytes_written = 0;
  uint64_t pinned_slot_drops = 0;  // frames gap-skipped (lag overrun) by streaming drain
  uint64_t roll_forced = 0;     // GOP alignment timed out and the slice was forced
  // REP-07 follow-up: a collected sample arrived with a mono_ns below the
  // slice's already-written watermark (a late publish from another struct,
  // i.e. an out-of-order tick). It is demoted to a GAP at the watermark —
  // counted here, never written out of order.
  uint64_t out_of_order = 0;
};

// h264 FrameHeader.flags bit convention (PinnedSlotStore is payload-agnostic;
// the producer sets this bit between Begin/Commit and the engine uses it for
// GOP-aligned slicing).
inline constexpr uint32_t kH264FlagIsIdr = 1u;

// wal.log trigger_id high mark for continuous slices (never a real trigger).
inline constexpr uint64_t kContinuousWalMark = 0xC07C000000000000ULL;

// wal.log journal format (package ABI — retention, replay tools, and tests
// share these): header then fixed 24-byte rows; the last row of a completed
// capture has op == kWalOpSeal.
struct WalHeader {
  char magic[8];  // kWalMagic
  uint64_t trigger_id;
};
struct WalRow {
  uint8_t op;      // kWalOp*
  uint8_t result;  // descriptor: kWalOk/kWalHole; lifecycle: abort reason
  uint16_t struct_id;  // lifecycle: PackageLifecycle value
  uint32_t len;        // lifecycle rows: 0
  // Descriptor rows: sample/frame index. Clock-anchor rows: CLOCK_MONOTONIC.
  // Lifecycle rows: CLOCK_REALTIME of the transition.
  uint64_t index;
  // Descriptor rows: the sample/frame CLOCK_MONOTONIC stamp. Seal rows
  // (TRG-14): CLOCK_MONOTONIC of the seal journal commit — the durable
  // per-package anchor for trigger->seal latency (pair with the header's
  // trigger_id). 0 on packages sealed before TRG-14 = unknown. Clock-anchor
  // rows: CLOCK_REALTIME paired with the monotonic timestamp in index.
  // Lifecycle rows: CLOCK_MONOTONIC of the transition.
  uint64_t mono_ns;
};
static_assert(sizeof(WalRow) == 24, "wal row layout is package ABI");
inline constexpr char kWalMagic[8] = {'T', 'R', 'I', 'G',
                                      'W', 'A', 'L', '1'};
inline constexpr uint8_t kWalOpDescriptor = 0;
inline constexpr uint8_t kWalOpClockAnchor = 1;
inline constexpr uint8_t kWalOpLifecycle = 2;
inline constexpr uint8_t kWalOpSeal = 0xFF;
// Written (in place of / after a page-cached seal row) when sealing fails
// mid-path: a package whose wal ends in abort is never complete, even if the
// seal bytes eventually reach disk.
inline constexpr uint8_t kWalOpAbort = 0xFE;
inline constexpr uint8_t kWalOk = 0;
inline constexpr uint8_t kWalHole = 1;

enum class PackageLifecycle : uint8_t {
  kUnknown = 0,
  kArmed = 1,
  kCapturing = 2,
  kSealing = 3,
  kSealed = 4,
  kAborted = 5,
};

enum class PackageAbortReason : uint8_t {
  kNone = 0,
  kUnspecified = 1,
  kWriteFailure = 2,
  kSealFailure = 3,
  kCrashRecovery = 4,
};

struct PackageLifecycleState {
  PackageLifecycle state = PackageLifecycle::kUnknown;
  PackageAbortReason abort_reason = PackageAbortReason::kNone;
  uint64_t mono_ns = 0;
  uint64_t realtime_ns = 0;
  uint64_t session_id = 0;
  bool legacy = false;  // Valid pre-lifecycle WAL, state inferred from seal/abort.
};

const char* PackageLifecycleName(PackageLifecycle state);
const char* PackageAbortReasonName(PackageAbortReason reason);
// Parses the same WAL used by PackageComplete. Invalid/truncated journals fail
// closed. Old valid journals are reported with legacy=true; an unsealed legacy
// journal is kCapturing because its more precise phase was not persisted.
bool ReadPackageLifecycle(const std::string& package_dir,
                          PackageLifecycleState* out);
// Startup recovery commits a terminal abort to a valid, non-terminal WAL.
// Existing sealed/aborted packages are left untouched. Returns false for an
// invalid WAL or an I/O failure; the caller still applies its incomplete policy.
bool AbortPackageAfterCrash(const std::string& package_dir);

// TRG-18: name a [channel.*] entry is addressed by in [source.*].channels.
// `name` when configured, otherwise the shm name; mirror-ring channels
// contribute all of their small structs.
std::string ChannelSourceName(const SourceConfig& source);
// Shared source/continuous parser and resolver. Exclusions win over includes;
// a non-'*' include overrides the decimation of a '*' include. The returned
// rule pointer is borrowed from `rules`; nullptr means not selected.
bool ParseChannelSelectors(const std::vector<std::string>& entries,
                           std::vector<ChannelSelectorRule>* out,
                           std::string* err);
const ChannelSelectorRule* MatchChannelSelector(
    const std::vector<ChannelSelectorRule>& rules,
    const SourceConfig& source);
// True when struct index sid of an aggregated mirror should be recorded.
// Empty rules or empty record_channels keep every struct. On a member match,
// *decimate (when non-null) becomes that rule's decimate.
bool SelectRecordStruct(const std::vector<ChannelSelectorRule>& rules,
                        const SourceConfig& source, uint16_t sid,
                        uint32_t* decimate);
// Compatibility wrapper for one source selector (empty lists mean all at the
// Trigger/ContinuousConfig layer, not here).
bool ChannelSelectorMatches(const std::string& selector,
                            const SourceConfig& source);

// REC-21: resolve a source's effective CRC policy — kDefault maps to
// kPackage for bulk pinned_slot/h264 channels (per-frame transport CRC off, the
// REC-18 package manifest is the integrity boundary) and to kFrame for
// small-struct mirror rings (per-frame/per-record CRC kept; producers may
// still opt a channel out with crc=off via the ABI-12 no-CRC seal).
SourceConfig::CrcPolicy EffectiveCrcPolicy(const SourceConfig& source);
// "package" | "frame" | "off"; kDefault resolves first (never "default").
const char* CrcPolicyName(SourceConfig::CrcPolicy policy);
// Parse the [channel.*] crc key value; accepts exactly package|frame|off.
bool ParseCrcPolicy(const std::string& text, SourceConfig::CrcPolicy* out);
integrity::ManifestMode EffectiveManifestMode(const SourceConfig& source,
                                              integrity::ManifestMode default_mode);
const char* ManifestPolicyName(SourceConfig::ManifestPolicy policy);
bool ParseManifestPolicy(const std::string& text, SourceConfig::ManifestPolicy* out);

class TriggeredRecorder {
 public:
  // Out-of-line ctor/dtor: SourceState is an incomplete type here (pimpl).
  TriggeredRecorder();
  ~TriggeredRecorder();
  TriggeredRecorder(const TriggeredRecorder&) = delete;
  TriggeredRecorder& operator=(const TriggeredRecorder&) = delete;

  // Attaches every source (strict: any failure fails Init).
  bool Init(const Config& cfg);
  // Why the last Init stopped. kBusy: a live writer already holds out_dir.
  // Earlier failures are kInvalidArgument. Success is kOk or kStolen.
  SessionLeaseResult init_lease_result() const { return init_lease_result_; }
  void Shutdown();

  Phase phase() const { return phase_; }
  const Stats& stats() const { return stats_; }
  const std::string& last_package_dir() const { return last_package_dir_; }
  integrity::ManifestMode integrity_mode() const { return integrity_mode_; }

  // TRG-14: the last successfully sealed triggered capture — CLOCK_MONOTONIC
  // of full seal completion (all files fsync'ed, wal seal row + REC-18
  // manifest written, package dir sync'ed) and its trigger id. This is the
  // SLO endpoint for trigger->seal latency; the start anchor is the center's
  // OBS-05 FSM kSubmitAccept event for the same trigger id. Both are 0 before
  // the first seal and unchanged by aborted/invalidated captures.
  uint64_t last_seal_mono_ns() const { return last_seal_mono_ns_; }
  uint64_t last_seal_trigger_id() const { return last_seal_trigger_id_; }

  // t0_mono_ns == 0 -> "now" (CLOCK_MONOTONIC). Only valid in kArmed;
  // otherwise counted (triggers_rejected) and false.
  // defer_settle: idle flush policy (design 6.2) — after the post window the
  // engine parks in kSettle WITHOUT draining until AllowFlush(); the host's
  // idle detector drives that call. Preempt() bypasses the gate.
  // channels (TRG-16): shared selector list; only selected channels and
  // decimated samples are harvested, manifested, and written. Empty = all
  // (the pre-TRG-18 behaviour). Invalid/zero-match/h264-decimated lists fail.
  bool Trigger(uint64_t trigger_id, uint64_t before_ns, uint64_t after_ns,
               uint64_t t0_mono_ns = 0, bool defer_settle = false,
               bool high_priority = false,
               const std::vector<std::string>& channels = {});

  // Idle-flush gate release (host idle detector). No-op outside kSettle.
  void AllowFlush();

  // Preemption (design 5.3): truncate the in-flight capture — post window
  // abandoned, already-queued descriptors drained and sealed immediately with
  // CaptureStatsRecord.flags |= kCaptureFlagTruncated. Returns false when
  // Armed (nothing to preempt).
  bool Preempt();

  // Tied to the memory quota (design 5.4/6.6): live pending bytes across BOTH
  // buckets (triggered + continuous queues share the quota), and eviction of
  // the oldest queued descriptors until `bytes` have been freed (continuous
  // first — continuous slices have the lowest priority). Returns freed bytes;
  // counted in pending_evicted.
  uint64_t pending_bytes() const { return pending_bytes_ + cont_pending_bytes_; }
  uint64_t EvictOldest(uint64_t bytes);
  // Evict helpers shared by quota enforcement: continuous bucket is drained
  // first (continuous slices have the lowest priority), then the triggered
  // queue.
  uint64_t EvictContinuous(uint64_t bytes);
  void PushContinuous(uint16_t source, bool pinned_slot, uint16_t struct_id,
                      uint64_t index, uint64_t mono_ns, uint32_t len,
                      uint32_t slot_seq);

  // REC-20 descriptor-count bound: capacity of each fixed ring, derived from
  // the pending byte quota as `quota / 1 KiB` (one descriptor per KiB of
  // accounting budget), clamped to [256, 65536] and rounded up to a power of
  // two. A SwapDescriptor is 40 bytes, so the two rings reserve at most
  // 2 * 65536 * 40 B = 5 MiB regardless of quota; at the default 64 MiB quota
  // the derived capacity is the 65536 cap. When the ring fills before the byte
  // quota, the OLDEST descriptor is evicted and counted exactly like a
  // byte-quota eviction (Stats::pending_evicted / ContinuousStats::
  // queue_evicted) — no descriptor is ever dropped uncounted.
  static uint32_t DescriptorRingCapacity(uint64_t pending_bytes_quota);

  // 6.5 disk-full state: set when a drain write fails (ENOSPC); the current
  // package is left unsealed (partial) and Trigger() refuses new captures until
  // ClearDiskFull() (the host runs retention between the two).
  bool disk_full() const { return disk_full_; }
  void ClearDiskFull() { disk_full_ = false; }

  // All sources currently attached (drives the center's kRecorderOffline).
  bool ready() const;

  // True while a crash-level (high/ zone) capture is in flight — the host
  // raises drain I/O priority for it (6.3).
  bool capture_high_priority() const {
    return high_priority_ && phase_ != Phase::kArmed;
  }

  // ---- continuous mode (6.6) ----
  // Arm starts continuous draining from the CURRENT heads (no backfill) and
  // rolls sealed slice packages continuous_<seq>_<ts>/ under out_dir. Safe to
  // call in any phase; continuous collection never interrupts a triggered
  // capture (separate descriptor bucket, shared pending_bytes quota —
  // continuous is evicted first under pressure, triggered packages first).
  bool ArmContinuous(const ContinuousConfig& cfg);
  void DisarmContinuous();  // seals the current slice
  bool continuous_armed() const { return cont_armed_; }
  const ContinuousStats& continuous_stats() const { return cont_stats_; }
  // Active slice directory (for retention exclusion); empty when disarmed.
  const std::string& current_slice_dir() const { return cont_dir_; }
  // Level-driven idle gate for continuous drain (driven by the host every tick
  // when wait_idle=true; unlike the one-shot AllowFlush of triggered mode).
  void SetContinuousGate(bool open) { cont_gate_open_ = open; }

  // Parse the channels whitelist/exclusion syntax (see ContinuousChannelRule).
  // Shared by ArmContinuous and the config layer's cross-validation.
  static bool ParseContinuousChannels(
      const std::vector<std::string>& entries,
      std::vector<ContinuousChannelRule>* out, std::string* err);

  // Host-driven work quantum: re-attach scan (Armed), post-window harvest
  // (Post), bounded drain (Settle). Never blocks on I/O beyond the batch.
  void Tick();

  // Package completeness: the parsed WAL lifecycle is sealed and its final
  // legacy seal row is present. An interrupted Settle is sealing/unsealed.
  static bool PackageComplete(const std::string& package_dir);

 private:
  struct SourceState;
  struct SwapDescriptor {
    uint16_t source = 0;   // index into sources_
    uint16_t struct_id = 0;  // mirror struct id (0 for pinned_slot)
    uint8_t pinned_slot = 0;
    uint8_t pad0 = 0;
    uint32_t pad1 = 0;
    uint64_t index = 0;     // publish_index / frame_index
    uint64_t mono_ns = 0;
    uint32_t len = 0;
    uint32_t slot_seq = 0;  // seqlock value at harvest; drain re-validates
  };

  // Fixed-capacity FIFO of SwapDescriptors (REC-20). Storage is assigned once
  // in Init() and never grows: push/pop are index updates on a preallocated
  // std::vector, so harvest/enqueue and drain/dequeue perform no heap
  // allocation. Oldest entry is at the front, exactly like the std::deque it
  // replaces. push_back() returns false only when the ring was never sized
  // (Init not run); callers count that as an eviction, never as a silent drop.
  struct DescriptorRing {
    std::vector<SwapDescriptor> slots;  // size == capacity, fixed after Init
    uint32_t head = 0;  // index of the oldest descriptor
    uint32_t count = 0;

    void Reset(uint32_t capacity) {
      slots.assign(capacity, SwapDescriptor{});
      head = 0;
      count = 0;
    }
    uint32_t capacity() const { return static_cast<uint32_t>(slots.size()); }
    bool empty() const { return count == 0; }
    bool full() const { return count == slots.size(); }
    uint32_t size() const { return count; }
    SwapDescriptor& front() { return slots[head]; }
    const SwapDescriptor& front() const { return slots[head]; }
    void pop_front() {
      if (++head == slots.size()) head = 0;
      --count;
    }
    bool push_back(const SwapDescriptor& d) {
      if (slots.empty() || count == slots.size()) return false;
      uint32_t tail = head + count;
      if (tail >= slots.size()) tail -= static_cast<uint32_t>(slots.size());
      slots[tail] = d;
      ++count;
      return true;
    }
    void clear() {
      head = 0;
      count = 0;
    }
  };
  static_assert(sizeof(SwapDescriptor) == 40,
                "descriptor ring memory bound assumes a 40-byte descriptor");

  // Ring-bound overflow (descriptor count before byte quota): drop the oldest
  // and account it exactly like a quota eviction.
  void DropOldestTriggered();
  void DropOldestContinuous();

  bool AttachSource(SourceState& s, bool initial);
  // ARCH-14: refresh the topology lease. False when this recorder is no
  // longer the holder (a live peer was not overwritten; the lease was lost).
  bool RefreshSessionLease();
  void ScanSources(uint64_t now);
  // TRG-18: is source si inside the active trigger's channel filter?
  bool TriggerIncluded(uint16_t source_idx) const {
    return trigger_mask_.empty() ||
           (source_idx < trigger_mask_.size() && trigger_mask_[source_idx] != 0);
  }
  void HarvestPre(uint64_t t0);
  uint64_t CollectPost();  // returns descriptors collected this sweep
  void FinishPost();
  // True while an included h264 source still owes a closing IDR and its
  // gop_ns bound has not elapsed.
  bool GopPostOpen(uint64_t now) const;
  // REP-07: order the queued descriptors by (mono_ns, source, kind, struct,
  // index) before the first drain. Harvest enqueues struct-by-struct, but
  // small.tbin is a single file across every struct: the replay reader's
  // per-source monotonicity contract (replay_reader.h) needs the drain to
  // write it in time order. One bounded, allocation-free sort of the
  // preallocated descriptor ring; runs once per capture, before any I/O.
  void SortQueueByMono();
  // REP-07 follow-up (continuous mode): same ordering, applied to the
  // continuous pending window at the start of every drain pass. Collect
  // enqueues struct-by-struct and the drain is tick-driven, so without this
  // a multi-struct slice writes struct-major rows and small.tbin regresses
  // in mono_ns. Sorting the pending ring (bounded by DescriptorRingCapacity,
  // at most one per-tick span in steady state) is allocation-free and keeps
  // the oldest-first eviction order meaningful.
  void SortContinuousQueue();
  void SortRingByMono(DescriptorRing& ring);
  static bool DescriptorLess(const SwapDescriptor& a, const SwapDescriptor& b);
  void DrainSettle();
  void PushDescriptor(uint16_t source, bool pinned_slot, uint16_t struct_id,
                      uint64_t index, uint64_t mono_ns, uint32_t len,
                      uint32_t slot_seq);
  // Drains one descriptor; returns disk bytes written (rate-bucket input).
  // fatal=true on write failure (ENOSPC path): the capture aborts unsealed.
  uint64_t DrainOne(const SwapDescriptor& d, bool& fatal);
  void AbortCapture();  // ENOSPC: drop queue, persist abort, set disk_full_
  bool WriteWalRow(uint8_t result, const SwapDescriptor& d);
  // Seal-path failure invalidation: overwrite/append an abort row in the wal
  // (best-effort fsync) and rename the package dir to <dir>.incomplete, so a
  // seal row that already reached the page cache can never produce a
  // "self-sealed" broken package. Closes *wal_pp. REC-18: after the rename a
  // PARTIAL integrity manifest (info.partial forced true, seal_time_ns =
  // invalidation time) is written over the surviving files, best-effort — a
  // manifest failure here (e.g. ENOSPC, the usual abort cause) is logged and
  // leaves the invalidated package without a manifest.
  void InvalidatePackage(FilePtr* wal_pp, bool terminal_rows_written,
                         const WalRow* overwritten_rows,
                         uint32_t overwritten_row_count,
                         PackageAbortReason reason, const std::string& dir,
                         integrity::PackageInfo info,
                         const std::vector<ThymeStructDesc>& registry,
                         integrity::PackageHashState* hashes);
  // ABI-09: schema sidecar over the final file set. Called before the
  // integrity manifest so the sidecar is covered by the package hash chain.
  // A failure on the seal path fails the seal (fail closed); the invalidation
  // path is best-effort (the abort is usually ENOSPC, the same cause).
  bool WriteSealedSchema(const std::string& dir,
                         const std::vector<ThymeStructDesc>& registry,
                         integrity::PackageHashState* hashes);
  void WritePartialSchema(const std::string& dir,
                          const std::vector<ThymeStructDesc>& registry,
                          integrity::PackageHashState* hashes);
  void CollectPayloadProfiles(bool continuous,
                              std::vector<schema::PayloadProfileDesc>* out) const;
  bool WritePayloadProfileRows(ThymeBinEncoder* enc, uint64_t mono_ns,
                               uint64_t* rec_seq, bool continuous);
  // ABI-10: manifest block for the active build, or nullptr when the host
  // supplied none (pre-ABI-10 format, byte-identical to before).
  const ThymeBinManifestBlock* CaptureManifestBlock() const {
    return has_manifest_block_ ? &manifest_block_ : nullptr;
  }

  // Manifest identity of the active capture / active slice (everything but
  // seal_time_ns, which the caller stamps). Shared by the seal path and the
  // REC-18 partial manifest written on invalidation.
  integrity::PackageInfo TriggerPackageInfo() const;
  integrity::PackageInfo SlicePackageInfo(uint64_t end_ns) const;
  void ApplyManifestPolicy(integrity::PackageInfo* info) const;
  bool HashRawPayload(const SourceConfig& source) const;
  bool WriteClockAnchor(std::FILE* wal,
                        integrity::PackageHashState* hashes, uint64_t mono_ns);
  bool WriteLifecycleState(std::FILE* wal,
                           integrity::PackageHashState* hashes,
                           PackageLifecycle state,
                           PackageAbortReason reason = PackageAbortReason::kNone);
  void WriteClockAnchors(uint64_t now, bool force);
  void CheckpointHashes(uint64_t now, bool force);

  static uint64_t MonoNs();
  static bool ShmIdentity(const char* name, uint64_t& ino, uint64_t& size);
  static bool Mkdirs(const std::string& dir);

  // ---- continuous mode state (6.6) ----
  void TickContinuous(uint64_t now);
  void CollectContinuous();
  void DrainContinuous(uint64_t now);
  // Streaming drain of whitelisted big-frame channels (no descriptor quota
  // use; goes through the rate bucket).
  void StreamPinnedSlots(uint64_t now);
  void RollSlice(uint64_t now, uint64_t cut_detail);
  void OpenSlice(uint64_t now);
  bool CloseSlice(uint64_t now, uint64_t cut_detail);  // false = sealing failed
  bool ContIncluded(uint16_t source_idx) const;
  const ContinuousChannelRule* ContRule(uint16_t source_idx) const;
  bool ContHasH264() const;  // an h264_stream channel is whitelisted and capturing
  uint64_t DrainContOne(const SwapDescriptor& d, bool& fatal);

  bool cont_armed_ = false;
  ContinuousConfig cont_cfg_;
  std::vector<ContinuousChannelRule> cont_rules_;
  ContinuousStats cont_stats_;
  uint64_t cont_seq_ = 0;
  uint64_t cont_slice_begin_ns_ = 0;
  uint64_t cont_slice_bytes_ = 0;
  uint64_t cont_slice_descriptors_ = 0;
  uint64_t cont_slice_evicted_ = 0;
  bool cont_gate_open_ = true;  // always open when wait_idle=false
  // GOP-aligned roll: once the byte/time limit is reached, set roll_pending;
  // an h264 channel waits for the next IDR.
  bool cont_roll_pending_ = false;
  uint64_t cont_roll_since_ns_ = 0;
  uint64_t cont_last_frame_mono_ = 0;
  std::string cont_dir_;
  DescriptorRing cont_queue_;  // separate bucket: independent of the trigger queue
  // Set by every successful PushContinuous; cleared by SortContinuousQueue()
  // once the pending window has been ordered for the next drain pass.
  bool cont_queue_dirty_ = false;
  // Per-slice watermark: the mono_ns of the last row written to small.tbin.
  // A descriptor below it cannot be appended without breaking the file's
  // monotonic contract, so it is demoted to a GAP (ContinuousStats::
  // out_of_order). Reset by OpenSlice (a new file starts a new order).
  uint64_t cont_small_last_mono_ = 0;
  uint64_t cont_pending_bytes_ = 0;
  integrity::PackageHashState cont_hashes_;
  std::unique_ptr<ThymeBinFileSink> cont_meta_sink_;
  std::unique_ptr<ThymeBinEncoder> cont_meta_enc_;
  std::unique_ptr<ThymeBinFileSink> cont_small_sink_;
  std::unique_ptr<ThymeBinEncoder> cont_small_enc_;
  bool cont_small_open_ = false;
  uint64_t cont_small_rec_seq_ = 0;
  FilePtr cont_wal_;
  struct ContFrameOut {
    FilePtr raw;
    std::string raw_path;
    std::unique_ptr<ThymeBinFileSink> idx_sink;
    std::unique_ptr<ThymeBinEncoder> idx_enc;
    uint64_t rec_seq = 0;
    uint64_t raw_off = 0;
    bool open = false;
  };
  std::vector<ContFrameOut> cont_frame_outs_;  // parallel to sources_
  io::TokenBucket cont_bucket_;  // continuous realtime rate limit (6.3/6.6)

  Config cfg_;
  integrity::ManifestMode integrity_mode_ = integrity::ManifestMode::kFull;
  std::vector<std::unique_ptr<SourceState>> sources_;
  Phase phase_ = Phase::kArmed;
  Stats stats_;
  // ABI-10: pinned manifest block for this build (see Config::record_manifest).
  ThymeBinManifestBlock manifest_block_{};
  bool has_manifest_block_ = false;
  // ABI-09: the registry of the active capture / slice, kept for the schema
  // sidecar written at seal/invalidation (the encoder has consumed its copy).
  std::vector<ThymeStructDesc> capture_registry_;
  std::vector<ThymeStructDesc> cont_registry_;
  std::vector<schema::PayloadProfileDesc> capture_profiles_;
  std::vector<schema::PayloadProfileDesc> cont_profiles_;

  // Active capture state.
  uint64_t trigger_id_ = 0;
  uint64_t t0_ = 0;
  uint64_t before_ns_ = 0;
  uint64_t after_ns_ = 0;
  uint64_t post_deadline_ = 0;
  std::string pkg_dir_;
  DescriptorRing queue_;
  uint64_t pending_bytes_ = 0;
  // TRG-16 selector rules and resolved filter for the active capture.
  std::vector<ChannelSelectorRule> trigger_rules_;
  std::vector<uint8_t> trigger_mask_;
  std::vector<uint32_t> trigger_decimate_;
  uint64_t capture_samples_ = 0;
  uint64_t capture_frames_ = 0;
  uint64_t capture_holes_ = 0;
  uint64_t capture_evicted_ = 0;
  uint64_t capture_torn_ = 0;
  uint64_t capture_queued_ = 0;
  uint64_t capture_flags_ = 0;
  // TRG-14: last fully sealed triggered capture (endpoint of the trigger->
  // seal latency SLO). Not touched by continuous slices or failed seals.
  uint64_t last_seal_mono_ns_ = 0;
  uint64_t last_seal_trigger_id_ = 0;
  bool defer_settle_ = false;   // idle flush policy for the active capture
  bool flush_allowed_ = true;   // host idle-detector gate
  bool high_priority_ = false;  // package goes to <out_dir>/high/
  bool disk_full_ = false;      // set on drain write failure (6.5)
  // 6.3 token bucket (active for idle-flushed captures with a rate limit).
  bool rate_limited_ = false;
  io::TokenBucket bucket_;
  std::string last_package_dir_;

  // Settle-side open files (closed/sealed when the queue drains).
  integrity::PackageHashState capture_hashes_;
  uint64_t last_checkpoint_ns_ = 0;
  uint64_t last_clock_anchor_ns_ = 0;
  std::unique_ptr<ThymeBinFileSink> meta_sink_;  // open from Trigger to seal
  std::unique_ptr<ThymeBinEncoder> meta_enc_;
  uint64_t meta_rec_seq_ = 0;
  std::unique_ptr<ThymeBinFileSink> small_sink_;
  std::unique_ptr<ThymeBinEncoder> small_enc_;
  bool small_open_ = false;
  uint64_t small_rec_seq_ = 0;
  uint64_t last_scan_ns_ = 0;
  FilePtr wal_;
  struct FrameOut {
    FilePtr raw;
    std::string raw_path;
    std::unique_ptr<ThymeBinFileSink> idx_sink;
    std::unique_ptr<ThymeBinEncoder> idx_enc;
    uint64_t rec_seq = 0;
    uint64_t raw_off = 0;
    bool open = false;
  };
  std::vector<FrameOut> frame_outs_;  // parallel to sources_ (pinned_slot only)
  std::vector<uint8_t> scratch_;      // drain payload scratch (max len seen)

  // ARCH-14: exclusive writer of out_dir. Released at the end of Shutdown.
  ShmRegion session_lease_;
  uint64_t session_lease_token_ = 0;
  SessionLeaseResult init_lease_result_ = SessionLeaseResult::kNotHeld;
};

}  // namespace triggered
}  // namespace thyme
