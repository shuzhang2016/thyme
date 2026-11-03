#include "yaml.h"
#include "graph_json.h"
#include "graph_shard.h"
#include "dag.h"
#include "emit.h"
#include "meta.h"
#include "scaffold.h"
#include "rate.h"
#include "depth.h"
#include "derive_template.h"
#include "validate.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef GEN_GRAPH_TEMPLATE_DIR
#error "GEN_GRAPH_TEMPLATE_DIR must be defined"
#endif

namespace fs = std::filesystem;

namespace {

std::string Dirname(const std::string& p) {
  auto pos = p.find_last_of('/');
  if (pos == std::string::npos) return ".";
  if (pos == 0) return "/";
  return p.substr(0, pos);
}

bool ValidUpdate(const std::string& u) {
  return u == "generated" || u == "mp" || u == "nodes" || u == "app" ||
         u == "all";
}

// Removes the materialized base ∪ overlay template view on every exit path.
struct TplScratch {
  std::string path;
  ~TplScratch() {
    if (path.empty()) return;
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

// GEN-02: print one line per semantic violation; caller returns 2.
int ReportInvalid(const std::vector<std::string>& errors) {
  for (const std::string& e : errors) {
    std::fprintf(stderr, "gen_graph: %s\n", e.c_str());
  }
  return 2;
}

// RATE_WARN fires when declared node hz deviates from the derived frame rate
// by more than this relative fraction.
constexpr double kHzDeviationWarnThreshold = 0.20;
// Default rate of the injected health_reporter node (--health-hz overrides).
constexpr int kDefaultHealthHz = 10;

}  // namespace

int main(int argc, char** argv) {
  std::string in;
  std::string from_json;
  std::string out;
  std::string update = "generated";
  bool update_given = false;
  bool force = false;
  bool emit_json = false;
  bool emit_yaml = false;
  bool explain_rates = false;
  bool explain_sched = false;
  SchedOptions sched_options;
  bool derive_template = false;
  bool check = false;
  bool version = false;
  bool no_health_reporter = false;
  int health_hz = kDefaultHealthHz;
  std::string template_out;      // --template-out (empty -> <out>/template)
  std::string project_name;      // --project-name (empty -> no banner.txt)
  std::string contracts_include; // --contracts-include fallback
  std::string tpl_dir;  // empty -> GEN_GRAPH_TEMPLATE_DIR
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--in") == 0 && i + 1 < argc) {
      in = argv[++i];
    } else if (std::strcmp(argv[i], "--from-json") == 0 && i + 1 < argc) {
      from_json = argv[++i];
    } else if (std::strcmp(argv[i], "--emit-json") == 0) {
      emit_json = true;
    } else if (std::strcmp(argv[i], "--emit-yaml") == 0) {
      emit_yaml = true;
    } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out = argv[++i];
    } else if (std::strcmp(argv[i], "--update") == 0 && i + 1 < argc) {
      update = argv[++i];
      update_given = true;
    } else if (std::strcmp(argv[i], "--force") == 0) {
      force = true;
    } else if (std::strcmp(argv[i], "--explain-rates") == 0) {
      explain_rates = true;
    } else if (std::strcmp(argv[i], "--explain-sched") == 0) {
      explain_sched = true;
    } else if (std::strncmp(argv[i], "--sched-priority=", 17) == 0) {
      if (!ParseSchedPriorityMode(std::string(argv[i] + 17),
                                  &sched_options.priority_mode)) {
        std::fprintf(stderr,
                     "gen_graph: --sched-priority must be best-effort or "
                     "strict (got %s)\n",
                     argv[i] + 17);
        return 2;
      }
    } else if (std::strncmp(argv[i], "--sched-admission=", 18) == 0) {
      if (!ParseSchedAdmissionMode(std::string(argv[i] + 18),
                                   &sched_options.admission_mode)) {
        std::fprintf(stderr,
                     "gen_graph: --sched-admission must be error, warn or "
                     "off (got %s)\n",
                     argv[i] + 18);
        return 2;
      }
    } else if (std::strncmp(argv[i], "--sched-capacity=", 17) == 0) {
      if (!ParseSchedCapacityMode(std::string(argv[i] + 17),
                                  &sched_options.capacity_mode)) {
        std::fprintf(stderr,
                     "gen_graph: --sched-capacity must be error or "
                     "best_effort (got %s)\n",
                     argv[i] + 17);
        return 2;
      }
    } else if (std::strcmp(argv[i], "--check") == 0) {
      check = true;
    } else if (std::strcmp(argv[i], "--version") == 0) {
      version = true;
    } else if (std::strcmp(argv[i], "--derive-template") == 0) {
      derive_template = true;
    } else if (std::strcmp(argv[i], "--no-health-reporter") == 0) {
      no_health_reporter = true;
    } else if (std::strcmp(argv[i], "--health-hz") == 0 && i + 1 < argc) {
      health_hz = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--template-out") == 0 && i + 1 < argc) {
      template_out = argv[++i];
    } else if (std::strcmp(argv[i], "--project-name") == 0 && i + 1 < argc) {
      project_name = argv[++i];
    } else if (std::strcmp(argv[i], "--contracts-include") == 0 && i + 1 < argc) {
      contracts_include = argv[++i];
    } else if (std::strcmp(argv[i], "--template-dir") == 0 && i + 1 < argc) {
      tpl_dir = argv[++i];
    } else if (std::strcmp(argv[i], "--process") == 0) {
      std::fprintf(stderr, "gen_graph: flag not implemented: --process\n");
      if (i + 1 < argc) ++i;
      return 2;
    } else {
      std::fprintf(stderr, "gen_graph: unknown arg %s\n", argv[i]);
      return 2;
    }
  }
  // GEN-09: print the generator semantic version and the generated output
  // format version, then exit without reading or writing any graph. Both are
  // also stamped into generated/.gen_meta.json and the file banners.
  if (version) {
    std::printf("%s\n", gen_meta::VersionSummary().c_str());
    return 0;
  }
  // ECO-08: canonical JSON is an alternate graph input (`--from-json`) and a
  // canonical dump of a yaml graph (`--emit-json`). Both emit modes are
  // read-only and do not combine with generation/check/explain flags.
  if (emit_json && emit_yaml) {
    std::fprintf(stderr,
                 "gen_graph: --emit-json cannot be combined with "
                 "--emit-yaml\n");
    return 2;
  }
  if (!from_json.empty() && !in.empty()) {
    std::fprintf(stderr,
                 "gen_graph: --in and --from-json are mutually exclusive\n");
    return 2;
  }
  if (emit_json && !from_json.empty()) {
    std::fprintf(stderr,
                 "gen_graph: --emit-json reads yaml; use --in (not "
                 "--from-json)\n");
    return 2;
  }
  if (emit_yaml && from_json.empty()) {
    std::fprintf(stderr,
                 "gen_graph: --emit-yaml requires --from-json <path>\n");
    return 2;
  }
  if ((emit_json || emit_yaml) && update_given) {
    std::fprintf(stderr,
                 "gen_graph: --emit-json/--emit-yaml cannot be combined with "
                 "--update\n");
    return 2;
  }
  if ((emit_json || emit_yaml) &&
      (check || derive_template || explain_rates || explain_sched)) {
    std::fprintf(stderr,
                 "gen_graph: --emit-json/--emit-yaml cannot be combined with "
                 "--check, --derive-template, --explain-rates or "
                 "--explain-sched\n");
    return 2;
  }
  if (emit_json && in.empty()) {
    std::fprintf(stderr, "gen_graph: --emit-json requires --in <graph.yaml>\n");
    return 2;
  }
  if (derive_template && update_given) {
    std::fprintf(stderr,
                 "gen_graph: --derive-template cannot be combined with "
                 "--update\n");
    return 2;
  }
  if (derive_template && explain_rates) {
    std::fprintf(stderr,
                 "gen_graph: --derive-template cannot be combined with "
                 "--explain-rates\n");
    return 2;
  }
  if (check && derive_template) {
    std::fprintf(stderr,
                 "gen_graph: --check cannot be combined with "
                 "--derive-template\n");
    return 2;
  }
  if (check && explain_rates) {
    std::fprintf(stderr,
                 "gen_graph: --check cannot be combined with "
                 "--explain-rates\n");
    return 2;
  }
  if (explain_sched &&
      (derive_template || explain_rates || check)) {
    std::fprintf(stderr,
                 "gen_graph: --explain-sched cannot be combined with "
                 "--derive-template, --explain-rates or --check\n");
    return 2;
  }
  if (check && update_given && update != "generated") {
    // --check regenerates and diffs the full generated/ tree; the other
    // --update modes write disjoint/partial trees that a diff cannot judge.
    std::fprintf(stderr,
                 "gen_graph: --check only applies to the generated tree "
                 "(--update generated)\n");
    return 2;
  }
  if (!ValidUpdate(update)) {
    std::fprintf(stderr, "gen_graph: --update %s not a valid value\n",
                 update.c_str());
    return 2;
  }
  if (health_hz <= 0) {
    std::fprintf(stderr, "gen_graph: --health-hz must be > 0 (got %d)\n",
                 health_hz);
    return 2;
  }

  // Bootstrap a missing graph.yaml from the template view. `out` must be known
  // before merging (the merged view is materialized under out/).
  bool bootstrap = false;
  if (in.empty() && from_json.empty()) {
    if (update != "all") {
      std::fprintf(stderr, "gen_graph: --in is required\n");
      return 2;
    }
    if (out.empty()) {
      std::fprintf(stderr, "gen_graph: --out is required when --in is omitted\n");
      return 2;
    }
    in = out + "/graph.yaml";
    bootstrap = true;
  }
  // --from-json replaces --in as the graph source; reuse its path only to
  // default --out (JSON sits next to the app root in the round-trip workflow).
  if (in.empty()) in = from_json;
  if (out.empty()) out = Dirname(in);

  // Effective template view: built-in scaffold base, overlaid by
  // --template-dir when given (same relative path: overlay shadows base;
  // overlay may add files). Materialized under <out>/.gen_graph_tpl so the
  // rest of the pipeline reads a single merged directory.
  std::string tpl = GEN_GRAPH_TEMPLATE_DIR;
  TplScratch scratch;
  if (!tpl_dir.empty()) {
    if (!fs::is_directory(tpl_dir)) {
      std::fprintf(stderr, "gen_graph: --template-dir %s is not a directory\n",
                   tpl_dir.c_str());
      return 2;
    }
    const fs::path merged = fs::path(out) / ".gen_graph_tpl";
    std::error_code ec;
    fs::remove_all(merged, ec);
    if (!MergeTemplateDir(GEN_GRAPH_TEMPLATE_DIR, tpl_dir, merged.string())) {
      std::fprintf(stderr,
                   "gen_graph: cannot merge --template-dir %s over the scaffold base\n",
                   tpl_dir.c_str());
      return 2;
    }
    scratch.path = merged.string();
    tpl = merged.string();
  }

  if (bootstrap && !fs::exists(in)) {
    std::error_code ec;
    fs::create_directories(out, ec);
    if (ec) {
      std::fprintf(stderr, "gen_graph: cannot write %s\n", in.c_str());
      return 2;
    }
    const fs::path src = fs::path(tpl) / "graph.yaml";
    std::ifstream s(src, std::ios::binary);
    if (!s) {
      std::fprintf(stderr, "gen_graph: cannot open template %s\n", src.c_str());
      return 2;
    }
    std::ofstream d(in, std::ios::binary);
    if (!d) {
      std::fprintf(stderr, "gen_graph: cannot write %s\n", in.c_str());
      return 2;
    }
    d << s.rdbuf();
  }

  Graph g;
  if (!from_json.empty()) {
    if (!LoadGraphJson(from_json, &g)) return 2;
  } else if (!LoadGraphYaml(in, &g)) {
    return 2;
  }

  // GEN-11: the graph must be a DAG. Reject any remaining directed cycle
  // (feedback:true + delay edges are cut) before any output is written, so a
  // cyclic graph never leaves half-generated trees behind.
  {
    std::vector<std::string> cycle;
    int closing_edge_line = 0;
    if (FindCycle(g, &cycle, &closing_edge_line)) {
      std::string path;
      for (std::size_t i = 0; i < cycle.size(); ++i) {
        if (i > 0) path += " -> ";
        path += cycle[i];
      }
      if (closing_edge_line > 0) {
        std::fprintf(stderr, "gen_graph: %s:%d: directed cycle rejected: %s\n",
                     g.source.c_str(), closing_edge_line, path.c_str());
      } else {
        std::fprintf(stderr, "gen_graph: directed cycle rejected: %s\n",
                     path.c_str());
      }
      std::fprintf(stderr,
                   "gen_graph: mark the loop-closing edge with `feedback: true` "
                   "and an explicit `delay` (> 0 ns) to allow a feedback loop\n");
      return 2;
    }
  }

  // GEN-02: semantic validation runs after load (+ the GEN-11 cycle check),
  // before any emit. The --derive-template / --explain-rates / --explain-sched
  // paths reflect graph.yaml as-written, so they validate without the template
  // catalog (the catalog-dependent shm-kind rule runs on the codegen path below).
  if (derive_template || explain_rates || explain_sched || emit_json ||
      emit_yaml) {
    std::vector<std::string> errors;
    if (!ValidateGraphSemantics(g, nullptr, &errors)) {
      return ReportInvalid(errors);
    }
  }

  if (emit_json) {
    const std::string json = GraphToCanonicalJson(g);
    std::fwrite(json.data(), 1, json.size(), stdout);
    return 0;
  }
  if (emit_yaml) {
    const std::string yaml = GraphToCanonicalYaml(g);
    std::fwrite(yaml.data(), 1, yaml.size(), stdout);
    return 0;
  }

  if (derive_template) {
    DeriveOptions dopt;
    dopt.template_out = template_out.empty() ? (out + "/template") : template_out;
    dopt.project_name = project_name;
    dopt.contracts_include = contracts_include;
    dopt.force = force;
    if (!DeriveTemplate(g, dopt)) return 2;
    return 0;
  }

  if (explain_sched) {
    Graph physical;
    std::string shard_error;
    if (!ProjectGraphShards(g, &physical, &shard_error)) {
      std::fprintf(stderr, "gen_graph: %s\n", shard_error.c_str());
      return 2;
    }
    SchedPlan plan;
    if (!DeriveSchedPlan(physical, sched_options, &plan)) {
      std::fprintf(stderr, "gen_graph: --explain-sched could not build a plan\n");
      return 2;
    }
    ApplyPhaseMode(&physical, &plan);
    const std::string json = RenderSchedJson(plan);
    const std::string human = RenderSchedHuman(plan);
    std::fwrite(json.data(), 1, json.size(), stdout);
    std::fwrite(human.data(), 1, human.size(), stderr);
    return SchedExitCode(plan);
  }

  if (explain_rates) {
    RateInfo info;
    std::vector<std::string> warnings;
    if (!DeriveRates(g, &info, &warnings)) return 2;
    WarnHzDeviation(g, info, kHzDeviationWarnThreshold, &warnings);
    WarnMixedEventRates(g, info, &warnings);
    WarnDepthBelowSuggest(g, info, &warnings);
    ExplainRatesJson(info, warnings);
    return 0;
  }

  // Default aggregator: unless the graph already declares one (or
  // --no-health-reporter is given), append a synthetic health_reporter node.
  // It is a "noworker" aggregator: it owns no data edges, borrows the worker
  // health slots, and must be the last node in yaml order (enforced by
  // graph_check.h). The python datatype generator (datatypes.json) never emits
  // this node, so this injection is the single source of the default
  // aggregator — it applies only to the codegen/scaffold path, never to
  // --derive-template / --explain-rates (which reflect graph.yaml as-written).
  bool injected_health_reporter = false;
  if (!no_health_reporter) {
    bool have = false;
    for (const auto& n : g.nodes) {
      if (n.type == "health_reporter") { have = true; break; }
    }
    if (!have) {
      Node hr;
      hr.id = "health_reporter";
      hr.type = "health_reporter";
      hr.process = "main";
      hr.hz = health_hz;
      g.nodes.push_back(hr);
      injected_health_reporter = true;
    }
  }
  {
    Graph physical;
    std::string shard_error;
    if (!ProjectGraphShards(g, &physical, &shard_error)) {
      std::fprintf(stderr, "gen_graph: %s\n", shard_error.c_str());
      return 2;
    }
    g = std::move(physical);
  }
  if (injected_health_reporter &&
      g.health_n < static_cast<int>(g.nodes.size())) {
    g.health_n = static_cast<int>(g.nodes.size());
  }
  if (injected_health_reporter) {
    std::fprintf(stderr,
                 "gen_graph: injected default health_reporter (hz=%d, "
                 "health_n=%d); disable with --no-health-reporter\n",
                 health_hz, g.health_n);
  }

  // GEN-02: the codegen/scaffold path validates the final graph (after the
  // default-aggregator injection) against the effective template catalog —
  // this is where the shm-kind rule applies. Runs before rate derivation and
  // any emit so an invalid graph never reaches code that assumes sane
  // topology, and never leaves partial output behind.
  {
    TemplateCatalog cat;
    if (!LoadCatalog(tpl, &cat)) return 2;
    std::vector<std::string> errors;
    if (!ValidateGraphSemantics(g, &cat, &errors)) {
      return ReportInvalid(errors);
    }
  }

  {
    RateInfo info;
    std::vector<std::string> warnings;
    if (DeriveRates(g, &info, &warnings)) {
      WarnHzDeviation(g, info, kHzDeviationWarnThreshold, &warnings);
      WarnMixedEventRates(g, info, &warnings);
      WarnDepthBelowSuggest(g, info, &warnings);
      PrintRateWarnings(warnings);
      for (const std::string& note : info.loop_cuts) {
        std::fprintf(stderr, "gen_graph: %s\n", note.c_str());
      }
    }
  }

  // PERF-02/SCH-02/SCH-05: derive and apply the scheduling plan before any
  // emit. Admission errors stop generation (no partial tree); warnings
  // continue. --check also fails on the same diagnostics.
  SchedPlan sched_plan;
  if (!DeriveSchedPlan(g, sched_options, &sched_plan)) {
    std::fprintf(stderr, "gen_graph: could not derive the scheduling plan\n");
    return 2;
  }
  ApplySchedPlan(&g, sched_plan);
  ApplyPhaseMode(&g, &sched_plan);
  PrintSchedDiagnostics(sched_plan);
  if (SchedExitCode(sched_plan) != 0) return 2;

  // GEN-05: --check regenerates generated/ into a staging dir and diffs it
  // against the checked-in tree (catches hand-edited or stale generated
  // files). Exit 1 with one line per differing file; nothing is written
  // outside the staging dir.
  if (check) {
    CheckReport report;
    if (!CheckGenerated(out, g, tpl, tpl_dir, &report, &sched_plan)) return 2;
    if (!report.meta_errors.empty() || !report.diffs.empty()) {
      for (const std::string& e : report.meta_errors) {
        std::fprintf(stderr, "gen_graph --check: %s\n", e.c_str());
      }
      for (const std::string& d : report.diffs) {
        std::fprintf(stderr, "gen_graph --check: generated/%s\n", d.c_str());
      }
      std::fprintf(stderr,
                   "gen_graph --check: %zu meta error(s), %zu file(s) differ; "
                   "regenerate with the same gen_graph invocation minus "
                   "--check\n",
                   report.meta_errors.size(), report.diffs.size());
      return 1;
    }
    std::printf("gen_graph --check: generated/ is up to date (%s)\n",
                gen_meta::VersionSummary().c_str());
    return 0;
  }

  if (update == "generated" || update == "all") {
    if (!EmitGenerated(out, g, tpl, tpl_dir, &sched_plan)) return 2;
  } else if (update == "mp") {
    if (!EmitGraphIrOnly(out, g, tpl)) return 2;
  }

  if (update == "nodes" || update == "app" || update == "all") {
    if (!Scaffold(out, g, update, force, tpl)) return 2;
  }

  std::printf("nodes=%zu pool_n=%d ring_cap=%d\n", g.nodes.size(), g.pool_n,
              g.ring_cap);
  return 0;
}
