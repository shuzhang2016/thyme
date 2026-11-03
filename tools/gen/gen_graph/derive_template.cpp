#include "derive_template.h"

#include "emit_internal.h"
#include "util.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Pool names strip this suffix before snake_casing ("ImagePool" -> "image").
constexpr std::string_view kPoolSuffix = "Pool";

// Full snake_case prefix (decision #1): strip trailing "Pool", insert '_'
// between a lowercase-or-digit char and an uppercase char, lowercase all.
std::string Snake(const std::string& pool) {
  std::string base = pool;
  if (base.size() >= kPoolSuffix.size() &&
      base.compare(base.size() - kPoolSuffix.size(), kPoolSuffix.size(),
                   kPoolSuffix) == 0) {
    base.erase(base.size() - kPoolSuffix.size());
  }
  std::string out;
  for (std::size_t i = 0; i < base.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(base[i]);
    if (std::isupper(c)) {
      if (i > 0) {
        const unsigned char p = static_cast<unsigned char>(base[i - 1]);
        if (std::islower(p) || std::isdigit(p)) out += '_';
      }
      out += static_cast<char>(std::tolower(c));
    } else {
      out += static_cast<char>(std::tolower(c));
    }
  }
  return out;
}

const Node* FindNode(const Graph& g, const std::string& id) {
  for (const auto& n : g.nodes) if (n.id == id) return &n;
  return nullptr;
}

struct PoolInfo {
  std::string type;      // pool name as written in graph.yaml
  std::string prefix;    // derived snake prefix
  std::string cpp_type;  // from the first edge using the pool
  bool index_pipe = false;
};

const PoolInfo* FindPool(const std::vector<PoolInfo>& pools,
                         const std::string& type) {
  for (const auto& p : pools) if (p.type == type) return &p;
  return nullptr;
}

bool ReadTrimmed(const fs::path& p, std::string* out) {
  std::string body;
  if (!gen_util::ReadFile(p, &body)) return false;
  *out = gen_util::Trim(body);
  return true;
}

// Resolution order: <template_out>/contracts_include.txt → fallback flag →
// "contracts/types.h".
std::string ResolveContractsInclude(const std::string& template_out,
                                    const std::string& fallback) {
  const fs::path p = fs::path(template_out) / "contracts_include.txt";
  std::string body;
  if (ReadTrimmed(p, &body) && !body.empty()) return body;
  if (!fallback.empty()) return fallback;
  return "contracts/types.h";
}

// Write `name` under `dir` unless it exists and !force (then keep). Reports
// `wrote <path>` / `kept <path>` to stdout; errors to stderr.
bool EmitFile(const fs::path& dir, const std::string& name,
              const std::string& content, bool force) {
  const fs::path p = dir / name;
  std::error_code ec;
  if (fs::exists(p, ec) && !force) {
    std::printf("kept %s\n", p.c_str());
    return true;
  }
  std::ofstream out(p, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "gen_graph: cannot write %s\n", p.c_str());
    return false;
  }
  out << content;
  if (!out) {
    std::fprintf(stderr, "gen_graph: cannot write %s\n", p.c_str());
    return false;
  }
  std::printf("wrote %s\n", p.c_str());
  return true;
}

// Ctor tokens for a known cyclic type: in-edge pools then out-edge pools
// (yaml order). Local pools use one LocalIndexQueue token; cross-process
// pools keep the raw pool + IndexPipe pair.
std::string CtorTokens(const Graph& g, const Node& n,
                       const std::vector<PoolInfo>& pools) {
  std::vector<std::string> toks;
  auto add_edge = [&](const Edge& e) {
    if (e.pool.empty()) return;
    const PoolInfo* p = FindPool(pools, e.pool);
    if (p == nullptr) return;
    if (p->index_pipe) {
      toks.push_back(p->prefix + "_pool");
      toks.push_back(p->prefix + "_pipe");
    } else {
      toks.push_back(p->prefix + "_queue");
    }
  };
  for (const auto& e : g.edges) if (e.to == n.id) add_edge(e);
  for (const auto& e : g.edges) if (e.from == n.id) add_edge(e);
  toks.push_back("{health}");
  std::string s;
  for (std::size_t i = 0; i < toks.size(); ++i) {
    if (i) s += ", ";
    s += toks[i];
  }
  return s;
}

}  // namespace

bool DeriveTemplate(const Graph& g, const DeriveOptions& opt) {
  std::error_code ec;
  fs::create_directories(opt.template_out, ec);
  if (ec) {
    std::fprintf(stderr, "gen_graph: cannot create %s\n",
                 opt.template_out.c_str());
    return false;
  }

  const std::string contracts = ResolveContractsInclude(opt.template_out,
                                                        opt.contracts_include);

  // Build the pool table in first-seen edge order: prefix (snake), cpp_type
  // (first occurrence wins), index_pipe (any cross-process edge using it).
  std::vector<PoolInfo> pools;
  std::set<std::string> seen_prefixes;
  for (const auto& e : g.edges) {
    if (e.pool.empty()) continue;  // pool-less event in-edges
    const Node* from = FindNode(g, e.from);
    const Node* to = FindNode(g, e.to);
    const bool cross = from != nullptr && to != nullptr &&
                       from->process != to->process;
    PoolInfo* p = nullptr;
    for (auto& q : pools) if (q.type == e.pool) p = &q;
    if (p == nullptr) {
      PoolInfo pi;
      pi.type = e.pool;
      pi.prefix = Snake(e.pool);
      pi.cpp_type = e.cpp_type;
      pi.index_pipe = cross;
      if (!gen_util::IsIdent(pi.prefix)) {
        std::fprintf(stderr,
                     "gen_graph: derived prefix '%s' for pool %s is not an "
                     "identifier\n",
                     pi.prefix.c_str(), e.pool.c_str());
        return false;
      }
      if (!seen_prefixes.insert(pi.prefix).second) {
        std::fprintf(stderr,
                     "gen_graph: derived prefix '%s' collides across pools\n",
                     pi.prefix.c_str());
        return false;
      }
      pools.push_back(std::move(pi));
    } else {
      if (p->cpp_type != e.cpp_type) {
        std::fprintf(stderr,
                     "gen_graph: pool %s maps to two cpp_types (%s and %s)\n",
                     e.pool.c_str(), p->cpp_type.c_str(), e.cpp_type.c_str());
        return false;
      }
      if (cross) p->index_pipe = true;
    }
  }

  // pools.txt: type|prefix|index_pipe, one row per unique pool (first-seen
  // edge order, pool-less edges skipped).
  {
    std::string body =
        "# pool|prefix|index_pipe(0/1) — derived from graph edges, first-seen "
        "order\n";
    for (const auto& p : pools) {
      body += p.type + "|" + p.prefix + "|" + (p.index_pipe ? "1" : "0") + "\n";
    }
    if (!EmitFile(opt.template_out, "pools.txt", body, opt.force)) return false;
  }

  // contracts.txt: from_type|channel|1 then to_type|channel|0 per edge (all
  // edges incl. notify), dedupe identical rows keeping first occurrence.
  {
    std::string body;
    std::set<std::string> seen;
    for (const auto& e : g.edges) {
      const Node* from = FindNode(g, e.from);
      const Node* to = FindNode(g, e.to);
      const std::string from_type = from ? from->type : "";
      const std::string to_type = to ? to->type : "";
      if (seen.insert(from_type + "|" + e.channel + "|1").second) {
        body += from_type + "|" + e.channel + "|1\n";
      }
      if (seen.insert(to_type + "|" + e.channel + "|0").second) {
        body += to_type + "|" + e.channel + "|0\n";
      }
    }
    if (!EmitFile(opt.template_out, "contracts.txt", body, opt.force))
      return false;
  }

  // known_types.txt: one row per cyclic type (first cyclic node of that type
  // as representative). exec=event types are skipped entirely; health_reporter
  // gets its static noworker row appended if present.
  {
    std::vector<std::string> type_order;
    std::map<std::string, const Node*> rep;
    bool has_health_reporter = false;
    for (const auto& n : g.nodes) {
      if (n.type == "health_reporter") { has_health_reporter = true; continue; }
      if (n.exec == "event") continue;
      if (rep.find(n.type) == rep.end()) {
        rep[n.type] = &n;
        type_order.push_back(n.type);
      }
    }
    // Guard: two cyclic nodes of one type must share a pooled-edge signature.
    for (const auto& t : type_order) {
      const std::string sig = CtorTokens(g, *rep[t], pools);
      for (const auto& n : g.nodes) {
        if (n.type != t || n.exec == "event" || &n == rep[t]) continue;
        if (CtorTokens(g, n, pools) != sig) {
          std::fprintf(stderr,
                       "gen_graph: nodes of type %s have different pooled edge "
                       "signatures (cannot derive one known_types row)\n",
                       t.c_str());
          return false;
        }
      }
    }
    std::string body;
    for (const auto& t : type_order) {
      const std::string cls = gen_util::PascalCase(t) + "Thread";
      body += t + "|" + cls + "|" + t + "|" + CtorTokens(g, *rep[t], pools) +
              "|\n";
    }
    if (has_health_reporter) {
      body += "health_reporter|HealthReporterThread|reporter|HEALTH|noworker\n";
    }
    if (!EmitFile(opt.template_out, "known_types.txt", body, opt.force))
      return false;
  }

  // channel_defaults.txt: channel|cpp_type|pool per pooled edge (editor-facing;
  // gen_graph never reads it). Pool-less event edges skipped.
  {
    std::string body =
        "# channel|cpp_type|pool — editor-facing defaults (gen_graph does not "
        "read this file)\n";
    for (const auto& e : g.edges) {
      if (e.pool.empty()) continue;
      body += e.channel + "|" + e.cpp_type + "|" + e.pool + "\n";
    }
    if (!EmitFile(opt.template_out, "channel_defaults.txt", body, opt.force))
      return false;
  }

  // shm_kinds.txt: cpp_type|cpp_type (one row per unique cpp_type on shm
  // edges). Emitted only when at least one shm edge exists.
  {
    std::string body;
    std::set<std::string> seen;
    bool any_shm = false;
    for (const auto& e : g.edges) {
      if (e.shm.empty()) continue;
      any_shm = true;
      if (seen.insert(e.cpp_type).second) {
        body += e.cpp_type + "|" + e.cpp_type + "\n";
      }
    }
    if (any_shm &&
        !EmitFile(opt.template_out, "shm_kinds.txt", body, opt.force))
      return false;
  }

  // graph_types.h.inc: first line carries DO NOT EDIT so the inc owns the
  // header (no banner prepend at emit time).
  {
    const std::string banner =
        opt.project_name.empty() ? "generated" : opt.project_name;
    std::string body;
    body += "// Code generated for the " + banner + " graph. DO NOT EDIT.\n";
    body += "// Hand logic belongs in src/nodes/.\n";
    body += "#pragma once\n";
    body += "#include \"" + contracts + "\"\n";
    body += "#include \"support/pipeline_io.h\"\n";
    body += "#include \"thyme/runtime/object_pool.h\"\n";
    body += "#include \"thyme/runtime/spsc_ring.h\"\n";
    body += "\n";
    body += "inline constexpr std::size_t kRingCap = {{RING_CAP}};\n";
    body += "inline constexpr std::size_t kPoolN = {{POOL_N}};\n";
    body += "inline constexpr uint64_t kSessionId = 1;\n";
    body += "\n";
    for (const auto& p : pools) {
      body += "using " + p.type + " = thyme::ObjectPool<" +
              gen_emit::Framed(p.cpp_type) + ", kPoolN>;\n";
    }
    body += "\n";
    body += "template <typename PoolT>\n";
    body += "using Ring = thyme::SpscRing<kRingCap>;\n";
    if (!EmitFile(opt.template_out, "graph_types.h.inc", body, opt.force))
      return false;
  }

  // mp_includes.inc: three include lines.
  {
    std::string body;
    body += "#include \"" + contracts + "\"\n";
    body += "#include \"support/index_pipe.h\"\n";
    body += "#include \"support/pipeline_io.h\"\n";
    if (!EmitFile(opt.template_out, "mp_includes.inc", body, opt.force))
      return false;
  }

  // banner.txt: only when --project-name given.
  if (!opt.project_name.empty() &&
      !EmitFile(opt.template_out, "banner.txt", opt.project_name + "\n",
                opt.force))
    return false;

  return true;
}
