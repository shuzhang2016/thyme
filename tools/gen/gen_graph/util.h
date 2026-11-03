#pragma once

// Small shared helpers for the gen_graph host tool (parser, catalog,
// scaffold, derive-template, validation). Header-only so the single-TU test
// binaries (test_gen_graph_yaml, ...) need no extra link unit. The emitter
// side (namespace gen_emit, emit_util.cpp) forwards here, so each algorithm
// has exactly one implementation.

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace gen_util {

namespace fs = std::filesystem;

inline std::string Trim(const std::string& s) {
  std::size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  std::size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

inline bool IsIdent(const std::string& s) {
  if (s.empty()) return false;
  const unsigned char c0 = static_cast<unsigned char>(s[0]);
  if (!std::isalpha(c0) && s[0] != '_') return false;
  for (std::size_t i = 1; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (!std::isalnum(c) && s[i] != '_') return false;
  }
  return true;
}

// "foo_bar" -> "FooBar" (per '_' segment, first char uppercased).
inline std::string PascalCase(const std::string& s) {
  std::string out;
  std::size_t i = 0;
  while (i < s.size()) {
    std::size_t j = s.find('_', i);
    if (j == std::string::npos) j = s.size();
    std::string seg = s.substr(i, j - i);
    if (!seg.empty()) {
      seg[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(seg[0])));
    }
    out += seg;
    i = j + 1;
  }
  return out;
}

inline std::string ReplaceAll(std::string s, const std::string& a,
                              const std::string& b) {
  if (a.empty()) return s;
  std::size_t pos = 0;
  while ((pos = s.find(a, pos)) != std::string::npos) {
    s.replace(pos, a.size(), b);
    pos += b.size();
  }
  return s;
}

// Silent file IO: no stderr — each caller decides how (or whether) to report.
inline bool ReadFile(const fs::path& p, std::string* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

inline bool WriteFile(const fs::path& p, const std::string& data) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  if (!out) return false;
  out << data;
  return static_cast<bool>(out);
}

}  // namespace gen_util
