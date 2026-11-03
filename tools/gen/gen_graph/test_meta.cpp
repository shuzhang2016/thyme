// GEN-09: generated-tree metadata unit test.
//
// Covers the parts that must not depend on the filesystem or the binary:
//  * the source-graph hash is stable under diagnostic-only fields (yaml path,
//    line numbers) and changes with any semantic edit;
//  * .gen_meta.json renders in a stable field order and round-trips;
//  * the migration policy classifies a same-version tree as compatible and a
//    different format/version as a migration error.
#include "meta.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  if (ok) return;
  std::fprintf(stderr, "test_gen_graph_meta: FAIL: %s\n", what);
  ++failures;
}

Graph SampleGraph() {
  Graph g;
  g.source = "graph.yaml";
  g.version = 1;
  g.pool_n = 4;
  g.ring_cap = 8;
  g.shm_prefix = "/c9_%pid";
  g.health_n = 2;

  Node prod;
  prod.id = "prod";
  prod.type = "producer";
  prod.process = "main";
  prod.hz = 10;
  prod.line = 5;
  Node cons;
  cons.id = "cons";
  cons.type = "consumer";
  cons.process = "main";
  cons.hz = 10;
  cons.line = 9;
  g.nodes.push_back(prod);
  g.nodes.push_back(cons);

  Edge e;
  e.from = "prod";
  e.to = "cons";
  e.channel = "payload";
  e.cpp_type = "Foo";
  e.pool = "FooPool";
  e.transport = "spsc";
  e.line = 13;
  g.edges.push_back(e);

  Process p;
  p.id = "main";
  p.role = "main";
  p.line = 1;
  g.processes.push_back(p);
  return g;
}

}  // namespace

int main() {
  const Graph base = SampleGraph();

  // Diagnostic-only fields do not participate in the hash: moving the yaml,
  // renaming the source, or shifting line numbers keeps the identity.
  Graph same = base;
  same.source = "/somewhere/else/graph.yaml";
  same.nodes[0].line = 100;
  same.nodes[1].line = 200;
  same.edges[0].line = 300;
  same.processes[0].line = 400;
  Check(gen_meta::SourceGraphHash(base) == gen_meta::SourceGraphHash(same),
        "hash must ignore source path and yaml line numbers");

  // 16 lowercase hex characters (FNV-1a 64).
  const std::string hash = gen_meta::SourceGraphHash(base);
  Check(hash.size() == 16, "hash must be 16 hex characters");
  for (const char c : hash) {
    Check((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'),
          "hash must be lowercase hex");
  }

  // Every semantic field participates.
  {
    Graph g = base;
    g.pool_n = 5;
    Check(gen_meta::SourceGraphHash(g) != hash, "pool_n must change the hash");
  }
  {
    Graph g = base;
    g.nodes[1].hz = 20;
    Check(gen_meta::SourceGraphHash(g) != hash, "node hz must change the hash");
  }
  {
    Graph g = base;
    g.edges[0].record = false;
    Check(gen_meta::SourceGraphHash(g) != hash,
          "edge record flag must change the hash");
  }
  {
    Graph g = base;
    g.edges[0].delay_ns = 7;
    Check(gen_meta::SourceGraphHash(g) != hash,
          "edge delay must change the hash");
  }

  // render -> parse round-trip, stable key order, current versions stamped.
  const gen_meta::Meta meta = gen_meta::ForGraph(base);
  Check(meta.tool == gen_meta::kTool, "meta tool");
  Check(meta.generator_version == gen_meta::kGeneratorVersion,
        "meta generator version");
  Check(meta.format_version == gen_meta::kFormatVersion, "meta format version");
  Check(meta.source_graph_hash == hash, "meta graph hash");

  const std::string text = gen_meta::Render(meta);
  Check(text == gen_meta::Render(meta), "render must be deterministic");
  Check(text.find("\"tool\": \"gen_graph\"") != std::string::npos,
        "render must name the tool");
  Check(text.find("\"generator_version\": \"3.11.1\"") != std::string::npos,
        "render must carry the generator version");
  Check(text.find("\"format_version\": 3") != std::string::npos,
        "render must carry the output format version");
  Check(text.find("\"source_graph_hash\": \"" + hash + "\"") !=
            std::string::npos,
        "render must carry the source graph hash");

  gen_meta::Meta parsed;
  Check(gen_meta::Parse(text, &parsed), "render must parse back");
  Check(parsed.tool == meta.tool, "round-trip tool");
  Check(parsed.generator_version == meta.generator_version,
        "round-trip generator version");
  Check(parsed.format_version == meta.format_version,
        "round-trip format version");
  Check(parsed.source_graph_hash == meta.source_graph_hash,
        "round-trip graph hash");

  Check(!gen_meta::Parse("{\"tool\": \"gen_graph\"}", &parsed),
        "partial meta must not parse");
  Check(!gen_meta::Parse("", &parsed), "empty meta must not parse");

  // Migration policy: the current tree is compatible; format and generator
  // drift are both reported (with the actionable command in the message).
  Check(gen_meta::CompatibilityError(meta).empty(),
        "current meta must be compatible");

  gen_meta::Meta old_format = meta;
  old_format.format_version = gen_meta::kFormatVersion - 1;
  {
    const std::string err = gen_meta::CompatibilityError(old_format);
    Check(!err.empty(), "old output format must be rejected");
    Check(err.find("output format") != std::string::npos,
          "format error must name the output format");
    Check(err.find("--update generated") != std::string::npos,
          "format error must name the migration command");
  }

  gen_meta::Meta other_tool = meta;
  other_tool.tool = "other_gen";
  Check(!gen_meta::CompatibilityError(other_tool).empty(),
        "foreign tool must be rejected");

  gen_meta::Meta other_generator = meta;
  other_generator.generator_version = "0.0.0";
  {
    const std::string err = gen_meta::CompatibilityError(other_generator);
    Check(!err.empty(), "different generator version must be rejected");
    Check(err.find("0.0.0") != std::string::npos,
          "generator drift error must name the tree's version");
    Check(err.find("--update generated") != std::string::npos,
          "generator drift error must name the migration command");
  }

  const std::string summary = gen_meta::VersionSummary();
  Check(summary.find(gen_meta::kGeneratorVersion) != std::string::npos,
        "version summary must contain the generator version");
  Check(summary.find("output format") != std::string::npos,
        "version summary must contain the output format");
  Check(gen_meta::BannerStamp().find(summary) != std::string::npos,
        "banner stamp must contain the version summary");

  if (failures != 0) {
    std::fprintf(stderr, "test_gen_graph_meta: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_gen_graph_meta: OK\n");
  return 0;
}
