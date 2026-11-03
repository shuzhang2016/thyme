// test_trigger_config_schema.cpp — ARCH-13 slice 1: one schema, four consumers.
//
// The checked-in generated table (trigger_config_schema.h) is the same data the
// docs tables and --check-config use. This test proves the parser agrees with
// it for [trigger_center]:
//
//   * every implemented sample parses and materialises in DumpConfig;
//   * rejected/future rows are parse errors with the documented text;
//   * unknown and repeated keys are errors (no accepted-but-ignored key);
//   * each schema default equals the parser default (dump equality);
//   * ReloadRequiresRestart matches the declared reload class per key;
//   * ValidateConfigSchema accepts the effective key set and can fail.
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>

#include "thyme/check.h"
#include "thyme/record/trigger_config.h"
#include "thyme/record/trigger_config_schema.h"
#include "thyme/record/trigger_config_schema_check.h"

using namespace thyme;

namespace {

using Row = trigger_cfg::ConfigKeySchema;

const char *kBase = R"(
[trigger_center]
[channel./demo]
kind = mirror_ring
record_channels = demo
[source.manual]
channels = /demo
quota_class = manual
[source.second]
channels = /demo
quota_class = manual
[recorder]
[logging]
[security]
[continuous]
[quota.default]
)";

const char *kCenterHeader = "[trigger_center]";
const char *kRecorderHeader = "[recorder]";
const char *kLoggingHeader = "[logging]";
const char *kSecurityHeader = "[security]";
const char *kContinuousHeader = "[continuous]";
const char *kChassisHeader = "[chassis]";
const char *kQuotaHeader = "[quota.default]";
const char *kSourceHeader = "[source.manual]";
const char *kChannelHeader = "[channel./demo]";

bool Parse(const std::string &text, trigger_cfg::CenterConfig *cfg,
           std::string *err, std::string *warn = nullptr) {
  return trigger_cfg::ParseCenterConfig(text, cfg, err, warn);
}

std::string InjectAfterCenterHeader(const std::string &text,
                                    const std::string &body) {
  std::string out(text);
  const std::size_t at = out.find(kCenterHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kCenterHeader), body);
  return out;
}

std::string Inject(const std::string &key, const std::string &value) {
  return InjectAfterCenterHeader(kBase, "\n" + key + " = " + value);
}

std::string InjectRecorder(const std::string &key, const std::string &value) {
  std::string out(kBase);
  const std::size_t at = out.find(kRecorderHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kRecorderHeader), "\n" + key + " = " + value);
  return out;
}

std::string InjectLogging(const std::string &key, const std::string &value) {
  std::string out(kBase);
  const std::size_t at = out.find(kLoggingHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kLoggingHeader), "\n" + key + " = " + value);
  return out;
}

std::string InjectSecurity(const std::string &key, const std::string &value) {
  std::string out(kBase);
  const std::size_t at = out.find(kSecurityHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kSecurityHeader), "\n" + key + " = " + value);
  return out;
}

std::string InjectContinuous(const std::string &key, const std::string &value) {
  std::string out(kBase);
  const std::size_t at = out.find(kContinuousHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kContinuousHeader), "\n" + key + " = " + value);
  return out;
}

std::string InjectChassis(const std::string &key, const std::string &value) {
  return std::string(kBase) + "\n" + kChassisHeader + "\n" + key + " = " +
         value;
}

std::string InjectQuota(const std::string &key, const std::string &value) {
  std::string out(kBase);
  const std::size_t at = out.find(kQuotaHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kQuotaHeader), "\n" + key + " = " + value);
  return out;
}

std::string InjectSource(const std::string &key, const std::string &value) {
  std::string out(kBase);
  const std::size_t at = out.find(kSourceHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kSourceHeader), "\n" + key + " = " + value);
  return out;
}

std::string InjectChannel(const std::string &key, const std::string &value) {
  std::string out(kBase);
  const std::size_t at = out.find(kChannelHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kChannelHeader), "\n" + key + " = " + value);
  return out;
}

std::string MaximalChannel() {
  std::string body;
  for (std::size_t i = 0; i < trigger_cfg::kChannelSchemaCount; ++i) {
    const Row &row = trigger_cfg::kChannelSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    // kBase already sets kind and record_channels on [channel./demo].
    // gop_s sample is only legal on h264; the dump still emits the default 0.
    if (std::string(row.key) == "kind" ||
        std::string(row.key) == "record_channels" ||
        std::string(row.key) == "profile" ||
        std::string(row.key) == "profile_version" ||
        std::string(row.key) == "profile_params" ||
        std::string(row.key) == "gop_s")
      continue;
    body.append("\n").append(row.key).append(" = ").append(row.sample);
  }
  std::string out(kBase);
  const std::size_t at = out.find(kChannelHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kChannelHeader), body);
  return out;
}

std::string MaximalSource() {
  std::string body;
  for (std::size_t i = 0; i < trigger_cfg::kSourceSchemaCount; ++i) {
    const Row &row = trigger_cfg::kSourceSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    // kBase already sets these two on [source.manual].
    if (std::string(row.key) == "channels" ||
        std::string(row.key) == "quota_class")
      continue;
    body.append("\n").append(row.key).append(" = ").append(row.sample);
  }
  std::string out(kBase);
  const std::size_t at = out.find(kSourceHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kSourceHeader), body);
  return out;
}

std::string MaximalQuota() {
  std::string body;
  for (std::size_t i = 0; i < trigger_cfg::kQuotaSchemaCount; ++i) {
    const Row &row = trigger_cfg::kQuotaSchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      body.append("\n").append(row.key).append(" = ").append(row.sample);
  }
  std::string out(kBase);
  const std::size_t at = out.find(kQuotaHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kQuotaHeader), body);
  return out;
}

std::string MaximalChassis() {
  std::string body;
  for (std::size_t i = 0; i < trigger_cfg::kChassisSchemaCount; ++i) {
    const Row &row = trigger_cfg::kChassisSchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      body.append("\n").append(row.key).append(" = ").append(row.sample);
  }
  std::string out(kBase);
  if (body.find("page_shm") == std::string::npos) {
    body.append("\npage_shm = /tmp/schema_chassis");
  }
  out.append("\n").append(kChassisHeader).append(body);
  return out;
}

std::string MaximalContinuous() {
  std::string body;
  for (std::size_t i = 0; i < trigger_cfg::kContinuousSchemaCount; ++i) {
    const Row &row = trigger_cfg::kContinuousSchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      body.append("\n").append(row.key).append(" = ").append(row.sample);
  }
  std::string out(kBase);
  const std::size_t at = out.find(kContinuousHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kContinuousHeader), body);
  return out;
}

std::string MaximalSecurity() {
  std::string body;
  for (std::size_t i = 0; i < trigger_cfg::kSecuritySchemaCount; ++i) {
    const Row &row = trigger_cfg::kSecuritySchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      body.append("\n").append(row.key).append(" = ").append(row.sample);
  }
  std::string out(kBase);
  const std::size_t at = out.find(kSecurityHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kSecurityHeader), body);
  return out;
}

std::string MaximalLogging() {
  std::string body;
  for (std::size_t i = 0; i < trigger_cfg::kLoggingSchemaCount; ++i) {
    const Row &row = trigger_cfg::kLoggingSchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      body.append("\n").append(row.key).append(" = ").append(row.sample);
  }
  std::string out(kBase);
  const std::size_t at = out.find(kLoggingHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kLoggingHeader), body);
  return out;
}

std::string MaximalRecorder() {
  std::string body;
  for (std::size_t i = 0; i < trigger_cfg::kRecorderSchemaCount; ++i) {
    const Row &row = trigger_cfg::kRecorderSchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      body.append("\n").append(row.key).append(" = ").append(row.sample);
  }
  std::string out(kBase);
  const std::size_t at = out.find(kRecorderHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kRecorderHeader), body);
  return out;
}

std::string Maximal() {
  std::string body;
  for (std::size_t i = 0; i < trigger_cfg::kTriggerCenterSchemaCount; ++i) {
    const Row &row = trigger_cfg::kTriggerCenterSchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      body.append("\n").append(row.key).append(" = ").append(row.sample);
  }
  return InjectAfterCenterHeader(kBase, body);
}

std::set<std::string> DumpSectionKeys(const std::string &dump,
                                     const std::string &section_name) {
  std::set<std::string> keys;
  bool in_section = false;
  std::size_t pos = 0;
  while (pos < dump.size()) {
    const std::size_t nl = dump.find('\n', pos);
    const std::string line = dump.substr(pos, nl - pos);
    pos = nl == std::string::npos ? dump.size() : nl + 1;
    if (line.empty() || line.front() == '#') continue;
    if (line.front() == '[') {
      in_section = line == "[" + section_name + "]";
      continue;
    }
    if (!in_section) continue;
    const std::size_t eq = line.find('=');
    CHECK(eq != std::string::npos);
    keys.insert(line.substr(0, eq));
  }
  return keys;
}

const Row *FindRow(const std::string &key) {
  for (std::size_t i = 0; i < trigger_cfg::kTriggerCenterSchemaCount; ++i) {
    if (key == trigger_cfg::kTriggerCenterSchema[i].key)
      return &trigger_cfg::kTriggerCenterSchema[i];
  }
  return nullptr;
}

using Mutator = void (*)(trigger_cfg::CenterConfig &);

const std::map<std::string, Mutator> &Mutators() {
  static const std::map<std::string, Mutator> table = {
      {"socket",
       [](trigger_cfg::CenterConfig &c) { c.socket_path += ".reloaded"; }},
      {"tick_ms",
       [](trigger_cfg::CenterConfig &c) { c.tick_ns += 1000000ull; }},
      {"socket_poll_budget_us",
       [](trigger_cfg::CenterConfig &c) {
         c.socket_poll_budget_ns += 1000ull;
       }},
      {"max_trigger_rate_hz",
       [](trigger_cfg::CenterConfig &c) { c.max_trigger_rate_hz += 1; }},
      {"dedup_window_entries",
       [](trigger_cfg::CenterConfig &c) { c.dedup_window_entries += 1; }},
      {"dedup_window_ms",
       [](trigger_cfg::CenterConfig &c) { c.dedup_window_ns += 1000000ull; }},
      {"quota_state",
       [](trigger_cfg::CenterConfig &c) { c.quota_state_path += ".reloaded"; }},
      {"default_source",
       [](trigger_cfg::CenterConfig &c) { c.default_source = "second"; }},
      {"allow_empty_sources",
       [](trigger_cfg::CenterConfig &c) {
         c.allow_empty_sources = !c.allow_empty_sources;
       }},
      {"crash_priority",
       [](trigger_cfg::CenterConfig &c) { c.crash_priority += 1; }},
  };
  return table;
}

const std::map<std::string, Mutator> &RecorderMutators() {
  static const std::map<std::string, Mutator> table = {
      {"output_dir",
       [](trigger_cfg::CenterConfig &c) { c.recorder_out_dir += "/new"; }},
      {"socket", [](trigger_cfg::CenterConfig &c) { c.recorder_socket += ".2"; }},
      {"pending_bytes_quota",
       [](trigger_cfg::CenterConfig &c) { c.recorder_pending_quota += 4096; }},
      {"flush_policy",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_flush_realtime = !c.recorder_flush_realtime;
       }},
      {"idle_hold_s",
       [](trigger_cfg::CenterConfig &c) { c.recorder_idle_hold_ns += 1000000; }},
      {"idle_fallback",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_idle_fallback_idle = !c.recorder_idle_fallback_idle;
       }},
      {"io_rate_limit_mbps",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_io_rate_limit_bps += 1000000;
       }},
      {"disk_quota_mb",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_disk_quota_bytes += 1024 * 1024;
       }},
      {"high_reserved_mb",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_high_reserved_bytes += 1024 * 1024;
       }},
      {"incomplete_policy",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_incomplete_policy = "delete_immediately";
       }},
      {"incomplete_max_age_s",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_incomplete_max_age_ns += 1000000000;
       }},
  };
  return table;
}

void TestRecorderReloadMatrix() {
  trigger_cfg::CenterConfig before;
  std::string err;
  CHECK(Parse(MaximalRecorder(), &before, &err));
  const std::string before_dump = trigger_cfg::DumpConfig(before);
  const auto &mutators = RecorderMutators();
  for (std::size_t i = 0; i < trigger_cfg::kRecorderSchemaCount; ++i) {
    const Row &row = trigger_cfg::kRecorderSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    const auto it = mutators.find(row.key);
    if (it == mutators.end()) {
      std::fprintf(stderr, "[recorder] %s has no reload mutator\n", row.key);
      CHECK(false);
      continue;
    }
    trigger_cfg::CenterConfig after = before;
    it->second(after);
    CHECK(trigger_cfg::DumpConfig(after) != before_dump);
    const bool restart = trigger_cfg::ReloadRequiresRestart(before, after);
    if (row.reload == trigger_cfg::ConfigKeyReload::kRestartRequired && !restart) {
      std::fprintf(stderr,
                   "[recorder] %s is declared restart-required but reload "
                   "accepts it\n",
                   row.key);
      CHECK(false);
    }
    if (row.reload != trigger_cfg::ConfigKeyReload::kRestartRequired &&
        restart) {
      std::fprintf(stderr, "[recorder] %s is declared hot/fixed but requires "
                           "restart\n",
                   row.key);
      CHECK(false);
    }
  }
}

void TestSchemaShape() {
  CHECK(trigger_cfg::kTriggerCenterSchemaCount > 0);
  std::set<std::string> keys;
  for (std::size_t i = 0; i < trigger_cfg::kTriggerCenterSchemaCount; ++i) {
    const Row &row = trigger_cfg::kTriggerCenterSchema[i];
    CHECK(std::string(row.section) == "trigger_center");
    CHECK(keys.insert(row.key).second);
    CHECK(row.type != nullptr && row.type[0] != '\0');
    CHECK(row.default_value != nullptr);
    CHECK(row.note != nullptr && row.note[0] != '\0');
    CHECK(row.contract != nullptr && row.contract[0] != '\0');
    switch (row.status) {
      case trigger_cfg::ConfigKeyStatus::kImplemented:
      case trigger_cfg::ConfigKeyStatus::kDeprecated:
        CHECK(row.error_match != nullptr && row.error_match[0] == '\0');
        CHECK(row.sample != nullptr && row.sample[0] != '\0');
        break;
      case trigger_cfg::ConfigKeyStatus::kRejected:
      case trigger_cfg::ConfigKeyStatus::kFuture:
        CHECK(row.error_match != nullptr && row.error_match[0] != '\0');
        CHECK(row.reload == trigger_cfg::ConfigKeyReload::kNotApplicable);
        break;
    }
  }
}

void TestImplementedAndRejectedRows() {
  int implemented = 0;
  for (std::size_t i = 0; i < trigger_cfg::kTriggerCenterSchemaCount; ++i) {
    const Row &row = trigger_cfg::kTriggerCenterSchema[i];
    trigger_cfg::CenterConfig cfg;
    std::string err;
    std::string warn;
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented) {
      ++implemented;
      if (!Parse(Inject(row.key, row.sample), &cfg, &err, &warn)) {
        std::fprintf(stderr, "implemented [trigger_center] %s sample %s does "
                             "not parse: %s\n",
                     row.key, row.sample, err.c_str());
        CHECK(false);
      }
      const std::set<std::string> dump =
          DumpSectionKeys(trigger_cfg::DumpConfig(cfg), "trigger_center");
      if (dump.count(row.key) == 0) {
        std::fprintf(stderr,
                     "accepted-but-ignored: [trigger_center] %s is absent "
                     "from the effective dump\n",
                     row.key);
        CHECK(false);
      }
      std::string schema_err;
      CHECK(trigger_cfg::ValidateConfigSchema(cfg, &schema_err));
    } else if (row.status == trigger_cfg::ConfigKeyStatus::kDeprecated) {
      CHECK(Parse(Inject(row.key, row.sample), &cfg, &err, &warn));
      CHECK(!warn.empty());
      CHECK(DumpSectionKeys(trigger_cfg::DumpConfig(cfg), "trigger_center").count(row.key) == 0);
    } else {
      if (Parse(Inject(row.key, row.sample), &cfg, &err)) {
        std::fprintf(stderr, "%s is documented as rejected but parses\n",
                     row.key);
        CHECK(false);
      }
      if (err.find(row.error_match) == std::string::npos) {
        std::fprintf(stderr, "%s error text changed: got '%s', want '%s'\n",
                     row.key, err.c_str(), row.error_match);
        CHECK(false);
      }
    }
  }
  CHECK(implemented > 0);
}

void TestUnknownAndDuplicateKeys() {
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(!Parse(Inject("definitely_not_a_key", "1"), &cfg, &err));
  CHECK(err.find("unknown trigger_center key definitely_not_a_key") !=
        std::string::npos);

  const std::string duplicate = InjectAfterCenterHeader(
      kBase, "\ntick_ms = 1\ntick_ms = 2");
  CHECK(!Parse(duplicate, &cfg, &err));
  CHECK(err.find("duplicate trigger_center key tick_ms") != std::string::npos);

  // max_concurrent_captures is fixed at 1: keep the existing migration text.
  CHECK(!Parse(Inject("max_concurrent_captures", "2"), &cfg, &err));
  CHECK(err.find("max_concurrent_captures > 1 is not implemented") !=
        std::string::npos);
}

void TestRecorderSchemaShape() {
  CHECK(trigger_cfg::kRecorderSchemaCount > 0);
  std::set<std::string> keys;
  for (std::size_t i = 0; i < trigger_cfg::kRecorderSchemaCount; ++i) {
    const Row &row = trigger_cfg::kRecorderSchema[i];
    CHECK(std::string(row.section) == "recorder");
    CHECK(keys.insert(row.key).second);
    CHECK(row.type != nullptr && row.type[0] != '\0');
    CHECK(row.default_value != nullptr);
    CHECK(row.note != nullptr && row.note[0] != '\0');
  }
}

void TestRecorderDefaultsAndKeys() {
  trigger_cfg::CenterConfig baseline;
  std::string err;
  CHECK(Parse(kBase, &baseline, &err));
  const std::string baseline_dump = trigger_cfg::DumpConfig(baseline);
  for (std::size_t i = 0; i < trigger_cfg::kRecorderSchemaCount; ++i) {
    const Row &row = trigger_cfg::kRecorderSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    if (row.default_value[0] == '<' || row.default_value[0] == '\0') continue;
    trigger_cfg::CenterConfig cfg;
    if (!Parse(InjectRecorder(row.key, row.default_value), &cfg, &err)) {
      std::fprintf(stderr, "schema default [recorder] %s=%s does not parse: %s\n",
                   row.key, row.default_value, err.c_str());
      CHECK(false);
    }
    if (trigger_cfg::DumpConfig(cfg) != baseline_dump) {
      std::fprintf(stderr,
                   "schema default [recorder] %s=%s differs from the parser "
                   "default\n",
                   row.key, row.default_value);
      CHECK(false);
    }
  }

  trigger_cfg::CenterConfig cfg;
  CHECK(Parse(MaximalRecorder(), &cfg, &err));
  std::set<std::string> implemented;
  for (std::size_t i = 0; i < trigger_cfg::kRecorderSchemaCount; ++i) {
    const Row &row = trigger_cfg::kRecorderSchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      implemented.insert(row.key);
  }
  const std::set<std::string> dumped =
      DumpSectionKeys(trigger_cfg::DumpConfig(cfg), "recorder");
  if (dumped != implemented) {
    for (const std::string &key : dumped) {
      if (implemented.count(key) == 0)
        std::fprintf(stderr, "[recorder] dump key %s missing from schema\n",
                     key.c_str());
    }
    for (const std::string &key : implemented) {
      if (dumped.count(key) == 0)
        std::fprintf(stderr, "[recorder] schema key %s missing from dump\n",
                     key.c_str());
    }
    CHECK(false);
  }
  std::string schema_err;
  CHECK(trigger_cfg::ValidateConfigSchema(cfg, &schema_err));
  CHECK(!trigger_cfg::ValidateConfigSchema(cfg, nullptr, 0, &schema_err));
}

void TestRecorderDuplicateKeyRejected() {
  std::string out(kBase);
  const std::size_t at = out.find(kRecorderHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kRecorderHeader),
             "\ndisk_quota_mb = 1\ndisk_quota_mb = 2");
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(!Parse(out, &cfg, &err));
  CHECK(err.find("duplicate recorder key") != std::string::npos);
}

void TestRecorderRejectedRows() {
  for (std::size_t i = 0; i < trigger_cfg::kRecorderSchemaCount; ++i) {
    const Row &row = trigger_cfg::kRecorderSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kRejected) continue;
    trigger_cfg::CenterConfig cfg;
    std::string err;
    CHECK(!Parse(InjectRecorder(row.key, row.sample), &cfg, &err));
    CHECK(err.find(row.error_match) != std::string::npos);
  }
}

void TestLoggingSchemaShape() {
  CHECK(trigger_cfg::kLoggingSchemaCount > 0);
  std::set<std::string> keys;
  for (std::size_t i = 0; i < trigger_cfg::kLoggingSchemaCount; ++i) {
    const Row &row = trigger_cfg::kLoggingSchema[i];
    CHECK(std::string(row.section) == "logging");
    CHECK(keys.insert(row.key).second);
    CHECK(row.type != nullptr && row.type[0] != '\0');
    CHECK(row.default_value != nullptr);
  }
}

void TestLoggingDefaultsAndKeys() {
  trigger_cfg::CenterConfig baseline;
  std::string err;
  CHECK(Parse(kBase, &baseline, &err));
  const std::string baseline_dump = trigger_cfg::DumpConfig(baseline);
  for (std::size_t i = 0; i < trigger_cfg::kLoggingSchemaCount; ++i) {
    const Row &row = trigger_cfg::kLoggingSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    if (row.default_value[0] == '<' || row.default_value[0] == '\0') continue;
    trigger_cfg::CenterConfig cfg;
    if (!Parse(InjectLogging(row.key, row.default_value), &cfg, &err)) {
      std::fprintf(stderr, "schema default [logging] %s=%s does not parse: %s\n",
                   row.key, row.default_value, err.c_str());
      CHECK(false);
    }
    if (trigger_cfg::DumpConfig(cfg) != baseline_dump) {
      std::fprintf(stderr,
                   "schema default [logging] %s=%s differs from the parser "
                   "default\n",
                   row.key, row.default_value);
      CHECK(false);
    }
  }

  trigger_cfg::CenterConfig cfg;
  CHECK(Parse(MaximalLogging(), &cfg, &err));
  std::set<std::string> implemented;
  for (std::size_t i = 0; i < trigger_cfg::kLoggingSchemaCount; ++i) {
    const Row &row = trigger_cfg::kLoggingSchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      implemented.insert(row.key);
  }
  const std::set<std::string> dumped =
      DumpSectionKeys(trigger_cfg::DumpConfig(cfg), "logging");
  if (dumped != implemented) {
    for (const std::string &key : dumped) {
      if (implemented.count(key) == 0)
        std::fprintf(stderr, "[logging] dump key %s missing from schema\n",
                     key.c_str());
    }
    for (const std::string &key : implemented) {
      if (dumped.count(key) == 0)
        std::fprintf(stderr, "[logging] schema key %s missing from dump\n",
                     key.c_str());
    }
    CHECK(false);
  }
  std::string schema_err;
  CHECK(trigger_cfg::ValidateConfigSchema(cfg, &schema_err));
}

void TestLoggingDuplicateKeyRejected() {
  std::string out(kBase);
  const std::size_t at = out.find(kLoggingHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kLoggingHeader),
             "\nfsm_max_bytes = 1\nfsm_max_bytes = 2");
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(!Parse(out, &cfg, &err));
  CHECK(err.find("duplicate logging key") != std::string::npos);
}

void TestSecuritySchemaShape() {
  CHECK(trigger_cfg::kSecuritySchemaCount > 0);
  std::set<std::string> keys;
  for (std::size_t i = 0; i < trigger_cfg::kSecuritySchemaCount; ++i) {
    const Row &row = trigger_cfg::kSecuritySchema[i];
    CHECK(std::string(row.section) == "security");
    CHECK(keys.insert(row.key).second);
    CHECK(row.type != nullptr && row.type[0] != '\0');
    CHECK(row.default_value != nullptr);
  }
}

void TestSecurityDefaultsAndKeys() {
  trigger_cfg::CenterConfig baseline;
  std::string err;
  CHECK(Parse(kBase, &baseline, &err));
  const std::string baseline_dump = trigger_cfg::DumpConfig(baseline);
  for (std::size_t i = 0; i < trigger_cfg::kSecuritySchemaCount; ++i) {
    const Row &row = trigger_cfg::kSecuritySchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    if (row.default_value[0] == '<' || row.default_value[0] == '\0') continue;
    trigger_cfg::CenterConfig cfg;
    if (!Parse(InjectSecurity(row.key, row.default_value), &cfg, &err)) {
      std::fprintf(stderr, "schema default [security] %s=%s does not parse: %s\n",
                   row.key, row.default_value, err.c_str());
      CHECK(false);
    }
    if (trigger_cfg::DumpConfig(cfg) != baseline_dump) {
      std::fprintf(stderr,
                   "schema default [security] %s=%s differs from the parser "
                   "default\n",
                   row.key, row.default_value);
      CHECK(false);
    }
  }

  trigger_cfg::CenterConfig cfg;
  CHECK(Parse(MaximalSecurity(), &cfg, &err));
  std::set<std::string> implemented;
  for (std::size_t i = 0; i < trigger_cfg::kSecuritySchemaCount; ++i) {
    const Row &row = trigger_cfg::kSecuritySchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      implemented.insert(row.key);
  }
  const std::set<std::string> dumped =
      DumpSectionKeys(trigger_cfg::DumpConfig(cfg), "security");
  if (dumped != implemented) {
    for (const std::string &key : dumped) {
      if (implemented.count(key) == 0)
        std::fprintf(stderr, "[security] dump key %s missing from schema\n",
                     key.c_str());
    }
    for (const std::string &key : implemented) {
      if (dumped.count(key) == 0)
        std::fprintf(stderr, "[security] schema key %s missing from dump\n",
                     key.c_str());
    }
    CHECK(false);
  }
  std::string schema_err;
  CHECK(trigger_cfg::ValidateConfigSchema(cfg, &schema_err));
}

void TestSecurityDuplicateKeyRejected() {
  std::string out(kBase);
  const std::size_t at = out.find(kSecurityHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kSecurityHeader),
             "\nallow_root = true\nallow_root = false");
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(!Parse(out, &cfg, &err));
  CHECK(err.find("duplicate security key") != std::string::npos);
}

void TestContinuousSchemaShape() {
  CHECK(trigger_cfg::kContinuousSchemaCount > 0);
  std::set<std::string> keys;
  for (std::size_t i = 0; i < trigger_cfg::kContinuousSchemaCount; ++i) {
    const Row &row = trigger_cfg::kContinuousSchema[i];
    CHECK(std::string(row.section) == "continuous");
    CHECK(keys.insert(row.key).second);
    CHECK(row.type != nullptr && row.type[0] != '\0');
    CHECK(row.default_value != nullptr);
  }
}

void TestContinuousDefaultsAndKeys() {
  trigger_cfg::CenterConfig baseline;
  std::string err;
  CHECK(Parse(kBase, &baseline, &err));
  const std::string baseline_dump = trigger_cfg::DumpConfig(baseline);
  for (std::size_t i = 0; i < trigger_cfg::kContinuousSchemaCount; ++i) {
    const Row &row = trigger_cfg::kContinuousSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    if (row.default_value[0] == '<' || row.default_value[0] == '\0') continue;
    trigger_cfg::CenterConfig cfg;
    if (!Parse(InjectContinuous(row.key, row.default_value), &cfg, &err)) {
      std::fprintf(stderr,
                   "schema default [continuous] %s=%s does not parse: %s\n",
                   row.key, row.default_value, err.c_str());
      CHECK(false);
    }
    if (trigger_cfg::DumpConfig(cfg) != baseline_dump) {
      std::fprintf(stderr,
                   "schema default [continuous] %s=%s differs from the parser "
                   "default\n",
                   row.key, row.default_value);
      CHECK(false);
    }
  }

  trigger_cfg::CenterConfig cfg;
  CHECK(Parse(MaximalContinuous(), &cfg, &err));
  std::set<std::string> implemented;
  for (std::size_t i = 0; i < trigger_cfg::kContinuousSchemaCount; ++i) {
    const Row &row = trigger_cfg::kContinuousSchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      implemented.insert(row.key);
  }
  const std::set<std::string> dumped =
      DumpSectionKeys(trigger_cfg::DumpConfig(cfg), "continuous");
  if (dumped != implemented) {
    for (const std::string &key : dumped) {
      if (implemented.count(key) == 0)
        std::fprintf(stderr, "[continuous] dump key %s missing from schema\n",
                     key.c_str());
    }
    for (const std::string &key : implemented) {
      if (dumped.count(key) == 0)
        std::fprintf(stderr, "[continuous] schema key %s missing from dump\n",
                     key.c_str());
    }
    CHECK(false);
  }
  std::string schema_err;
  CHECK(trigger_cfg::ValidateConfigSchema(cfg, &schema_err));
}

void TestContinuousDeprecatedAndDuplicate() {
  trigger_cfg::CenterConfig cfg;
  std::string err;
  std::string warn;
  CHECK(Parse(InjectContinuous("enable", "true"), &cfg, &err, &warn));
  CHECK(warn.find("deprecated") != std::string::npos);
  CHECK(cfg.cont_auto_arm);
  CHECK(DumpSectionKeys(trigger_cfg::DumpConfig(cfg), "continuous").count(
            "enable") == 0);

  std::string out(kBase);
  const std::size_t at = out.find(kContinuousHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kContinuousHeader),
             "\nslice_mb = 1\nslice_mb = 2");
  CHECK(!Parse(out, &cfg, &err));
  CHECK(err.find("duplicate continuous key") != std::string::npos);
}

void TestChassisSchemaShape() {
  CHECK(trigger_cfg::kChassisSchemaCount > 0);
  std::set<std::string> keys;
  for (std::size_t i = 0; i < trigger_cfg::kChassisSchemaCount; ++i) {
    const Row &row = trigger_cfg::kChassisSchema[i];
    CHECK(std::string(row.section) == "chassis");
    CHECK(keys.insert(row.key).second);
    CHECK(row.type != nullptr && row.type[0] != '\0');
    CHECK(row.default_value != nullptr);
  }
}

void TestChassisDefaultsAndKeys() {
  std::string page_shm = std::string(kBase) + "\n" + kChassisHeader +
                         "\npage_shm = /tmp/schema_chassis";
  trigger_cfg::CenterConfig baseline;
  std::string err;
  CHECK(Parse(page_shm, &baseline, &err));
  const std::string baseline_dump = trigger_cfg::DumpConfig(baseline);
  for (std::size_t i = 0; i < trigger_cfg::kChassisSchemaCount; ++i) {
    const Row &row = trigger_cfg::kChassisSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    if (row.default_value[0] == '<' || row.default_value[0] == '\0') continue;
    std::string text = page_shm + "\n" + row.key + " = " + row.default_value;
    trigger_cfg::CenterConfig cfg;
    if (!Parse(text, &cfg, &err)) {
      std::fprintf(stderr, "schema default [chassis] %s=%s does not parse: %s\n",
                   row.key, row.default_value, err.c_str());
      CHECK(false);
    }
    if (trigger_cfg::DumpConfig(cfg) != baseline_dump) {
      std::fprintf(stderr,
                   "schema default [chassis] %s=%s differs from the parser "
                   "default\n",
                   row.key, row.default_value);
      CHECK(false);
    }
  }

  trigger_cfg::CenterConfig cfg;
  CHECK(Parse(MaximalChassis(), &cfg, &err));
  std::set<std::string> implemented;
  for (std::size_t i = 0; i < trigger_cfg::kChassisSchemaCount; ++i) {
    const Row &row = trigger_cfg::kChassisSchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      implemented.insert(row.key);
  }
  const std::set<std::string> dumped =
      DumpSectionKeys(trigger_cfg::DumpConfig(cfg), "chassis");
  if (dumped != implemented) {
    for (const std::string &key : dumped) {
      if (implemented.count(key) == 0)
        std::fprintf(stderr, "[chassis] dump key %s missing from schema\n",
                     key.c_str());
    }
    for (const std::string &key : implemented) {
      if (dumped.count(key) == 0)
        std::fprintf(stderr, "[chassis] schema key %s missing from dump\n",
                     key.c_str());
    }
    CHECK(false);
  }
  std::string schema_err;
  CHECK(trigger_cfg::ValidateConfigSchema(cfg, &schema_err));
}

void TestChassisDuplicateKeyRejected() {
  std::string out = std::string(kBase) + "\n" + kChassisHeader +
                    "\npage_shm = /tmp/schema_chassis"
                    "\nstruct_id = 1\nstruct_id = 2";
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(!Parse(out, &cfg, &err));
  CHECK(err.find("duplicate chassis key") != std::string::npos);
}

void TestQuotaSchemaShape() {
  CHECK(trigger_cfg::kQuotaSchemaCount > 0);
  for (std::size_t i = 0; i < trigger_cfg::kQuotaSchemaCount; ++i) {
    const Row &row = trigger_cfg::kQuotaSchema[i];
    CHECK(std::string(row.section) == "quota.*");
    CHECK(row.type != nullptr && row.type[0] != '\0');
  }
}

void TestQuotaDefaultsAndKeys() {
  trigger_cfg::CenterConfig baseline;
  std::string err;
  CHECK(Parse(kBase, &baseline, &err));
  const std::string baseline_dump = trigger_cfg::DumpConfig(baseline);
  for (std::size_t i = 0; i < trigger_cfg::kQuotaSchemaCount; ++i) {
    const Row &row = trigger_cfg::kQuotaSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    if (row.default_value[0] == '<' || row.default_value[0] == '\0') continue;
    trigger_cfg::CenterConfig cfg;
    if (!Parse(InjectQuota(row.key, row.default_value), &cfg, &err)) {
      std::fprintf(stderr, "schema default [quota.*] %s=%s does not parse: %s\n",
                   row.key, row.default_value, err.c_str());
      CHECK(false);
    }
    if (std::string(row.key) == "max_triggers_per_hour") {
      // An explicit 0 and an absent quota are equivalent; the parser keeps no
      // entry for the default, so compare the effective value instead of the
      // dump bytes.
      CHECK(cfg.quota_limits.size() == 1);
      CHECK(cfg.quota_limits[0].second == 0);
      continue;
    }
    if (trigger_cfg::DumpConfig(cfg) != baseline_dump) {
      std::fprintf(stderr,
                   "schema default [quota.*] %s=%s differs from the parser "
                   "default\n",
                   row.key, row.default_value);
      CHECK(false);
    }
  }

  trigger_cfg::CenterConfig cfg;
  CHECK(Parse(MaximalQuota(), &cfg, &err));
  std::set<std::string> implemented;
  for (std::size_t i = 0; i < trigger_cfg::kQuotaSchemaCount; ++i) {
    const Row &row = trigger_cfg::kQuotaSchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      implemented.insert(row.key);
  }
  const std::set<std::string> dumped =
      DumpSectionKeys(trigger_cfg::DumpConfig(cfg), "quota.default");
  if (dumped != implemented) {
    for (const std::string &key : dumped) {
      if (implemented.count(key) == 0)
        std::fprintf(stderr, "[quota.*] dump key %s missing from schema\n",
                     key.c_str());
    }
    for (const std::string &key : implemented) {
      if (dumped.count(key) == 0)
        std::fprintf(stderr, "[quota.*] schema key %s missing from dump\n",
                     key.c_str());
    }
    CHECK(false);
  }
  std::string schema_err;
  CHECK(trigger_cfg::ValidateConfigSchema(cfg, &schema_err));
}

void TestQuotaDuplicateKeyRejected() {
  std::string out(kBase);
  const std::size_t at = out.find(kQuotaHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kQuotaHeader),
             "\nmax_triggers_per_hour = 1\nmax_triggers_per_hour = 2");
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(!Parse(out, &cfg, &err));
  CHECK(err.find("duplicate max_triggers_per_hour") != std::string::npos);
}

void TestSourceSchemaShape() {
  CHECK(trigger_cfg::kSourceSchemaCount > 0);
  std::set<std::string> keys;
  for (std::size_t i = 0; i < trigger_cfg::kSourceSchemaCount; ++i) {
    const Row &row = trigger_cfg::kSourceSchema[i];
    CHECK(std::string(row.section) == "source.*");
    CHECK(keys.insert(row.key).second);
    CHECK(row.type != nullptr && row.type[0] != '\0');
  }
}

void TestSourceDefaultsAndKeys() {
  trigger_cfg::CenterConfig baseline;
  std::string err;
  CHECK(Parse(kBase, &baseline, &err));
  const std::string baseline_dump = trigger_cfg::DumpConfig(baseline);
  for (std::size_t i = 0; i < trigger_cfg::kSourceSchemaCount; ++i) {
    const Row &row = trigger_cfg::kSourceSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    if (row.default_value[0] == '<' || row.default_value[0] == '\0') continue;
    trigger_cfg::CenterConfig cfg;
    if (!Parse(InjectSource(row.key, row.default_value), &cfg, &err)) {
      std::fprintf(stderr, "schema default [source.*] %s=%s does not parse: %s\n",
                   row.key, row.default_value, err.c_str());
      CHECK(false);
    }
    if (trigger_cfg::DumpConfig(cfg) != baseline_dump) {
      std::fprintf(stderr,
                   "schema default [source.*] %s=%s differs from the parser "
                   "default\n",
                   row.key, row.default_value);
      CHECK(false);
    }
  }

  trigger_cfg::CenterConfig cfg;
  if (!Parse(MaximalSource(), &cfg, &err)) {
    std::fprintf(stderr, "MaximalSource parse failed: %s\n", err.c_str());
    CHECK(false);
  }
  std::set<std::string> implemented;
  for (std::size_t i = 0; i < trigger_cfg::kSourceSchemaCount; ++i) {
    const Row &row = trigger_cfg::kSourceSchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      implemented.insert(row.key);
  }
  const std::set<std::string> dumped =
      DumpSectionKeys(trigger_cfg::DumpConfig(cfg), "source.manual");
  if (dumped != implemented) {
    for (const std::string &key : dumped) {
      if (implemented.count(key) == 0)
        std::fprintf(stderr, "[source.*] dump key %s missing from schema\n",
                     key.c_str());
    }
    for (const std::string &key : implemented) {
      if (dumped.count(key) == 0)
        std::fprintf(stderr, "[source.*] schema key %s missing from dump\n",
                     key.c_str());
    }
    CHECK(false);
  }
  std::string schema_err;
  CHECK(trigger_cfg::ValidateConfigSchema(cfg, &schema_err));
}

void TestSourceRejectedAndDuplicate() {
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(!Parse(InjectSource("name", "demo"), &cfg, &err));
  CHECK(err.find("name comes from the [source.<name>] header") !=
        std::string::npos);

  std::string out(kBase);
  const std::size_t at = out.find(kSourceHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kSourceHeader),
             "\npriority = 1\npriority = 2");
  CHECK(!Parse(out, &cfg, &err));
  CHECK(err.find("duplicate source key") != std::string::npos);
}

void TestChannelSchemaShape() {
  CHECK(trigger_cfg::kChannelSchemaCount > 0);
  std::set<std::string> keys;
  for (std::size_t i = 0; i < trigger_cfg::kChannelSchemaCount; ++i) {
    const Row &row = trigger_cfg::kChannelSchema[i];
    CHECK(std::string(row.section) == "channel.*");
    CHECK(keys.insert(row.key).second);
    CHECK(row.type != nullptr && row.type[0] != '\0');
  }
}

void TestChannelDefaultsAndKeys() {
  trigger_cfg::CenterConfig baseline;
  std::string err;
  CHECK(Parse(kBase, &baseline, &err));
  const std::string baseline_dump = trigger_cfg::DumpConfig(baseline);
  for (std::size_t i = 0; i < trigger_cfg::kChannelSchemaCount; ++i) {
    const Row &row = trigger_cfg::kChannelSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    if (std::string(row.key) == "kind" ||
        std::string(row.key) == "record_channels" ||
        std::string(row.key) == "profile" ||
        std::string(row.key) == "profile_version" ||
        std::string(row.key) == "profile_params")
      continue;
    if (row.default_value[0] == '<' || row.default_value[0] == '\0') continue;
    trigger_cfg::CenterConfig cfg;
    if (!Parse(InjectChannel(row.key, row.default_value), &cfg, &err)) {
      std::fprintf(stderr,
                   "schema default [channel.*] %s=%s does not parse: %s\n",
                   row.key, row.default_value, err.c_str());
      CHECK(false);
    }
    // Explicit zero rate/size fields set the "present" flag, which changes
    // DumpConfig even though the effective values are the defaults.
    if (std::string(row.key) == "rate_hz" ||
        std::string(row.key) == "avg_frame_bytes" ||
        std::string(row.key) == "bitrate_kbps") {
      continue;
    }
    if (trigger_cfg::DumpConfig(cfg) != baseline_dump) {
      std::fprintf(stderr,
                   "schema default [channel.*] %s=%s differs from the parser "
                   "default\n",
                   row.key, row.default_value);
      CHECK(false);
    }
  }

  trigger_cfg::CenterConfig cfg;
  if (!Parse(MaximalChannel(), &cfg, &err)) {
    std::fprintf(stderr, "MaximalChannel parse failed: %s\n", err.c_str());
    CHECK(false);
  }
  std::set<std::string> implemented;
  for (std::size_t i = 0; i < trigger_cfg::kChannelSchemaCount; ++i) {
    const Row &row = trigger_cfg::kChannelSchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      implemented.insert(row.key);
  }
  const std::set<std::string> dumped =
      DumpSectionKeys(trigger_cfg::DumpConfig(cfg), "channel./demo");
  if (dumped != implemented) {
    for (const std::string &key : dumped) {
      if (implemented.count(key) == 0)
        std::fprintf(stderr, "[channel.*] dump key %s missing from schema\n",
                     key.c_str());
    }
    for (const std::string &key : implemented) {
      if (dumped.count(key) == 0)
        std::fprintf(stderr, "[channel.*] schema key %s missing from dump\n",
                     key.c_str());
    }
    CHECK(false);
  }
  std::string schema_err;
  CHECK(trigger_cfg::ValidateConfigSchema(cfg, &schema_err));
}

void TestChannelRejectedAndDuplicate() {
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(!Parse(InjectChannel("gop_s", "1"), &cfg, &err));
  CHECK(err.find("gop_s is only valid for h264_stream") != std::string::npos);

  CHECK(!Parse(InjectChannel("profile", "inno-lidar/pointcloud@v2"), &cfg,
               &err));
  CHECK(err.find("profile is only valid for pinned_slot") != std::string::npos);

  std::string out(kBase);
  const std::size_t at = out.find(kChannelHeader);
  CHECK(at != std::string::npos);
  out.insert(at + std::strlen(kChannelHeader),
             "\nrate_hz = 1\nrate_hz = 2");
  CHECK(!Parse(out, &cfg, &err));
  CHECK(err.find("duplicate channel key") != std::string::npos);
}

void TestDefaults() {
  trigger_cfg::CenterConfig baseline;
  std::string err;
  CHECK(Parse(kBase, &baseline, &err));
  const std::string baseline_dump = trigger_cfg::DumpConfig(baseline);
  for (std::size_t i = 0; i < trigger_cfg::kTriggerCenterSchemaCount; ++i) {
    const Row &row = trigger_cfg::kTriggerCenterSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    if (row.default_value[0] == '<' || row.default_value[0] == '\0') continue;
    trigger_cfg::CenterConfig cfg;
    if (!Parse(Inject(row.key, row.default_value), &cfg, &err)) {
      std::fprintf(stderr, "schema default %s=%s does not parse: %s\n",
                   row.key, row.default_value, err.c_str());
      CHECK(false);
    }
    if (trigger_cfg::DumpConfig(cfg) != baseline_dump) {
      std::fprintf(stderr,
                   "schema default %s=%s differs from the parser default\n",
                   row.key, row.default_value);
      CHECK(false);
    }
  }
}

const std::map<std::string, Mutator> &LoggingMutators() {
  static const std::map<std::string, Mutator> table = {
      {"service_file",
       [](trigger_cfg::CenterConfig &c) { c.log_service_file += ".log"; }},
      {"service_max_bytes",
       [](trigger_cfg::CenterConfig &c) { c.log_service_max_bytes += 1; }},
      {"service_rotate_keep",
       [](trigger_cfg::CenterConfig &c) { c.log_service_rotate_keep += 1; }},
      {"fsm_max_bytes",
       [](trigger_cfg::CenterConfig &c) { c.log_fsm_max_bytes += 1; }},
      {"fsm_rotate_keep",
       [](trigger_cfg::CenterConfig &c) { c.log_fsm_rotate_keep += 1; }},
      {"perf_max_bytes",
       [](trigger_cfg::CenterConfig &c) { c.log_perf_max_bytes += 1; }},
      {"perf_rotate_keep",
       [](trigger_cfg::CenterConfig &c) { c.log_perf_rotate_keep += 1; }},
      {"total_budget_mb",
       [](trigger_cfg::CenterConfig &c) { c.log_total_budget_bytes += 1048576; }},
  };
  return table;
}

void TestLoggingReloadMatrix() {
  trigger_cfg::CenterConfig before;
  std::string err;
  CHECK(Parse(MaximalLogging(), &before, &err));
  const std::string before_dump = trigger_cfg::DumpConfig(before);
  const auto &mutators = LoggingMutators();
  for (std::size_t i = 0; i < trigger_cfg::kLoggingSchemaCount; ++i) {
    const Row &row = trigger_cfg::kLoggingSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    const auto it = mutators.find(row.key);
    if (it == mutators.end()) {
      std::fprintf(stderr, "[logging] %s has no reload mutator\n", row.key);
      CHECK(false);
      continue;
    }
    trigger_cfg::CenterConfig after = before;
    it->second(after);
    CHECK(trigger_cfg::DumpConfig(after) != before_dump);
    const bool restart = trigger_cfg::ReloadRequiresRestart(before, after);
    if (row.reload == trigger_cfg::ConfigKeyReload::kRestartRequired && !restart) {
      std::fprintf(stderr,
                   "[logging] %s is declared restart-required but reload "
                   "accepts it\n",
                   row.key);
      CHECK(false);
    }
    if (row.reload != trigger_cfg::ConfigKeyReload::kRestartRequired &&
        restart) {
      std::fprintf(stderr,
                   "[logging] %s is declared hot/fixed but requires restart\n",
                   row.key);
      CHECK(false);
    }
  }
}

const std::map<std::string, Mutator> &SecurityMutators() {
  static const std::map<std::string, Mutator> table = {
      {"allowed_uids",
       [](trigger_cfg::CenterConfig &c) {
         c.allowed_uids.clear();
         c.allowed_uids.push_back(4242);
       }},
      {"allow_root",
       [](trigger_cfg::CenterConfig &c) { c.allow_root = !c.allow_root; }},
      {"require_peer_pid",
       [](trigger_cfg::CenterConfig &c) {
         c.require_peer_pid = !c.require_peer_pid;
       }},
      {"max_priority_hint",
       [](trigger_cfg::CenterConfig &c) { c.max_priority_hint += 1; }},
      {"allow_legacy_unsequenced",
       [](trigger_cfg::CenterConfig &c) {
         c.allow_legacy_unsequenced = !c.allow_legacy_unsequenced;
       }},
      {"allow_legacy_v2",
       [](trigger_cfg::CenterConfig &c) {
         c.allow_legacy_v2 = !c.allow_legacy_v2;
       }},
      {"allow_legacy_control",
       [](trigger_cfg::CenterConfig &c) {
         c.allow_legacy_control = !c.allow_legacy_control;
       }},
      {"signing_key",
       [](trigger_cfg::CenterConfig &c) { c.signing_key_path += ".2"; }},
      {"manifest",
       [](trigger_cfg::CenterConfig &c) {
         c.manifest_mode = integrity::ManifestMode::kOff;
       }},
      {"audit_log",
       [](trigger_cfg::CenterConfig &c) { c.audit_log_path += ".2"; }},
      {"control_audit_log",
       [](trigger_cfg::CenterConfig &c) {
         c.control_audit_log_path += ".2";
       }},
      {"audit_max_bytes",
       [](trigger_cfg::CenterConfig &c) { c.audit_max_bytes += 1; }},
      {"audit_rotate_keep",
       [](trigger_cfg::CenterConfig &c) { c.audit_rotate_keep += 1; }},
      {"audit_fsync_interval_s",
       [](trigger_cfg::CenterConfig &c) {
         c.audit_fsync_interval_ns += 1000000;
       }},
  };
  return table;
}

void TestSecurityReloadMatrix() {
  trigger_cfg::CenterConfig before;
  std::string err;
  CHECK(Parse(MaximalSecurity(), &before, &err));
  const std::string before_dump = trigger_cfg::DumpConfig(before);
  const auto &mutators = SecurityMutators();
  for (std::size_t i = 0; i < trigger_cfg::kSecuritySchemaCount; ++i) {
    const Row &row = trigger_cfg::kSecuritySchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    const auto it = mutators.find(row.key);
    if (it == mutators.end()) {
      std::fprintf(stderr, "[security] %s has no reload mutator\n", row.key);
      CHECK(false);
      continue;
    }
    trigger_cfg::CenterConfig after = before;
    it->second(after);
    if (trigger_cfg::DumpConfig(after) == before_dump) {
      std::fprintf(stderr, "[security] mutator %s did not change the dump\n",
                   row.key);
      CHECK(false);
    }
    const bool restart = trigger_cfg::ReloadRequiresRestart(before, after);
    const bool expect_restart =
        row.reload == trigger_cfg::ConfigKeyReload::kRestartRequired;
    if (restart != expect_restart) {
      std::fprintf(stderr,
                   "[security] %s reload class mismatch: declared restart=%d "
                   "actual=%d\n",
                   row.key, static_cast<int>(expect_restart),
                   static_cast<int>(restart));
      CHECK(false);
    }
  }
}

const std::map<std::string, Mutator> &ContinuousMutators() {
  static const std::map<std::string, Mutator> table = {
      {"auto_arm",
       [](trigger_cfg::CenterConfig &c) { c.cont_auto_arm = !c.cont_auto_arm; }},
      {"channels",
       [](trigger_cfg::CenterConfig &c) {
         c.cont_channels = {"pinned_slot:*"};
       }},
      {"slice_mb",
       [](trigger_cfg::CenterConfig &c) {
         c.cont_slice_bytes += 1024 * 1024;
       }},
      {"slice_s",
       [](trigger_cfg::CenterConfig &c) {
         c.cont_slice_ns += 1000000000;
       }},
      {"wait_idle",
       [](trigger_cfg::CenterConfig &c) {
         c.cont_wait_idle = !c.cont_wait_idle;
       }},
      {"demo_idle",
       [](trigger_cfg::CenterConfig &c) {
         c.cont_demo_idle = !c.cont_demo_idle;
       }},
      {"rate_check",
       [](trigger_cfg::CenterConfig &c) {
         c.cont_rate_check = c.cont_rate_check == "warn" ? "enforce" : "warn";
       }},
  };
  return table;
}

void TestContinuousReloadMatrix() {
  trigger_cfg::CenterConfig before;
  std::string err;
  CHECK(Parse(MaximalContinuous(), &before, &err));
  const std::string before_dump = trigger_cfg::DumpConfig(before);
  const auto &mutators = ContinuousMutators();
  for (std::size_t i = 0; i < trigger_cfg::kContinuousSchemaCount; ++i) {
    const Row &row = trigger_cfg::kContinuousSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    const auto it = mutators.find(row.key);
    if (it == mutators.end()) {
      std::fprintf(stderr, "[continuous] %s has no reload mutator\n",
                   row.key);
      CHECK(false);
      continue;
    }
    trigger_cfg::CenterConfig after = before;
    it->second(after);
    CHECK(trigger_cfg::DumpConfig(after) != before_dump);
    const bool restart = trigger_cfg::ReloadRequiresRestart(before, after);
    const bool expect_restart =
        row.reload == trigger_cfg::ConfigKeyReload::kRestartRequired;
    if (restart != expect_restart) {
      std::fprintf(stderr,
                   "[continuous] %s reload class mismatch: declared restart=%d "
                   "actual=%d\n",
                   row.key, static_cast<int>(expect_restart),
                   static_cast<int>(restart));
      CHECK(false);
    }
  }
}

const std::map<std::string, Mutator> &ChassisMutators() {
  static const std::map<std::string, Mutator> table = {
      {"page_shm",
       [](trigger_cfg::CenterConfig &c) { c.chassis.page_shm += ".2"; }},
      {"struct_id",
       [](trigger_cfg::CenterConfig &c) { c.chassis.struct_id += 1; }},
      {"epb_offset",
       [](trigger_cfg::CenterConfig &c) { c.chassis.epb_offset += 1; }},
      {"speed_offset",
       [](trigger_cfg::CenterConfig &c) { c.chassis.speed_offset += 1; }},
      {"speed_threshold",
       [](trigger_cfg::CenterConfig &c) { c.chassis.speed_threshold += 0.1; }},
  };
  return table;
}

void TestChassisReloadMatrix() {
  trigger_cfg::CenterConfig before;
  std::string err;
  CHECK(Parse(MaximalChassis(), &before, &err));
  const std::string before_dump = trigger_cfg::DumpConfig(before);
  const auto &mutators = ChassisMutators();
  for (std::size_t i = 0; i < trigger_cfg::kChassisSchemaCount; ++i) {
    const Row &row = trigger_cfg::kChassisSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    const auto it = mutators.find(row.key);
    if (it == mutators.end()) {
      std::fprintf(stderr, "[chassis] %s has no reload mutator\n", row.key);
      CHECK(false);
      continue;
    }
    trigger_cfg::CenterConfig after = before;
    it->second(after);
    CHECK(trigger_cfg::DumpConfig(after) != before_dump);
    const bool restart = trigger_cfg::ReloadRequiresRestart(before, after);
    const bool expect_restart =
        row.reload == trigger_cfg::ConfigKeyReload::kRestartRequired;
    if (restart != expect_restart) {
      std::fprintf(stderr,
                   "[chassis] %s reload class mismatch: declared restart=%d "
                   "actual=%d\n",
                   row.key, static_cast<int>(expect_restart),
                   static_cast<int>(restart));
      CHECK(false);
    }
  }
}

void TestQuotaReloadMatrix() {
  trigger_cfg::CenterConfig before;
  std::string err;
  CHECK(Parse(MaximalQuota(), &before, &err));
  const std::string before_dump = trigger_cfg::DumpConfig(before);
  for (std::size_t i = 0; i < trigger_cfg::kQuotaSchemaCount; ++i) {
    const Row &row = trigger_cfg::kQuotaSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    trigger_cfg::CenterConfig after = before;
    CHECK(!after.quota_limits.empty());
    after.quota_limits[0].second += 1;
    CHECK(trigger_cfg::DumpConfig(after) != before_dump);
    const bool restart = trigger_cfg::ReloadRequiresRestart(before, after);
    if (restart) {
      std::fprintf(stderr, "[quota.*] %s is declared hot but requires restart\n",
                   row.key);
      CHECK(false);
    }
  }
}

const std::map<std::string, Mutator> &SourceMutators() {
  static const std::map<std::string, Mutator> table = {
      {"channels",
       [](trigger_cfg::CenterConfig &c) {
         c.sources[0].channels = {"demo"};
       }},
      {"before_s",
       [](trigger_cfg::CenterConfig &c) {
         c.sources[0].before_ns += 1000000000;
       }},
      {"after_s",
       [](trigger_cfg::CenterConfig &c) {
         c.sources[0].after_ns += 1000000000;
       }},
      {"priority",
       [](trigger_cfg::CenterConfig &c) { c.sources[0].priority += 1; }},
      {"mute_s",
       [](trigger_cfg::CenterConfig &c) {
         c.sources[0].mute_ns += 1000000000;
       }},
      {"flush_policy",
       [](trigger_cfg::CenterConfig &c) {
         c.sources[0].flush_realtime = !c.sources[0].flush_realtime;
         c.sources[0].has_flush = true;
       }},
      {"quota_class",
       [](trigger_cfg::CenterConfig &c) {
         c.sources[0].quota_class = "second";
       }},
  };
  return table;
}

void TestSourceReloadMatrix() {
  trigger_cfg::CenterConfig before;
  std::string err;
  CHECK(Parse(MaximalSource(), &before, &err));
  const std::string before_dump = trigger_cfg::DumpConfig(before);
  const auto &mutators = SourceMutators();
  for (std::size_t i = 0; i < trigger_cfg::kSourceSchemaCount; ++i) {
    const Row &row = trigger_cfg::kSourceSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    const auto it = mutators.find(row.key);
    if (it == mutators.end()) {
      std::fprintf(stderr, "[source.*] %s has no reload mutator\n", row.key);
      CHECK(false);
      continue;
    }
    trigger_cfg::CenterConfig after = before;
    it->second(after);
    CHECK(trigger_cfg::DumpConfig(after) != before_dump);
    const bool restart = trigger_cfg::ReloadRequiresRestart(before, after);
    if (restart) {
      std::fprintf(stderr,
                   "[source.*] %s is declared hot but requires restart\n",
                   row.key);
      CHECK(false);
    }
  }
}

const std::map<std::string, Mutator> &ChannelMutators() {
  static const std::map<std::string, Mutator> table = {
      {"kind",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_sources[0].kind =
             triggered::SourceConfig::Kind::kPinnedSlot;
       }},
      {"name",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_sources[0].channel_name += ".2";
       }},
      {"record_channels",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_source_metadata[0].record_channels = {"other"};
       }},
      {"rate_hz",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_sources[0].rate_hz += 1;
       }},
      {"avg_frame_bytes",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_sources[0].avg_frame_bytes += 1;
       }},
      {"bitrate_kbps",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_sources[0].bitrate_kbps += 1;
       }},
      {"crc",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_sources[0].crc = triggered::SourceConfig::CrcPolicy::kFrame;
       }},
      {"manifest",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_sources[0].kind =
             triggered::SourceConfig::Kind::kPinnedSlot;
         c.recorder_sources[0].manifest =
             triggered::SourceConfig::ManifestPolicy::kMetadata;
       }},
      {"profile",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_sources[0].kind =
             triggered::SourceConfig::Kind::kPinnedSlot;
         c.recorder_sources[0].payload_profile = "inno-lidar/pointcloud@v2";
       }},
      {"profile_version",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_sources[0].kind =
             triggered::SourceConfig::Kind::kPinnedSlot;
         c.recorder_sources[0].payload_profile = "inno-lidar/pointcloud@v2";
         c.recorder_sources[0].payload_profile_version = 1;
       }},
      {"profile_params",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_sources[0].kind =
             triggered::SourceConfig::Kind::kPinnedSlot;
         c.recorder_sources[0].payload_profile = "inno-lidar/pointcloud@v2";
         c.recorder_sources[0].payload_profile_params = "time_field=offset:0";
       }},
      {"gop_s",
       [](trigger_cfg::CenterConfig &c) {
         c.recorder_sources[0].h264_stream = true;
         c.recorder_sources[0].gop_ns = 2000000000ULL;
       }},
  };
  return table;
}

void TestChannelReloadMatrix() {
  trigger_cfg::CenterConfig before;
  std::string err;
  CHECK(Parse(MaximalChannel(), &before, &err));
  const std::string before_dump = trigger_cfg::DumpConfig(before);
  const auto &mutators = ChannelMutators();
  for (std::size_t i = 0; i < trigger_cfg::kChannelSchemaCount; ++i) {
    const Row &row = trigger_cfg::kChannelSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    const auto it = mutators.find(row.key);
    if (it == mutators.end()) {
      std::fprintf(stderr, "[channel.*] %s has no reload mutator\n", row.key);
      CHECK(false);
      continue;
    }
    trigger_cfg::CenterConfig after = before;
    it->second(after);
    CHECK(trigger_cfg::DumpConfig(after) != before_dump);
    const bool restart = trigger_cfg::ReloadRequiresRestart(before, after);
    const bool expect_restart =
        row.reload == trigger_cfg::ConfigKeyReload::kRestartRequired;
    if (restart != expect_restart) {
      std::fprintf(stderr,
                   "[channel.*] %s reload class mismatch: declared restart=%d "
                   "actual=%d\n",
                   row.key, static_cast<int>(expect_restart),
                   static_cast<int>(restart));
      CHECK(false);
    }
  }
}

void TestDumpKeySetAndSchemaCheck() {
  trigger_cfg::CenterConfig cfg;
  std::string err;
  CHECK(Parse(Maximal(), &cfg, &err));

  std::set<std::string> implemented;
  for (std::size_t i = 0; i < trigger_cfg::kTriggerCenterSchemaCount; ++i) {
    const Row &row = trigger_cfg::kTriggerCenterSchema[i];
    if (row.status == trigger_cfg::ConfigKeyStatus::kImplemented)
      implemented.insert(row.key);
  }
  const std::set<std::string> dumped =
      DumpSectionKeys(trigger_cfg::DumpConfig(cfg), "trigger_center");
  if (dumped != implemented) {
    for (const std::string &key : dumped) {
      if (implemented.count(key) == 0)
        std::fprintf(stderr, "dump key %s missing from schema\n", key.c_str());
    }
    for (const std::string &key : implemented) {
      if (dumped.count(key) == 0)
        std::fprintf(stderr, "schema key %s missing from dump\n", key.c_str());
    }
    CHECK(false);
  }

  std::string schema_err;
  CHECK(trigger_cfg::ValidateConfigSchema(cfg, &schema_err));
  // Negative seam: an empty table rejects every effective key, proving the
  // --check-config gate is not a no-op.
  CHECK(!trigger_cfg::ValidateConfigSchema(cfg, nullptr, 0, &schema_err));
  CHECK(schema_err.find("not declared in the generated config schema") !=
        std::string::npos);
}

void TestReloadMatrix() {
  trigger_cfg::CenterConfig before;
  std::string err;
  CHECK(Parse(Maximal(), &before, &err));
  const std::string before_dump = trigger_cfg::DumpConfig(before);

  const auto &mutators = Mutators();
  for (std::size_t i = 0; i < trigger_cfg::kTriggerCenterSchemaCount; ++i) {
    const Row &row = trigger_cfg::kTriggerCenterSchema[i];
    if (row.status != trigger_cfg::ConfigKeyStatus::kImplemented) continue;
    const auto it = mutators.find(row.key);
    if (row.reload == trigger_cfg::ConfigKeyReload::kFixedAtStartup) {
      CHECK(it == mutators.end());
      continue;
    }
    if (it == mutators.end()) {
      std::fprintf(stderr, "schema key %s has no reload mutator\n", row.key);
      CHECK(false);
    }
    trigger_cfg::CenterConfig after = before;
    it->second(after);
    CHECK(trigger_cfg::DumpConfig(after) != before_dump);
    const bool restart = trigger_cfg::ReloadRequiresRestart(before, after);
    switch (row.reload) {
      case trigger_cfg::ConfigKeyReload::kHot:
        if (restart) {
          std::fprintf(stderr, "%s is declared hot but requires restart\n",
                       row.key);
          CHECK(false);
        }
        break;
      case trigger_cfg::ConfigKeyReload::kRestartRequired:
        if (!restart) {
          std::fprintf(stderr,
                       "%s is declared restart-required but reload accepts it\n",
                       row.key);
          CHECK(false);
        }
        break;
      case trigger_cfg::ConfigKeyReload::kStartupOnlyKept:
        // Existing TRG-10 contract: the host-bound socket path is kept
        // silently, not rejected.
        if (restart) {
          std::fprintf(stderr, "%s is declared startup-kept but reload rejects "
                               "the change\n",
                       row.key);
          CHECK(false);
        }
        break;
      case trigger_cfg::ConfigKeyReload::kFixedAtStartup:
      case trigger_cfg::ConfigKeyReload::kNotApplicable:
        CHECK(false);
        break;
    }
  }
  for (const auto &entry : mutators) {
    const Row *row = FindRow(entry.first);
    CHECK(row != nullptr);
    CHECK(row->status == trigger_cfg::ConfigKeyStatus::kImplemented);
    CHECK(row->reload != trigger_cfg::ConfigKeyReload::kFixedAtStartup);
  }
}

}  // namespace

int main() {
  TestSchemaShape();
  TestImplementedAndRejectedRows();
  TestUnknownAndDuplicateKeys();
  TestDefaults();
  TestRecorderSchemaShape();
  TestRecorderDefaultsAndKeys();
  TestRecorderRejectedRows();
  TestRecorderDuplicateKeyRejected();
  TestRecorderReloadMatrix();
  TestLoggingSchemaShape();
  TestLoggingDefaultsAndKeys();
  TestLoggingDuplicateKeyRejected();
  TestLoggingReloadMatrix();
  TestSecuritySchemaShape();
  TestSecurityDefaultsAndKeys();
  TestSecurityDuplicateKeyRejected();
  TestSecurityReloadMatrix();
  TestContinuousSchemaShape();
  TestContinuousDefaultsAndKeys();
  TestContinuousDeprecatedAndDuplicate();
  TestContinuousReloadMatrix();
  TestChassisSchemaShape();
  TestChassisDefaultsAndKeys();
  TestChassisDuplicateKeyRejected();
  TestChassisReloadMatrix();
  TestQuotaSchemaShape();
  TestQuotaDefaultsAndKeys();
  TestQuotaDuplicateKeyRejected();
  TestQuotaReloadMatrix();
  TestSourceSchemaShape();
  TestSourceDefaultsAndKeys();
  TestSourceRejectedAndDuplicate();
  TestSourceReloadMatrix();
  TestChannelSchemaShape();
  TestChannelDefaultsAndKeys();
  TestChannelRejectedAndDuplicate();
  TestChannelReloadMatrix();
  TestDumpKeySetAndSchemaCheck();
  TestReloadMatrix();
  std::puts("test_trigger_config_schema: OK");
  return 0;
}
