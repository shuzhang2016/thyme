#include "thyme/record/trigger_config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>

#include "thyme/record/file_ptr.h"
#include "thyme/record/record_schema.h"
#include "thyme/record/retention.h"  // REC-10: ParseIncompletePolicy

namespace thyme {
namespace trigger_cfg {
namespace {

std::string Trim(const std::string &s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a])) != 0) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])) != 0) --b;
  return s.substr(a, b - a);
}

bool ParseU64(const std::string &v, uint64_t *out) {
  if (v.empty()) return false;
  char *end = nullptr;
  const unsigned long long x = std::strtoull(v.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return false;
  *out = static_cast<uint64_t>(x);
  return true;
}

bool ParseI32(const std::string &v, int32_t *out) {
  if (v.empty()) return false;
  char *end = nullptr;
  const long x = std::strtol(v.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return false;
  *out = static_cast<int32_t>(x);
  return true;
}

// Seconds (float allowed) -> nanoseconds.
bool ParseSecondsNs(const std::string &v, uint64_t *out) {
  if (v.empty()) return false;
  char *end = nullptr;
  const double s = std::strtod(v.c_str(), &end);
  if (end == nullptr || *end != '\0' || s < 0) return false;
  const double ns = s * 1e9;
  // Reject NaN/inf and values that would overflow the nanosecond range.
  if (!(ns >= 0.0) ||
      ns >= static_cast<double>(std::numeric_limits<uint64_t>::max())) {
    return false;  // out of range: never wrap into a bogus duration
  }
  *out = static_cast<uint64_t>(std::round(ns));
  return true;
}

bool ParseDouble(const std::string &v, double *out) {
  if (v.empty()) return false;
  char *end = nullptr;
  const double x = std::strtod(v.c_str(), &end);
  // NaN/inf must not pass: "x < 0" is false for NaN, and a non-finite rate
  // would later poison the continuous bit-rate accounting.
  if (end == nullptr || *end != '\0' || !std::isfinite(x) || x < 0) {
    return false;
  }
  *out = x;
  return true;
}

std::vector<std::string> SplitList(const std::string &v) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos <= v.size()) {
    const std::size_t comma = v.find(',', pos);
    const std::string item = Trim(v.substr(pos, comma - pos));
    if (!item.empty())
      out.push_back(item);
    if (comma == std::string::npos)
      break;
    pos = comma + 1;
  }
  return out;
}

struct Err {
  std::string *err;
  int line;
  bool Fail(const std::string &msg) const {
    if (err != nullptr) {
      char buf[256];
      std::snprintf(buf, sizeof(buf), "line %d: %s", line, msg.c_str());
      *err = buf;
    }
    return false;
  }
};

}  // namespace

bool ParseCenterConfig(const std::string &text, CenterConfig *out,
                       std::string *err, std::string *warn) {
  if (out == nullptr) return false;
  CenterConfig cfg;
  SourceRule *cur_source = nullptr;
  std::string cur_quota_class;
  std::string cur_channel_shm;
  // ARCH-13: schema-backed sections reject repeated keys. A repeated key
  // would shadow the earlier value while both parse, and the generated schema
  // calls every accepted key effective.
  std::set<std::string> center_keys_seen;
  std::set<std::string> recorder_keys_seen;
  std::set<std::string> logging_keys_seen;
  std::set<std::string> security_keys_seen;
  std::set<std::string> continuous_keys_seen;
  std::set<std::string> chassis_keys_seen;
  std::set<std::string> source_keys_seen;
  std::set<std::string> channel_keys_seen;
  bool cont_section_present = false;
  enum class Section {
    kNone,
    kCenter,
    kRecorder,
    kChannel,
    kSource,
    kQuota,
    kContinuous,
    kChassis,
    kSecurity,
    kLogging
  };
  Section section = Section::kNone;

  std::size_t pos = 0;
  int line_no = 0;
  int cont_channels_line = 0;
  while (pos <= text.size()) {
    const std::size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, nl - pos);
    pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
    ++line_no;
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos)
      line = line.substr(0, hash);
    line = Trim(line);
    if (line.empty())
      continue;
    const Err E{err, line_no};
    // INI is a text format; an embedded NUL would silently truncate every
    // c_str() consumer downstream (DumpConfig, socket paths, channel names),
    // breaking the parse->dump->reparse contract (found by the TST-03 fuzzer).
    if (line.find('\0') != std::string::npos) {
      return E.Fail("NUL byte in config line");
    }

    if (line.front() == '[') {
      if (line.back() != ']')
        return E.Fail("malformed section header");
      const std::string name = Trim(line.substr(1, line.size() - 2));
      cur_source = nullptr;
      cur_quota_class.clear();
      cur_channel_shm.clear();
      if (name == "trigger_center") {
        section = Section::kCenter;
      } else if (name == "recorder") {
        section = Section::kRecorder;
      } else if (name == "continuous") {
        section = Section::kContinuous;
        cont_section_present = true;
        cfg.cont_section_present = true;
      } else if (name == "chassis") {
        section = Section::kChassis;
        cfg.chassis_present = true;
      } else if (name == "security") {
        section = Section::kSecurity;
      } else if (name == "logging") {
        section = Section::kLogging;
      } else if (name.compare(0, 7, "source.") == 0 && name.size() > 7) {
        section = Section::kSource;
        for (const SourceRule &existing : cfg.sources) {
          if (existing.name == name.substr(7)) {
            return E.Fail("duplicate source " + name.substr(7));
          }
        }
        SourceRule rule;
        rule.name = name.substr(7);
        cfg.sources.push_back(rule);
        cur_source = &cfg.sources.back();
        source_keys_seen.clear();
      } else if (name.compare(0, 6, "quota.") == 0 && name.size() > 6) {
        section = Section::kQuota;
        cur_quota_class = name.substr(6);
        for (const auto &q : cfg.quota_limits) {
          if (q.first == cur_quota_class) {
            return E.Fail("duplicate quota class " + cur_quota_class);
          }
        }
      } else if (name.compare(0, 8, "channel.") == 0 && name.size() > 8) {
        section = Section::kChannel;
        cur_channel_shm = name.substr(8);
        for (const auto &sc : cfg.recorder_sources) {
          if (sc.shm_name == cur_channel_shm) {
            return E.Fail("duplicate channel " + cur_channel_shm);
          }
        }
        triggered::SourceConfig sc;
        sc.shm_name = cur_channel_shm;
        cfg.recorder_sources.push_back(sc);
        cfg.recorder_source_metadata.emplace_back();
        channel_keys_seen.clear();
      } else {
        return E.Fail("unknown section [" + name + "]");
      }
      continue;
    }

    const std::size_t eq = line.find('=');
    if (eq == std::string::npos)
      return E.Fail("expected key = value");
    const std::string key = Trim(line.substr(0, eq));
    const std::string val = Trim(line.substr(eq + 1));
    if (key.empty() || val.empty())
      return E.Fail("empty key or value");

    switch (section) {
      case Section::kCenter:
        if (!center_keys_seen.insert(key).second) {
          return E.Fail("duplicate trigger_center key " + key);
        }
        if (key == "socket") {
          cfg.socket_path = val;
        } else if (key == "max_concurrent_captures") {
          uint64_t x = 0;
          if (!ParseU64(val, &x) || x == 0) {
            return E.Fail("bad max_concurrent_captures");
          }
          // TRG-17: the FSM only implements one capture at a time. Accepting
          // >1 would silently degrade, so it is a hard parse error.
          if (x > 1) {
            return E.Fail(
                "max_concurrent_captures > 1 is not implemented (only 1 is "
                "supported); set it to 1");
          }
          cfg.max_concurrent_captures = static_cast<uint32_t>(x);
      } else if (key == "tick_ms") {
        uint64_t x = 0;
        if (!ParseU64(val, &x) || x == 0 || x > 1000) {
          return E.Fail("bad tick_ms (require 1..1000)");
        }
        cfg.tick_ns = x * 1000000ull;
      } else if (key == "socket_poll_budget_us") {
        uint64_t x = 0;
        if (!ParseU64(val, &x) || x == 0 || x > 1000000) {
          return E.Fail("bad socket_poll_budget_us (require 1..1000000)");
        }
        cfg.socket_poll_budget_ns = x * 1000ull;
      } else if (key == "max_trigger_rate_hz") {
        uint64_t x = 0;
        if (!ParseU64(val, &x) || x == 0 || x > 100000) {
          return E.Fail("bad max_trigger_rate_hz (require 1..100000)");
        }
        cfg.max_trigger_rate_hz = static_cast<uint32_t>(x);
      } else if (key == "dedup_window_entries") {
        uint64_t x = 0;
        if (!ParseU64(val, &x) || x == 0 || x > 16384) {
          return E.Fail("bad dedup_window_entries (require 1..16384)");
        }
        cfg.dedup_window_entries = static_cast<uint32_t>(x);
      } else if (key == "dedup_window_ms") {
        uint64_t x = 0;
        if (!ParseU64(val, &x) || x == 0 || x > 600000) {
          return E.Fail("bad dedup_window_ms (require 1..600000)");
        }
        cfg.dedup_window_ns = x * 1000000ull;
        } else if (key == "quota_state") {
          cfg.quota_state_path = val == "none" ? std::string() : val;
        } else if (key == "default_source") {
          cfg.default_source = val;
        } else if (key == "allow_empty_sources") {
          if (val == "true") {
            cfg.allow_empty_sources = true;
          } else if (val == "false") {
            cfg.allow_empty_sources = false;
          } else {
            return E.Fail("bad allow_empty_sources " + val);
          }
        } else if (key == "crash_priority") {
          if (!ParseI32(val, &cfg.crash_priority)) {
            return E.Fail("bad crash_priority");
          }
        } else if (key == "pending_memory_mb") {
          return E.Fail(
              "pending_memory_mb is removed; configure the single pending "
              "quota with [recorder].pending_bytes_quota");
        } else {
          return E.Fail("unknown trigger_center key " + key);
        }
        break;
      case Section::kRecorder:
        if (!recorder_keys_seen.insert(key).second) {
          return E.Fail("duplicate recorder key " + key);
        }
        if (key == "output_dir") {
          cfg.recorder_out_dir = val;
        } else if (key == "socket") {
          cfg.recorder_socket = val;
        } else if (key == "pending_bytes_quota") {
          if (!ParseU64(val, &cfg.recorder_pending_quota)) {
            return E.Fail("bad pending_bytes_quota");
          }
        } else if (key == "flush_policy") {
          if (val == "realtime") {
            cfg.recorder_flush_realtime = true;
          } else if (val == "idle") {
            cfg.recorder_flush_realtime = false;
          } else {
            return E.Fail("bad recorder flush_policy " + val);
          }
        } else if (key == "idle_hold_s") {
          if (!ParseSecondsNs(val, &cfg.recorder_idle_hold_ns)) {
            return E.Fail("bad idle_hold_s");
          }
        } else if (key == "idle_fallback") {
          if (val == "assume_idle") {
            cfg.recorder_idle_fallback_idle = true;
          } else if (val == "always_busy") {
            cfg.recorder_idle_fallback_idle = false;
          } else {
            return E.Fail("bad idle_fallback " + val);
          }
        } else if (key == "io_rate_limit_mbps") {
          uint64_t x = 0;
          if (!ParseU64(val, &x)) {
            return E.Fail("bad io_rate_limit_mbps");
          }
          cfg.recorder_io_rate_limit_bps = x * 1000 * 1000;
        } else if (key == "use_cgroup") {
          // TRG-17: cgroup v2 io.max is applied by the deployment (systemd
          // slice), not by thyme. The key is no longer consumed here, so a
          // config that sets it is rejected instead of silently ignored.
          return E.Fail(
              "use_cgroup is deployment-side (cgroup v2 io.max) and is not "
              "consumed by trigger_center; remove the key (see "
              "docs/guides/trigger-config.md)");
        } else if (key == "disk_quota_mb") {
          uint64_t x = 0;
          if (!ParseU64(val, &x)) {
            return E.Fail("bad disk_quota_mb");
          }
          cfg.recorder_disk_quota_bytes = x * 1024 * 1024;
        } else if (key == "high_reserved_mb") {
          uint64_t x = 0;
          if (!ParseU64(val, &x)) {
            return E.Fail("bad high_reserved_mb");
          }
          cfg.recorder_high_reserved_bytes = x * 1024 * 1024;
        } else if (key == "incomplete_policy") {
          // Retention policy for .incomplete partial packages.
          retention::IncompletePolicy pol = retention::IncompletePolicy::kKeep;
          if (!retention::ParseIncompletePolicy(val, &pol)) {
            return E.Fail("bad incomplete_policy " + val);
          }
          cfg.recorder_incomplete_policy = val;
        } else if (key == "incomplete_max_age_s") {
          if (!ParseSecondsNs(val, &cfg.recorder_incomplete_max_age_ns)) {
            return E.Fail("bad incomplete_max_age_s");
          }
        } else {
          return E.Fail("unknown recorder key " + key);
        }
        break;
      case Section::kChannel: {
        triggered::SourceConfig &sc = cfg.recorder_sources.back();
        ChannelConfigMetadata &metadata = cfg.recorder_source_metadata.back();
        if (!channel_keys_seen.insert(key).second) {
          return E.Fail("duplicate channel key " + key);
        }
        if (key == "kind") {
          if (val == "mirror_ring") {
            sc.kind = triggered::SourceConfig::Kind::kMirrorRing;
          } else if (val == "pinned_slot") {
            sc.kind = triggered::SourceConfig::Kind::kPinnedSlot;
          } else if (val == "h264_stream") {
            sc.kind = triggered::SourceConfig::Kind::kPinnedSlot;
            sc.h264_stream = true;
          } else {
            return E.Fail("unknown channel kind " + val);
          }
        } else if (key == "name") {
          sc.channel_name = val;
        } else if (key == "record_channels") {
          metadata.record_channels = SplitList(val);
          if (metadata.record_channels.empty()) {
            return E.Fail("empty record_channels");
          }
          sc.record_channels = metadata.record_channels;
        } else if (key == "rate_hz") {
          if (!ParseDouble(val, &sc.rate_hz)) {
            return E.Fail("bad rate_hz");
          }
          metadata.rate_hz_present = true;
        } else if (key == "avg_frame_bytes") {
          if (!ParseU64(val, &sc.avg_frame_bytes)) {
            return E.Fail("bad avg_frame_bytes");
          }
          metadata.avg_frame_bytes_present = true;
        } else if (key == "bitrate_kbps") {
          uint64_t x = 0;
          if (!ParseU64(val, &x)) {
            return E.Fail("bad bitrate_kbps");
          }
          sc.bitrate_kbps = static_cast<uint32_t>(x);
          metadata.bitrate_kbps_present = true;
        } else if (key == "crc") {
          // REC-21: package|frame|off; defaults materialize after parsing
          // (kind may be declared after crc inside the section).
          if (!triggered::ParseCrcPolicy(val, &sc.crc)) {
            return E.Fail("bad crc " + val + " (package|frame|off)");
          }
        } else if (key == "manifest") {
          if (!triggered::ParseManifestPolicy(val, &sc.manifest)) {
            return E.Fail("bad channel manifest " + val +
                          " (full|metadata|off)");
          }
        } else if (key == "profile") {
          if (val == "none") {
            sc.payload_profile.clear();
          } else {
            sc.payload_profile = val;
          }
        } else if (key == "profile_version") {
          uint64_t x = 0;
          if (!ParseU64(val, &x) || x > 0xFFFFFFFFull) {
            return E.Fail("bad profile_version");
          }
          sc.payload_profile_version = static_cast<uint32_t>(x);
        } else if (key == "profile_params") {
          if (val == "none") {
            sc.payload_profile_params.clear();
          } else {
            sc.payload_profile_params = val;
          }
        } else if (key == "gop_s") {
          // OPS-13: seconds of IDR slack around a trigger window. 0 keeps
          // the nominal window. Kind is checked after the section, because
          // `kind` may follow this key.
          uint64_t x = 0;
          if (!ParseU64(val, &x) || x > 60) {
            return E.Fail("bad gop_s (integer seconds, 0..60)");
          }
          sc.gop_ns = x * 1000000000ULL;
        } else {
          return E.Fail("unknown channel key " + key);
        }
        break;
      }
      case Section::kQuota:
        if (key != "max_triggers_per_hour") {
          return E.Fail("unknown quota key " + key);
        }
        {
          uint64_t x = 0;
          if (!ParseU64(val, &x)) {
            return E.Fail("bad max_triggers_per_hour");
          }
          // Duplicate section headers are rejected above, but a repeated key
          // inside one section would otherwise append a shadowed second limit
          // (QuotaLimit reads the first) and break the DumpConfig round trip
          // (found by the TST-03 INI fuzzer).
          for (const auto &q : cfg.quota_limits) {
            if (q.first == cur_quota_class) {
              return E.Fail("duplicate max_triggers_per_hour in [quota." +
                            cur_quota_class + "]");
            }
          }
          cfg.quota_limits.emplace_back(cur_quota_class,
                                        static_cast<uint32_t>(x));
        }
        break;
      case Section::kSource: {
        SourceRule &r = *cur_source;
        if (!source_keys_seen.insert(key).second) {
          return E.Fail("duplicate source key " + key);
        }
        if (key == "channels") {
          r.channels = SplitList(val);
        } else if (key == "before_s") {
        if (!ParseSecondsNs(val, &r.before_ns))
          return E.Fail("bad before_s");
        } else if (key == "after_s") {
        if (!ParseSecondsNs(val, &r.after_ns))
          return E.Fail("bad after_s");
        } else if (key == "mute_s") {
        if (!ParseSecondsNs(val, &r.mute_ns))
          return E.Fail("bad mute_s");
        } else if (key == "priority") {
        if (!ParseI32(val, &r.priority))
          return E.Fail("bad priority");
        } else if (key == "flush_policy") {
          if (val == "realtime") {
            r.flush_realtime = true;
          } else if (val == "idle") {
            r.flush_realtime = false;
          } else {
            return E.Fail("bad flush_policy " + val);
          }
          r.has_flush = true;  // explicit: [recorder] default must not win
        } else if (key == "quota_class") {
          r.quota_class = val;
        } else if (key == "name") {
          return E.Fail("name comes from the [source.<name>] header");
        } else {
          return E.Fail("unknown source key " + key);
        }
        break;
      }
      case Section::kNone:
        return E.Fail("key outside any section");
      case Section::kSecurity: {
        if (!security_keys_seen.insert(key).second) {
          return E.Fail("duplicate security key " + key);
        }
        if (key == "allowed_uids") {
          cfg.allowed_uids.clear();
          const std::vector<std::string> items = SplitList(val);
          if (items.empty()) {
            return E.Fail(
                "allowed_uids must list at least one uid (omit the key to "
                "accept only the center's euid)");
          }
          for (const std::string &item : items) {
            uint64_t x = 0;
            if (!ParseU64(item, &x) || x > 0xffffffffull) {
              return E.Fail("bad allowed_uids entry " + item);
            }
            const uint32_t uid = static_cast<uint32_t>(x);
            bool dup = false;
            for (uint32_t seen : cfg.allowed_uids) {
            if (seen == uid)
              dup = true;
            }
          if (!dup)
            cfg.allowed_uids.push_back(uid);
          }
        } else if (key == "allow_root") {
          if (val == "true") {
            cfg.allow_root = true;
          } else if (val == "false") {
            cfg.allow_root = false;
          } else {
            return E.Fail("bad allow_root " + val);
          }
        } else if (key == "require_peer_pid") {
          if (val == "true") {
            cfg.require_peer_pid = true;
          } else if (val == "false") {
            cfg.require_peer_pid = false;
          } else {
            return E.Fail("bad require_peer_pid " + val);
          }
        } else if (key == "max_priority_hint") {
          if (!ParseI32(val, &cfg.max_priority_hint) ||
              cfg.max_priority_hint < 0) {
            return E.Fail("bad max_priority_hint (must be >= 0)");
          }
        } else if (key == "allow_legacy_unsequenced") {
          if (val == "true") {
            cfg.allow_legacy_unsequenced = true;
          } else if (val == "false") {
            cfg.allow_legacy_unsequenced = false;
          } else {
            return E.Fail("bad allow_legacy_unsequenced " + val);
          }
        } else if (key == "allow_legacy_v2") {
          if (val == "true") {
            cfg.allow_legacy_v2 = true;
          } else if (val == "false") {
            cfg.allow_legacy_v2 = false;
          } else {
            return E.Fail("bad allow_legacy_v2 " + val);
          }
        } else if (key == "allow_legacy_control") {
          if (val == "true") {
            cfg.allow_legacy_control = true;
          } else if (val == "false") {
            cfg.allow_legacy_control = false;
          } else {
            return E.Fail("bad allow_legacy_control " + val);
          }
        } else if (key == "signing_key") {
          cfg.signing_key_path = val;
        } else if (key == "manifest") {
          if (!integrity::ParseManifestMode(val, &cfg.manifest_mode)) {
            return E.Fail("bad manifest " + val + " (full|metadata|off)");
          }
        } else if (key == "audit_log") {
          cfg.audit_log_path = val;
        } else if (key == "control_audit_log") {
          cfg.control_audit_log_path = val;
        } else if (key == "audit_max_bytes") {
        if (!ParseU64(val, &cfg.audit_max_bytes) || cfg.audit_max_bytes == 0) {
            return E.Fail("bad audit_max_bytes (must be > 0)");
          }
        } else if (key == "audit_rotate_keep") {
          uint64_t x = 0;
          if (!ParseU64(val, &x) || x == 0 || x > 32) {
            return E.Fail("bad audit_rotate_keep (require 1..32)");
          }
          cfg.audit_rotate_keep = static_cast<uint32_t>(x);
        } else if (key == "audit_fsync_interval_s") {
          if (!ParseSecondsNs(val, &cfg.audit_fsync_interval_ns)) {
            return E.Fail("bad audit_fsync_interval_s");
          }
        } else {
          return E.Fail("unknown security key " + key);
        }
        break;
      }
      case Section::kLogging: {
        if (!logging_keys_seen.insert(key).second) {
          return E.Fail("duplicate logging key " + key);
        }
        if (key == "service_file") {
          cfg.log_service_file = val == "stderr" ? std::string() : val;
        } else if (key == "service_max_bytes") {
          if (!ParseU64(val, &cfg.log_service_max_bytes) ||
              cfg.log_service_max_bytes == 0) {
            return E.Fail("bad service_max_bytes (must be > 0)");
          }
        } else if (key == "service_rotate_keep") {
          uint64_t x = 0;
          if (!ParseU64(val, &x) || x == 0 || x > 32) {
            return E.Fail("bad service_rotate_keep (require 1..32)");
          }
          cfg.log_service_rotate_keep = static_cast<uint32_t>(x);
        } else if (key == "fsm_max_bytes") {
          if (!ParseU64(val, &cfg.log_fsm_max_bytes) ||
              cfg.log_fsm_max_bytes == 0) {
            return E.Fail("bad fsm_max_bytes (must be > 0)");
          }
        } else if (key == "fsm_rotate_keep") {
          uint64_t x = 0;
          if (!ParseU64(val, &x) || x == 0 || x > 32) {
            return E.Fail("bad fsm_rotate_keep (require 1..32)");
          }
          cfg.log_fsm_rotate_keep = static_cast<uint32_t>(x);
        } else if (key == "perf_max_bytes") {
          if (!ParseU64(val, &cfg.log_perf_max_bytes) ||
              cfg.log_perf_max_bytes == 0) {
            return E.Fail("bad perf_max_bytes (must be > 0)");
          }
        } else if (key == "perf_rotate_keep") {
          uint64_t x = 0;
          if (!ParseU64(val, &x) || x == 0 || x > 32) {
            return E.Fail("bad perf_rotate_keep (require 1..32)");
          }
          cfg.log_perf_rotate_keep = static_cast<uint32_t>(x);
        } else if (key == "total_budget_mb") {
          uint64_t x = 0;
          if (!ParseU64(val, &x)) {
            return E.Fail("bad total_budget_mb");
          }
          cfg.log_total_budget_bytes = x * 1024 * 1024;
        } else {
          return E.Fail("unknown logging key " + key);
        }
        break;
      }
      case Section::kChassis: {
        if (!chassis_keys_seen.insert(key).second) {
          return E.Fail("duplicate chassis key " + key);
        }
        if (key == "page_shm") {
          cfg.chassis.page_shm = val;
        } else if (key == "struct_id") {
          uint64_t x = 0;
          if (!ParseU64(val, &x) || x > 63) {
            return E.Fail("bad struct_id");
          }
          cfg.chassis.struct_id = static_cast<uint16_t>(x);
        } else if (key == "epb_offset") {
          uint64_t x = 0;
          if (!ParseU64(val, &x)) {
            return E.Fail("bad epb_offset");
          }
          cfg.chassis.epb_offset = static_cast<uint32_t>(x);
        } else if (key == "speed_offset") {
          uint64_t x = 0;
          if (!ParseU64(val, &x)) {
            return E.Fail("bad speed_offset");
          }
          cfg.chassis.speed_offset = static_cast<uint32_t>(x);
        } else if (key == "speed_threshold") {
          if (!ParseDouble(val, &cfg.chassis.speed_threshold)) {
            return E.Fail("bad speed_threshold");
          }
        } else {
          return E.Fail("unknown chassis key " + key);
        }
        break;
      }
      case Section::kContinuous: {
        if (!continuous_keys_seen.insert(key).second) {
          return E.Fail("duplicate continuous key " + key);
        }
        if (key == "auto_arm") {
          if (val == "true") {
            cfg.cont_auto_arm = true;
          } else if (val == "false") {
            cfg.cont_auto_arm = false;
          } else {
            return E.Fail("bad continuous auto_arm " + val);
          }
        } else if (key == "enable") {
          // TRG-17: deprecated spelling. Still works, but the warning keeps
          // "accepted key" == "documented/effective key" honest.
          if (val == "true") {
            cfg.cont_auto_arm = true;
          } else if (val == "false") {
            cfg.cont_auto_arm = false;
          } else {
            return E.Fail("bad continuous enable " + val);
          }
          if (warn != nullptr) {
            if (!warn->empty()) {
              warn->push_back('\n');
            }
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "line %d: [continuous].enable is deprecated; use "
                          "auto_arm (still honored)",
                          line_no);
            warn->append(buf);
          }
        } else if (key == "channels") {
          cfg.cont_channels = SplitList(val);
        } else if (key == "slice_mb") {
          uint64_t x = 0;
          if (!ParseU64(val, &x)) {
            return E.Fail("bad slice_mb");
          }
          if (x == 0) {
            return E.Fail("slice_mb must be > 0");
          }
          cfg.cont_slice_bytes = x * 1024 * 1024;
        } else if (key == "slice_s") {
          if (!ParseSecondsNs(val, &cfg.cont_slice_ns)) {
            return E.Fail("bad slice_s");
          }
        } else if (key == "wait_idle") {
          if (val == "true") {
            cfg.cont_wait_idle = true;
          } else if (val == "false") {
            cfg.cont_wait_idle = false;
          } else {
            return E.Fail("bad wait_idle " + val);
          }
        } else if (key == "demo_idle") {
          if (val == "true") {
            cfg.cont_demo_idle = true;
          } else if (val == "false") {
            cfg.cont_demo_idle = false;
          } else {
            return E.Fail("bad demo_idle " + val);
          }
        } else if (key == "rate_check") {
          if (val != "enforce" && val != "warn" && val != "off") {
            return E.Fail("bad rate_check " + val);
          }
          cfg.cont_rate_check = val;
        } else {
          return E.Fail("unknown continuous key " + key);
        }
        break;
      }
    }
    if (section == Section::kContinuous && key == "channels") {
      cont_channels_line = line_no;  // used later to validate decimation combos
    }
  }

  // TRG-16 shared selector syntax + h264 decimation cross-check.
  std::vector<triggered::ChannelSelectorRule> cont_rules;
  if (!cfg.cont_channels.empty() &&
      !triggered::ParseChannelSelectors(cfg.cont_channels, &cont_rules, err)) {
    if (err != nullptr) {
      char buf[320];
      std::snprintf(buf, sizeof(buf), "line %d: %s", cont_channels_line,
                    err->c_str());
      *err = buf;
    }
    return false;
  }
  for (const triggered::SourceConfig &sc : cfg.recorder_sources) {
    const triggered::ChannelSelectorRule *rule =
        triggered::MatchChannelSelector(cont_rules, sc);
    if (sc.h264_stream && rule != nullptr && rule->decimate > 1) {
      if (err != nullptr) {
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "line %d: h264_stream channel %s cannot be "
                      "decimated (GOP stream)",
                      cont_channels_line, sc.channel_name.c_str());
        *err = buf;
      }
      return false;
    }
  }

  // REC-21: materialize the per-channel CRC default by kind (pinned_slot /
  // h264_stream -> package, mirror_ring -> frame) and reject nonsensical
  // combos. package on a mirror channel would be a silent no-op — small-struct
  // .tbin rows always carry their per-record CRC — so it is a config error
  // like any other unimplementable key (typo-safe, TRG-17).
  for (triggered::SourceConfig &sc : cfg.recorder_sources) {
    if (sc.crc == triggered::SourceConfig::CrcPolicy::kDefault) {
      sc.crc = triggered::EffectiveCrcPolicy(sc);
    }
    if (sc.kind == triggered::SourceConfig::Kind::kMirrorRing &&
        sc.crc == triggered::SourceConfig::CrcPolicy::kPackage) {
      if (err != nullptr) {
        *err = "channel " + sc.shm_name +
               ": crc=package is only valid for pinned_slot channels "
               "(small structs keep per-record CRC; use frame|off)";
      }
      return false;
    }
    if (sc.kind == triggered::SourceConfig::Kind::kMirrorRing &&
        (sc.manifest == triggered::SourceConfig::ManifestPolicy::kMetadata ||
         sc.manifest == triggered::SourceConfig::ManifestPolicy::kOff)) {
      if (err != nullptr) {
        *err = "channel " + sc.shm_name +
               ": manifest=metadata|off is only valid for bulk channels; "
               "small structs and control records always use full hashing";
      }
      return false;
    }
  }
  for (std::size_t i = 0; i < cfg.recorder_sources.size(); ++i) {
    auto &source = cfg.recorder_sources[i];
    const auto &metadata = cfg.recorder_source_metadata[i];
    if (source.kind != triggered::SourceConfig::Kind::kMirrorRing &&
        !metadata.record_channels.empty()) {
      if (err != nullptr) {
        *err = "channel " + source.shm_name +
               ": record_channels is only valid for generated mirror_ring sources";
      }
      return false;
    }
    if (!source.payload_profile.empty() &&
        source.kind != triggered::SourceConfig::Kind::kPinnedSlot) {
      if (err != nullptr) {
        *err = "channel " + source.shm_name +
               ": profile is only valid for pinned_slot/h264_stream channels";
      }
      return false;
    }
    if (source.gop_ns > 0 && !source.h264_stream) {
      if (err != nullptr) {
        *err = "channel " + source.shm_name +
               ": gop_s is only valid for h264_stream";
      }
      return false;
    }
    if (source.payload_profile.empty() &&
        (source.payload_profile_version != 0 ||
         !source.payload_profile_params.empty())) {
      if (err != nullptr) {
        *err = "channel " + source.shm_name +
               ": profile_version/profile_params require profile";
      }
      return false;
    }
    if (source.payload_profile.size() >= schema::kMaxProfileBytes) {
      if (err != nullptr) {
        *err = "channel " + source.shm_name + ": profile is too long";
      }
      return false;
    }
    source.payload_profile_digest =
        schema::HashProfileParams(source.payload_profile_params);
    for (std::size_t j = 0; j < i; ++j) {
      for (const std::string &channel : metadata.record_channels) {
        const auto &previous = cfg.recorder_source_metadata[j].record_channels;
        if (std::find(previous.begin(), previous.end(), channel) != previous.end()) {
          if (err != nullptr) {
            *err = "record channel " + channel + " is assigned to multiple SHM sources";
          }
          return false;
        }
      }
    }
  }

  // Empty trigger topology is almost always a config error; require an
  // explicit opt-in for a read-only/test center with no sources. A continuous-
  // only recorder config ([continuous] auto_arm = true) legitimately has no
  // trigger sources and is exempt.
  const bool continuous_only = cfg.cont_auto_arm;
  if (cfg.sources.empty() && !cfg.allow_empty_sources && !continuous_only) {
    if (err != nullptr) {
      *err = "no [source.*] configured; set [trigger_center] "
          "allow_empty_sources = true to allow an empty center";
    }
    return false;
  }

  // A [chassis] section without page_shm can never attach at runtime
  // (AttachReadOnly("") fails); reject it like any other incomplete section
  // (found by the TST-03 INI fuzzer via the DumpConfig round trip).
  if (cfg.chassis_present && cfg.chassis.page_shm.empty()) {
    if (err != nullptr)
      *err = "[chassis] requires page_shm";
    return false;
  }

  if (cfg.cont_wait_idle && !cfg.chassis_present && !cfg.cont_demo_idle) {
    if (err != nullptr) {
      *err = "[continuous] wait_idle = true requires a real [chassis] or "
          "explicit demo_idle = true";
    }
    return false;
  }

  // Cross-checks: default_source must resolve; quota classes need not exist.
  if (!cfg.default_source.empty()) {
    bool found = false;
    for (const SourceRule &r : cfg.sources) {
      if (r.name == cfg.default_source) found = true;
    }
    if (!found) {
      if (err != nullptr) *err = "default_source not in trigger_sources";
      return false;
    }
  }
  for (const SourceRule &r : cfg.sources) {
    if (r.quota_class.empty()) {
      if (err != nullptr) *err = "source " + r.name + ": missing quota_class";
      return false;
    }
  }

  // TRG-17: slice_mb must fit the disk quota (0 is already rejected above).
  // Enforced whenever a [continuous] section is present, even with
  // auto_arm = false: the slice key still has to describe a possible slice.
  if (cont_section_present && cfg.recorder_disk_quota_bytes > 0 &&
      cfg.cont_slice_bytes > cfg.recorder_disk_quota_bytes) {
    if (err != nullptr) {
      *err = "[continuous].slice_mb exceeds [recorder].disk_quota_mb "
          "(require 0 < slice_mb <= disk_quota_mb)";
    }
    return false;
  }

  // TRG-17: [recorder].flush_policy is the default for sources that do not
  // set their own; an explicit idle must never be overwritten by it.
  for (SourceRule &r : cfg.sources) {
    if (!r.has_flush) r.flush_realtime = cfg.recorder_flush_realtime;
  }

  // TRG-16: every positive source selector must match a configured channel;
  // exclusions may be broad, but the effective selection may not be empty.
  for (const SourceRule &r : cfg.sources) {
    if (!r.channels.empty() && cfg.recorder_sources.empty()) {
      if (err != nullptr) {
        *err = "source " + r.name +
               ": channels requires at least one [channel.*] entry";
      }
      return false;
    }
    std::vector<triggered::ChannelSelectorRule> rules;
    if (!triggered::ParseChannelSelectors(r.channels, &rules, err)) {
      if (err != nullptr) {
        *err = "source " + r.name + ": " + *err;
      }
      return false;
    }
    for (const std::string &sel : r.channels) {
      if (!sel.empty() && sel[0] == '!') {
        continue;
      }
      bool matched = false;
      for (const triggered::SourceConfig &sc : cfg.recorder_sources) {
        if (triggered::ChannelSelectorMatches(sel, sc)) {
          matched = true;
          break;
        }
      }
      if (!matched) {
        if (err != nullptr) {
          *err = "source " + r.name + ": channels selector \"" + sel +
                 "\" matches no [channel.*] entry";
        }
        return false;
      }
    }
    uint32_t selected = 0;
    for (const triggered::SourceConfig &sc : cfg.recorder_sources) {
      const triggered::ChannelSelectorRule *rule =
          triggered::MatchChannelSelector(rules, sc);
      if (rule == nullptr) {
        continue;
      }
      ++selected;
      if (sc.h264_stream && rule->decimate > 1) {
        if (err != nullptr) {
          *err = "source " + r.name + ": h264_stream channel " +
                 sc.channel_name + " cannot be decimated (GOP stream)";
        }
        return false;
      }
    }
    if (!r.channels.empty() && selected == 0) {
      if (err != nullptr) {
        *err = "source " + r.name + ": channels select no [channel.*] entry";
      }
      return false;
    }
  }

  if (cfg.socket_path == cfg.recorder_socket) {
    if (err != nullptr) {
      *err = "[recorder].socket must differ from [trigger_center].socket";
    }
    return false;
  }

  // SEC-02: the audit file must never be the control socket itself (or the
  // first Append would unlink/clobber the bound socket path).
  if (!cfg.audit_log_path.empty() && cfg.audit_log_path == cfg.socket_path) {
    if (err != nullptr) {
      *err = "[security].audit_log must not equal [trigger_center].socket";
    }
    return false;
  }
  if (!cfg.audit_log_path.empty() &&
      cfg.audit_log_path == cfg.recorder_socket) {
    if (err != nullptr) {
      *err = "[security].audit_log must not equal [recorder].socket";
    }
    return false;
  }

  if (!cfg.control_audit_log_path.empty() &&
      (cfg.control_audit_log_path == cfg.socket_path ||
       cfg.control_audit_log_path == cfg.recorder_socket ||
       cfg.control_audit_log_path == cfg.audit_log_path)) {
    if (err != nullptr) {
      *err =
          "[security].control_audit_log must be distinct from both sockets "
          "and audit_log";
    }
    return false;
  }

  // SEC-02: replay protection is off for V1 peers — make the downgrade loud
  // in --check-config and at startup.
  if (cfg.allow_legacy_unsequenced && warn != nullptr) {
    if (!warn->empty()) warn->push_back('\n');
    warn->append(
        "[security].allow_legacy_unsequenced=true: V1 requests are accepted "
        "without replay protection");
  }
  if (cfg.allow_legacy_v2 && warn != nullptr) {
    if (!warn->empty()) warn->push_back('\n');
    warn->append(
        "[security].allow_legacy_v2=true: V2 requests are accepted without "
        "the CRC32 trailer (no per-datagram corruption detection)");
  }
  if (cfg.allow_legacy_control && warn != nullptr) {
    if (!warn->empty()) warn->push_back('\n');
    warn->append(
        "[security].allow_legacy_control=true: recorder V1 text commands are "
        "accepted without sequence or CRC32 replay/integrity protection");
  }
  if (warn != nullptr) {
    if (cfg.manifest_mode != integrity::ManifestMode::kFull) {
      if (!warn->empty()) warn->push_back('\n');
      warn->append("[security].manifest=");
      warn->append(integrity::ManifestModeName(cfg.manifest_mode));
      warn->append(
          ": explicit integrity downgrade; payload verification is reduced");
    }
    for (const triggered::SourceConfig &sc : cfg.recorder_sources) {
      if (sc.manifest != triggered::SourceConfig::ManifestPolicy::kMetadata &&
          sc.manifest != triggered::SourceConfig::ManifestPolicy::kOff) {
        continue;
      }
      if (!warn->empty()) warn->push_back('\n');
      warn->append("channel " + sc.shm_name + ": manifest=");
      warn->append(triggered::ManifestPolicyName(sc.manifest));
      warn->append(": explicit bulk integrity downgrade");
    }
  }

  *out = cfg;
  return true;
}

bool LoadCenterConfig(const char *path, CenterConfig *out, std::string *err,
                      std::string *warn) {
  FilePtr f(std::fopen(path, "rb"));
  if (!f) {
    if (err != nullptr) *err = std::string("cannot open ") + path;
    return false;
  }
  std::string text;
  char buf[4096];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f.get())) > 0) {
    text.append(buf, n);
  }
  return ParseCenterConfig(text, out, err, warn);
}

bool LoadRecordManifest(const char *path, std::vector<std::string> *channels,
                        std::string *err) {
  if (channels == nullptr) return false;
  FilePtr file(std::fopen(path, "rb"));
  if (!file) {
    if (err != nullptr) *err = std::string("cannot open record manifest ") + path;
    return false;
  }
  std::string text;
  char buffer[4096];
  std::size_t bytes = 0;
  while ((bytes = std::fread(buffer, 1, sizeof(buffer), file.get())) > 0) {
    text.append(buffer, bytes);
  }
  auto fail = [&](const std::string &message) {
    if (err != nullptr) *err = std::string(path) + ": " + message;
    return false;
  };
  const std::string count_key = "kRecordManifestCount = ";
  const std::size_t count_pos = text.find(count_key);
  if (count_pos == std::string::npos) return fail("missing kRecordManifestCount");
  const std::size_t count_end = text.find(';', count_pos + count_key.size());
  if (count_end == std::string::npos) return fail("malformed kRecordManifestCount");
  uint64_t count = 0;
  if (!ParseU64(Trim(text.substr(count_pos + count_key.size(),
                                 count_end - count_pos - count_key.size())),
                &count)) {
    return fail("malformed kRecordManifestCount");
  }
  const std::string table_key = "kRecordManifestChannels[] = {";
  const std::size_t table_pos = text.find(table_key);
  if (table_pos == std::string::npos) return fail("missing kRecordManifestChannels");
  const std::size_t body_pos = table_pos + table_key.size();
  const std::size_t table_end = text.find("};", body_pos);
  if (table_end == std::string::npos) return fail("unterminated kRecordManifestChannels");
  const std::string body = Trim(text.substr(body_pos, table_end - body_pos));
  std::vector<std::string> parsed;
  if (count == 0) {
    if (body != "nullptr") return fail("empty manifest must have a null channel sentinel");
  } else {
    std::size_t pos = 0;
    while (pos < body.size()) {
      if (body[pos] != '"') return fail("malformed channel entry");
      const std::size_t quote = body.find('"', pos + 1);
      if (quote == std::string::npos || quote == pos + 1) {
        return fail("malformed channel entry");
      }
      const std::string channel = body.substr(pos + 1, quote - pos - 1);
      for (const char c : channel) {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_') {
          return fail("invalid channel entry");
        }
      }
      parsed.push_back(channel);
      pos = quote + 1;
      while (pos < body.size() &&
             std::isspace(static_cast<unsigned char>(body[pos])) != 0) {
        ++pos;
      }
      if (pos == body.size()) break;
      if (body[pos] != ',') return fail("malformed channel separator");
      ++pos;
      while (pos < body.size() &&
             std::isspace(static_cast<unsigned char>(body[pos])) != 0) {
        ++pos;
      }
    }
  }
  if (parsed.size() != count) return fail("channel count does not match kRecordManifestCount");
  const std::string bulk_key = "kRecordBulkChannels[] = {";
  const std::size_t bulk_pos = text.find(bulk_key);
  if (bulk_pos != std::string::npos) {
    const std::size_t bulk_body = bulk_pos + bulk_key.size();
    const std::size_t bulk_end = text.find("};", bulk_body);
    if (bulk_end == std::string::npos) {
      return fail("unterminated kRecordBulkChannels");
    }
    const std::string bulk = Trim(text.substr(bulk_body, bulk_end - bulk_body));
    std::size_t pos = 0;
    if (bulk.empty()) return fail("empty kRecordBulkChannels");
    while (pos < bulk.size()) {
      if (bulk[pos] != '"') return fail("malformed bulk channel entry");
      const std::size_t quote = bulk.find('"', pos + 1);
      if (quote == std::string::npos || quote == pos + 1) {
        return fail("malformed bulk channel entry");
      }
      const std::string channel = bulk.substr(pos + 1, quote - pos - 1);
      for (const std::string &existing : parsed) {
        if (existing == channel) return fail("duplicate bulk channel " + channel);
      }
      parsed.push_back(channel);
      pos = quote + 1;
      while (pos < bulk.size() &&
             std::isspace(static_cast<unsigned char>(bulk[pos])) != 0) {
        ++pos;
      }
      if (pos == bulk.size()) break;
      if (bulk[pos] != ',') return fail("malformed bulk channel separator");
      ++pos;
      while (pos < bulk.size() &&
             std::isspace(static_cast<unsigned char>(bulk[pos])) != 0) {
        ++pos;
      }
    }
  }
  *channels = std::move(parsed);
  return true;
}

namespace {

bool ValidateLayeredSections(const std::string &text, bool base,
                             std::string *err) {
  std::size_t pos = 0;
  int32_t line_no = 0;
  while (pos <= text.size()) {
    const std::size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, nl - pos);
    pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
    ++line_no;
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    line = Trim(line);
    if (line.empty() || line.front() != '[' || line.back() != ']') {
      continue;
    }
    const std::string name = Trim(line.substr(1, line.size() - 2));
    const bool channel = name.compare(0, 8, "channel.") == 0;
    const bool chassis = name == "chassis";
    // Generated facts live in the base: channels and the chassis predicate.
    // The policy cannot replace either of them.
    const bool rejected = base ? !(channel || chassis) : (channel || chassis);
    if (rejected) {
      if (err != nullptr) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s line %d: ",
                      base ? "base" : "policy", line_no);
        *err = buf;
        *err += base ? "only [channel.*] and [chassis] sections are allowed "
                       "in the base"
                     : (channel
                            ? "[channel.*] sections are not allowed in the policy"
                            : "[chassis] is not allowed in the policy");
      }
      return false;
    }
  }
  return true;
}

std::string MapLayeredLine(const std::string &message, int32_t base_lines) {
  if (message.compare(0, 5, "line ") != 0) {
    return message;
  }
  char *end = nullptr;
  const int64_t combined_line = std::strtoll(message.c_str() + 5, &end, 10);
  if (end == nullptr || *end != ':') {
    return message;
  }
  const bool base = combined_line <= base_lines;
  const int32_t line =
      static_cast<int32_t>(combined_line - (base ? 0 : base_lines));
  return std::string(base ? "base line " : "policy line ") +
         std::to_string(line) + end;
}

bool ReadLayeredFile(const char *path, const char *role, std::string *text,
                     std::string *err) {
  FilePtr f(std::fopen(path, "rb"));
  if (!f) {
    if (err != nullptr) {
      *err = std::string("cannot open ") + role + " config " + path;
    }
    return false;
  }
  char buf[4096];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f.get())) > 0) {
    text->append(buf, n);
  }
  return true;
}

}  // namespace

bool ParseLayeredCenterConfig(const std::string &base_text,
                              const std::string &policy_text, CenterConfig *out,
                              std::string *err, std::string *warn) {
  if (!ValidateLayeredSections(base_text, true, err) ||
      !ValidateLayeredSections(policy_text, false, err)) {
    return false;
  }
  std::string combined = base_text;
  if (!combined.empty() && combined.back() != '\n') {
    combined.push_back('\n');
  }
  const int32_t base_lines = static_cast<int32_t>(
      std::count(combined.begin(), combined.end(), '\n'));
  combined += policy_text;
  if (!ParseCenterConfig(combined, out, err, warn)) {
    if (err != nullptr) {
      *err = MapLayeredLine(*err, base_lines);
    }
    return false;
  }
  std::vector<std::string> manifest;
  for (std::size_t i = 0; i < out->recorder_sources.size(); ++i) {
    const auto &source = out->recorder_sources[i];
    const auto &metadata = out->recorder_source_metadata[i];
    if (!metadata.record_channels.empty()) {
      manifest.insert(manifest.end(), metadata.record_channels.begin(),
                      metadata.record_channels.end());
    } else {
      manifest.push_back(source.channel_name.empty() ? source.shm_name
                                                    : source.channel_name);
    }
  }
  std::vector<const char *> table;
  table.reserve(manifest.size());
  for (const std::string &name : manifest) {
    table.push_back(name.c_str());
  }
  return ValidateManifestSubset(*out, table.empty() ? nullptr : table.data(),
                                table.size(), err);
}

namespace {

void ReplacePidToken(std::string *text, const std::string &pid) {
  const std::string token = "%pid";
  for (;;) {
    const std::size_t at = text->find(token);
    if (at == std::string::npos) return;
    text->replace(at, token.size(), pid);
  }
}

bool TextHasPidToken(const std::string &text) {
  return text.find("%pid") != std::string::npos;
}

}  // namespace

bool BindShmPid(CenterConfig *cfg, uint32_t supervisor_pid, std::string *err) {
  if (cfg == nullptr) return false;
  bool need = false;
  for (const triggered::SourceConfig &source : cfg->recorder_sources) {
    if (TextHasPidToken(source.shm_name)) need = true;
  }
  if (TextHasPidToken(cfg->chassis.page_shm)) need = true;
  for (const SourceRule &rule : cfg->sources) {
    for (const std::string &channel : rule.channels) {
      if (TextHasPidToken(channel)) need = true;
    }
  }
  for (const std::string &channel : cfg->cont_channels) {
    if (TextHasPidToken(channel)) need = true;
  }
  if (!need) return true;
  if (supervisor_pid == 0) {
    if (err != nullptr) {
      *err = "shm name contains %pid; pass the supervisor pid "
             "(trigger_center --shm-pid)";
    }
    return false;
  }
  const std::string pid = std::to_string(supervisor_pid);
  for (triggered::SourceConfig &source : cfg->recorder_sources) {
    ReplacePidToken(&source.shm_name, pid);
  }
  ReplacePidToken(&cfg->chassis.page_shm, pid);
  for (SourceRule &rule : cfg->sources) {
    for (std::string &channel : rule.channels) {
      ReplacePidToken(&channel, pid);
    }
  }
  for (std::string &channel : cfg->cont_channels) {
    ReplacePidToken(&channel, pid);
  }
  return true;
}

bool LoadLayeredCenterConfig(const char *base_path, const char *policy_path,
                             CenterConfig *out, std::string *err,
                             std::string *warn) {
  std::string base_text;
  std::string policy_text;
  if (!ReadLayeredFile(base_path, "base", &base_text, err) ||
      !ReadLayeredFile(policy_path, "policy", &policy_text, err)) {
    return false;
  }
  return ParseLayeredCenterConfig(base_text, policy_text, out, err, warn);
}

namespace {

bool ValidateManifestSubsetImpl(
    const CenterConfig &cfg, const std::vector<std::string> &manifest_channels,
    bool generated_manifest, std::string *err) {
  // The standalone host passes an empty vector to disable this optional check.
  // A generated empty table instead means that the graph records nothing.
  if (manifest_channels.empty() && !generated_manifest) return true;
  auto in_manifest = [&manifest_channels](const std::string &name) {
    if (name.empty()) return false;
    for (const std::string &m : manifest_channels) {
      if (m == name) return true;
      // [channel.<shm>] suffixes commonly carry a leading '/' while graph
      // channels are bare C identifiers.
      if (!name.empty() && name[0] == '/' && m == name.substr(1)) return true;
    }
    return false;
  };
  for (std::size_t i = 0; i < cfg.recorder_sources.size(); ++i) {
    const auto &sc = cfg.recorder_sources[i];
    const auto &metadata = cfg.recorder_source_metadata[i];
    if (!metadata.record_channels.empty()) {
      for (const std::string &channel : metadata.record_channels) {
        if (!in_manifest(channel)) {
          if (err != nullptr) {
            *err = "[channel." + sc.shm_name + "] record channel '" + channel +
                   "' is not recorded in the graph record manifest";
          }
          return false;
        }
      }
      continue;
    }
    const std::string &key =
        sc.channel_name.empty() ? sc.shm_name : sc.channel_name;
    if (in_manifest(key)) continue;
    if (err != nullptr) {
      *err = "[channel." + sc.shm_name + "] channel '" + key +
             "' is not recorded in the graph record manifest";
    }
    return false;
  }
  auto validate_selectors = [&](const std::vector<std::string> &entries,
                                const std::string &owner) {
    std::vector<triggered::ChannelSelectorRule> rules;
    if (!triggered::ParseChannelSelectors(entries, &rules, err)) {
      return false;
    }
    if (manifest_channels.empty() && rules.empty()) {
      if (err != nullptr) {
        *err = owner + " selects from an empty graph record manifest";
      }
      return false;
    }
    for (const triggered::ChannelSelectorRule &rule : rules) {
      if (rule.exclude) {
        continue;
      }
      bool matched = false;
      for (const std::string &name : manifest_channels) {
        for (uint32_t kind = 0; kind < 2 && !matched; ++kind) {
          triggered::SourceConfig candidate;
          candidate.kind = kind == 0
                               ? triggered::SourceConfig::Kind::kMirrorRing
                               : triggered::SourceConfig::Kind::kPinnedSlot;
          candidate.shm_name = "/" + name;
          candidate.channel_name = name;
          matched =
              triggered::MatchChannelSelector({rule}, candidate) != nullptr;
        }
        if (matched) {
          break;
        }
      }
      if (!matched) {
        if (err != nullptr) {
          *err = owner +
                 " channel selector is outside the graph record manifest";
        }
        return false;
      }
    }
    return true;
  };
  for (const SourceRule &source : cfg.sources) {
    if (!validate_selectors(source.channels, "source " + source.name)) {
      return false;
    }
  }
  if (cfg.cont_section_present) {
    std::vector<std::string> entries = cfg.cont_channels;
    if (entries.empty()) {
      entries = {"small_struct:*"};
    }
    if (!validate_selectors(entries, "continuous")) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool ValidateManifestSubset(const CenterConfig &cfg,
                            const std::vector<std::string> &manifest_channels,
                            std::string *err) {
  return ValidateManifestSubsetImpl(cfg, manifest_channels, false, err);
}

bool ValidateManifestSubset(const CenterConfig &cfg,
                            const char *const *manifest_channels,
                            std::size_t count, std::string *err) {
  if (manifest_channels == nullptr && count != 0) {
    if (err != nullptr) *err = "generated record manifest table is null";
    return false;
  }
  std::vector<std::string> channels;
  channels.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    if (manifest_channels[i] == nullptr) {
      if (err != nullptr) {
        *err = "generated record manifest contains a null channel";
      }
      return false;
    }
    channels.emplace_back(manifest_channels[i]);
  }
  return ValidateManifestSubsetImpl(cfg, channels, true, err);
}

bool ValidateContinuousPlan(const CenterConfig &cfg, ContinuousPlanReport *out,
                            std::string *warn, std::string *err) {
  if (out == nullptr) return false;
  if (cfg.cont_slice_bytes > 0 && cfg.recorder_disk_quota_bytes > 0 &&
      cfg.cont_slice_bytes > cfg.recorder_disk_quota_bytes) {
    if (err != nullptr) {
      *err = "continuous slice_mb exceeds recorder disk_quota_mb";
    }
    return false;
  }
  // Channel list syntax (default small_struct:*).
  std::vector<std::string> entries = cfg.cont_channels;
  if (entries.empty())
    entries = {"small_struct:*"};
  std::vector<triggered::ContinuousChannelRule> rules;
  if (!triggered::TriggeredRecorder::ParseContinuousChannels(entries, &rules,
                                                             err)) {
    return false;
  }
  auto included = [&](const triggered::SourceConfig &sc,
                      uint32_t *decimate) -> bool {
    const triggered::ChannelSelectorRule *match =
        triggered::MatchChannelSelector(rules, sc);
    if (match != nullptr && decimate != nullptr) {
      *decimate = match->decimate;
    }
    return match != nullptr;
  };

  uint64_t sustained = 0;
  uint32_t matched = 0;
  for (std::size_t i = 0; i < cfg.recorder_sources.size(); ++i) {
    const auto &sc = cfg.recorder_sources[i];
    uint32_t dec = 1;
    if (!included(sc, &dec)) continue;
    ++matched;
    if (i < cfg.recorder_source_metadata.size()) {
      const auto &metadata = cfg.recorder_source_metadata[i];
      const bool complete = sc.h264_stream
                                ? metadata.bitrate_kbps_present && sc.bitrate_kbps > 0
                                : metadata.rate_hz_present &&
                                      metadata.avg_frame_bytes_present && sc.rate_hz > 0 &&
                                      sc.avg_frame_bytes > 0;
      if (!complete) {
        if (err != nullptr) {
          *err = "channel " + sc.shm_name +
                 ": continuous rate metadata is incomplete; regenerate trigger.base.ini "
                 "with contract sizes and resolvable graph rates";
        }
        return false;
      }
    }
    uint64_t rate = 0;
    if (sc.h264_stream) {
      rate = static_cast<uint64_t>(sc.bitrate_kbps) * 1000 / 8;  // bitrate / 8
    } else {
      // pinned_slot: rate_hz x avg_frame_bytes / dec; small_struct: rate_hz x
      // avg_frame_bytes (an approximation from the contract payload).
      rate = static_cast<uint64_t>(sc.rate_hz *
                                   static_cast<double>(sc.avg_frame_bytes)) /
             dec;
    }
    sustained += rate;
  }
  if (matched == 0) {
    if (err != nullptr) {
      *err = "continuous channel selector matches no configured source";
    }
    return false;
  }
  out->sustained_bps = sustained;
  out->disk_bytes_per_hour = sustained * 3600;
  out->quota_coverage_hours =
      sustained > 0 && cfg.recorder_disk_quota_bytes > 0
          ? static_cast<double>(cfg.recorder_disk_quota_bytes) /
                static_cast<double>(sustained) / 3600.0
          : (sustained > 0 ? -1.0 : 0.0);  // -1: no quota constraint
  if (cfg.recorder_io_rate_limit_bps > 0 &&
      sustained > cfg.recorder_io_rate_limit_bps) {
    out->rate_limit_too_low = true;
    char buf[256];
    std::snprintf(
        buf, sizeof(buf),
        "continuous sustained rate %llu B/s exceeds io_rate_limit "
        "%llu B/s (continuous traffic will be throttled into lag and dropped)",
        static_cast<unsigned long long>(sustained),
        static_cast<unsigned long long>(cfg.recorder_io_rate_limit_bps));
    if (cfg.cont_rate_check == "enforce") {
      if (err != nullptr) *err = buf;
      return false;
    }
    if (cfg.cont_rate_check == "warn" && warn != nullptr) *warn = buf;
  }
  return true;
}

namespace {

bool SameRecorderSource(const triggered::SourceConfig &a,
                        const triggered::SourceConfig &b) {
  return a.kind == b.kind && a.shm_name == b.shm_name &&
         a.channel_name == b.channel_name && a.h264_stream == b.h264_stream &&
         a.rate_hz == b.rate_hz && a.avg_frame_bytes == b.avg_frame_bytes &&
         a.bitrate_kbps == b.bitrate_kbps && a.gop_ns == b.gop_ns &&
         a.crc == b.crc &&
         a.manifest == b.manifest &&
         a.payload_profile == b.payload_profile &&
         a.payload_profile_version == b.payload_profile_version &&
         a.payload_profile_params == b.payload_profile_params &&
         a.record_channels == b.record_channels;
}

bool SameChassis(const idle::ChassisSpec &a, const idle::ChassisSpec &b) {
  return a.page_shm == b.page_shm && a.struct_id == b.struct_id &&
         a.epb_offset == b.epb_offset && a.speed_offset == b.speed_offset &&
         a.speed_threshold == b.speed_threshold;
}

}  // namespace

bool ReloadRequiresRestart(const CenterConfig &before,
                           const CenterConfig &after) {
  // TRG-10: SIGHUP only re-reads policy the Center consumes live (sources,
  // quotas, security). Everything the host binds once at startup — the
  // recorder/retention/idle objects and the [chassis] predicate — must be
  // rejected here, otherwise `config reloaded` would report a change that
  // never takes effect (silent drift).
  if (before.recorder_out_dir != after.recorder_out_dir ||
      before.recorder_pending_quota != after.recorder_pending_quota ||
      before.recorder_io_rate_limit_bps != after.recorder_io_rate_limit_bps ||
      before.recorder_disk_quota_bytes != after.recorder_disk_quota_bytes ||
      before.recorder_high_reserved_bytes !=
          after.recorder_high_reserved_bytes ||
      before.recorder_incomplete_policy != after.recorder_incomplete_policy ||
      before.recorder_incomplete_max_age_ns !=
          after.recorder_incomplete_max_age_ns ||
      before.recorder_socket != after.recorder_socket ||
      before.manifest_mode != after.manifest_mode ||
      before.recorder_flush_realtime != after.recorder_flush_realtime ||
      before.recorder_idle_hold_ns != after.recorder_idle_hold_ns ||
      before.recorder_idle_fallback_idle != after.recorder_idle_fallback_idle ||
      before.chassis_present != after.chassis_present ||
      !SameChassis(before.chassis, after.chassis)) {
    return true;
  }
  // TRG-19: socket geometry and the allocated dedup window are startup-only.
  // T5: the state file and its exclusive lock are also bound at Init.
  if (before.tick_ns != after.tick_ns ||
      before.socket_poll_budget_ns != after.socket_poll_budget_ns ||
      before.max_trigger_rate_hz != after.max_trigger_rate_hz ||
      before.dedup_window_entries != after.dedup_window_entries ||
      before.dedup_window_ns != after.dedup_window_ns ||
      before.quota_state_path != after.quota_state_path) {
    return true;
  }
  // OPS-05: the log writers/budget are opened once at startup.
  if (before.log_fsm_max_bytes != after.log_fsm_max_bytes ||
      before.log_fsm_rotate_keep != after.log_fsm_rotate_keep ||
      before.log_perf_max_bytes != after.log_perf_max_bytes ||
      before.log_perf_rotate_keep != after.log_perf_rotate_keep ||
      before.log_service_file != after.log_service_file ||
      before.log_service_max_bytes != after.log_service_max_bytes ||
      before.log_service_rotate_keep != after.log_service_rotate_keep ||
      before.log_total_budget_bytes != after.log_total_budget_bytes) {
    return true;
  }
  if (before.cont_auto_arm != after.cont_auto_arm ||
      before.cont_channels != after.cont_channels ||
      before.cont_slice_bytes != after.cont_slice_bytes ||
      before.cont_slice_ns != after.cont_slice_ns ||
      before.cont_wait_idle != after.cont_wait_idle ||
      before.cont_demo_idle != after.cont_demo_idle ||
      before.cont_rate_check != after.cont_rate_check) {
    return true;
  }
  // Recorder channel layout is baked into the recorder/shm pages at startup.
  if (before.recorder_sources.size() != after.recorder_sources.size() ||
      before.recorder_source_metadata.size() != after.recorder_source_metadata.size()) {
    return true;
  }
  for (std::size_t i = 0; i < before.recorder_sources.size(); ++i) {
    const auto &a = before.recorder_sources[i];
    const auto &b = after.recorder_sources[i];
    if (!SameRecorderSource(a, b)) return true;
    if (i < before.recorder_source_metadata.size()) {
      const auto &ma = before.recorder_source_metadata[i];
      const auto &mb = after.recorder_source_metadata[i];
      if (ma.rate_hz_present != mb.rate_hz_present ||
          ma.avg_frame_bytes_present != mb.avg_frame_bytes_present ||
          ma.bitrate_kbps_present != mb.bitrate_kbps_present ||
          ma.record_channels != mb.record_channels) {
        return true;
      }
    }
  }
  return false;
}

std::string DumpConfig(const CenterConfig &cfg) {
  std::string out;
  char buf[512];
  auto line = [&](const char *fmt, auto... args) {
    // Names come from the config text and are unbounded; format into an
    // exact-size buffer on overflow so a long [channel.<name>] / socket path
    // survives the dump->reparse round trip instead of being silently
    // truncated at 512 bytes (found by the TST-03 INI fuzzer).
    const int n = std::snprintf(buf, sizeof(buf), fmt, args...);
    if (n < 0)
      return;
    if (static_cast<std::size_t>(n) < sizeof(buf)) {
      out.append(buf, static_cast<std::size_t>(n));
    } else {
      std::vector<char> big(static_cast<std::size_t>(n) + 1);
      std::snprintf(big.data(), big.size(), fmt, args...);
      out.append(big.data(), static_cast<std::size_t>(n));
    }
    out.push_back('\n');
  };
  auto secs = [](uint64_t ns) { return static_cast<double>(ns) / 1e9; };

  out.append("# effective trigger_center configuration (TRG-17)\n");
  out.append("[trigger_center]\n");
  line("socket=%s", cfg.socket_path.c_str());
  line("max_concurrent_captures=%u", cfg.max_concurrent_captures);
  line("tick_ms=%llu",
       static_cast<unsigned long long>(cfg.tick_ns / 1000000ull));
  line("socket_poll_budget_us=%llu",
       static_cast<unsigned long long>(cfg.socket_poll_budget_ns / 1000ull));
  line("max_trigger_rate_hz=%u", cfg.max_trigger_rate_hz);
  line("dedup_window_entries=%u", cfg.dedup_window_entries);
  line("dedup_window_ms=%llu",
       static_cast<unsigned long long>(cfg.dedup_window_ns / 1000000ull));
  if (cfg.quota_state_path.empty()) {
    out.append("quota_state=none\n");
  } else {
    line("quota_state=%s", cfg.quota_state_path.c_str());
  }
  if (cfg.default_source.empty()) {
    out.append("# default_source=<unset: unknown sources are rejected>\n");
  } else {
    line("default_source=%s", cfg.default_source.c_str());
  }
  line("allow_empty_sources=%s", cfg.allow_empty_sources ? "true" : "false");
  line("crash_priority=%d", cfg.crash_priority);

  out.append("[security]\n");
  if (cfg.allowed_uids.empty()) {
    out.append(
        "# allowed_uids=<unset: only the center's own euid is accepted>\n");
  } else {
    out.append("allowed_uids=");
    for (std::size_t i = 0; i < cfg.allowed_uids.size(); ++i) {
      if (i != 0)
        out.push_back(',');
      char uid_buf[16];
      std::snprintf(uid_buf, sizeof(uid_buf), "%u", cfg.allowed_uids[i]);
      out.append(uid_buf);
    }
    out.push_back('\n');
  }
  line("allow_root=%s", cfg.allow_root ? "true" : "false");
  line("require_peer_pid=%s", cfg.require_peer_pid ? "true" : "false");
  line("max_priority_hint=%d", cfg.max_priority_hint);
  line("allow_legacy_unsequenced=%s",
       cfg.allow_legacy_unsequenced ? "true" : "false");
  line("allow_legacy_v2=%s", cfg.allow_legacy_v2 ? "true" : "false");
  line("allow_legacy_control=%s", cfg.allow_legacy_control ? "true" : "false");
  line("manifest=%s", integrity::ManifestModeName(cfg.manifest_mode));
  if (cfg.signing_key_path.empty()) {
    out.append("# signing_key=<unset: evidence manifests are unsigned>\n");
  } else {
    line("signing_key=%s", cfg.signing_key_path.c_str());
  }
  if (cfg.audit_log_path.empty()) {
    line("audit_log=%s.audit", cfg.socket_path.c_str());
  } else {
    line("audit_log=%s", cfg.audit_log_path.c_str());
  }
  if (cfg.control_audit_log_path.empty()) {
    line("control_audit_log=%s.audit", cfg.recorder_socket.c_str());
  } else {
    line("control_audit_log=%s", cfg.control_audit_log_path.c_str());
  }
  line("audit_max_bytes=%llu",
       static_cast<unsigned long long>(cfg.audit_max_bytes));
  line("audit_rotate_keep=%u", cfg.audit_rotate_keep);
  line("audit_fsync_interval_s=%.9g", secs(cfg.audit_fsync_interval_ns));

  out.append("[logging]\n");
  if (cfg.log_service_file.empty()) {
    out.append("service_file=stderr\n");
  } else {
    line("service_file=%s", cfg.log_service_file.c_str());
  }
  line("service_max_bytes=%llu",
       static_cast<unsigned long long>(cfg.log_service_max_bytes));
  line("service_rotate_keep=%u", cfg.log_service_rotate_keep);
  line("fsm_max_bytes=%llu",
       static_cast<unsigned long long>(cfg.log_fsm_max_bytes));
  line("fsm_rotate_keep=%u", cfg.log_fsm_rotate_keep);
  line("perf_max_bytes=%llu",
       static_cast<unsigned long long>(cfg.log_perf_max_bytes));
  line("perf_rotate_keep=%u", cfg.log_perf_rotate_keep);
  line("total_budget_mb=%llu", static_cast<unsigned long long>(
                                   cfg.log_total_budget_bytes / (1024ULL * 1024ULL)));

  out.append("[recorder]\n");
  if (cfg.recorder_out_dir.empty()) {
    out.append("# output_dir=<unset>\n");
  } else {
    line("output_dir=%s", cfg.recorder_out_dir.c_str());
  }
  line("socket=%s", cfg.recorder_socket.c_str());
  line("pending_bytes_quota=%llu",
       static_cast<unsigned long long>(cfg.recorder_pending_quota));
  line("flush_policy=%s", cfg.recorder_flush_realtime ? "realtime" : "idle");
  line("idle_hold_s=%.9g", secs(cfg.recorder_idle_hold_ns));
  line("idle_fallback=%s",
       cfg.recorder_idle_fallback_idle ? "assume_idle" : "always_busy");
  line("io_rate_limit_mbps=%llu",
       static_cast<unsigned long long>(cfg.recorder_io_rate_limit_bps /
                                       (1000ULL * 1000ULL)));
  line("disk_quota_mb=%llu",
       static_cast<unsigned long long>(cfg.recorder_disk_quota_bytes /
                                       (1024ULL * 1024ULL)));
  line("high_reserved_mb=%llu",
       static_cast<unsigned long long>(cfg.recorder_high_reserved_bytes /
                                       (1024ULL * 1024ULL)));
  line("incomplete_policy=%s", cfg.recorder_incomplete_policy.c_str());
  line("incomplete_max_age_s=%.9g", secs(cfg.recorder_incomplete_max_age_ns));

  for (std::size_t i = 0; i < cfg.recorder_sources.size(); ++i) {
    const auto &sc = cfg.recorder_sources[i];
    const ChannelConfigMetadata empty_metadata;
    const auto &metadata = i < cfg.recorder_source_metadata.size()
                               ? cfg.recorder_source_metadata[i]
                               : empty_metadata;
    line("[channel.%s]", sc.shm_name.c_str());
    const char *kind = "mirror_ring";
    if (sc.kind == triggered::SourceConfig::Kind::kPinnedSlot) {
      kind = sc.h264_stream ? "h264_stream" : "pinned_slot";
    }
    line("kind=%s", kind);
    if (!sc.channel_name.empty()) {
      line("name=%s", sc.channel_name.c_str());
    } else {
      line("# name=%s (default: shm name)", sc.shm_name.c_str());
    }
    if (!metadata.record_channels.empty()) {
      out.append("record_channels=");
      for (std::size_t j = 0; j < metadata.record_channels.size(); ++j) {
        if (j != 0) out.push_back(',');
        out.append(metadata.record_channels[j]);
      }
      out.push_back('\n');
    }
    if (i >= cfg.recorder_source_metadata.size() || metadata.rate_hz_present) {
      line("rate_hz=%.9g", sc.rate_hz);
    }
    if (i >= cfg.recorder_source_metadata.size() || metadata.avg_frame_bytes_present) {
      line("avg_frame_bytes=%llu",
           static_cast<unsigned long long>(sc.avg_frame_bytes));
    }
    if (i >= cfg.recorder_source_metadata.size() || metadata.bitrate_kbps_present) {
      line("bitrate_kbps=%u", sc.bitrate_kbps);
    }
    line("gop_s=%llu",
         static_cast<unsigned long long>(sc.gop_ns / 1000000000ULL));
    // REC-21: effective policy (defaults are materialised at parse time).
    line("profile=%s",
         sc.payload_profile.empty() ? "none" : sc.payload_profile.c_str());
    line("profile_version=%u", sc.payload_profile_version);
    line("profile_params=%s", sc.payload_profile_params.empty()
                                  ? "none"
                                  : sc.payload_profile_params.c_str());
    line("crc=%s", triggered::CrcPolicyName(triggered::EffectiveCrcPolicy(sc)));
    const integrity::ManifestMode manifest =
        sc.kind == triggered::SourceConfig::Kind::kMirrorRing
            ? integrity::ManifestMode::kFull
            : triggered::EffectiveManifestMode(sc, cfg.manifest_mode);
    line("manifest=%s", integrity::ManifestModeName(manifest));
  }

  for (const SourceRule &r : cfg.sources) {
    line("[source.%s]", r.name.c_str());
    if (r.channels.empty()) {
      out.append("# channels=* (effective: all configured channels)\n");
    } else {
      out.append("channels=");
      for (std::size_t i = 0; i < r.channels.size(); ++i) {
        if (i != 0)
          out.push_back(',');
        out.append(r.channels[i]);
      }
      out.push_back('\n');
    }
    line("before_s=%.9g", secs(r.before_ns));
    line("after_s=%.9g", secs(r.after_ns));
    line("priority=%d", r.priority);
    line("mute_s=%.9g", secs(r.mute_ns));
    line("flush_policy=%s", r.flush_realtime ? "realtime" : "idle");
    line("quota_class=%s", r.quota_class.c_str());
  }

  for (const auto &q : cfg.quota_limits) {
    line("[quota.%s]", q.first.c_str());
    line("max_triggers_per_hour=%u", q.second);
  }

  if (cfg.cont_section_present) {
    out.append("[continuous]\n");
    line("auto_arm=%s", cfg.cont_auto_arm ? "true" : "false");
    if (cfg.cont_channels.empty()) {
      out.append("# channels=small_struct:* (effective default)\n");
    } else {
      out.append("channels=");
      for (std::size_t i = 0; i < cfg.cont_channels.size(); ++i) {
        if (i != 0)
          out.push_back(',');
        out.append(cfg.cont_channels[i]);
      }
      out.push_back('\n');
    }
    line("slice_mb=%llu",
         static_cast<unsigned long long>(cfg.cont_slice_bytes / (1024ULL * 1024ULL)));
    line("slice_s=%.9g", secs(cfg.cont_slice_ns));
    line("wait_idle=%s", cfg.cont_wait_idle ? "true" : "false");
    line("demo_idle=%s", cfg.cont_demo_idle ? "true" : "false");
    line("rate_check=%s", cfg.cont_rate_check.c_str());
  } else {
    out.append(
        "# [continuous] not configured (defaults: auto_arm=false, "
        "channels=small_struct:*, slice_mb=512, slice_s=0, wait_idle=false, "
        "demo_idle=false, rate_check=warn)\n");
  }

  if (cfg.chassis_present) {
    out.append("[chassis]\n");
    line("page_shm=%s", cfg.chassis.page_shm.c_str());
    line("struct_id=%u", static_cast<unsigned>(cfg.chassis.struct_id));
    line("epb_offset=%u", cfg.chassis.epb_offset);
    line("speed_offset=%u", cfg.chassis.speed_offset);
    line("speed_threshold=%.9g", cfg.chassis.speed_threshold);
  } else {
    out.append("# [chassis] not configured\n");
  }
  return out;
}

}  // namespace trigger_cfg
}  // namespace thyme
