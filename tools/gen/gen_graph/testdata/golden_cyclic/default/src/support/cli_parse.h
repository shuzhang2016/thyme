#pragma once

// Shared CLI parsing helpers for ad_stack and tools (viz_bridge, ...).

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string>
#include <vector>

namespace cli {

// Split a comma-separated list. Spaces around items are trimmed (leading and
// trailing); empty items are skipped.
inline void SplitCsv(const char* csv, std::vector<std::string>& out) {
  if (csv == nullptr || csv[0] == '\0') return;
  const char* p = csv;
  while (*p != '\0') {
    while (*p == ',' || *p == ' ') ++p;
    if (*p == '\0') break;
    const char* start = p;
    while (*p != '\0' && *p != ',') ++p;
    const char* end = p;
    while (end > start && end[-1] == ' ') --end;  // trim trailing spaces
    if (end > start) out.emplace_back(start, end);
  }
}

// Strict long parse: the whole string must be a valid integer within
// [lo, hi]. Returns false on garbage, trailing junk, or overflow.
inline bool ParseLong(const char* s, long lo, long hi, long& out) {
  if (s == nullptr || s[0] == '\0') return false;
  char* end = nullptr;
  errno = 0;
  const long v = std::strtol(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') return false;
  if (v < lo || v > hi) return false;
  out = v;
  return true;
}

// Strict double parse: the whole string must be a valid finite number within
// [lo, hi].
inline bool ParseDouble(const char* s, double lo, double hi, double& out) {
  if (s == nullptr || s[0] == '\0') return false;
  char* end = nullptr;
  errno = 0;
  const double v = std::strtod(s, &end);
  if (errno != 0 || end == s || *end != '\0') return false;
  if (!(v >= lo && v <= hi)) return false;  // also rejects NaN
  out = v;
  return true;
}

}  // namespace cli
