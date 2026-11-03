#pragma once

#include "graph.h"

#include <string>

// `source` names the text in diagnostics (`<source>:<line>: ...`); callers
// loading from a file should pass the path. Defaults to "<yaml>".
bool LoadGraphYamlFromString(const std::string& text, Graph* out,
                             const std::string& source = "<yaml>");
bool LoadGraphYaml(const std::string& path, Graph* out);
