// GENERATED FILE - DO NOT EDIT.
// Source: tools/gen/config_schema/trigger_config.schema.json
// Regenerate: python3 tools/gen/config_schema/gen_config_schema.py
// Check:      python3 tools/gen/config_schema/gen_config_schema.py --check
//
// ARCH-13: schema-backed INI section metadata. parser_layer is
// handwritten (trigger_config.cpp). test_trigger_config_schema
// cross-checks parser + ReloadRequiresRestart against these tables.
// --check also requires gen_graph trigger.base.ini goldens to emit
// only keys present here (RecordManifest unidirectional subset).
#pragma once

#include <cstddef>
#include <cstdint>

namespace thyme {
namespace trigger_cfg {

enum class ConfigKeyStatus : std::uint8_t {
  kImplemented,
  kDeprecated,
  kRejected,
  kFuture,
};

enum class ConfigKeyReload : std::uint8_t {
  kHot,              // policy reload: SIGHUP applies it
  kRestartRequired,  // ReloadRequiresRestart rejects the change
  kStartupOnlyKept,  // host-bound; SIGHUP keeps the running value
  kFixedAtStartup,   // only one valid value; no reload delta
  kNotApplicable,    // rejected/future key: never effective
};

struct ConfigKeySchema {
  const char* section;
  const char* key;
  ConfigKeyStatus status;
  ConfigKeyReload reload;
  const char* type;
  const char* default_value;
  const char* sample;
  const char* error_match;  // rejected/future only; empty otherwise
  const char* contract;
  const char* note;
  const char* reload_note;
};

inline constexpr ConfigKeySchema kTriggerCenterSchema[] = {
    {"trigger_center", "socket", ConfigKeyStatus::kImplemented, ConfigKeyReload::kStartupOnlyKept, "path", "/tmp/thyme_trigger.sock", "/tmp/thyme_schema_trigger.sock", "", "TRG-17", "trigger datagram socket path (startup-only)", "the listening socket survives the reload"},
    {"trigger_center", "max_concurrent_captures", ConfigKeyStatus::kImplemented, ConfigKeyReload::kFixedAtStartup, "uint32", "1", "1", "", "TRG-11", "only `1` supported; `> 1` is a parse error", "`1` only; `> 1` is a parse error and there is no reload delta"},
    {"trigger_center", "tick_ms", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "1", "2", "", "TRG-19", "host Tick period and socket queue sizing (startup-only)", "socket geometry and queue sizing are bound at `Init`"},
    {"trigger_center", "socket_poll_budget_us", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "200", "250", "", "TRG-19", "per-Tick receive time budget (startup-only)", "per-Tick poll budget is bound at `Init`"},
    {"trigger_center", "max_trigger_rate_hz", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "10000", "20000", "", "TRG-19", "maximum trigger-rate sizing input (startup-only)", "socket queue sizing is bound at `Init`"},
    {"trigger_center", "dedup_window_entries", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "1024", "2048", "", "TRG-19", "V4 central dedup capacity (startup-only)", "the dedup cache is allocated at `Init`"},
    {"trigger_center", "dedup_window_ms", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "60000", "30000", "", "TRG-19", "V4 cached-result retention (startup-only)", "the dedup cache is allocated at `Init`"},
    {"trigger_center", "quota_state", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "none", "/tmp/thyme_schema_quota.state", "", "TRG-04", "durable one-hour quota windows; `none` = process-local (startup-only)", "the state file and its exclusive lock are bound at `Init`"},
    {"trigger_center", "default_source", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "source-name", "<unset: unknown sources are rejected>", "manual", "", "TRG-17", "fallback rule for unknown sources", "policy reload"},
    {"trigger_center", "allow_empty_sources", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "bool", "false", "true", "", "TRG-17", "allow a center with no `[source.*]`", "policy reload"},
    {"trigger_center", "crash_priority", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "int32", "100", "150", "", "TRG-12", "crash-level threshold (hot-reloadable)", "policy reload"},
    {"trigger_center", "pending_memory_mb", ConfigKeyStatus::kRejected, ConfigKeyReload::kNotApplicable, "rejected", "—", "64", "pending_memory_mb is removed", "TRG-17", "removed; use `[recorder].pending_bytes_quota`", "always a parse error"},
};
inline constexpr std::size_t kTriggerCenterSchemaCount =
    sizeof(kTriggerCenterSchema) / sizeof(kTriggerCenterSchema[0]);

inline constexpr ConfigKeySchema kRecorderSchema[] = {
    {"recorder", "output_dir", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "path", "<unset>", "/var/lib/thyme/pkgs", "", "OPS-13", "package root; required by the host", "retention/recorder objects bind the root at Init"},
    {"recorder", "socket", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "path", "/tmp/thyme_recorder.sock", "/tmp/thyme_schema_recorder.sock", "", "TRG-15", "continuous-control (Arm/Disarm/Status) socket", "the listening socket is bound at Init"},
    {"recorder", "pending_bytes_quota", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "67108864", "134217728", "", "TRG-10", "pending queue byte quota; also sizes the descriptor rings", "recorder rings are sized at Init"},
    {"recorder", "flush_policy", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "idle", "realtime", "", "TRG-17", "default for sources without an explicit flush_policy", "drain/recorder objects are constructed once"},
    {"recorder", "idle_hold_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "30", "10", "", "TRG-17", "vehicle-stationary hold before an idle flush (fractional allowed)", "idle detector is constructed at Init"},
    {"recorder", "idle_fallback", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "always_busy", "assume_idle", "", "TRG-17", "behaviour when the chassis mirror is unreadable", "idle detector is constructed at Init"},
    {"recorder", "io_rate_limit_mbps", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "20", "10", "", "REC-10", "drain rate limit for idle-flushed captures (decimal MB/s)", "the bucket is created at Init"},
    {"recorder", "use_cgroup", ConfigKeyStatus::kRejected, ConfigKeyReload::kNotApplicable, "rejected", "—", "true", "use_cgroup is deployment-side", "OPS-04", "cgroup v2 io.max is deployment-side; setting it is a parse error", "n/a (parse error)"},
    {"recorder", "disk_quota_mb", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "4096", "", "REC-10", "retention disk quota (0 = unlimited)", "retention is constructed at Init"},
    {"recorder", "high_reserved_mb", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "1024", "", "REC-10", "reserved high-priority zone never reclaimed by low-priority retention", "retention is constructed at Init"},
    {"recorder", "incomplete_policy", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "keep", "delete_after_age", "", "REC-10", "retention action for .incomplete packages", "retention is constructed at Init"},
    {"recorder", "incomplete_max_age_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "86400", "", "REC-10", "age threshold for delete_after_age (fractional allowed)", "retention is constructed at Init"},
};
inline constexpr std::size_t kRecorderSchemaCount =
    sizeof(kRecorderSchema) / sizeof(kRecorderSchema[0]);

inline constexpr ConfigKeySchema kLoggingSchema[] = {
    {"logging", "service_file", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "path", "stderr", "/var/log/thyme/service.log", "", "OPS-05", "service log destination; a path enables the file sink", "the sink is opened at Init"},
    {"logging", "service_max_bytes", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "8388608", "16777216", "", "OPS-05", "service log rotation threshold (must be > 0)", "the writer is opened at Init"},
    {"logging", "service_rotate_keep", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "2", "3", "", "OPS-05", "rotated service log generations kept (1..32)", "the writer is opened at Init"},
    {"logging", "fsm_max_bytes", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "4194304", "8388608", "", "OPS-05", "rotation threshold for <socket>.fsm (must be > 0)", "the FSM dump writer is opened at Init"},
    {"logging", "fsm_rotate_keep", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "2", "3", "", "OPS-05", "rotated FSM generations kept (1..32)", "the FSM dump writer is opened at Init"},
    {"logging", "perf_max_bytes", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "67108864", "134217728", "", "OPS-05", "rotation threshold for each PerfLogDumper file (must be > 0)", "host perf writers are opened at Init"},
    {"logging", "perf_rotate_keep", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "2", "3", "", "OPS-05", "rotated perf generations kept (1..32)", "host perf writers are opened at Init"},
    {"logging", "total_budget_mb", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "512", "", "OPS-05", "per-process cap over service+audit+FSM+perf; 0 disables the aggregate check", "the budget counter is owned by the open writers"},
};
inline constexpr std::size_t kLoggingSchemaCount =
    sizeof(kLoggingSchema) / sizeof(kLoggingSchema[0]);

inline constexpr ConfigKeySchema kSecuritySchema[] = {
    {"security", "allowed_uids", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "string", "<unset: only the center's euid>", "1000", "", "SEC-02", "comma-separated allowlist; a non-empty list replaces the default", "policy reload"},
    {"security", "allow_root", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "bool", "false", "true", "", "SEC-02", "also accept peer uid 0", "policy reload"},
    {"security", "require_peer_pid", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "bool", "true", "false", "", "SEC-02", "reject datagrams without a kernel-supplied peer pid", "policy reload"},
    {"security", "max_priority_hint", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "int32", "1000", "500", "", "SEC-02", "priority_hint clamp upper bound (may only lower a rule priority)", "policy reload"},
    {"security", "allow_legacy_unsequenced", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "bool", "false", "true", "", "SEC-02", "accept 88-byte V1 requests (no replay protection)", "policy reload"},
    {"security", "allow_legacy_v2", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "bool", "false", "true", "", "SEC-04", "accept 96-byte V2 requests (no CRC)", "policy reload"},
    {"security", "allow_legacy_control", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "bool", "false", "true", "", "SEC-10", "accept V1 recorder text commands (default deny)", "policy reload"},
    {"security", "signing_key", ConfigKeyStatus::kImplemented, ConfigKeyReload::kStartupOnlyKept, "path", "<unset = unsigned>", "/etc/thyme/evidence.key", "", "SEC-03", "HMAC-SHA256 key for evidence manifests; unreadable key fails closed", "the path is kept; the next seal reads the file contents"},
    {"security", "manifest", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "full", "metadata", "", "REC-23", "evidence scope: full/metadata/off", "recorder integrity mode is bound at Init"},
    {"security", "audit_log", ConfigKeyStatus::kImplemented, ConfigKeyReload::kStartupOnlyKept, "path", "<trigger socket>.audit", "/tmp/schema_audit.log", "", "SEC-02", "append-only trigger audit path; must differ from both sockets", "the audit file is open; a change warns and keeps the path"},
    {"security", "control_audit_log", ConfigKeyStatus::kImplemented, ConfigKeyReload::kStartupOnlyKept, "path", "<recorder socket>.audit", "/tmp/schema_control_audit.log", "", "SEC-10", "recorder-control audit path, independent of audit_log", "the audit file is open; a change keeps the running path"},
    {"security", "audit_max_bytes", ConfigKeyStatus::kImplemented, ConfigKeyReload::kStartupOnlyKept, "uint64", "4194304", "8388608", "", "SEC-02", "audit rotation threshold (must be > 0)", "rotation geometry is fixed at Init"},
    {"security", "audit_rotate_keep", ConfigKeyStatus::kImplemented, ConfigKeyReload::kStartupOnlyKept, "uint32", "2", "3", "", "SEC-02", "rotated audit generations kept (1..32)", "rotation geometry is fixed at Init"},
    {"security", "audit_fsync_interval_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kStartupOnlyKept, "uint64", "0", "5", "", "SEC-02", "0 = fdatasync only at shutdown; otherwise at most once per interval", "the sync cadence is fixed at Init"},
};
inline constexpr std::size_t kSecuritySchemaCount =
    sizeof(kSecuritySchema) / sizeof(kSecuritySchema[0]);

inline constexpr ConfigKeySchema kContinuousSchema[] = {
    {"continuous", "auto_arm", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "bool", "false", "true", "", "TRG-10", "arm continuous capture at startup; does not affect manual Arm", "continuous session state is bound at Init"},
    {"continuous", "channels", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "string", "<unset: all small structs>", "small_struct:*", "", "TRG-16", "continuous channel selector grammar", "the recorder continuous plan is built at Init"},
    {"continuous", "slice_mb", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "512", "256", "", "TRG-10", "slice size; must be > 0 and <= disk_quota_mb when set", "slice geometry is bound at Init"},
    {"continuous", "slice_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "60", "", "TRG-10", "time-based roll; 0 = size only", "slice geometry is bound at Init"},
    {"continuous", "wait_idle", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "bool", "false", "true", "", "TRG-17", "gate continuous drain on the idle detector", "the drain policy is bound at Init"},
    {"continuous", "demo_idle", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "bool", "false", "true", "", "TRG-17", "explicit demo idle predicate when no [chassis] exists", "the drain policy is bound at Init"},
    {"continuous", "rate_check", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "warn", "enforce", "", "TRG-10", "enforce/warn/off; runs on every Arm and --check-config", "the continuous plan is bound at Init"},
    {"continuous", "enable", ConfigKeyStatus::kDeprecated, ConfigKeyReload::kRestartRequired, "bool", "false", "true", "", "TRG-17", "old spelling of auto_arm; accepted with a deprecation warning", "it changes auto_arm, which requires restart"},
};
inline constexpr std::size_t kContinuousSchemaCount =
    sizeof(kContinuousSchema) / sizeof(kContinuousSchema[0]);

inline constexpr ConfigKeySchema kChassisSchema[] = {
    {"chassis", "page_shm", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "path", "<unset: no chassis predicate>", "/thyme_chassis_rec", "", "TRG-10", "record-mirror page shm name for the chassis mirror", "the chassis predicate attaches at Init"},
    {"chassis", "struct_id", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "0", "1", "", "TRG-10", "struct id inside the chassis mirror page", "the chassis predicate attaches at Init"},
    {"chassis", "epb_offset", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "0", "8", "", "TRG-10", "uint8 park-brake status offset (!=0 means engaged)", "the chassis predicate attaches at Init"},
    {"chassis", "speed_offset", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "4", "12", "", "TRG-10", "float vehicle-speed offset (m/s)", "the chassis predicate attaches at Init"},
    {"chassis", "speed_threshold", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "string", "0.1", "0.2", "", "TRG-10", "speed threshold below which the vehicle is considered stationary", "the chassis predicate attaches at Init"},
};
inline constexpr std::size_t kChassisSchemaCount =
    sizeof(kChassisSchema) / sizeof(kChassisSchema[0]);

inline constexpr ConfigKeySchema kQuotaSchema[] = {
    {"quota.*", "max_triggers_per_hour", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "uint64", "0", "100", "", "TRG-04", "sliding one-hour trigger budget; 0 = unlimited", "quota policy reload"},
};
inline constexpr std::size_t kQuotaSchemaCount =
    sizeof(kQuotaSchema) / sizeof(kQuotaSchema[0]);

inline constexpr ConfigKeySchema kSourceSchema[] = {
    {"source.*", "channels", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "string", "<unset: all channels>", "/demo", "", "TRG-16", "per-source channel selector list (glob/exclusion/decimate grammar)", "policy reload"},
    {"source.*", "before_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "uint64", "0", "2", "", "TRG-10", "pre-roll (fractions allowed)", "policy reload"},
    {"source.*", "after_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "uint64", "0", "3", "", "TRG-10", "post-roll (fractions allowed)", "policy reload"},
    {"source.*", "priority", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "int32", "0", "5", "", "TRG-12", "base priority; a submitter hint may only lower it", "policy reload"},
    {"source.*", "mute_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "uint64", "0", "10", "", "TRG-10", "mute window after the capture completes", "policy reload"},
    {"source.*", "flush_policy", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "enum", "<inherit [recorder].flush_policy>", "realtime", "", "TRG-17", "idle or realtime; an explicit value overrides [recorder]", "policy reload"},
    {"source.*", "quota_class", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "string", "<required: must match a [quota.*] class>", "manual", "", "TRG-04", "quota class; must exist at center start", "policy reload"},
    {"source.*", "name", ConfigKeyStatus::kRejected, ConfigKeyReload::kNotApplicable, "rejected", "—", "demo", "name comes from the [source.<name>] header", "TRG-17", "the name comes from the [source.<name>] section header", "n/a (parse error)"},
};
inline constexpr std::size_t kSourceSchemaCount =
    sizeof(kSourceSchema) / sizeof(kSourceSchema[0]);

inline constexpr ConfigKeySchema kChannelSchema[] = {
    {"channel.*", "kind", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "mirror_ring", "pinned_slot", "", "REC-19", "mirror_ring (small structs), pinned_slot, h264_stream", "recorder sources are built at Init"},
    {"channel.*", "name", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "string", "<shm name>", "demo", "", "TRG-16", "addressable channel name used by source/continuous selectors", "recorder sources are built at Init"},
    {"channel.*", "record_channels", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "string", "<unset: generated aggregate>", "demo", "", "GEN-12", "record-manifest structs aggregated into one mirror source (normally generated)", "record registry is built at Init"},
    {"channel.*", "rate_hz", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "10", "", "TRG-10", "nominal sample rate for continuous rate accounting", "the continuous plan is built at Init"},
    {"channel.*", "avg_frame_bytes", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "128", "", "TRG-10", "average payload for continuous rate accounting", "the continuous plan is built at Init"},
    {"channel.*", "bitrate_kbps", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "4096", "", "TRG-10", "encoded bitrate for h264_stream rate accounting", "the continuous plan is built at Init"},
    {"channel.*", "crc", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "<kind default>", "off", "", "REC-21", "package/frame/off per-channel CRC policy", "the recorder plan is built at Init"},
    {"channel.*", "manifest", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "<inherit [security]>", "full", "", "REC-23", "bulk .raw override: full/metadata/off", "the recorder plan is built at Init"},
    {"channel.*", "profile", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "string", "none", "inno-lidar/pointcloud@v2", "", "REC-27", "opaque big-frame payload decoder id; none = unset; mirror_ring rejects", "the recorder plan is built at Init"},
    {"channel.*", "profile_version", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "0", "1", "", "REC-27", "payload profile version; requires profile", "the recorder plan is built at Init"},
    {"channel.*", "profile_params", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "string", "none", "time_field=offset:0", "", "REC-27", "opaque parameter string hashed into params_digest; none = empty", "the recorder plan is built at Init"},
    {"channel.*", "gop_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "2", "", "OPS-13", "h264 trigger window IDR slack in seconds; 0 disables; 1..60 only on h264_stream", "the recorder plan is built at Init"},
};
inline constexpr std::size_t kChannelSchemaCount =
    sizeof(kChannelSchema) / sizeof(kChannelSchema[0]);

// Union of every schema-backed section (--check-config walks all).
inline constexpr ConfigKeySchema kTriggerConfigSchema[] = {
    // trigger_center
    {"trigger_center", "socket", ConfigKeyStatus::kImplemented, ConfigKeyReload::kStartupOnlyKept, "path", "/tmp/thyme_trigger.sock", "/tmp/thyme_schema_trigger.sock", "", "TRG-17", "trigger datagram socket path (startup-only)", "the listening socket survives the reload"},
    {"trigger_center", "max_concurrent_captures", ConfigKeyStatus::kImplemented, ConfigKeyReload::kFixedAtStartup, "uint32", "1", "1", "", "TRG-11", "only `1` supported; `> 1` is a parse error", "`1` only; `> 1` is a parse error and there is no reload delta"},
    {"trigger_center", "tick_ms", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "1", "2", "", "TRG-19", "host Tick period and socket queue sizing (startup-only)", "socket geometry and queue sizing are bound at `Init`"},
    {"trigger_center", "socket_poll_budget_us", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "200", "250", "", "TRG-19", "per-Tick receive time budget (startup-only)", "per-Tick poll budget is bound at `Init`"},
    {"trigger_center", "max_trigger_rate_hz", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "10000", "20000", "", "TRG-19", "maximum trigger-rate sizing input (startup-only)", "socket queue sizing is bound at `Init`"},
    {"trigger_center", "dedup_window_entries", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "1024", "2048", "", "TRG-19", "V4 central dedup capacity (startup-only)", "the dedup cache is allocated at `Init`"},
    {"trigger_center", "dedup_window_ms", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "60000", "30000", "", "TRG-19", "V4 cached-result retention (startup-only)", "the dedup cache is allocated at `Init`"},
    {"trigger_center", "quota_state", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "none", "/tmp/thyme_schema_quota.state", "", "TRG-04", "durable one-hour quota windows; `none` = process-local (startup-only)", "the state file and its exclusive lock are bound at `Init`"},
    {"trigger_center", "default_source", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "source-name", "<unset: unknown sources are rejected>", "manual", "", "TRG-17", "fallback rule for unknown sources", "policy reload"},
    {"trigger_center", "allow_empty_sources", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "bool", "false", "true", "", "TRG-17", "allow a center with no `[source.*]`", "policy reload"},
    {"trigger_center", "crash_priority", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "int32", "100", "150", "", "TRG-12", "crash-level threshold (hot-reloadable)", "policy reload"},
    {"trigger_center", "pending_memory_mb", ConfigKeyStatus::kRejected, ConfigKeyReload::kNotApplicable, "rejected", "—", "64", "pending_memory_mb is removed", "TRG-17", "removed; use `[recorder].pending_bytes_quota`", "always a parse error"},
    // recorder
    {"recorder", "output_dir", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "path", "<unset>", "/var/lib/thyme/pkgs", "", "OPS-13", "package root; required by the host", "retention/recorder objects bind the root at Init"},
    {"recorder", "socket", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "path", "/tmp/thyme_recorder.sock", "/tmp/thyme_schema_recorder.sock", "", "TRG-15", "continuous-control (Arm/Disarm/Status) socket", "the listening socket is bound at Init"},
    {"recorder", "pending_bytes_quota", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "67108864", "134217728", "", "TRG-10", "pending queue byte quota; also sizes the descriptor rings", "recorder rings are sized at Init"},
    {"recorder", "flush_policy", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "idle", "realtime", "", "TRG-17", "default for sources without an explicit flush_policy", "drain/recorder objects are constructed once"},
    {"recorder", "idle_hold_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "30", "10", "", "TRG-17", "vehicle-stationary hold before an idle flush (fractional allowed)", "idle detector is constructed at Init"},
    {"recorder", "idle_fallback", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "always_busy", "assume_idle", "", "TRG-17", "behaviour when the chassis mirror is unreadable", "idle detector is constructed at Init"},
    {"recorder", "io_rate_limit_mbps", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "20", "10", "", "REC-10", "drain rate limit for idle-flushed captures (decimal MB/s)", "the bucket is created at Init"},
    {"recorder", "use_cgroup", ConfigKeyStatus::kRejected, ConfigKeyReload::kNotApplicable, "rejected", "—", "true", "use_cgroup is deployment-side", "OPS-04", "cgroup v2 io.max is deployment-side; setting it is a parse error", "n/a (parse error)"},
    {"recorder", "disk_quota_mb", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "4096", "", "REC-10", "retention disk quota (0 = unlimited)", "retention is constructed at Init"},
    {"recorder", "high_reserved_mb", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "1024", "", "REC-10", "reserved high-priority zone never reclaimed by low-priority retention", "retention is constructed at Init"},
    {"recorder", "incomplete_policy", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "keep", "delete_after_age", "", "REC-10", "retention action for .incomplete packages", "retention is constructed at Init"},
    {"recorder", "incomplete_max_age_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "86400", "", "REC-10", "age threshold for delete_after_age (fractional allowed)", "retention is constructed at Init"},
    // logging
    {"logging", "service_file", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "path", "stderr", "/var/log/thyme/service.log", "", "OPS-05", "service log destination; a path enables the file sink", "the sink is opened at Init"},
    {"logging", "service_max_bytes", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "8388608", "16777216", "", "OPS-05", "service log rotation threshold (must be > 0)", "the writer is opened at Init"},
    {"logging", "service_rotate_keep", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "2", "3", "", "OPS-05", "rotated service log generations kept (1..32)", "the writer is opened at Init"},
    {"logging", "fsm_max_bytes", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "4194304", "8388608", "", "OPS-05", "rotation threshold for <socket>.fsm (must be > 0)", "the FSM dump writer is opened at Init"},
    {"logging", "fsm_rotate_keep", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "2", "3", "", "OPS-05", "rotated FSM generations kept (1..32)", "the FSM dump writer is opened at Init"},
    {"logging", "perf_max_bytes", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "67108864", "134217728", "", "OPS-05", "rotation threshold for each PerfLogDumper file (must be > 0)", "host perf writers are opened at Init"},
    {"logging", "perf_rotate_keep", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "2", "3", "", "OPS-05", "rotated perf generations kept (1..32)", "host perf writers are opened at Init"},
    {"logging", "total_budget_mb", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "512", "", "OPS-05", "per-process cap over service+audit+FSM+perf; 0 disables the aggregate check", "the budget counter is owned by the open writers"},
    // security
    {"security", "allowed_uids", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "string", "<unset: only the center's euid>", "1000", "", "SEC-02", "comma-separated allowlist; a non-empty list replaces the default", "policy reload"},
    {"security", "allow_root", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "bool", "false", "true", "", "SEC-02", "also accept peer uid 0", "policy reload"},
    {"security", "require_peer_pid", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "bool", "true", "false", "", "SEC-02", "reject datagrams without a kernel-supplied peer pid", "policy reload"},
    {"security", "max_priority_hint", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "int32", "1000", "500", "", "SEC-02", "priority_hint clamp upper bound (may only lower a rule priority)", "policy reload"},
    {"security", "allow_legacy_unsequenced", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "bool", "false", "true", "", "SEC-02", "accept 88-byte V1 requests (no replay protection)", "policy reload"},
    {"security", "allow_legacy_v2", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "bool", "false", "true", "", "SEC-04", "accept 96-byte V2 requests (no CRC)", "policy reload"},
    {"security", "allow_legacy_control", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "bool", "false", "true", "", "SEC-10", "accept V1 recorder text commands (default deny)", "policy reload"},
    {"security", "signing_key", ConfigKeyStatus::kImplemented, ConfigKeyReload::kStartupOnlyKept, "path", "<unset = unsigned>", "/etc/thyme/evidence.key", "", "SEC-03", "HMAC-SHA256 key for evidence manifests; unreadable key fails closed", "the path is kept; the next seal reads the file contents"},
    {"security", "manifest", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "full", "metadata", "", "REC-23", "evidence scope: full/metadata/off", "recorder integrity mode is bound at Init"},
    {"security", "audit_log", ConfigKeyStatus::kImplemented, ConfigKeyReload::kStartupOnlyKept, "path", "<trigger socket>.audit", "/tmp/schema_audit.log", "", "SEC-02", "append-only trigger audit path; must differ from both sockets", "the audit file is open; a change warns and keeps the path"},
    {"security", "control_audit_log", ConfigKeyStatus::kImplemented, ConfigKeyReload::kStartupOnlyKept, "path", "<recorder socket>.audit", "/tmp/schema_control_audit.log", "", "SEC-10", "recorder-control audit path, independent of audit_log", "the audit file is open; a change keeps the running path"},
    {"security", "audit_max_bytes", ConfigKeyStatus::kImplemented, ConfigKeyReload::kStartupOnlyKept, "uint64", "4194304", "8388608", "", "SEC-02", "audit rotation threshold (must be > 0)", "rotation geometry is fixed at Init"},
    {"security", "audit_rotate_keep", ConfigKeyStatus::kImplemented, ConfigKeyReload::kStartupOnlyKept, "uint32", "2", "3", "", "SEC-02", "rotated audit generations kept (1..32)", "rotation geometry is fixed at Init"},
    {"security", "audit_fsync_interval_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kStartupOnlyKept, "uint64", "0", "5", "", "SEC-02", "0 = fdatasync only at shutdown; otherwise at most once per interval", "the sync cadence is fixed at Init"},
    // continuous
    {"continuous", "auto_arm", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "bool", "false", "true", "", "TRG-10", "arm continuous capture at startup; does not affect manual Arm", "continuous session state is bound at Init"},
    {"continuous", "channels", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "string", "<unset: all small structs>", "small_struct:*", "", "TRG-16", "continuous channel selector grammar", "the recorder continuous plan is built at Init"},
    {"continuous", "slice_mb", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "512", "256", "", "TRG-10", "slice size; must be > 0 and <= disk_quota_mb when set", "slice geometry is bound at Init"},
    {"continuous", "slice_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "60", "", "TRG-10", "time-based roll; 0 = size only", "slice geometry is bound at Init"},
    {"continuous", "wait_idle", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "bool", "false", "true", "", "TRG-17", "gate continuous drain on the idle detector", "the drain policy is bound at Init"},
    {"continuous", "demo_idle", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "bool", "false", "true", "", "TRG-17", "explicit demo idle predicate when no [chassis] exists", "the drain policy is bound at Init"},
    {"continuous", "rate_check", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "warn", "enforce", "", "TRG-10", "enforce/warn/off; runs on every Arm and --check-config", "the continuous plan is bound at Init"},
    {"continuous", "enable", ConfigKeyStatus::kDeprecated, ConfigKeyReload::kRestartRequired, "bool", "false", "true", "", "TRG-17", "old spelling of auto_arm; accepted with a deprecation warning", "it changes auto_arm, which requires restart"},
    // chassis
    {"chassis", "page_shm", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "path", "<unset: no chassis predicate>", "/thyme_chassis_rec", "", "TRG-10", "record-mirror page shm name for the chassis mirror", "the chassis predicate attaches at Init"},
    {"chassis", "struct_id", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "0", "1", "", "TRG-10", "struct id inside the chassis mirror page", "the chassis predicate attaches at Init"},
    {"chassis", "epb_offset", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "0", "8", "", "TRG-10", "uint8 park-brake status offset (!=0 means engaged)", "the chassis predicate attaches at Init"},
    {"chassis", "speed_offset", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "4", "12", "", "TRG-10", "float vehicle-speed offset (m/s)", "the chassis predicate attaches at Init"},
    {"chassis", "speed_threshold", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "string", "0.1", "0.2", "", "TRG-10", "speed threshold below which the vehicle is considered stationary", "the chassis predicate attaches at Init"},
    // quota.*
    {"quota.*", "max_triggers_per_hour", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "uint64", "0", "100", "", "TRG-04", "sliding one-hour trigger budget; 0 = unlimited", "quota policy reload"},
    // source.*
    {"source.*", "channels", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "string", "<unset: all channels>", "/demo", "", "TRG-16", "per-source channel selector list (glob/exclusion/decimate grammar)", "policy reload"},
    {"source.*", "before_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "uint64", "0", "2", "", "TRG-10", "pre-roll (fractions allowed)", "policy reload"},
    {"source.*", "after_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "uint64", "0", "3", "", "TRG-10", "post-roll (fractions allowed)", "policy reload"},
    {"source.*", "priority", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "int32", "0", "5", "", "TRG-12", "base priority; a submitter hint may only lower it", "policy reload"},
    {"source.*", "mute_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "uint64", "0", "10", "", "TRG-10", "mute window after the capture completes", "policy reload"},
    {"source.*", "flush_policy", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "enum", "<inherit [recorder].flush_policy>", "realtime", "", "TRG-17", "idle or realtime; an explicit value overrides [recorder]", "policy reload"},
    {"source.*", "quota_class", ConfigKeyStatus::kImplemented, ConfigKeyReload::kHot, "string", "<required: must match a [quota.*] class>", "manual", "", "TRG-04", "quota class; must exist at center start", "policy reload"},
    {"source.*", "name", ConfigKeyStatus::kRejected, ConfigKeyReload::kNotApplicable, "rejected", "—", "demo", "name comes from the [source.<name>] header", "TRG-17", "the name comes from the [source.<name>] section header", "n/a (parse error)"},
    // channel.*
    {"channel.*", "kind", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "mirror_ring", "pinned_slot", "", "REC-19", "mirror_ring (small structs), pinned_slot, h264_stream", "recorder sources are built at Init"},
    {"channel.*", "name", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "string", "<shm name>", "demo", "", "TRG-16", "addressable channel name used by source/continuous selectors", "recorder sources are built at Init"},
    {"channel.*", "record_channels", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "string", "<unset: generated aggregate>", "demo", "", "GEN-12", "record-manifest structs aggregated into one mirror source (normally generated)", "record registry is built at Init"},
    {"channel.*", "rate_hz", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "10", "", "TRG-10", "nominal sample rate for continuous rate accounting", "the continuous plan is built at Init"},
    {"channel.*", "avg_frame_bytes", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "128", "", "TRG-10", "average payload for continuous rate accounting", "the continuous plan is built at Init"},
    {"channel.*", "bitrate_kbps", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "4096", "", "TRG-10", "encoded bitrate for h264_stream rate accounting", "the continuous plan is built at Init"},
    {"channel.*", "crc", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "<kind default>", "off", "", "REC-21", "package/frame/off per-channel CRC policy", "the recorder plan is built at Init"},
    {"channel.*", "manifest", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "enum", "<inherit [security]>", "full", "", "REC-23", "bulk .raw override: full/metadata/off", "the recorder plan is built at Init"},
    {"channel.*", "profile", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "string", "none", "inno-lidar/pointcloud@v2", "", "REC-27", "opaque big-frame payload decoder id; none = unset; mirror_ring rejects", "the recorder plan is built at Init"},
    {"channel.*", "profile_version", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint32", "0", "1", "", "REC-27", "payload profile version; requires profile", "the recorder plan is built at Init"},
    {"channel.*", "profile_params", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "string", "none", "time_field=offset:0", "", "REC-27", "opaque parameter string hashed into params_digest; none = empty", "the recorder plan is built at Init"},
    {"channel.*", "gop_s", ConfigKeyStatus::kImplemented, ConfigKeyReload::kRestartRequired, "uint64", "0", "2", "", "OPS-13", "h264 trigger window IDR slack in seconds; 0 disables; 1..60 only on h264_stream", "the recorder plan is built at Init"},
};
inline constexpr std::size_t kTriggerConfigSchemaCount =
    sizeof(kTriggerConfigSchema) / sizeof(kTriggerConfigSchema[0]);

}  // namespace trigger_cfg
}  // namespace thyme
