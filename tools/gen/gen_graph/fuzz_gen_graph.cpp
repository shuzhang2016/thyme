// fuzz_gen_graph.cpp — GEN-07 structured fuzz driver for the gen_graph tool.
//
// Unlike the parser-level libFuzzer harness (fuzz_yaml.cpp, M8.2), this driver
// exercises the real binary end to end: every input is written to a bounded
// temp file and run through generated, explain-rates, derive-template or
// update-all mode. That makes the asserted properties observable only from
// outside the process:
//
//   * exit code is always 0 or 2 — a crash, a signal or a hang is a failure;
//   * exit 2 (rejected) prints a structured diagnostic. generated rejects all
//     output; explain-rates rejects all output on both exit paths; update all
//     permits only a complete generated/ tree after a later scaffold failure;
//     derive-template is not transactional and may leave early derived files.
//     Parse/validation diagnostics must carry `<input>:<line>:`; graph-level
//     rejections (cycles, emit-stage failures) must at least carry the
//     `gen_graph: ` prefix;
//   * accepted generated/update-all runs produce generated/wiring.cpp and pass
//     --check generated. Accepted derive-template runs produce the real catalog
//     artifacts, byte-identically in two independent output directories.
//
// Mutations are structured, not byte noise: they start from the real fixture
// graphs under tools/gen/gen_graph/{scaffold,testdata} and apply YAML-aware edits
// (truncation, duplicated keys, wrong types, unknown fields, cycle insertion,
// feedback edges without delay, oversized/deep nesting, invalid identifiers,
// mixed indentation/tabs, UTF-8 damage, NUL bytes, huge numbers, ...). A
// "valid family" pass only applies obviously-safe edits (in-range numbers,
// whitespace, comments) and requires exit 0, so the accept path is covered
// too, not just rejections.
//
// No temp dirs leak: everything lives under GEN_GRAPH_FUZZ_TMPDIR (build dir)
// and is removed on success; on failure the dir is kept for inspection.
// /dev/shm is never used.
//
// Usage: fuzz_gen_graph [iterations] [seed]
//   env THYME_FUZZ_ITERS / THYME_FUZZ_SEED override argv,
//   env THYME_GEN_GRAPH_BIN overrides the compiled-in binary path.
// Quick (mr_gate): fuzz_gen_graph 2000; full (nightly): 10000.

#include "fuzz_harness.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef GEN_GRAPH_BIN
#error "GEN_GRAPH_BIN must be defined"
#endif
#ifndef GEN_GRAPH_FIXTURE_DIR
#error "GEN_GRAPH_FIXTURE_DIR must be defined"
#endif
#ifndef GEN_GRAPH_FUZZ_TMPDIR
#error "GEN_GRAPH_FUZZ_TMPDIR must be defined"
#endif

namespace fs = std::filesystem;
using thyme_fuzz::SplitMix64;

namespace {

constexpr std::size_t kMaxInput = 64u << 10;
constexpr uint64_t kDefaultIters = 2000;
constexpr uint64_t kDefaultSeed = 0x47454E47u;  // "GENG"
constexpr int kChildTimeoutSeconds = 20;

struct FuzzFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// ---- Seed corpus ----------------------------------------------------------

struct Seed {
  const char* file;  // relative to GEN_GRAPH_FIXTURE_DIR
  const char* tpl;   // optional --template-dir, "" for none
  bool valid;        // expected to be accepted by gen_graph
};

const Seed kSeeds[] = {
    {"scaffold/graph.yaml", "", true},
    {"testdata/minimal_norecord/graph.yaml", "", true},
    {"testdata/ports_tpl/graph.yaml", "testdata/ports_tpl", true},
    {"testdata/event_tpl/graph.yaml", "testdata/event_tpl", true},
    {"testdata/cycle/feedback.yaml", "testdata/cycle/tpl", true},
    // Rejected fixtures are still useful mutation bases, and replaying them
    // proves the diagnostics stay stable.
    {"testdata/cycle/cycle.yaml", "testdata/cycle/tpl", false},
    {"testdata/cycle/feedback_no_delay.yaml", "testdata/cycle/tpl", false},
    {"testdata/rate/product_cyclic.yaml", "", false},
};
constexpr std::size_t kSeedCount = sizeof(kSeeds) / sizeof(kSeeds[0]);

std::string ReadFileOrDie(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw FuzzFailure("cannot read seed " + path);
  }
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  return text;
}

// ---- Text helpers ---------------------------------------------------------

bool StartsWith(const std::string& s, const char* prefix) {
  const std::size_t n = std::strlen(prefix);
  return s.size() >= n && s.compare(0, n, prefix) == 0;
}

std::string TrimLeft(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

std::string TrimRight(const std::string& s) {
  std::size_t n = s.size();
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) --n;
  return s.substr(0, n);
}

std::string Trim(const std::string& s) { return TrimRight(TrimLeft(s)); }

int IndentOf(const std::string& s) {
  int n = 0;
  while (n < static_cast<int>(s.size()) && s[static_cast<std::size_t>(n)] == ' ') {
    ++n;
  }
  return n;
}

std::string KeyOf(const std::string& line) {
  const std::string t = Trim(line);
  if (t.empty() || t[0] == '#' || t[0] == '-') return "";
  const std::size_t colon = t.find(':');
  if (colon == std::string::npos) return "";
  return Trim(t.substr(0, colon));
}

std::string ValueOf(const std::string& line) {
  const std::size_t colon = line.find(':');
  if (colon == std::string::npos) return "";
  std::string v = Trim(line.substr(colon + 1));
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
    v = v.substr(1, v.size() - 2);
  }
  return v;
}

bool ReplaceValue(std::vector<std::string>* lines, std::size_t idx,
                  const std::string& value) {
  std::string& line = (*lines)[idx];
  const std::size_t colon = line.find(':');
  if (colon == std::string::npos) return false;
  line = line.substr(0, colon + 1) + " " + value;
  return true;
}

std::vector<std::size_t> LinesWithKey(const std::vector<std::string>& lines,
                                      const std::vector<std::string>& keys) {
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string key = KeyOf(lines[i]);
    if (key.empty()) continue;
    if (std::find(keys.begin(), keys.end(), key) != keys.end()) out.push_back(i);
  }
  return out;
}

// Inclusive-exclusive range [begin, end) of the `key:` section: its key line
// through the line before the next top-level key.
bool SectionRange(const std::vector<std::string>& lines, const std::string& key,
                  std::size_t* begin, std::size_t* end) {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (IndentOf(lines[i]) != 0 || KeyOf(lines[i]) != key) continue;
    std::size_t j = i + 1;
    while (j < lines.size()) {
      const std::string t = Trim(lines[j]);
      if (!t.empty() && t[0] != '#' && IndentOf(lines[j]) == 0 &&
          lines[j].find(':') != std::string::npos) {
        break;
      }
      ++j;
    }
    *begin = i + 1;
    *end = j;
    return true;
  }
  return false;
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t nl = text.find('\n', start);
    if (nl == std::string::npos) {
      if (start < text.size()) out.push_back(text.substr(start));
      break;
    }
    out.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  return out;
}

std::string JoinLines(const std::vector<std::string>& lines) {
  std::string out;
  for (const std::string& l : lines) {
    out += l;
    out += '\n';
  }
  return out;
}

// ---- Structured mutations -------------------------------------------------

enum Family {
  kTruncate = 0,
  kDuplicateKey,
  kWrongType,
  kUnknownField,
  kCycleInsert,
  kFeedbackNoDelay,
  kOversizedNesting,
  kInvalidIdent,
  kMixedIndent,
  kUtf8Damage,
  kNulByte,
  kHugeNumber,
  kDeleteRequired,
  kDuplicateNodeId,
  kNumTweak,
  kWhitespace,
  kCommentInsert,
  kRepeatedEdge,
  kFamilyCount
};

const char* const kFamilyNames[kFamilyCount] = {
    "truncate",       "duplicate-key", "wrong-type",     "unknown-field",
    "cycle-insert",   "feedback-no-delay", "oversized-nesting", "invalid-ident",
    "mixed-indent",   "utf8-damage",   "nul-byte",       "huge-number",
    "delete-required", "duplicate-node-id", "num-tweak", "whitespace",
    "comment-insert", "repeated-edge"};

bool IsValidFamily(int family) {
  return family == kNumTweak || family == kWhitespace ||
         family == kCommentInsert;
}

bool MutateTruncate(std::vector<std::string>* lines, SplitMix64* rng) {
  if (lines->size() < 2) return false;
  if (rng->Coin(2)) {
    const std::size_t keep = rng->Below(lines->size());
    lines->resize(keep);
    return true;
  }
  const std::size_t at = rng->Below(lines->size());
  const std::string& line = (*lines)[at];
  if (line.empty()) return false;
  (*lines)[at] = line.substr(0, rng->Below(line.size()));
  return true;
}

bool MutateDuplicateKey(std::vector<std::string>* lines, SplitMix64* rng) {
  std::vector<std::size_t> candidates;
  for (std::size_t i = 0; i < lines->size(); ++i) {
    const std::string t = Trim((*lines)[i]);
    if (t.empty() || t[0] == '#' || t[0] == '-') continue;
    if (t.find(':') == std::string::npos) continue;
    if (IndentOf((*lines)[i]) == 0 ||
        IndentOf((*lines)[i]) == 4) {
      candidates.push_back(i);
    }
  }
  if (candidates.empty()) return false;
  const std::size_t at = rng->Pick(candidates);
  lines->insert(lines->begin() + static_cast<std::ptrdiff_t>(at + 1),
                (*lines)[at]);
  return true;
}

bool MutateWrongType(std::vector<std::string>* lines, SplitMix64* rng) {
  static const std::vector<std::string> kValues = {
      "abc", "-1",  "1.5", "true", "[]",  "0x10",
      "++7", "1e9", "\"\"", "false", "-0"};
  const std::vector<std::size_t> keys = LinesWithKey(
      *lines, {"version", "pool_n", "ring_cap", "hz", "health_n", "depth",
               "priority", "deadline_ns", "delay", "cpu_set", "mp_gate",
               "feedback", "record"});
  if (keys.empty()) return false;
  return ReplaceValue(lines, rng->Pick(keys), rng->Pick(kValues));
}

bool MutateUnknownField(std::vector<std::string>* lines, SplitMix64* rng) {
  std::vector<std::size_t> candidates;
  for (std::size_t i = 0; i < lines->size(); ++i) {
    if (IndentOf((*lines)[i]) == 4 && !KeyOf((*lines)[i]).empty()) {
      candidates.push_back(i);
    }
  }
  const std::string key =
      "fuzz_unknown_" + std::to_string(rng->Below(1000));
  if (candidates.empty()) {
    lines->push_back(key + ": 1");
    return true;
  }
  const std::size_t at = rng->Pick(candidates);
  const std::string indent(static_cast<std::size_t>(IndentOf((*lines)[at])), ' ');
  lines->insert(lines->begin() + static_cast<std::ptrdiff_t>(at + 1),
                indent + key + ": 1");
  return true;
}

bool MutateCycleInsert(std::vector<std::string>* lines, SplitMix64* rng) {
  std::size_t begin = 0, end = 0;
  std::vector<std::string> node_ids;
  for (const std::string& line : *lines) {
    const std::size_t dash = line.find("- id:");
    if (dash != std::string::npos) node_ids.push_back(ValueOf(line));
  }
  if (node_ids.empty()) return false;

  std::vector<std::pair<std::string, std::string>> edges;
  if (SectionRange(*lines, "edges", &begin, &end)) {
    std::string from, to;
    for (std::size_t i = begin; i < end; ++i) {
      const std::string key = KeyOf((*lines)[i]);
      if (key == "from") from = ValueOf((*lines)[i]);
      if (key == "to") to = ValueOf((*lines)[i]);
      if (!from.empty() && !to.empty()) {
        edges.emplace_back(from, to);
        from.clear();
        to.clear();
      }
    }
  }
  const std::string channel = "fuzz_cyc_" + std::to_string(rng->Below(1000));
  std::string a = rng->Pick(node_ids);
  std::string b = rng->Pick(node_ids);
  if (!edges.empty() && rng->Coin(2)) {
    const auto& e = rng->Pick(edges);
    a = e.second;  // reverse an existing edge -> directed cycle
    b = e.first;
  }
  std::vector<std::string> block = {"  - from: " + a,
                                    "    to: " + b,
                                    "    channel: " + channel,
                                    "    cpp_type: FuzzMsg",
                                    "    pool: FuzzPool",
                                    "    transport: spsc"};
  if (!SectionRange(*lines, "edges", &begin, &end)) {
    // No edges section: insert one after the nodes section (or at EOF).
    std::vector<std::string> full = {"edges:"};
    full.insert(full.end(), block.begin(), block.end());
    std::size_t nodes_begin = 0, nodes_end = 0;
    const std::size_t at = SectionRange(*lines, "nodes", &nodes_begin, &nodes_end)
                               ? nodes_end
                               : lines->size();
    lines->insert(lines->begin() + static_cast<std::ptrdiff_t>(at), full.begin(),
                  full.end());
    return true;
  }
  // `edges: []` is a single line; replace it with a real list.
  if (begin == end && Trim((*lines)[begin - 1]) == "edges: []") {
    (*lines)[begin - 1] = "edges:";
  }
  lines->insert(lines->begin() + static_cast<std::ptrdiff_t>(end), block.begin(),
                block.end());
  return true;
}

bool MutateFeedbackNoDelay(std::vector<std::string>* lines, SplitMix64* rng) {
  for (std::size_t i = 0; i < lines->size(); ++i) {
    if (KeyOf((*lines)[i]) != "feedback" || ValueOf((*lines)[i]) != "true") {
      continue;
    }
    // Drop the companion delay line, leaving feedback without a delay.
    for (std::size_t j = i + 1; j < lines->size(); ++j) {
      const std::string key = KeyOf((*lines)[j]);
      if (key == "delay") {
        lines->erase(lines->begin() + static_cast<std::ptrdiff_t>(j));
        return true;
      }
      if (StartsWith(TrimLeft((*lines)[j]), "- ")) break;
    }
    return ReplaceValue(lines, i, "true");  // already no delay: keep as-is
  }
  const std::vector<std::size_t> transport =
      LinesWithKey(*lines, {"transport"});
  if (transport.empty()) return false;
  const std::size_t at = rng->Pick(transport);
  lines->insert(lines->begin() + static_cast<std::ptrdiff_t>(at + 1),
                "    feedback: true");
  return true;
}

bool MutateOversizedNesting(std::vector<std::string>* lines, SplitMix64* rng) {
  switch (static_cast<int>(rng->Below(3))) {
    case 0: {  // one absurdly long scalar
      std::string line = "fuzz_long: ";
      line.append(2000, 'x');
      lines->insert(lines->begin() + static_cast<std::ptrdiff_t>(
                                        rng->Below(lines->size() + 1)),
                    line);
      return true;
    }
    case 1: {  // deep indentation is not accepted by the 0/2/4 grammar
      std::string line(2048, ' ');
      line += "deep: 1";
      lines->insert(lines->begin() + static_cast<std::ptrdiff_t>(
                                        rng->Below(lines->size() + 1)),
                    line);
      return true;
    }
    default: {  // many extra nodes, bounded by kMaxInput
      std::size_t begin = 0, end = 0;
      if (!SectionRange(*lines, "nodes", &begin, &end)) return false;
      if (JoinLines(*lines).size() + 512 > kMaxInput) return false;
      std::vector<std::string> block;
      for (int i = 0; i < 20; ++i) {
        block.push_back("  - id: fuzz_fill_" + std::to_string(i));
        block.push_back("    type: fuzz_fill");
        block.push_back("    hz: 10");
        block.push_back("    process: fuzz_fill");
      }
      lines->insert(lines->begin() + static_cast<std::ptrdiff_t>(end),
                    block.begin(), block.end());
      return true;
    }
  }
}

bool MutateInvalidIdent(std::vector<std::string>* lines, SplitMix64* rng) {
  static const char* const kBad[] = {"1bad",  "my-name", "has space", "",
                                     "a.b",   "class",   "-x",        "x\ty"};
  const std::vector<std::size_t> keys =
      LinesWithKey(*lines, {"id", "type", "process", "channel", "pool",
                            "cpp_type", "shm"});
  if (keys.empty()) return false;
  std::vector<std::string> choices(kBad, kBad + sizeof(kBad) / sizeof(kBad[0]));
  return ReplaceValue(lines, rng->Pick(keys), rng->Pick(choices));
}

bool MutateMixedIndent(std::vector<std::string>* lines, SplitMix64* rng) {
  std::vector<std::size_t> candidates;
  for (std::size_t i = 0; i < lines->size(); ++i) {
    const int ind = IndentOf((*lines)[i]);
    if (ind >= 2 && !Trim((*lines)[i]).empty()) candidates.push_back(i);
  }
  if (candidates.empty()) return false;
  const std::size_t at = rng->Pick(candidates);
  const int ind = IndentOf((*lines)[at]);
  const int next = rng->Coin(2) ? std::max(0, ind - 2) : ind + 2;
  (*lines)[at] = std::string(static_cast<std::size_t>(next), ' ') +
                 TrimLeft((*lines)[at]);
  return true;
}

bool MutateUtf8Damage(std::vector<std::string>* lines, SplitMix64* rng) {
  // Explicit lengths: the sequences are not NUL-terminated prose, they are
  // truncated/invalid UTF-8 byte runs.
  static const std::vector<std::string> kBad = {
      std::string("\xFF\xFE", 2), std::string("\xC3\x28", 2),
      std::string("\xED\xA0\x80", 3), std::string("\xF0\x28\x8C\x28", 4),
      std::string("\x80\x80", 2)};
  if (lines->empty()) return false;
  const std::size_t at = rng->Below(lines->size());
  std::string& line = (*lines)[at];
  const std::size_t pos = line.empty() ? 0 : rng->Below(line.size() + 1);
  line.insert(pos, rng->Pick(kBad));
  return true;
}

bool MutateNulByte(std::vector<std::string>* lines, SplitMix64* rng) {
  if (lines->empty()) return false;
  const std::size_t at = rng->Below(lines->size());
  std::string& line = (*lines)[at];
  const std::size_t pos = line.empty() ? 0 : rng->Below(line.size() + 1);
  line.insert(pos, 1, '\0');
  return true;
}

bool MutateHugeNumber(std::vector<std::string>* lines, SplitMix64* rng) {
  static const std::vector<std::string> kHuge = {
      "999999999999999999999999999999", "4294967296", "18446744073709551616",
      "-9223372036854775809"};
  const std::vector<std::size_t> keys =
      LinesWithKey(*lines, {"version", "pool_n", "ring_cap", "hz", "health_n",
                            "depth", "priority", "deadline_ns", "delay"});
  if (keys.empty()) return false;
  return ReplaceValue(lines, rng->Pick(keys), rng->Pick(kHuge));
}

bool MutateDeleteRequired(std::vector<std::string>* lines, SplitMix64* rng) {
  const std::vector<std::size_t> keys = LinesWithKey(
      *lines, {"version", "pool_n", "ring_cap", "nodes", "edges", "processes",
               "supervisor", "id", "type", "hz", "process", "from", "to",
               "channel", "cpp_type", "pool", "transport"});
  if (keys.empty()) return false;
  lines->erase(lines->begin() + static_cast<std::ptrdiff_t>(rng->Pick(keys)));
  return true;
}

bool MutateDuplicateNodeId(std::vector<std::string>* lines, SplitMix64* rng) {
  std::vector<std::string> ids;
  std::vector<std::string> processes;
  for (const std::string& line : *lines) {
    const std::string key = KeyOf(line);
    if (key == "id" && line.find("- id:") != std::string::npos) {
      // Node/process id; node items are the ones with a `- ` marker.
      if (StartsWith(TrimLeft(line), "- id:")) ids.push_back(ValueOf(line));
    }
    if (key == "process" && !ValueOf(line).empty()) {
      processes.push_back(ValueOf(line));
    }
  }
  if (ids.empty()) return false;
  std::size_t begin = 0, end = 0;
  if (!SectionRange(*lines, "nodes", &begin, &end)) return false;
  const std::string id = rng->Pick(ids);
  const std::string proc =
      processes.empty() ? "main" : rng->Pick(processes);
  std::vector<std::string> block = {"  - id: " + id,
                                    "    type: fuzz_dup",
                                    "    hz: 10",
                                    "    process: " + proc};
  lines->insert(lines->begin() + static_cast<std::ptrdiff_t>(end), block.begin(),
                block.end());
  return true;
}

bool MutateNumTweak(std::vector<std::string>* lines, SplitMix64* rng) {
  struct Choice {
    const char* key;
    std::vector<const char*> values;
  };
  const Choice choices[] = {
      {"pool_n", {"4", "8", "16", "32"}},
      {"ring_cap", {"8", "16", "32", "64"}},
      {"hz", {"1", "5", "10", "20", "50", "1000", "10000"}},
      {"health_n", {"8", "16", "32", "64"}},
  };
  std::vector<std::pair<std::size_t, const Choice*>> sites;
  for (std::size_t i = 0; i < lines->size(); ++i) {
    const std::string key = KeyOf((*lines)[i]);
    for (const Choice& c : choices) {
      if (key == c.key) sites.emplace_back(i, &c);
    }
  }
  if (sites.empty()) return false;
  const auto& site = rng->Pick(sites);
  const std::string value = rng->Pick(site.second->values);
  return ReplaceValue(lines, site.first, value);
}

bool MutateWhitespace(std::vector<std::string>* lines, SplitMix64* rng) {
  if (lines->empty()) return false;
  switch (static_cast<int>(rng->Below(4))) {
    case 0:
      lines->insert(lines->begin() + static_cast<std::ptrdiff_t>(
                                        rng->Below(lines->size() + 1)),
                    "");
      return true;
    case 1: {
      const std::size_t at = rng->Below(lines->size());
      (*lines)[at] += "   ";
      return true;
    }
    case 2:
      lines->insert(lines->begin(), "# fuzz: leading comment");
      return true;
    default: {
      const std::size_t at = rng->Below(lines->size());
      (*lines)[at] += "\r";
      return true;
    }
  }
}

bool MutateCommentInsert(std::vector<std::string>* lines, SplitMix64* rng) {
  if (lines->empty()) return false;
  const std::size_t at = rng->Below(lines->size() + 1);
  lines->insert(lines->begin() + static_cast<std::ptrdiff_t>(at),
                "# fuzz comment " + std::to_string(rng->Below(100000)));
  return true;
}

bool MutateRepeatedEdge(std::vector<std::string>* lines, SplitMix64* rng) {
  std::size_t begin = 0, end = 0;
  if (!SectionRange(*lines, "edges", &begin, &end)) return false;
  if (begin >= end) return false;
  // Last item start inside [begin, end).
  std::size_t item = end;
  for (std::size_t i = begin; i < end; ++i) {
    if (StartsWith(TrimLeft((*lines)[i]), "- ")) item = i;
  }
  if (item == end) return false;
  std::vector<std::string> block(lines->begin() + static_cast<std::ptrdiff_t>(item),
                                 lines->begin() + static_cast<std::ptrdiff_t>(end));
  if (JoinLines(*lines).size() + block.size() * 64 > kMaxInput) return false;
  lines->insert(lines->begin() + static_cast<std::ptrdiff_t>(end), block.begin(),
                block.end());
  (void)rng;
  return true;
}

bool ApplyFamily(int family, std::vector<std::string>* lines, SplitMix64* rng) {
  switch (family) {
    case kTruncate: return MutateTruncate(lines, rng);
    case kDuplicateKey: return MutateDuplicateKey(lines, rng);
    case kWrongType: return MutateWrongType(lines, rng);
    case kUnknownField: return MutateUnknownField(lines, rng);
    case kCycleInsert: return MutateCycleInsert(lines, rng);
    case kFeedbackNoDelay: return MutateFeedbackNoDelay(lines, rng);
    case kOversizedNesting: return MutateOversizedNesting(lines, rng);
    case kInvalidIdent: return MutateInvalidIdent(lines, rng);
    case kMixedIndent: return MutateMixedIndent(lines, rng);
    case kUtf8Damage: return MutateUtf8Damage(lines, rng);
    case kNulByte: return MutateNulByte(lines, rng);
    case kHugeNumber: return MutateHugeNumber(lines, rng);
    case kDeleteRequired: return MutateDeleteRequired(lines, rng);
    case kDuplicateNodeId: return MutateDuplicateNodeId(lines, rng);
    case kNumTweak: return MutateNumTweak(lines, rng);
    case kWhitespace: return MutateWhitespace(lines, rng);
    case kCommentInsert: return MutateCommentInsert(lines, rng);
    case kRepeatedEdge: return MutateRepeatedEdge(lines, rng);
    default: return false;
  }
}

// ---- Subprocess driver ----------------------------------------------------

struct RunResult {
  int code = -1;
  bool signaled = false;
  int signal = 0;
  std::string out;
  std::string err;
};

std::string ReadWhole(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

RunResult Spawn(const std::string& bin,
                const std::vector<std::string>& args,
                const fs::path& scratch) {
  const fs::path out_path = scratch / "child.stdout";
  const fs::path err_path = scratch / "child.stderr";
  RunResult result;
  const pid_t pid = fork();
  if (pid < 0) throw FuzzFailure("fork failed");
  if (pid == 0) {
    const int fo = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int fe = open(err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fo >= 0) {
      dup2(fo, STDOUT_FILENO);
      close(fo);
    }
    if (fe >= 0) {
      dup2(fe, STDERR_FILENO);
      close(fe);
    }
    alarm(kChildTimeoutSeconds);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(bin.c_str()));
    for (const std::string& a : args) {
      argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    execv(bin.c_str(), argv.data());
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) throw FuzzFailure("waitpid failed");
  }
  result.out = ReadWhole(out_path);
  result.err = ReadWhole(err_path);
  if (WIFSIGNALED(status)) {
    result.signaled = true;
    result.signal = WTERMSIG(status);
    result.code = -1;
  } else {
    result.code = WEXITSTATUS(status);
  }
  return result;
}

std::vector<std::string> BaseArgs(const std::string& in, const std::string& out,
                                  const std::string& tpl, bool check,
                                  const std::string& update = "generated") {
  std::vector<std::string> args = {"--in", in, "--out", out,
                                   "--update", update};
  if (!tpl.empty()) {
    args.push_back("--template-dir");
    args.push_back(tpl);
  }
  if (check) args.push_back("--check");
  return args;
}

bool DirHasEntries(const fs::path& dir) {
  std::error_code ec;
  if (!fs::exists(dir, ec)) return false;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    (void)entry;
    return true;
  }
  return false;
}

bool HasStagingEntries(const fs::path& dir) {
  std::error_code ec;
  if (!fs::exists(dir, ec)) return false;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (StartsWith(name, ".generated.") || StartsWith(name, ".gen_graph_tmp") ||
        StartsWith(name, ".gen_graph_tpl")) {
      return true;
    }
  }
  return false;
}

using TreeSnapshot = std::vector<std::pair<std::string, std::string>>;

TreeSnapshot SnapshotTree(const fs::path& root) {
  TreeSnapshot files;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    files.emplace_back(fs::relative(entry.path(), root).generic_string(),
                       ReadWhole(entry.path()));
  }
  std::sort(files.begin(), files.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  return files;
}

struct Stats {
  uint64_t accepted = 0;
  uint64_t rejected = 0;
  uint64_t runs = 0;
};

enum Mode {
  kModeGenerated = 0,
  kModeExplainRates,
  kModeDeriveTemplate,
  kModeUpdateAll,
  kModeCount
};

const char* const kModeNames[kModeCount] = {
    "generated", "explain-rates", "derive-template", "update-all"};

struct Runner {
  std::string bin;
  fs::path scratch;
  std::string in_path;
  std::string out_path;

  void CheckGenerated(const std::string& tpl, const std::string& context) {
    const RunResult chk =
        Spawn(bin, BaseArgs(in_path, out_path, tpl, true), scratch);
    if (chk.signaled || chk.code != 0) {
      throw FuzzFailure(context +
                        ": --check rejected a just-generated tree (exit " +
                        std::to_string(chk.code) + ")\n" + chk.err);
    }
  }

  void RunOne(const std::string& text, const Seed& seed, Stats* stats,
              const std::string& context, Mode mode = kModeGenerated,
              Stats* mode_stats = nullptr) {
    {
      std::ofstream f(in_path, std::ios::binary | std::ios::trunc);
      if (!f) throw FuzzFailure("cannot write " + in_path);
      f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    std::error_code ec;
    fs::remove_all(out_path, ec);
    const fs::path derive_root2 = out_path + ".derive2";
    fs::remove_all(derive_root2, ec);

    const std::string tpl =
        seed.tpl[0] == '\0'
            ? ""
            : std::string(GEN_GRAPH_FIXTURE_DIR) + "/" + seed.tpl;
    std::vector<std::string> args;
    if (mode == kModeExplainRates) {
      args = {"--in", in_path, "--out", out_path, "--explain-rates"};
    } else if (mode == kModeDeriveTemplate) {
      args = {"--in", in_path, "--out", out_path, "--template-out",
              (fs::path(out_path) / "template").string(), "--derive-template"};
    } else {
      args = BaseArgs(in_path, out_path, tpl, false,
                      mode == kModeUpdateAll ? "all" : "generated");
    }
    const RunResult r = Spawn(bin, args, scratch);
    ++stats->runs;
    if (mode_stats != nullptr) ++mode_stats->runs;
    if (r.signaled) {
      throw FuzzFailure(context + ": gen_graph killed by signal " +
                        std::to_string(r.signal));
    }
    if (r.code != 0 && r.code != 2) {
      throw FuzzFailure(context + ": exit " + std::to_string(r.code) +
                        " (expected 0 or 2)\n" + r.err);
    }
    if (r.code == 2) {
      if (r.err.empty()) {
        throw FuzzFailure(context + ": exit 2 with no diagnostic");
      }
      const bool has_location = r.err.find(in_path + ":") != std::string::npos;
      // Catalog and output I/O failures have no meaningful YAML line. Graph
      // semantic/emit errors carry GraphLoc; accept only a recognized tool
      // prefix for the remaining configuration diagnostics.
      const bool has_tool_diag =
          r.err.find("gen_graph: ") != std::string::npos ||
          r.err.find("EmitGenerated: ") != std::string::npos ||
          r.err.find("EmitGraphIrOnly: ") != std::string::npos ||
          r.err.find("LoadCatalog: ") != std::string::npos;
      if (!has_location && !has_tool_diag) {
        throw FuzzFailure(context +
                          ": exit 2 without a structured diagnostic\n" + r.err);
      }
      if (mode == kModeGenerated || mode == kModeExplainRates) {
        if (fs::exists(fs::path(out_path) / "generated")) {
          throw FuzzFailure(context +
                            ": rejected input left a generated/ tree behind");
        }
        if (DirHasEntries(out_path)) {
          throw FuzzFailure(context +
                            ": rejected input left files in the output dir");
        }
      } else if (mode == kModeUpdateAll) {
        if (HasStagingEntries(out_path)) {
          throw FuzzFailure(context +
                            ": rejected update all left staging output behind");
        }
        // A scaffold failure may happen after the atomic generated/ install.
        // Permit that complete tree, but never a partial or stale one.
        if (fs::exists(fs::path(out_path) / "generated")) {
          if (!fs::exists(fs::path(out_path) / "generated" / "wiring.cpp")) {
            throw FuzzFailure(context +
                              ": rejected update all left partial generated output");
          }
          CheckGenerated(tpl, context + " rejected update all");
        }
      }
      // derive-template is intentionally not transactional: a rejection may
      // leave files derived before the failing table.
      ++stats->rejected;
      if (mode_stats != nullptr) ++mode_stats->rejected;
      return;
    }

    if (mode == kModeExplainRates) {
      if (DirHasEntries(out_path)) {
        throw FuzzFailure(context + ": --explain-rates emitted output files");
      }
    } else if (mode == kModeDeriveTemplate) {
      const fs::path derived = fs::path(out_path) / "template";
      static const char* const kRequired[] = {
          "pools.txt", "contracts.txt", "known_types.txt",
          "channel_defaults.txt", "graph_types.h.inc", "mp_includes.inc"};
      for (const char* name : kRequired) {
        if (!fs::is_regular_file(derived / name)) {
          throw FuzzFailure(context + ": accepted derive-template produced no " +
                            name);
        }
      }
      const fs::path derived2 = derive_root2 / "template";
      std::vector<std::string> args2 = {
          "--in", in_path, "--out", derive_root2.string(), "--template-out",
          derived2.string(), "--derive-template"};
      const RunResult repeat = Spawn(bin, args2, scratch);
      if (repeat.signaled || repeat.code != 0) {
        throw FuzzFailure(context +
                          ": deterministic derive-template rerun failed\n" +
                          repeat.err);
      }
      if (SnapshotTree(derived) != SnapshotTree(derived2)) {
        throw FuzzFailure(context +
                          ": derive-template outputs differ across directories");
      }
    } else {
      if (!fs::exists(fs::path(out_path) / "generated" / "wiring.cpp")) {
        throw FuzzFailure(context + ": accepted input produced no wiring.cpp");
      }
      CheckGenerated(tpl, context);
    }
    ++stats->accepted;
    if (mode_stats != nullptr) ++mode_stats->accepted;
  }
};

std::vector<int> PickFamilies(SplitMix64* rng, bool valid_only) {
  std::vector<int> pool;
  for (int f = 0; f < kFamilyCount; ++f) {
    if (!valid_only || IsValidFamily(f)) pool.push_back(f);
  }
  const int count = 1 + static_cast<int>(rng->Below(3));
  std::vector<int> chosen;
  for (int i = 0; i < count; ++i) chosen.push_back(rng->Pick(pool));
  return chosen;
}

std::string DescribeFamilies(const std::vector<int>& families) {
  std::string out;
  for (std::size_t i = 0; i < families.size(); ++i) {
    if (i > 0) out += "+";
    out += kFamilyNames[families[i]];
  }
  return out;
}

}  // namespace

#if defined(THYME_FUZZ_LIBFUZZER)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0 || size > kMaxInput) return 0;
  static Runner runner = [] {
    Runner r;
    const char* env = std::getenv("THYME_GEN_GRAPH_BIN");
    r.bin = env != nullptr && env[0] != '\0' ? env : GEN_GRAPH_BIN;
    r.scratch = fs::path(GEN_GRAPH_FUZZ_TMPDIR) / "libfuzzer";
    fs::remove_all(r.scratch);
    fs::create_directories(r.scratch);
    r.in_path = (r.scratch / "in.yaml").string();
    r.out_path = (r.scratch / "out").string();
    return r;
  }();
  Stats stats;
  static const Seed kScaffold = {"scaffold/graph.yaml", "", true};
  runner.RunOne(std::string(reinterpret_cast<const char*>(data), size),
                kScaffold, &stats, "libFuzzer input");
  return 0;
}
#else
int main(int argc, char** argv) {
  const thyme_fuzz::RunConfig cfg =
      thyme_fuzz::ParseRunConfig(argc, argv, kDefaultIters, kDefaultSeed);

  try {
    const char* env_bin = std::getenv("THYME_GEN_GRAPH_BIN");
    const std::string bin =
        env_bin != nullptr && env_bin[0] != '\0' ? env_bin : GEN_GRAPH_BIN;
    const char* env_tmp = std::getenv("THYME_FUZZ_TMPDIR");
    const std::string base =
        env_tmp != nullptr && env_tmp[0] != '\0' ? env_tmp
                                                 : GEN_GRAPH_FUZZ_TMPDIR;
    if (StartsWith(base, "/dev/shm")) {
      throw FuzzFailure("refusing to use /dev/shm for fuzz scratch space");
    }
    if (!fs::exists(bin)) {
      throw FuzzFailure("gen_graph binary not found: " + bin);
    }

    const fs::path root =
        fs::path(base) / ("run." + std::to_string(static_cast<long>(getpid())));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    if (ec) throw FuzzFailure("cannot create " + root.string());

    Runner runner;
    runner.bin = bin;
    runner.scratch = root;
    runner.in_path = (root / "in.yaml").string();
    runner.out_path = (root / "out").string();

    std::vector<std::string> seed_texts;
    for (const Seed& seed : kSeeds) {
      const std::string path =
          std::string(GEN_GRAPH_FIXTURE_DIR) + "/" + seed.file;
      seed_texts.push_back(ReadFileOrDie(path));
      if (seed_texts.back().size() > kMaxInput) {
        throw FuzzFailure("seed exceeds the input cap: " + path);
      }
    }

    Stats stats;
    Stats mode_stats[kModeCount];

    // Phase 1 — replay every fixture unmutated. Valid seeds must be accepted
    // and pass --check; invalid seeds must be rejected (exit 2).
    for (std::size_t s = 0; s < kSeedCount; ++s) {
      const uint64_t accepted_before = stats.accepted;
      const uint64_t rejected_before = stats.rejected;
      runner.RunOne(seed_texts[s], kSeeds[s], &stats,
                    std::string("seed ") + kSeeds[s].file);
      if (kSeeds[s].valid) {
        if (stats.accepted != accepted_before + 1) {
          throw FuzzFailure(std::string("valid seed rejected: ") +
                            kSeeds[s].file);
        }
      } else if (stats.rejected != rejected_before + 1) {
        throw FuzzFailure(std::string("invalid seed accepted: ") +
                          kSeeds[s].file);
      }
    }

    // Phase 2 — valid-family mutations of known-good seeds must stay accepted.
    uint64_t valid_seeds = 0;
    for (const Seed& seed : kSeeds) {
      if (seed.valid) ++valid_seeds;
    }
    const uint64_t valid_iters =
        std::max<uint64_t>(20, cfg.iterations / 40);
    for (std::size_t s = 0; s < kSeedCount; ++s) {
      if (!kSeeds[s].valid) continue;
      for (uint64_t i = 0; i < valid_iters; ++i) {
        SplitMix64 rng(thyme_fuzz::IterSeed(cfg.seed ^ 0x5EED5EEDull,
                                            s * 1000003ull + i));
        std::vector<std::string> lines = SplitLines(seed_texts[s]);
        const std::vector<int> families = PickFamilies(&rng, true);
        for (int f : families) ApplyFamily(f, &lines, &rng);
        const std::string text = JoinLines(lines);
        const uint64_t accepted_before = stats.accepted;
        runner.RunOne(text, kSeeds[s], &stats,
                      "valid-family " + DescribeFamilies(families) + " seed " +
                          kSeeds[s].file);
        if (stats.accepted != accepted_before + 1) {
          throw FuzzFailure("valid-family mutation was rejected: " +
                            DescribeFamilies(families) + " on " +
                            kSeeds[s].file);
        }
      }
    }
    mode_stats[kModeGenerated] = stats;

    // Phase 3 — general structured mutations from every seed. The round-robin
    // offset makes mode selection a deterministic function of seed/iteration
    // while guaranteeing balanced coverage in both quick and full runs.
    for (uint64_t i = 0; i < cfg.iterations; ++i) {
      SplitMix64 rng(thyme_fuzz::IterSeed(cfg.seed, i));
      const Mode mode =
          static_cast<Mode>((i + cfg.seed % kModeCount) % kModeCount);
      const std::size_t s = rng.Below(kSeedCount);
      std::vector<std::string> lines = SplitLines(seed_texts[s]);
      const std::vector<int> families = PickFamilies(&rng, false);
      std::size_t applied = 0;
      for (int f : families) {
        if (ApplyFamily(f, &lines, &rng)) ++applied;
      }
      if (applied == 0) {
        // Keep the iteration count deterministic even when a family is not
        // applicable to this seed.
        lines.push_back("# fuzz: no-op mutation");
      }
      std::string text = JoinLines(lines);
      if (text.size() > kMaxInput) text.resize(kMaxInput);
      runner.RunOne(text, kSeeds[s], &stats,
                    "iter " + std::to_string(i) + " mode " + kModeNames[mode] +
                        " (" + DescribeFamilies(families) + ") seed " +
                        kSeeds[s].file,
                    mode, &mode_stats[mode]);
    }

    if (stats.accepted == 0 || stats.rejected == 0) {
      throw FuzzFailure("coverage collapsed: accepted=" +
                        std::to_string(stats.accepted) + " rejected=" +
                        std::to_string(stats.rejected));
    }
    for (int mode = 0; mode < kModeCount; ++mode) {
      const Stats& s = mode_stats[mode];
      if (s.accepted == 0 || s.rejected == 0) {
        throw FuzzFailure(std::string("mode coverage collapsed: ") +
                          kModeNames[mode] + " accepted=" +
                          std::to_string(s.accepted) + " rejected=" +
                          std::to_string(s.rejected));
      }
    }

    std::printf(
        "fuzz_gen_graph: %llu runs (%llu accepted, %llu rejected, seed=%llu, "
        "valid-family=%llu) OK\n",
        static_cast<unsigned long long>(stats.runs),
        static_cast<unsigned long long>(stats.accepted),
        static_cast<unsigned long long>(stats.rejected),
        static_cast<unsigned long long>(cfg.seed),
        static_cast<unsigned long long>(valid_iters * valid_seeds));
    for (int mode = 0; mode < kModeCount; ++mode) {
      const Stats& s = mode_stats[mode];
      std::printf("fuzz_gen_graph: mode %s: %llu runs (%llu accepted, "
                  "%llu rejected)\n",
                  kModeNames[mode], static_cast<unsigned long long>(s.runs),
                  static_cast<unsigned long long>(s.accepted),
                  static_cast<unsigned long long>(s.rejected));
    }
    fs::remove_all(root, ec);
    return 0;
  } catch (const FuzzFailure& e) {
    std::fprintf(stderr, "fuzz_gen_graph: FAIL: %s\n", e.what());
    // Preserve the last input for reproduction; the scratch dir itself is
    // intentionally kept so the child stdout/stderr can be inspected.
    const char* env_tmp = std::getenv("THYME_FUZZ_TMPDIR");
    const std::string base =
        env_tmp != nullptr && env_tmp[0] != '\0' ? env_tmp
                                                 : GEN_GRAPH_FUZZ_TMPDIR;
    const fs::path kept = fs::path(base) / "failure.yaml";
    std::fprintf(stderr, "fuzz_gen_graph: failing input kept as %s\n",
                 kept.string().c_str());
    try {
      const fs::path current =
          fs::path(base) /
          ("run." + std::to_string(static_cast<long>(getpid())));
      if (fs::exists(current / "in.yaml")) {
        std::error_code ec;
        fs::copy_file(current / "in.yaml", kept,
                      fs::copy_options::overwrite_existing, ec);
      }
    } catch (...) {
    }
    return 1;
  }
}
#endif
