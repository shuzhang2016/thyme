#pragma once

// GEN-09: generator identity, generated-tree metadata, and the migration
// policy that `--check` enforces.
//
// `generated/` is generator-owned: a tree is only ever migrated by
// regenerating it. The metadata file records which generator and which output
// format produced the tree, plus a hash of the effective graph, so `--check`
// can explain "this tree is from another format/version" instead of only
// listing byte diffs.

#include "graph.h"

#include <string>

namespace gen_meta {

// Generator semantic version.
//  MAJOR: kFormatVersion changed (old trees are not comparable),
//  MINOR/PATCH: output may change byte-wise within the same format.
inline constexpr const char* kTool = "gen_graph";
inline constexpr const char* kGeneratorVersion = "3.11.1";

// Output format version of the generated/ tree. `--check` treats a tree
// stamped with any other value as stale-by-format: regenerate it in full.
inline constexpr int kFormatVersion = 3;

struct Meta {
  std::string tool = kTool;
  std::string generator_version = kGeneratorVersion;
  int format_version = kFormatVersion;
  std::string source_graph_hash;
};

// Stable FNV-1a 64 hash of the effective graph (after the default
// health_reporter injection). Diagnostic-only fields (yaml path, line
// numbers) are excluded, so reformatting or moving graph.yaml does not change
// the hash; semantic changes do.
std::string SourceGraphHash(const Graph& g);

Meta ForGraph(const Graph& g);

// Canonical `.gen_meta.json` body: stable key order, trailing newline.
std::string Render(const Meta& m);

// Parses a `.gen_meta.json` body. False when required fields are missing or
// malformed (an unreadable meta is itself a stale-tree signal).
bool Parse(const std::string& text, Meta* out);

// Reads `<generated_dir>/.gen_meta.json` into `out`; false when absent or
// unparseable.
bool Read(const std::string& generated_dir, Meta* out);

// Human-readable migration diagnostic when `m` cannot describe the tree this
// generator produces (different tool, output format, or generator version).
// Empty string means compatible. `--check` prints this and exits 1.
std::string CompatibilityError(const Meta& m);

// "gen_graph 3.11.1 (output format 3)" - used by `--version` and the banner.
std::string VersionSummary();

// One-line stamp appended to every generated file that carries the codegen
// banner, so a version bump is visible in a plain diff as well as in the meta.
std::string BannerStamp();

}  // namespace gen_meta
