#include "yaml.h"

#include "util.h"

#include "thyme/contracts/abi.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Every loader diagnostic carries `<source>:<line>:` so a failing graph.yaml
// names the exact line. `g_source`/`g_line` are thread-local and updated by
// LoadGraphYamlFromString before each parse step (the loader is single-pass;
// the post-load checks set g_line to the offending element's line).
thread_local const std::string* g_source = nullptr;
thread_local int g_line = 0;

// Expands to two fprintf calls so the existing "LoadGraphYaml: ..." format
// strings and their arguments stay unchanged.
#define LOAD_ERR(...)                                                \
  do {                                                               \
    std::fprintf(stderr, "%s:%d: ",                                  \
                 g_source != nullptr ? g_source->c_str() : "<yaml>", \
                 g_line);                                            \
    std::fprintf(stderr, __VA_ARGS__);                               \
  } while (0)

std::string Unquote(const std::string& s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

int Indent(const std::string& line) {
  int n = 0;
  while (n < static_cast<int>(line.size()) && line[static_cast<std::size_t>(n)] == ' ') {
    ++n;
  }
  return n;
}

bool SplitKeyVal(const std::string& line, std::string* key, std::string* val) {
  auto pos = line.find(':');
  if (pos == std::string::npos) return false;
  *key = gen_util::Trim(line.substr(0, pos));
  *val = Unquote(gen_util::Trim(line.substr(pos + 1)));
  return !key->empty();
}

bool RejectUnsupportedYamlValue(const std::string& line,
                                const std::string& key) {
  const std::size_t pos = line.find(':');
  if (pos == std::string::npos) return true;
  const std::string val = gen_util::Trim(line.substr(pos + 1));
  if (line.rfind("- &", 0) == 0 || (!val.empty() && val.front() == '&')) {
    LOAD_ERR("LoadGraphYaml: unsupported YAML anchor '&'\n");
    return false;
  }
  if (line.rfind("- *", 0) == 0 || (!val.empty() && val.front() == '*')) {
    LOAD_ERR("LoadGraphYaml: unsupported YAML alias '*'\n");
    return false;
  }
  if (!val.empty() && (val.front() == '|' || val.front() == '>')) {
    LOAD_ERR("LoadGraphYaml: unsupported YAML block scalar\n");
    return false;
  }
  if (line.rfind("- {", 0) == 0 || (!val.empty() && val.front() == '{')) {
    LOAD_ERR("LoadGraphYaml: unsupported YAML flow map\n");
    return false;
  }
  if (!val.empty() && val.front() == '[' && key != "cpu_set" &&
      !(val == "[]" &&
        (key == "nodes" || key == "edges" || key == "processes"))) {
    LOAD_ERR("LoadGraphYaml: unsupported YAML flow list\n");
    return false;
  }
  return true;
}

bool ParseStrictUInt32(const std::string& val, uint32_t* out) {
  if (out == nullptr || val.empty()) return false;
  std::size_t i = 0;
  if (val[i] == '+') ++i;
  if (i >= val.size()) return false;
  for (; i < val.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(val[i]))) return false;
  }
  char* end = nullptr;
  const unsigned long long n = std::strtoull(val.c_str(), &end, 10);
  if (end == val.c_str() || *end != '\0') return false;
  if (n > UINT32_MAX) return false;
  *out = static_cast<uint32_t>(n);
  return true;
}

bool ParseStrictUInt64(const std::string& val, uint64_t* out) {
  if (out == nullptr || val.empty()) return false;
  std::size_t i = 0;
  if (val[i] == '+') ++i;
  if (i >= val.size()) return false;
  for (; i < val.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(val[i]))) return false;
  }
  char* end = nullptr;
  const unsigned long long n = std::strtoull(val.c_str(), &end, 10);
  if (end == val.c_str() || *end != '\0') return false;
  *out = static_cast<uint64_t>(n);
  return true;
}

bool ParseStrictInt32(const std::string& val, int32_t* out) {
  if (out == nullptr || val.empty()) return false;
  std::size_t i = 0;
  if (val[i] == '+' || val[i] == '-') ++i;
  if (i >= val.size()) return false;
  for (; i < val.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(val[i]))) return false;
  }
  char* end = nullptr;
  const long long n = std::strtoll(val.c_str(), &end, 10);
  if (end == val.c_str() || *end != '\0') return false;
  if (n < INT32_MIN || n > INT32_MAX) return false;
  *out = static_cast<int32_t>(n);
  return true;
}

bool ParseCpuSet(const std::string& val, std::vector<int>* out) {
  if (out == nullptr) return false;
  out->clear();
  if (val.size() < 2 || val.front() != '[' || val.back() != ']') return false;
  const std::string inner = gen_util::Trim(val.substr(1, val.size() - 2));
  if (inner.empty()) return false;
  std::size_t start = 0;
  while (start <= inner.size()) {
    const std::size_t comma = inner.find(',', start);
    std::string tok = (comma == std::string::npos)
                          ? inner.substr(start)
                          : inner.substr(start, comma - start);
    tok = gen_util::Trim(tok);
    if (tok.empty()) return false;
    int32_t v = 0;
    if (!ParseStrictInt32(tok, &v)) return false;
    out->push_back(v);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return true;
}

constexpr uint32_t kMaxNodeHz = 10000;

bool FitsPositiveInt32(uint32_t v) {
  return v <= static_cast<uint32_t>(INT_MAX);
}

bool ParseNodeHz(const std::string& val, int* hz, const char* node_id) {
  uint32_t u = 0;
  if (!ParseStrictUInt32(val, &u)) {
    LOAD_ERR("LoadGraphYaml: bad hz for node %s (%s)\n",
                 node_id ? node_id : "?", val.c_str());
    return false;
  }
  if (u == 0) {
    LOAD_ERR("LoadGraphYaml: hz must be > 0 for node %s\n",
                 node_id ? node_id : "?");
    return false;
  }
  if (u > kMaxNodeHz) {
    LOAD_ERR("LoadGraphYaml: hz must be <= %u for node %s\n",
                 kMaxNodeHz, node_id ? node_id : "?");
    return false;
  }
  *hz = static_cast<int>(u);
  return true;
}

bool ApplyNodeKey(const std::string& key, const std::string& val, Node* n) {
  if (key == "id") {
    n->id = val;
  } else if (key == "type") {
    n->type = val;
  } else if (key == "hz") {
    if (!ParseNodeHz(val, &n->hz, n->id.c_str())) return false;
  } else if (key == "process") {
    n->process = val;
  } else if (key == "cpu_set") {
    if (!ParseCpuSet(val, &n->cpu_set)) {
      LOAD_ERR("LoadGraphYaml: bad cpu_set\n");
      return false;
    }
  } else if (key == "sched_policy") {
    if (!val.empty() && val != "fifo" && val != "rr" && val != "other") {
      LOAD_ERR("LoadGraphYaml: illegal sched_policy %s\n", val.c_str());
      return false;
    }
    n->sched_policy = val;
  } else if (key == "priority") {
    int32_t p = 0;
    if (!ParseStrictInt32(val, &p)) {
      LOAD_ERR("LoadGraphYaml: bad priority (%s)\n", val.c_str());
      return false;
    }
    n->priority = p;
  } else if (key == "placement_fail") {
    if (!val.empty() && val != "best_effort" && val != "require") {
      LOAD_ERR("LoadGraphYaml: illegal placement_fail %s\n", val.c_str());
      return false;
    }
    n->placement_fail = val;
  } else if (key == "wait_inputs") {
    if (!val.empty() && val != "none" && val != "period") {
      LOAD_ERR("LoadGraphYaml: illegal wait_inputs %s\n", val.c_str());
      return false;
    }
    n->wait_inputs = val;
  } else if (key == "exec") {
    if (!val.empty() && val != "cyclic" && val != "event") {
      LOAD_ERR("LoadGraphYaml: illegal exec %s\n", val.c_str());
      return false;
    }
    n->exec = val;
  } else if (key == "publish") {
    n->publish = val;
  } else if (key == "deadline_ns") {
    uint64_t d = 0;
    if (!ParseStrictUInt64(val, &d) || d == 0) {
      LOAD_ERR("LoadGraphYaml: bad deadline_ns (%s)\n", val.c_str());
      return false;
    }
    n->deadline_ns = d;
  } else if (key == "wcet_ns") {
    uint64_t w = 0;
    if (!ParseStrictUInt64(val, &w) || w == 0) {
      LOAD_ERR("LoadGraphYaml: bad wcet_ns (%s); must be > 0\n", val.c_str());
      return false;
    }
    n->wcet_ns = w;
  } else if (key == "input_budget") {
    uint32_t v = 0;
    if (!ParseStrictUInt32(val, &v) || v == 0) {
      LOAD_ERR("LoadGraphYaml: bad input_budget (%s)\n", val.c_str());
      return false;
    }
    n->input_budget = v;
  } else if (key == "produce") {
    if (!val.empty() && val != "one" && val != "many") {
      LOAD_ERR("LoadGraphYaml: illegal produce %s (want one|many)\n",
               val.c_str());
      return false;
    }
    n->produce = val;
  } else if (key == "overrun") {
    if (!val.empty() && val != "skip" && val != "degrade" &&
        val != "restart") {
      LOAD_ERR("LoadGraphYaml: illegal overrun %s\n", val.c_str());
      return false;
    }
    n->overrun = val;
  } else if (key == "threads") {
    uint32_t v = 0;
    if (!ParseStrictUInt32(val, &v) || v == 0 || v > 64) {
      LOAD_ERR("LoadGraphYaml: bad threads (%s); must be in 1..64\n",
               val.c_str());
      return false;
    }
    n->threads = v;
  } else {
    LOAD_ERR("LoadGraphYaml: unknown node key %s\n", key.c_str());
    return false;
  }
  return true;
}

bool ApplyEdgeKey(const std::string& key, const std::string& val, Edge* e) {
  if (key == "from") {
    e->from = val;
  } else if (key == "to") {
    e->to = val;
  } else if (key == "channel") {
    e->channel = val;
  } else if (key == "cpp_type") {
    e->cpp_type = val;
  } else if (key == "pool") {
    e->pool = val;
  } else if (key == "transport") {
    e->transport = val;
  } else if (key == "shm") {
    e->shm = val;
  } else if (key == "delivery") {
    if (!val.empty() && val != "latest" && val != "notify" && val != "queue" &&
        val != "broadcast") {
      LOAD_ERR("LoadGraphYaml: illegal delivery %s\n", val.c_str());
      return false;
    }
    e->delivery = val;
  } else if (key == "read") {
    if (!val.empty() && val != "latest" && val != "follow") {
      LOAD_ERR("LoadGraphYaml: illegal read %s\n", val.c_str());
      return false;
    }
    e->read = val;
  } else if (key == "record_priority") {
    if (!val.empty() && val != "p0" && val != "p1" && val != "p2") {
      LOAD_ERR("LoadGraphYaml: illegal record_priority %s\n", val.c_str());
      return false;
    }
    e->record_priority = val;
  } else if (key == "depth") {
    uint32_t v = 0;
    if (!ParseStrictUInt32(val, &v) || v == 0 || !FitsPositiveInt32(v)) {
      LOAD_ERR("LoadGraphYaml: bad depth (%s)\n", val.c_str());
      return false;
    }
    if (v < 2 || (v & (v - 1)) != 0) {
      LOAD_ERR("LoadGraphYaml: depth must be a power of two (%s)\n",
                   val.c_str());
      return false;
    }
    e->depth = static_cast<int>(v);
  } else if (key == "record") {
    // GEN-12: opt an edge (all edges of its channel) out of the record
    // manifest. Absent = true, so pre-GEN-12 graphs are unchanged.
    if (val != "true" && val != "false") {
      LOAD_ERR("LoadGraphYaml: illegal record %s\n", val.c_str());
      return false;
    }
    e->record = (val == "true");
  } else if (key == "record_gate") {
    if (val != "true" && val != "false") {
      LOAD_ERR("LoadGraphYaml: illegal record_gate %s\n", val.c_str());
      return false;
    }
    e->record_gate = (val == "true");
  } else if (key == "record_kind") {
    if (val != "mirror" && val != "pinned_slot" && val != "h264") {
      LOAD_ERR("LoadGraphYaml: illegal record_kind %s\n", val.c_str());
      return false;
    }
    e->record_kind = val == "mirror" ? std::string() : val;
  } else if (key == "bitrate_kbps") {
    uint64_t value = 0;
    if (!ParseStrictUInt64(val, &value) || value == 0 || value > 0xffffffffull) {
      LOAD_ERR("LoadGraphYaml: bad bitrate_kbps (%s)\n", val.c_str());
      return false;
    }
    e->bitrate_kbps = static_cast<uint32_t>(value);
  } else if (key == "avg_frame_bytes") {
    if (!ParseStrictUInt64(val, &e->avg_frame_bytes) || e->avg_frame_bytes == 0) {
      LOAD_ERR("LoadGraphYaml: bad avg_frame_bytes (%s)\n", val.c_str());
      return false;
    }
  } else if (key == "max_frame_bytes") {
    if (!ParseStrictUInt64(val, &e->max_frame_bytes) || e->max_frame_bytes == 0 ||
        e->max_frame_bytes > 0xffffffffull) {
      LOAD_ERR("LoadGraphYaml: bad max_frame_bytes (%s)\n", val.c_str());
      return false;
    }
  } else if (key == "record_shm") {
    e->record_shm = val;
  } else if (key == "dynamic") {
    if (val != "true" && val != "false") {
      LOAD_ERR("LoadGraphYaml: illegal dynamic %s\n", val.c_str());
      return false;
    }
    e->dynamic = (val == "true");
  } else if (key == "mp_gate") {
    if (val != "true" && val != "false") {
      LOAD_ERR("LoadGraphYaml: illegal mp_gate %s\n", val.c_str());
      return false;
    }
    e->mp_gate = (val == "true");
  } else if (key == "lossless") {
    if (val != "true" && val != "false") {
      LOAD_ERR("LoadGraphYaml: illegal lossless %s\n", val.c_str());
      return false;
    }
    e->lossless = (val == "true");
  } else if (key == "max_lag_ms" || key == "before_ms" || key == "after_ms" ||
             key == "per_frame_write_ms" || key == "pin_budget") {
    uint64_t value = 0;
    if (!ParseStrictUInt64(val, &value) || value == 0) {
      LOAD_ERR("LoadGraphYaml: bad %s (%s); must be > 0\n", key.c_str(),
               val.c_str());
      return false;
    }
    if (key == "max_lag_ms") {
      e->max_lag_ms = value;
    } else if (key == "before_ms") {
      e->before_ms = value;
    } else if (key == "after_ms") {
      e->after_ms = value;
    } else if (key == "per_frame_write_ms") {
      e->per_frame_write_ms = value;
    } else {
      e->pin_budget = value;
    }
  } else if (key == "feedback") {
    if (val != "true" && val != "false") {
      LOAD_ERR("LoadGraphYaml: illegal feedback %s\n", val.c_str());
      return false;
    }
    e->feedback = (val == "true");
  } else if (key == "delay") {
    // Delay in nanoseconds (plain integer, like deadline_ns). Range-checked
    // against feedback in the post-load validation below.
    if (!ParseStrictUInt64(val, &e->delay_ns)) {
      LOAD_ERR("LoadGraphYaml: bad delay (%s)\n", val.c_str());
      return false;
    }
  } else {
    LOAD_ERR("LoadGraphYaml: unknown edge key %s\n", key.c_str());
    return false;
  }
  return true;
}

// Derive delivery from topology (spec §1.A): cyclic targets → latest;
// same-process → event → notify; cross-process → event → queue.
std::string DeriveDelivery(const Node* from, const Node* to) {
  if (to != nullptr && to->exec == "event") {
    if (from != nullptr && from->process != to->process) return "queue";
    return "notify";
  }
  return "latest";
}

const Node* FindNode(const Graph& g, const std::string& id) {
  for (const auto& n : g.nodes) {
    if (n.id == id) return &n;
  }
  return nullptr;
}

int EdgeCap(const Graph& g, const Edge& e) {
  return e.depth > 0 ? e.depth : g.ring_cap;
}

struct ShmUse {
  std::string delivery;
  std::string from;
  std::string channel;
  std::string cpp_type;
  int cap = 0;
  std::vector<std::string> tos;
};

bool ApplyProcessKey(const std::string& key, const std::string& val, Process* p) {
  if (key == "id") {
    p->id = val;
  } else if (key == "role") {
    p->role = val;
  } else {
    LOAD_ERR("LoadGraphYaml: unknown process key %s\n", key.c_str());
    return false;
  }
  return true;
}

enum class Sec { kNone, kNodes, kEdges, kProcesses, kSupervisor };

}  // namespace

bool LoadGraphYamlFromString(const std::string& text, Graph* out,
                             const std::string& source) {
  if (out == nullptr) return false;
  *out = Graph{};
  out->source = source;
  g_source = &out->source;
  g_line = 1;
  const std::size_t tab = text.find('\t');
  if (tab != std::string::npos) {
    // Tabs are banned anywhere in the file (indentation is space-counted).
    // Report the exact line and 1-based column of the first offender.
    g_line = 1 + static_cast<int>(std::count(text.begin(), text.begin() +
                                             static_cast<std::ptrdiff_t>(tab),
                                             '\n'));
    const std::size_t bol = text.rfind('\n', tab);
    const std::size_t col = tab - (bol == std::string::npos ? 0 : bol + 1) + 1;
    LOAD_ERR("LoadGraphYaml: tabs are not allowed (column %zu)\n", col);
    return false;
  }

  Sec sec = Sec::kNone;
  Node cur_n;
  Edge cur_e;
  Process cur_p;
  bool have_n = false, have_e = false, have_p = false;
  int nodes_line = 0, processes_line = 0, version_line = 0;

  auto flush_n = [&]() {
    if (have_n) out->nodes.push_back(cur_n);
    cur_n = Node{};
    have_n = false;
  };
  auto flush_e = [&]() {
    if (have_e) out->edges.push_back(cur_e);
    cur_e = Edge{};
    have_e = false;
  };
  auto flush_p = [&]() {
    if (have_p) out->processes.push_back(cur_p);
    cur_p = Process{};
    have_p = false;
  };

  std::istringstream ls(text);
  std::string raw;
  int lineno = 0;
  while (std::getline(ls, raw)) {
    ++lineno;
    g_line = lineno;
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    const std::string trimmed = gen_util::Trim(raw);
    if (trimmed.empty() || trimmed[0] == '#') continue;

    const int ind = Indent(raw);
    std::string key, val;
    const std::string line = gen_util::Trim(raw);
    const bool kv = SplitKeyVal(line, &key, &val);
    if (kv && !RejectUnsupportedYamlValue(line, key)) return false;

    if (ind == 0 && kv) {
      flush_n();
      flush_e();
      flush_p();
      if (key == "version") {
        uint32_t v = 0;
        if (!ParseStrictUInt32(val, &v)) {
          LOAD_ERR("LoadGraphYaml: bad version (%s)\n", val.c_str());
          return false;
        }
        out->version = static_cast<int>(v);
        version_line = lineno;
        sec = Sec::kNone;
      } else if (key == "pool_n") {
        uint32_t v = 0;
        if (!ParseStrictUInt32(val, &v) || v == 0 || !FitsPositiveInt32(v)) {
          LOAD_ERR("LoadGraphYaml: bad pool_n (%s)\n", val.c_str());
          return false;
        }
        out->pool_n = static_cast<int>(v);
        sec = Sec::kNone;
      } else if (key == "ring_cap") {
        uint32_t v = 0;
        if (!ParseStrictUInt32(val, &v) || v == 0 || !FitsPositiveInt32(v)) {
          LOAD_ERR("LoadGraphYaml: bad ring_cap (%s)\n", val.c_str());
          return false;
        }
        if (v < 2 || (v & (v - 1)) != 0) {
          LOAD_ERR("LoadGraphYaml: ring_cap must be a power of two (%s)\n",
                       val.c_str());
          return false;
        }
        out->ring_cap = static_cast<int>(v);
        sec = Sec::kNone;
      } else if (key == "phase_mode") {
        if (!IsValidPhaseMode(val)) {
          LOAD_ERR("LoadGraphYaml: phase_mode must be off|best_effort|require "
                   "(%s)\n",
                   val.c_str());
          return false;
        }
        out->phase_mode = val;
        sec = Sec::kNone;
      } else if (key == "nodes") {
        sec = Sec::kNodes;
        nodes_line = lineno;
      } else if (key == "edges") {
        sec = Sec::kEdges;
      } else if (key == "processes") {
        sec = Sec::kProcesses;
        processes_line = lineno;
      } else if (key == "supervisor") {
        sec = Sec::kSupervisor;
      } else {
        LOAD_ERR("LoadGraphYaml: unknown top-level key %s\n",
                     key.c_str());
        return false;
      }
      continue;
    }

    if (sec == Sec::kSupervisor && ind == 2 && kv) {
      if (key == "shm_prefix") {
        out->shm_prefix = val;
      } else if (key == "health_n") {
        uint32_t v = 0;
        if (!ParseStrictUInt32(val, &v) || v == 0 || !FitsPositiveInt32(v)) {
          LOAD_ERR("LoadGraphYaml: bad health_n (%s)\n", val.c_str());
          return false;
        }
        out->health_n = static_cast<int>(v);
      } else {
        LOAD_ERR("LoadGraphYaml: unknown supervisor key %s\n",
                     key.c_str());
        return false;
      }
      continue;
    }

    if (ind == 2 && trimmed.size() >= 2 && trimmed[0] == '-' &&
        trimmed[1] == ' ') {
      std::string rest = gen_util::Trim(trimmed.substr(2));
      std::string ik, iv;
      if (!SplitKeyVal(rest, &ik, &iv)) {
        LOAD_ERR("LoadGraphYaml: list item is not `key: value` (%s)\n",
                 rest.c_str());
        return false;
      }
      if (sec == Sec::kNodes) {
        flush_n();
        cur_n = Node{};
        cur_n.line = lineno;
        have_n = true;
        if (!ApplyNodeKey(ik, iv, &cur_n)) return false;
      } else if (sec == Sec::kEdges) {
        flush_e();
        cur_e = Edge{};
        cur_e.line = lineno;
        have_e = true;
        if (!ApplyEdgeKey(ik, iv, &cur_e)) return false;
      } else if (sec == Sec::kProcesses) {
        flush_p();
        cur_p = Process{};
        cur_p.line = lineno;
        have_p = true;
        if (!ApplyProcessKey(ik, iv, &cur_p)) return false;
      } else {
        LOAD_ERR("LoadGraphYaml: list item outside nodes/edges/processes "
                 "section\n");
        return false;
      }
      continue;
    }

    if (ind == 4 && kv) {
      if (sec == Sec::kNodes && have_n) {
        if (!ApplyNodeKey(key, val, &cur_n)) return false;
      } else if (sec == Sec::kEdges && have_e) {
        if (!ApplyEdgeKey(key, val, &cur_e)) return false;
      } else if (sec == Sec::kProcesses && have_p) {
        if (!ApplyProcessKey(key, val, &cur_p)) return false;
      }
      continue;
    }
  }
  flush_n();
  flush_e();
  flush_p();

  if (out->version != 1) {
    g_line = version_line > 0 ? version_line : 1;
    LOAD_ERR("LoadGraphYaml: version must be 1\n");
    return false;
  }
  if (out->nodes.empty()) {
    g_line = nodes_line > 0 ? nodes_line : 1;
    LOAD_ERR("LoadGraphYaml: nodes empty\n");
    return false;
  }
  for (const auto& n : out->nodes) {
    g_line = n.line > 0 ? n.line : 1;
    if (!gen_util::IsIdent(n.id)) {
      LOAD_ERR("LoadGraphYaml: invalid or missing node id '%s'\n",
               n.id.c_str());
      return false;
    }
    if (n.type.empty()) {
      LOAD_ERR("LoadGraphYaml: empty type for node %s\n",
                   n.id.c_str());
      return false;
    }
    if (n.hz <= 0) {
      LOAD_ERR("LoadGraphYaml: hz must be > 0 for node %s\n",
                   n.id.c_str());
      return false;
    }
    if (n.exec == "event") {
      // Event nodes are woken by their input NotifPipes; wait_inputs is the
      // cyclic early-wake mechanism and does not apply. Placement follows the
      // same ThreadPlacement contract as cyclic nodes (EventThread applies it
      // at Start). hz stays required: expected steady-state messages/s
      // (not a Loop period). MinLoopsForProcess uses process min hz.
      if (!n.wait_inputs.empty()) {
        LOAD_ERR(
                     "LoadGraphYaml: wait_inputs not allowed on exec=event node %s\n",
                     n.id.c_str());
        return false;
      }
      // SCH-04: an event overrun policy without a dispatch budget never
      // fires — reject it instead of silently doing nothing (DOC-08).
      if (!n.overrun.empty() && n.overrun != "skip" &&
          n.deadline_ns == 0) {
        LOAD_ERR(
                     "LoadGraphYaml: overrun %s on exec=event node %s "
                     "requires deadline_ns\n",
                     n.overrun.c_str(), n.id.c_str());
        return false;
      }
    } else {
      if (n.deadline_ns > 0) {
        LOAD_ERR(
                     "LoadGraphYaml: deadline_ns only allowed on exec=event node %s\n",
                     n.id.c_str());
        return false;
      }
      // SCH-03 input budgets are an EventThread dispatch mechanism.
      if (n.input_budget > 0) {
        LOAD_ERR(
                     "LoadGraphYaml: input_budget only allowed on exec=event node %s\n",
                     n.id.c_str());
        return false;
      }
    }
  }
  // Fill / check delivery, then enforce shm-as-suffix rules. Old graphs that
  // only set shm still load: delivery is derived from process/exec topology.
  // Explicit delivery: broadcast is not derived and skips the conflict check.
  {
    std::map<std::string, ShmUse> shm_uses;
    for (auto& e : out->edges) {
      g_line = e.line > 0 ? e.line : 1;
      const Node* from = FindNode(*out, e.from);
      const Node* to = FindNode(*out, e.to);
      const std::string derived = DeriveDelivery(from, to);
      if (e.delivery == "broadcast") {
        // skip derived-conflict; do NOT overwrite
      } else if (e.delivery.empty()) {
        e.delivery = derived;
      } else if (e.delivery != derived) {
        LOAD_ERR(
                     "LoadGraphYaml: edge %s->%s (%s) delivery %s conflicts with "
                     "derived %s\n",
                     e.from.c_str(), e.to.c_str(), e.channel.c_str(),
                     e.delivery.c_str(), derived.c_str());
        return false;
      }
      const bool cross =
          from != nullptr && to != nullptr && from->process != to->process;
      // shm is a POSIX-name suffix only; layout is carried by delivery.
      if (e.delivery == "broadcast") {
        if (!cross) {
          LOAD_ERR(
                       "LoadGraphYaml: same-process broadcast edge %s->%s (%s) "
                       "is not allowed\n",
                       e.from.c_str(), e.to.c_str(), e.channel.c_str());
          return false;
        }
        if (e.shm.empty()) {
          LOAD_ERR(
                       "LoadGraphYaml: edge %s->%s (%s) delivery broadcast "
                       "requires shm\n",
                       e.from.c_str(), e.to.c_str(), e.channel.c_str());
          return false;
        }
        if (!e.pool.empty()) {
          LOAD_ERR(
                       "LoadGraphYaml: broadcast edge %s->%s (%s) must not use "
                       "pool\n",
                       e.from.c_str(), e.to.c_str(), e.channel.c_str());
          return false;
        }
      } else if (e.delivery == "notify") {
        if (!e.shm.empty()) {
          LOAD_ERR(
                       "LoadGraphYaml: notify edge %s->%s (%s) must not use shm\n",
                       e.from.c_str(), e.to.c_str(), e.channel.c_str());
          return false;
        }
      } else if (e.delivery == "queue" || (e.delivery == "latest" && cross)) {
        if (e.shm.empty()) {
          LOAD_ERR(
                       "LoadGraphYaml: edge %s->%s (%s) delivery %s requires shm\n",
                       e.from.c_str(), e.to.c_str(), e.channel.c_str(),
                       e.delivery.c_str());
          return false;
        }
      } else if (!e.shm.empty()) {
        LOAD_ERR(
                     "LoadGraphYaml: same-process latest edge %s->%s (%s) must "
                     "not use shm\n",
                     e.from.c_str(), e.to.c_str(), e.channel.c_str());
        return false;
      }
      if (!e.read.empty() && e.delivery != "broadcast") {
        LOAD_ERR(
                     "LoadGraphYaml: read on edge %s->%s (%s) is only allowed "
                     "on delivery broadcast (got %s)\n",
                     e.from.c_str(), e.to.c_str(), e.channel.c_str(),
                     e.delivery.c_str());
        return false;
      }
      if (!e.shm.empty()) {
        const int cap = EdgeCap(*out, e);
        auto it = shm_uses.find(e.shm);
        if (it == shm_uses.end()) {
          ShmUse u;
          u.delivery = e.delivery;
          u.from = e.from;
          u.channel = e.channel;
          u.cpp_type = e.cpp_type;
          u.cap = cap;
          u.tos.push_back(e.to);
          shm_uses.emplace(e.shm, std::move(u));
        } else {
          bool different_to = true;
          for (const auto& t : it->second.tos) {
            if (t == e.to) {
              different_to = false;
              break;
            }
          }
          const bool share_ok = it->second.delivery == "broadcast" &&
                                e.delivery == "broadcast" &&
                                it->second.from == e.from &&
                                it->second.channel == e.channel &&
                                it->second.cpp_type == e.cpp_type &&
                                it->second.cap == cap && different_to;
          if (share_ok) {
            it->second.tos.push_back(e.to);
            if (it->second.tos.size() > thyme::kAbiMaxSpmcReaders) {
              LOAD_ERR(
                           "LoadGraphYaml: shm suffix %s has %zu broadcast "
                           "readers, limit is %zu (kAbiMaxSpmcReaders)\n",
                           e.shm.c_str(), it->second.tos.size(),
                           thyme::kAbiMaxSpmcReaders);
              return false;
            }
          } else if (it->second.delivery != e.delivery) {
            LOAD_ERR(
                         "LoadGraphYaml: shm suffix %s maps to both %s and %s "
                         "layouts\n",
                         e.shm.c_str(), it->second.delivery.c_str(),
                         e.delivery.c_str());
            return false;
          } else {
            LOAD_ERR("LoadGraphYaml: duplicate shm suffix %s\n",
                         e.shm.c_str());
            return false;
          }
        }
      }
      if (e.depth > 0) {
        if (e.delivery != "notify" && e.delivery != "queue" &&
            e.delivery != "broadcast") {
          LOAD_ERR(
                       "LoadGraphYaml: depth on edge %s->%s (%s) requires "
                       "delivery notify, queue, or broadcast (got %s)\n",
                       e.from.c_str(), e.to.c_str(), e.channel.c_str(),
                       e.delivery.c_str());
          return false;
        }
      }
      if (e.lossless && e.delivery == "latest") {
        LOAD_ERR(
                     "LoadGraphYaml: lossless edge %s->%s (%s) requires "
                     "delivery notify, queue, or broadcast (got latest)\n",
                     e.from.c_str(), e.to.c_str(), e.channel.c_str());
        return false;
      }
      if (e.max_lag_ms > 0 && !e.lossless) {
        LOAD_ERR(
                     "LoadGraphYaml: max_lag_ms on edge %s->%s (%s) requires "
                     "lossless: true\n",
                     e.from.c_str(), e.to.c_str(), e.channel.c_str());
        return false;
      }
      const bool any_pin_window = e.before_ms > 0 || e.after_ms > 0 ||
                                  e.per_frame_write_ms > 0 || e.pin_budget > 0;
      const bool full_pin_window = e.before_ms > 0 && e.after_ms > 0 &&
                                   e.per_frame_write_ms > 0;
      if (any_pin_window && !full_pin_window) {
        LOAD_ERR(
                     "LoadGraphYaml: edge %s->%s (%s) requires before_ms, "
                     "after_ms and per_frame_write_ms together\n",
                     e.from.c_str(), e.to.c_str(), e.channel.c_str());
        return false;
      }
      if (e.feedback) {
        // GEN-11: a feedback edge closes a directed loop on purpose; the
        // explicit delay is what makes the loop safe (one stale frame), so
        // both are required together.
        if (e.delay_ns == 0) {
          LOAD_ERR(
                       "LoadGraphYaml: feedback edge %s->%s (%s) requires an "
                       "explicit delay > 0 (ns)\n",
                       e.from.c_str(), e.to.c_str(), e.channel.c_str());
          return false;
        }
      } else if (e.delay_ns > 0) {
        LOAD_ERR(
                     "LoadGraphYaml: delay on edge %s->%s (%s) requires "
                     "feedback: true\n",
                     e.from.c_str(), e.to.c_str(), e.channel.c_str());
        return false;
      }
      if (e.mp_gate) {
        // Supervisor success gates count frames on a cross-process
        // LatestSlot; anything else has no counter to wire the gate to.
        if (e.delivery != "latest") {
          LOAD_ERR(
                       "LoadGraphYaml: mp_gate on edge %s->%s (%s) requires "
                       "delivery latest (got %s)\n",
                       e.from.c_str(), e.to.c_str(), e.channel.c_str(),
                       e.delivery.c_str());
          return false;
        }
        if (!cross) {
          LOAD_ERR(
                       "LoadGraphYaml: mp_gate on edge %s->%s (%s) requires a "
                       "cross-process edge\n",
                       e.from.c_str(), e.to.c_str(), e.channel.c_str());
          return false;
        }
      }
    }
  }
  {
    std::map<std::string, const Edge*> by_channel;
    std::set<std::string> bulk_shm;
    for (const Edge& edge : out->edges) {
      g_line = edge.line > 0 ? edge.line : 1;
      const bool mirror = !EdgeRecordsBulk(edge);
      if (mirror && edge.bitrate_kbps != 0) {
        LOAD_ERR("LoadGraphYaml: edge %s->%s (%s) mirror record rejects "
                 "bitrate_kbps\n",
                 edge.from.c_str(), edge.to.c_str(), edge.channel.c_str());
        return false;
      }
      if (edge.record_kind == "h264" &&
          (edge.bitrate_kbps == 0 || edge.avg_frame_bytes != 0)) {
        LOAD_ERR("LoadGraphYaml: edge %s->%s (%s) h264 requires bitrate_kbps "
                 "and rejects avg_frame_bytes\n",
                 edge.from.c_str(), edge.to.c_str(), edge.channel.c_str());
        return false;
      }
      if (edge.record_kind == "pinned_slot" &&
          (edge.avg_frame_bytes == 0 || edge.bitrate_kbps != 0)) {
        LOAD_ERR("LoadGraphYaml: edge %s->%s (%s) pinned_slot requires "
                 "avg_frame_bytes and rejects bitrate_kbps\n",
                 edge.from.c_str(), edge.to.c_str(), edge.channel.c_str());
        return false;
      }
      if (!edge.record_shm.empty()) {
        bool ok = edge.record_shm.size() >= 2 && edge.record_shm[0] == '/';
        for (std::size_t i = 1; ok && i < edge.record_shm.size(); ++i) {
          const unsigned char c =
              static_cast<unsigned char>(edge.record_shm[i]);
          ok = std::isalnum(c) != 0 || c == '_' || c == '-';
        }
        if (!ok) {
          LOAD_ERR("LoadGraphYaml: bad record_shm %s\n", edge.record_shm.c_str());
          return false;
        }
      }
      if (!edge.record && !edge.record_gate) {
        LOAD_ERR("LoadGraphYaml: record_gate=false requires record=true\n");
        return false;
      }
      if (!edge.record) continue;
      const std::string shm =
          edge.record_shm.empty() ? "/" + edge.channel : edge.record_shm;
      const auto found = by_channel.find(edge.channel);
      if (found == by_channel.end()) {
        by_channel.emplace(edge.channel, &edge);
      } else {
        const Edge& prev = *found->second;
        const std::string prev_shm =
            prev.record_shm.empty() ? "/" + prev.channel : prev.record_shm;
        if (prev.record_kind != edge.record_kind ||
            prev.record_gate != edge.record_gate ||
            prev.bitrate_kbps != edge.bitrate_kbps ||
            prev.avg_frame_bytes != edge.avg_frame_bytes || prev_shm != shm) {
          LOAD_ERR("LoadGraphYaml: channel %s mixes record kinds\n",
                   edge.channel.c_str());
          return false;
        }
        if (EdgeRecordsBulk(edge) &&
            (prev.delivery != "broadcast" || edge.delivery != "broadcast" ||
             prev.shm != edge.shm)) {
          LOAD_ERR("LoadGraphYaml: bulk channel %s has more than one edge\n",
                   edge.channel.c_str());
          return false;
        }
      }
      if (!EdgeRecordsBulk(edge)) continue;
      if (!bulk_shm.insert(shm).second && edge.delivery != "broadcast") {
        LOAD_ERR("LoadGraphYaml: duplicate record shm %s\n", shm.c_str());
        return false;
      }
      if (shm == out->shm_prefix + "_rec") {
        LOAD_ERR("LoadGraphYaml: record shm %s collides with the mirror "
                 "aggregate\n",
                 shm.c_str());
        return false;
      }
    }
  }
  // SCH-09: physical names and topology are fixed before any emitter runs.
  {
    std::set<std::string> names;
    for (const auto& n : out->nodes) {
      names.insert(n.id);
    }
    for (const auto& n : out->nodes) {
      if (n.threads == 1) {
        continue;
      }
      g_line = n.line > 0 ? n.line : 1;
      if (n.exec != "event") {
        LOAD_ERR("LoadGraphYaml: threads > 1 requires exec=event node %s\n",
                 n.id.c_str());
        return false;
      }
      std::size_t inputs = 0;
      std::size_t outputs = 0;
      for (const auto& e : out->edges) {
        if (e.to == n.id) {
          ++inputs;
        }
        if (e.from == n.id) {
          ++outputs;
        }
      }
      if (outputs != 0) {
        LOAD_ERR("LoadGraphYaml: sharded node %s must be a sink (no out-edges)\n",
                 n.id.c_str());
        return false;
      }
      if (inputs == 0 || n.threads > inputs) {
        LOAD_ERR("LoadGraphYaml: sharded node %s requires 1 <= threads <= in-edges (%zu)\n",
                 n.id.c_str(), inputs);
        return false;
      }
      for (uint32_t shard = 0; shard < n.threads; ++shard) {
        const std::string id = n.id + "__s" + std::to_string(shard);
        if (names.count(id) != 0) {
          LOAD_ERR("LoadGraphYaml: physical worker name collision: %s\n",
                   id.c_str());
          return false;
        }
        names.insert(id);
      }
    }
  }
  // Per-process worker ceiling counts physical shards, not logical nodes.
  {
    std::map<std::string, std::size_t> per_process;
    std::map<std::string, int> process_first_line;
    for (const auto& n : out->nodes) {
      per_process[n.process] += n.threads;
      process_first_line.emplace(n.process, n.line);
    }
    g_line = processes_line > 0 ? processes_line : 1;
    for (const auto& kv : per_process) {
      if (kv.second > thyme::kAbiMaxThreads) {
        g_line = process_first_line[kv.first] > 0 ? process_first_line[kv.first]
                                                  : g_line;
        LOAD_ERR(
            "LoadGraphYaml: process '%s' has %zu nodes, limit is %zu "
            "(kAbiMaxThreads)\n",
            kv.first.empty() ? "<default>" : kv.first.c_str(), kv.second,
            thyme::kAbiMaxThreads);
        return false;
      }
    }
  }
  return true;
}

bool LoadGraphYaml(const std::string& path, Graph* out) {
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "LoadGraphYaml: cannot open %s\n", path.c_str());
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return LoadGraphYamlFromString(ss.str(), out, path);
}
