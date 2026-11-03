// trigger_config_schema.cpp — ARCH-13 schema/parser cross-check.
#include "thyme/record/trigger_config_schema_check.h"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "thyme/record/trigger_config.h"

namespace thyme {
namespace trigger_cfg {
namespace {

const char* StatusName(ConfigKeyStatus status) {
  switch (status) {
    case ConfigKeyStatus::kImplemented:
      return "implemented";
    case ConfigKeyStatus::kDeprecated:
      return "deprecated";
    case ConfigKeyStatus::kRejected:
      return "rejected";
    case ConfigKeyStatus::kFuture:
      return "future";
  }
  return "unknown";
}

bool SectionMatches(const char* pattern, const std::string& section) {
  const std::string p(pattern);
  if (p.size() >= 2 && p.compare(p.size() - 2, 2, ".*") == 0) {
    const std::string prefix = p.substr(0, p.size() - 1);  // keep the dot
    return section.size() > prefix.size() &&
           section.compare(0, prefix.size(), prefix) == 0;
  }
  return p == section;
}

bool IsSchemaBackedSection(const std::string& section,
                           const ConfigKeySchema* table, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    if (SectionMatches(table[i].section, section)) return true;
  }
  return false;
}

const ConfigKeySchema* FindKey(const ConfigKeySchema* table, std::size_t count,
                               const std::string& section,
                               const std::string& key) {
  for (std::size_t i = 0; i < count; ++i) {
    if (SectionMatches(table[i].section, section) && key == table[i].key)
      return &table[i];
  }
  return nullptr;
}

}  // namespace

bool ValidateConfigSchema(const CenterConfig& cfg, std::string* err) {
  return ValidateConfigSchema(cfg, kTriggerConfigSchema,
                              kTriggerConfigSchemaCount, err);
}

bool ValidateConfigSchema(const CenterConfig& cfg,
                          const ConfigKeySchema* table, std::size_t count,
                          std::string* err) {
  const std::string dump = DumpConfig(cfg);
  std::istringstream lines(dump);
  std::string line;
  std::string section;
  while (std::getline(lines, line)) {
    if (line.empty() || line.front() == '#') continue;
    if (line.front() == '[') {
      const std::size_t end = line.find(']');
      section = end == std::string::npos ? line.substr(1)
                                         : line.substr(1, end - 1);
      continue;
    }
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    // Only sections the generated schema owns are checked; the remaining
    // hand-maintained doc sections are a later ARCH-13 slice. The empty-table
    // test seam still checks [trigger_center].
    const bool checked_section =
        count == 0 ? section == "trigger_center"
                   : IsSchemaBackedSection(section, table, count);
    if (!checked_section) continue;
    const ConfigKeySchema* row = FindKey(table, count, section, key);
    if (row == nullptr) {
      if (err != nullptr) {
        *err = "config schema: [" + section + "] key '" + key +
               "' is accepted by the parser but not declared in the "
               "generated config schema (regenerate "
               "tools/gen/config_schema/trigger_config.schema.json)";
      }
      return false;
    }
    if (row->status != ConfigKeyStatus::kImplemented) {
      if (err != nullptr) {
        *err = "config schema: [" + section + "] key '" + key +
               "' is declared " + StatusName(row->status) +
               " but is effective in DumpConfig";
      }
      return false;
    }
  }
  return true;
}

}  // namespace trigger_cfg
}  // namespace thyme
