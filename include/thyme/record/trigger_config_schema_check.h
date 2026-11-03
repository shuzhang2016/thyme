#pragma once

// trigger_config_schema_check.h — ARCH-13 slice 1 schema/parser cross-check.
//
// `trigger_config_schema.h` is generated from the single machine-readable
// schema (tools/gen/config_schema/trigger_config.schema.json). This header
// exposes the runtime check that `--check-config` and
// `test_trigger_config_schema` use to prove the parser's effective
// `[trigger_center]` key set is declared by that schema.
//
// The check is one-directional by construction: it sees the keys the parser
// materialised in DumpConfig. The opposite direction (every schema key is
// implemented by the parser) is covered by the test that injects each
// schema sample into a valid config.

#include <cstddef>
#include <string>

#include "thyme/record/trigger_config_schema.h"

namespace thyme {
namespace trigger_cfg {

struct CenterConfig;

// Validate the effective DumpConfig() key set against the generated
// [trigger_center] table. Fails when the parser accepts a key that the
// schema does not declare, or when a rejected/future key is effective.
bool ValidateConfigSchema(const CenterConfig& cfg, std::string* err);

// Test seam: run the same check against an arbitrary table. A null/empty
// table rejects every effective key, which proves the check can fail.
bool ValidateConfigSchema(const CenterConfig& cfg,
                          const ConfigKeySchema* table, std::size_t count,
                          std::string* err);

}  // namespace trigger_cfg
}  // namespace thyme
