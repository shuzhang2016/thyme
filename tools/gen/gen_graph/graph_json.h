#pragma once

// ECO-08: canonical JSON representation of `graph.yaml`, shared with an
// external graph editor.
//
// The JSON is a lossless projection of the *loaded* graph (delivery defaults
// applied, diagnostics-only fields such as source/line dropped). Key order is
// fixed and output is byte-stable, so `yaml -> JSON -> yaml -> JSON` can be
// compared directly. Parsing is strict: unknown or duplicate keys, wrong
// types and out-of-range values are errors (the loader's YAML rules are then
// re-applied to the reconstructed yaml, so both entry points agree).

#include "graph.h"

#include <string>

// Canonical JSON / YAML for a loaded graph. Both end with a newline.
std::string GraphToCanonicalJson(const Graph& g);
std::string GraphToCanonicalYaml(const Graph& g);

// Strict canonical-JSON load. On failure returns false and prints
// `<source>: <message>` to stderr; *out is untouched.
bool LoadGraphJsonFromString(const std::string& text, Graph* out,
                             const std::string& source = "<json>");
bool LoadGraphJson(const std::string& path, Graph* out);
