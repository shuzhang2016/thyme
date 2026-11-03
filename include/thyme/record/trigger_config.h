#pragma once

// trigger_config.h — trigger-center configuration (design doc 5.2/7).
//
// The design's canonical schema is YAML
// (docs/design/triggered-acquisition-design.md, chapter 7). The repo has no
// reusable generic YAML parser (tools/gen/gen_graph's yaml.cpp is
// Graph-schema-specific) and new dependencies are not allowed, so the on-disk
// format here is a minimal INI dialect with a 1:1 field mapping to the YAML
// schema:
//
//   [trigger_center]                <- trigger_center:
//   socket = /tmp/thyme_trigger.sock
//   max_concurrent_captures = 1
//   tick_ms = 1                      # host Tick period; sizes SO_RCVBUF
//   socket_poll_budget_us = 200      # per-Tick drain time budget
//   max_trigger_rate_hz = 10000      # admission sizing, not a rate limiter
//   dedup_window_entries = 1024      # V4 bounded response cache
//   dedup_window_ms = 60000          # V4 retry result retention
//   quota_state = none               # or a durable path; startup-only (T5)
//   default_source = manual          # fallback for unknown sources; unset = reject
//   crash_priority = 100             # priority >= this value is crash level
//
//   [recorder]                       <- recorder: the pending quota is owned by
//                                    #   the recorder; Center links to it via
//                                    #   the pending_bytes hook
//   output_dir = /opt/usr/triggered_data
//   pending_bytes_quota = 67108864
//   flush_policy = idle              # default when a source does not set it
//   # use_cgroup was removed: cgroup v2 io.max is deployment-side, parsing
//   # fails.
//
//   [channel./h264_front]            <- channels: list item (host side)
//   kind = pinned_slot                  # mirror_ring | pinned_slot | h264_stream
//   name = h264_front
//   # CRC policy (defaults by kind: pinned_slot/h264_stream -> package,
//   #   mirror_ring -> frame):
//   crc = package                    # package | frame | off
//                                    #   package: per-frame transport CRC off,
//                                    #     drain does not verify per frame, the
//                                    #     package-level integrity.manifest is
//                                    #     the safety net; the index still
//                                    #     records payload_crc for on-demand
//                                    #     recomputation by --verify
//                                    #   frame:   drain verifies the transport
//                                    #     CRC per frame (small-struct .tbin
//                                    #     per-record CRC is always kept)
//                                    #   off:     like package but the index
//                                    #     payload_crc=0 (no per-frame anchor);
//                                    #     mirror_ring channels reject package
//   manifest = metadata                # bulk override full|metadata|off;
//                                    #   mirror_ring only full, default inherits security
//   record_channels = front          # manifest structs of the generated aggregate mirror
//   # Recording parameters may be omitted, but continuous mode rejects this
//   # source when its metadata is incomplete:
//   rate_hz = 10                     # pinned_slot/small_struct
//   avg_frame_bytes = 1048576        # pinned_slot
//   bitrate_kbps = 4000              # h264_stream
//
//   [source.crash]                   <- trigger_sources: list item
//   channels = /h264_*, /cloud_*, /can_chassis
//                                    # shared with continuous glob/!/dec
//                                    # syntax; empty list / '*' = all. Every
//                                    # positive selector must match one
//                                    # [channel.*].
//   before_s = 10                    # decimals allowed; stored internally as ns
//   after_s = 5
//   priority = 100
//   mute_s = 0
//   flush_policy = realtime          # inherits [recorder].flush_policy when unset
//   quota_class = safety
//
//   [quota.safety]                   <- quotas.max_triggers_per_hour
//   max_triggers_per_hour = 0        # 0 = unlimited
//
//   [security]                       <- control-surface authz/audit
//   allowed_uids = 1000,1001         # empty/unset = only the center's own euid
//   allow_root = false               # extra explicit uid 0 allowance
//   require_peer_pid = true          # require SCM_CREDENTIALS with a valid pid
//   max_priority_hint = 1000         # request hint ceiling (can only lower)
//   allow_legacy_unsequenced = false # accept V1 88B requests (no replay
//                                    #   protection, not recommended)
//   allow_legacy_v2 = false          # accept V2 96B requests (no CRC,
//                                    #   downgrade warning)
//   allow_legacy_control = false     # accept recorder V1 text commands
//   (downgrade warning) signing_key = /path/to/key       # evidence-package
//   HMAC-SHA256 key file;
//                                    # empty/unset = manifests are unsigned
//   manifest = full                    # full|metadata|off; non-full must be
//                                    #   configured explicitly and is written
//                                    #   to the startup audit
//   audit_log = /var/log/....jsonl   # empty = <trigger socket>.audit
//   control_audit_log = /var/log/... # empty = <recorder socket>.audit
//   audit_max_bytes = 4194304        # rotation threshold (>0, one write/record)
//   audit_rotate_keep = 2            # keep <path>.1..N
//   audit_fsync_interval_s = 0       # 0 = fdatasync only at Shutdown
//
//   [logging]                        <- rotation/budget of non-package append logs
//   service_file = stderr            # or a service log path; an explicit open
//                                    #   failure refuses to start
//   service_max_bytes = 8388608      # service log rotation threshold (>0)
//   service_rotate_keep = 2          # keep .1..N (1..32)
//   fsm_max_bytes = 4194304          # <socket>.fsm rotation threshold (>0)
//   fsm_rotate_keep = 2              # keep .1..N (1..32)
//   perf_max_bytes = 67108864        # PerfLogDumper per-file threshold (>0)
//   perf_rotate_keep = 2             # keep .1..N (1..32)
//   total_budget_mb = 0              # process-wide log budget (0 = per-file
//                                    #   caps only); covers service+audit+fsm+
//                                    #   perf, packages are bounded separately
//                                    #   by [recorder].disk_quota_mb
//
//   [continuous]                     <- recorder.continuous: (6.6)
//   auto_arm = true                  # old name enable still works but warns
//   channels = small_struct:*, pinned_slot:/cloud_x@dec=3, !small_struct:/x
//                                      # whitelist/exclude/decimate (syntax in
//                                      #   triggered_recorder.h
//                                      #   ChannelSelectorRule;
//                                      #   dec=N on an h264_stream channel is
//                                      #   an error)
//   slice_mb = 512
//   slice_s = 0                      # 0 = do not roll by time
//   wait_idle = false
//   rate_check = warn                # startup rate check: enforce (reject) | warn | off
//
// Syntax: `#` comments, one `key = value` per line, comma-separated lists.
// Unknown keys/sections are errors (typo-safe), unknown source/channel names
// are not (free-form strings).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "thyme/record/idle_detector.h"
#include "thyme/record/triggered_recorder.h"

namespace thyme {
namespace trigger_cfg {

struct SourceRule {
  std::string name;
  std::vector<std::string> channels;  // shared selector syntax; empty = all
  uint64_t before_ns = 0;
  uint64_t after_ns = 0;
  int32_t priority = 0;
  uint64_t mute_ns = 0;
  bool flush_realtime = false;  // effective value (from [recorder] when unset)
  bool has_flush = false;       // explicit flush_policy
  std::string quota_class;
};

struct ChannelConfigMetadata {
  bool rate_hz_present = false;
  bool avg_frame_bytes_present = false;
  bool bitrate_kbps_present = false;
  std::vector<std::string> record_channels;
};

struct CenterConfig {
  std::string socket_path = "/tmp/thyme_trigger.sock";
  // The FSM only implements 1 for now; >1 is a parse error (no silent
  // downgrade).
  uint32_t max_concurrent_captures = 1;
  // TRG-19 ingress geometry (startup-only). SO_RCVBUF and the sysctl check use
  // max_trigger_rate_hz * tick_ns; PollSocket stops at EAGAIN, its time budget,
  // or Center's fixed per-Tick safety cap, whichever comes first.
  uint64_t tick_ns = 1000000ull;
  uint64_t socket_poll_budget_ns = 200000ull;
  uint32_t max_trigger_rate_hz = 10000;
  uint32_t dedup_window_entries = 1024;
  uint64_t dedup_window_ns = 60000000000ull;
  std::string default_source;  // empty = unknown sources rejected
  bool allow_empty_sources = false;  // explicitly allow no [source.*] (test/read-only center)
  int32_t crash_priority = 100;  // >= this value: preempt/quota override/mute bypass
  std::vector<SourceRule> sources;
  // quota_class -> max_triggers_per_hour (0 = unlimited); unlisted classes are
  // unlimited.
  std::vector<std::pair<std::string, uint32_t>> quota_limits;
  // T5: optional durable one-hour quota windows. Empty preserves the legacy
  // process-local behavior; when set, state I/O failures are fail-closed.
  std::string quota_state_path;

  // For a trigger carrying its own timestamp_ns, t0 must not be earlier than
  // now - the source's before_ns (the pre window would be all holes) and not
  // later than now + this tolerance (a far-out post deadline would hold the
  // FSM for a long time); out of bounds is rejected with kInvalidTimestamp.
  uint64_t timestamp_future_tolerance_ns = 5000000000ull;  // 5 s

  // [security] (SEC-02). allowlist empty = only the center's own euid, which
  // is the pre-SEC-02 single-user behaviour. A non-empty list REPLACES that
  // default (so a deployment can pin exactly who may trigger); uid 0 needs an
  // explicit allowlist entry or allow_root = true.
  std::vector<uint32_t> allowed_uids;
  bool allow_root = false;
  bool require_peer_pid = true;
  // Request `priority_hint` is clamped to [0, max_priority_hint] and can still
  // only lower the source rule's priority. 1000 keeps every deployed rule
  // unchanged while bounding a hostile self-elevation.
  int32_t max_priority_hint = 1000;
  // V1 requests carry no sequence and cannot be replay-checked. Off by
  // default (fail closed); enabling it is an explicit, audited downgrade.
  bool allow_legacy_unsequenced = false;
  // V2 requests carry a sequence but no CRC32. Off by default: the server
  // answers V2 with kUnsupportedVersion. Enabling it is a documented
  // downgrade (no per-datagram corruption detection).
  bool allow_legacy_v2 = false;
  // SEC-10: accept replayable V1 text commands on the independent recorder
  // control socket. Default false; V2 sequence+CRC is always required
  // otherwise.
  bool allow_legacy_control = false;
  // SEC-03: raw HMAC-SHA256 key file for evidence manifests. Empty = unsigned
  // manifests. Readable check at startup; a seal with an unreadable key is
  // invalidated (fail closed).
  std::string signing_key_path;
  // REC-23: evidence content hashing. full is the default; metadata hashes
  // everything except bulk .raw payloads, off retains only file list + size.
  integrity::ManifestMode manifest_mode = integrity::ManifestMode::kFull;
  // Empty = <trigger socket>.audit / <recorder socket>.audit respectively.
  std::string audit_log_path;
  std::string control_audit_log_path;
  uint64_t audit_max_bytes = 4ull * 1024 * 1024;  // rotation threshold (> 0)
  uint32_t audit_rotate_keep = 2;                 // <path>.1 .. <path>.N
  uint64_t audit_fsync_interval_ns = 0;           // 0 = only at Shutdown

  // [logging] (OPS-05). Rotation for the non-package append-only files.
  // Packages are bounded separately by [recorder].disk_quota_mb (REC-10).
  //   <socket>.fsm   : log_fsm_max_bytes  x (log_fsm_rotate_keep + 1)
  //   perf dumps     : log_perf_max_bytes x (log_perf_rotate_keep + 1)
  //   audit          : audit_max_bytes    x (audit_rotate_keep + 1)
  // total_budget_bytes is a per-process upper bound over service and all three
  // existing writers (0 = per-file caps only). Non-mandatory writers
  // (service/fsm/perf) drop records once it is reached; audit is never dropped.
  uint64_t log_fsm_max_bytes = 4ull * 1024 * 1024;    // 4 MiB
  uint32_t log_fsm_rotate_keep = 2;                   // <path>.1 .. <path>.N
  uint64_t log_perf_max_bytes = 64ull * 1024 * 1024;  // 64 MiB
  uint32_t log_perf_rotate_keep = 2;
  // OBS-08/FUP-32: service_file empty = stderr; explicit file startup is
  // fail-closed. The file sink participates in the process log budget.
  std::string log_service_file;
  uint64_t log_service_max_bytes = 8ull * 1024 * 1024;
  uint32_t log_service_rotate_keep = 2;
  uint64_t log_total_budget_bytes = 0;  // total_budget_mb; 0 = caps only

  // [recorder] / [channel.*] sections are consumed by the trigger_center host.
  // Center also reads the single pending quota through its pending_bytes hook.
  std::string recorder_out_dir;
  uint64_t recorder_pending_quota = 64ull * 1024 * 1024;
  // 6.2/6.3/6.4 host-side flush & retention policy (the fields map to chapter 7
  // of the design: recorder.flush / high_priority / disk_quota_mb).
  bool recorder_flush_realtime = false;  // flush.policy: default for sources that do not set it
  uint64_t recorder_idle_hold_ns = 30000000000ull;  // idle_hold_s (default 30)
  bool recorder_idle_fallback_idle = false;  // idle_fallback: always_busy default
  uint64_t recorder_io_rate_limit_bps = 20ull * 1024 * 1024;  // io_rate_limit
  uint64_t recorder_disk_quota_bytes = 0;     // disk_quota_mb (0 = unlimited)
  uint64_t recorder_high_reserved_bytes = 0;  // high_priority.reserved_mb
  // Policy for .incomplete partial packages (keep | delete_after_age |
  // delete_immediately, default keep); the age cap for delete_after_age is
  // below.
  std::string recorder_incomplete_policy = "keep";
  uint64_t recorder_incomplete_max_age_ns = 0;  // incomplete_max_age_s
  std::string recorder_socket = "/tmp/thyme_recorder.sock";  // control-plane socket
  std::vector<triggered::SourceConfig> recorder_sources;
  std::vector<ChannelConfigMetadata> recorder_source_metadata;

  // [continuous] section (6.6; consumed by the host, Center does not take part
  // in continuous mode). auto_arm only controls the automatic Arm at startup;
  // rate_check runs on every Arm (automatic or manual).
  bool cont_auto_arm = false;
  bool cont_section_present = false;       // [continuous] present at all
  std::vector<std::string> cont_channels;  // default = small_struct:*
  uint64_t cont_slice_bytes = 512ull * 1024 * 1024;  // slice_mb
  uint64_t cont_slice_ns = 0;  // slice_s (0 = do not roll by time)
  bool cont_wait_idle = false;
  bool cont_demo_idle = false;  // explicit opt-in when no [chassis] is present
  // Startup rate check (6.6 hardening): enforce = refuse to start over the
  // limit, warn = warn only.
  std::string cont_rate_check = "warn";

  // [chassis] section: chassis mirror source for the idle decision; when
  // absent the host uses the demo predicate. Field offsets are replaced by
  // contract bindings once the feature is promoted.
  bool chassis_present = false;
  idle::ChassisSpec chassis;
};

// Startup accounting result (ValidateContinuousPlan).
struct ContinuousPlanReport {
  uint64_t disk_bytes_per_hour = 0;  // projected disk growth of the continuous channels
  double quota_coverage_hours = 0;  // hours covered by disk_quota (no quota=inf)
  uint64_t sustained_bps = 0;       // sum of the continuous channels' realtime rate
  bool rate_limit_too_low = false;  // io_rate_limit < sustained_bps
};

// Accounting from the [channel.*] recording parameters and the [continuous]
// channel list (design 4.5/6.6). A syntax error/illegal combination -> false +
// err (with a line number); when rate_limit_too_low, rate_check=enforce ->
// false, warn -> true + warning message.
bool ValidateContinuousPlan(const CenterConfig &cfg, ContinuousPlanReport *out,
                            std::string *warn, std::string *err);

// TRG-10: SIGHUP may reload source/quota/security policy, but anything the
// host binds at startup requires a restart: continuous geometry, the recorder
// channel layout and its recording parameters, [recorder] policy (output dir,
// quotas, io rate limit, flush/idle defaults, recorder socket) and [chassis].
// Those objects are constructed once, so accepting a change here would print
// "config reloaded" while the old value stayed in force (silent drift).
bool ReloadRequiresRestart(const CenterConfig &before,
                           const CenterConfig &after);

// Datagrams the kernel must hold for one configured Tick (rounded up).
inline uint64_t TriggerDatagramsPerTick(const CenterConfig &cfg) {
  const uint64_t rate = cfg.max_trigger_rate_hz;
  return (rate * cfg.tick_ns + 999999999ull) / 1000000000ull;
}

// err receives "line N: message" on failure. warn (optional) collects
// non-fatal messages; currently only the deprecated [continuous].enable key
// (renamed auto_arm, TRG-17).
bool ParseCenterConfig(const std::string &text, CenterConfig *out,
                       std::string *err, std::string *warn = nullptr);
bool LoadCenterConfig(const char *path, CenterConfig *out, std::string *err,
                      std::string *warn = nullptr);
// Read the generator-owned kRecordManifestChannels table for the standalone
// trigger_center's mandatory --record-manifest input. Count/shape mismatches fail.
bool LoadRecordManifest(const char *path, std::vector<std::string> *channels,
                        std::string *err);

// OPS-13: generated base + deployment policy. The base may contain
// [channel.*] and [chassis]; the policy may contain every other supported
// section except [chassis]. The combined text is parsed once, then aggregate
// record_channels (or legacy channel names) are used for strict selector checks.
// The standalone host additionally validates against --record-manifest.
bool ParseLayeredCenterConfig(const std::string &base_text,
                              const std::string &policy_text, CenterConfig *out,
                              std::string *err, std::string *warn = nullptr);
bool LoadLayeredCenterConfig(const char *base_path, const char *policy_path,
                             CenterConfig *out, std::string *err,
                             std::string *warn = nullptr);

// OPS-13: replace `%pid` in recorder shm names, chassis page_shm, and source
// or continuous channel selectors with the supervisor pid. The token is the
// supervisor that created `/c9_<pid>_rec`, not the trigger_center process.
// No token is a no-op. A remaining token with pid 0 fails closed.
bool BindShmPid(CenterConfig *cfg, uint32_t supervisor_pid, std::string *err);

// GEN-12/TRG-16: reject [channel.*] entries that name a channel outside the
// generated record manifest. `manifest_channels` is the generated
// `kRecordManifestChannels` view (generated/record_manifest.h); the host that
// links the generated table passes it. The vector form is the standalone API:
// an empty vector disables this optional check for pre-GEN-12 deployments.
//
// Matching: a [channel.<shm>] entry is accepted when its `name` (pinned_slot) or
// its shm suffix (leading '/' ignored) is a manifest channel. Positive
// trigger-source and [continuous] selectors must also match the manifest;
// exclusions cannot broaden the selected set.
bool ValidateManifestSubset(const CenterConfig &cfg,
                            const std::vector<std::string> &manifest_channels,
                            std::string *err);
// Generated-table form: `ValidateManifestSubset(cfg, kRecordManifestChannels,
// kRecordManifestCount, &err)` with no call-site conversion. Unlike the
// standalone vector API, count=0 is a strict empty topology: every channel and
// every select-all source/continuous plan is rejected. Null tables/entries are
// malformed and fail closed.
bool ValidateManifestSubset(const CenterConfig &cfg,
                            const char *const *manifest_channels,
                            std::size_t count, std::string *err);

// TRG-17: render the EFFECTIVE configuration (defaults materialised) as INI
// text. The output parses back to an equivalent config, so --dump-config
// shows exactly what took effect instead of echoing the raw file.
std::string DumpConfig(const CenterConfig &cfg);

}  // namespace trigger_cfg
}  // namespace thyme
