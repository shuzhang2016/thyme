#include "emit.h"

#include "emit_internal.h"
#include "meta.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace gen_emit;

namespace {

// GEN-09: template .inc bodies may own their first line ("DO NOT EDIT").
// Insert the generator version stamp right after that line so the emitted
// file still identifies the generator + output format. Already-stamped bodies
// pass through unchanged (idempotent, and overlays stay byte-stable).
std::string StampIncBody(std::string body) {
  if (body.find("output format") != std::string::npos) return body;
  const std::size_t nl = body.find('\n');
  if (nl == std::string::npos) return body;
  if (body.substr(0, nl).find("DO NOT EDIT") == std::string::npos) return body;
  return body.substr(0, nl + 1) + gen_meta::BannerStamp() + body.substr(nl + 1);
}

// Shared preamble for EmitGenerated / CheckGenerated: catalog, shm_prefix
// gate, pool/event validation, and the single edge-manifest walk.
bool PrepareGenerated(const Graph& g, const std::string& tpl_dir,
                      const char* ctx, TemplateCatalog* cat,
                      EdgeManifest* manifest) {
  if (!LoadCatalog(tpl_dir, cat)) return false;
  if (g.shm_prefix != "/c9_%pid") {
    std::fprintf(stderr, "%s: unsupported shm_prefix %s (only /c9_%%pid)\n",
                 ctx, g.shm_prefix.c_str());
    return false;
  }
  if (!ValidateGraphPools(g, *cat)) return false;
  if (!ValidateEventNodes(g, *cat)) return false;
  if (!BuildEdgeManifest(g, *cat, manifest)) return false;
  return true;
}

// Fills tmp (an existing, empty directory) with the full generated/ tree.
bool FillGeneratedTmp(const fs::path& tmp, const Graph& g,
                      const std::string& tpl_dir,
                      const std::string& tpl_display,
                      const TemplateCatalog& cat, const EdgeManifest& manifest,
                      const SchedPlan* sched_plan) {
  const fs::path tpl(tpl_dir);
  // Codegen always emits the plan table. Callers pass the same plan they used
  // to apply priorities; the fallback keeps direct emit calls working.
  SchedPlan local_plan;
  if (sched_plan == nullptr) {
    SchedOptions options;
    if (!DeriveSchedPlan(g, options, &local_plan)) return false;
    sched_plan = &local_plan;
  }

  auto subst = [&](std::string s) {
    s = ReplaceAll(s, "{{POOL_N}}", std::to_string(g.pool_n));
    s = ReplaceAll(s, "{{RING_CAP}}", std::to_string(g.ring_cap));
    std::string ids;
    for (const auto& n : g.nodes) {
      if (!ids.empty()) ids += " ";
      ids += n.id;
    }
    s = ReplaceAll(s, "{{NODE_IDS}}", ids);
    return s;
  };

  // graph_types.h: if the inc's first line already carries DO NOT EDIT, the
  // inc owns the header (dummy golden stays byte-identical apart from the
  // GEN-09 version stamp inserted after it). Otherwise the engine prepends
  // the banner header.
  std::string body;
  if (!ReadFile(tpl / "graph_types.h.inc", &body)) return false;
  {
    const std::size_t nl = body.find('\n');
    const std::string first_line = (nl == std::string::npos) ? body : body.substr(0, nl);
    std::string out_body = subst(body);
    if (first_line.find("DO NOT EDIT") == std::string::npos) {
      out_body = BannerHeader(cat) + out_body;
    } else {
      out_body = StampIncBody(out_body);
    }
    if (!WriteFile(tmp / "graph_types.h", out_body)) return false;
  }
  if (!ReadFile(tpl / "nodes.cmake.inc", &body)) return false;
  if (!WriteFile(tmp / "nodes.cmake", subst(body))) return false;
  if (!EmitWiringH(tmp.string(), g, tpl_dir)) return false;
  if (!EmitWiringCpp(tmp.string(), g, tpl_dir)) return false;
  TemplateCatalog cat_sched;
  if (!LoadCatalog(tpl_dir, &cat_sched)) return false;
  if (UsesSchedulingSetup(tpl_dir)) {
    if (!EmitSchedulingApplyInc(tmp.string(), g, cat_sched)) return false;
  }
  // scheduling_setup.h (scaffold support) consumes the generated ring
  // aggregate, so emit it for every graph.
  if (!EmitSchedulingRings(tmp.string(), g, cat_sched)) return false;

  // Per-type projection header. Known cyclic nodes are codegen'd from
  // known_types.txt + graph edges; noworker types still use .inc templates.
  for (const auto& n : g.nodes) {
    const fs::path out_name = std::string(n.id) + "_generated.h";
    if (IsKnownType(cat, n.type) && !IsEventNode(n)) {
      const KnownCat* k = FindKnown(cat, n.type);
      if (k == nullptr) {
        const std::string loc = GraphLoc(g, n.line);
        std::fprintf(stderr,
                     "gen_graph: %sknown cyclic type '%s' missing catalog row\n",
                     loc.c_str(), n.type.c_str());
        return false;
      }
      if (k->flags.find("noworker") != std::string::npos) {
        if (!HasInc(tpl_dir, n.type)) {
          const std::string loc = GraphLoc(g, n.line);
          std::fprintf(stderr,
                       "gen_graph: %snoworker type '%s' requires %s_generated.h.inc\n",
                       loc.c_str(), n.type.c_str(), n.type.c_str());
          return false;
        }
        std::string inc;
        if (!ReadFile(tpl / (n.type + "_generated.h.inc"), &inc)) return false;
        inc = ReplaceAll(inc, "{{ID}}", n.id);
        inc = ReplaceAll(inc, "{{HZ}}", std::to_string(n.hz));
        inc = StampIncBody(std::move(inc));
        if (!WriteFile(tmp / out_name, inc)) return false;
      } else if (!EmitKnownCyclicGenerated(tmp / out_name, g, n, cat)) {
        return false;
      }
    } else if (IsKnownType(cat, n.type) && IsEventNode(n)) {
      if (!EmitKnownEventGenerated(tmp / out_name, g, n, cat)) return false;
    } else if (IsEventNode(n)) {
      if (!EmitEventGenerated(tmp / out_name, g, n, cat)) return false;
    } else {
      if (!EmitUnknownGenerated(tmp / out_name, g, n, cat)) return false;
    }
  }

  if (!EmitGraphIr(tmp.string(), g, tpl_dir, manifest)) return false;
  if (!EmitChannelCapabilities(tmp.string(), g, cat, manifest)) return false;
  // PERF-02/SCH-02/SCH-11: per-node priority/phase table (always emitted, so
  // --check sees plan drift like any other generated file).
  if (!EmitSchedPlan(tmp.string(), g, *sched_plan, cat)) return false;
  if (!EmitShmSlots(tmp.string(), g, tpl_dir, manifest)) return false;
  if (!EmitGraphCheck(tmp.string(), g, tpl_dir)) return false;
  // GEN-12: always emitted (count 0 included) so --check sees manifest drift.
  if (!EmitRecordManifest(tmp.string(), g, cat, manifest)) return false;
  if (!EmitTriggerBase(tmp.string(), g, cat, manifest)) return false;
  if (manifest.has_record) {
    if (!EmitRecordRegistry(tmp.string(), g, cat, manifest)) return false;
    if (!EmitRecordTable(tmp.string(), g, cat, manifest)) return false;
    if (!EmitRecordTapHelpers(tmp.string(), g, cat, manifest)) return false;
    if (!EmitGraphRecordCheck(tmp.string(), g, cat, manifest)) return false;
    if (!EmitReplayInject(tmp.string(), g, cat, manifest)) return false;
    if (!EmitReplayStages(tmp.string(), g, cat, manifest)) return false;
    if (!EmitVizSer(tmp.string(), g, cat, manifest)) return false;
  }
  if (!EmitIntegrationGuide(tmp.string(), g, cat, tpl_display, manifest)) return false;
  if (!EmitMpUnknown(tmp.string(), g, tpl_dir)) return false;
  if (!EmitMpPorts(tmp.string(), g, cat)) return false;
  if (!EmitMpRoleLocals(tmp.string(), g, cat)) return false;
  if (!EmitMpRoles(tmp.string(), g, cat)) return false;
  // GEN-09: stamp the tree identity. The meta is part of generated/ so
  // --check diffs it like any other generated file; it is written last, after
  // every emitter succeeded.
  if (!WriteFile(tmp / ".gen_meta.json",
                 gen_meta::Render(gen_meta::ForGraph(g)))) {
    return false;
  }
  return true;
}

// Sorted relative paths of all regular files under dir ("" when missing).
std::vector<std::string> ListTreeFiles(const fs::path& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return out;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (!it->is_regular_file()) continue;
    out.push_back(fs::relative(it->path(), dir, ec).generic_string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

bool EmitGenerated(const std::string& root, const Graph& g,
                   const std::string& tpl_dir,
                   const std::string& tpl_display,
                   const SchedPlan* sched_plan) {
  TemplateCatalog cat;
  EdgeManifest manifest;
  if (!PrepareGenerated(g, tpl_dir, "EmitGenerated", &cat, &manifest)) {
    return false;
  }
  const fs::path r(root);
  const fs::path dest = r / "generated";
  const fs::path tmp = StagingDir(r, "tmp");
  const fs::path trash = StagingDir(r, "trash");
  DirGuard tmp_guard{tmp};
  DirGuard trash_guard{trash};
  std::error_code ec;
  // Defensive: clear a leftover from a crashed run that reused the pid.
  fs::remove_all(tmp, ec);
  fs::remove_all(trash, ec);
  fs::create_directories(tmp, ec);
  if (ec) { std::fprintf(stderr, "EmitGenerated: mkdir tmp failed\n"); return false; }
  if (!FillGeneratedTmp(tmp, g, tpl_dir, tpl_display, cat, manifest,
                        sched_plan)) {
    return false;  // guards remove the half-filled staging dir
  }

  // Atomic install: move the old tree aside, then move the new tree in. Both
  // renames are atomic per directory, so generated/ is never half-written: a
  // crash before the swap leaves the old tree at generated/, a crash between
  // the renames leaves it at the trash path (and the next run regenerates).
  const bool had_old = fs::exists(dest, ec);
  if (had_old) {
    fs::rename(dest, trash, ec);
    if (ec) {
      std::fprintf(stderr, "EmitGenerated: cannot move %s aside: %s\n",
                   dest.c_str(), ec.message().c_str());
      return false;
    }
  }
  fs::rename(tmp, dest, ec);
  if (ec) {
    std::fprintf(stderr, "EmitGenerated: cannot install %s: %s\n",
                 dest.c_str(), ec.message().c_str());
    if (had_old) {
      std::error_code ec2;
      fs::rename(trash, dest, ec2);  // roll back: restore the old tree
      if (ec2) {
        // Rollback failed: keep the old tree where it is for manual recovery
        // instead of letting the guard delete the only remaining copy.
        trash_guard.Dismiss();
        std::fprintf(stderr,
                     "EmitGenerated: rollback failed: %s; previous tree kept at %s\n",
                     ec2.message().c_str(), trash.c_str());
      }
    }
    return false;
  }
  tmp_guard.Dismiss();
  return true;  // trash_guard removes the superseded tree
}

bool DiffGeneratedTrees(const std::string& fresh_dir,
                        const std::string& existing_dir,
                        std::vector<std::string>* diffs) {
  if (diffs == nullptr) return false;
  diffs->clear();
  const std::vector<std::string> fresh = ListTreeFiles(fresh_dir);
  const std::vector<std::string> existing = ListTreeFiles(existing_dir);
  std::size_t i = 0, j = 0;
  while (i < fresh.size() || j < existing.size()) {
    if (j >= existing.size() || (i < fresh.size() && fresh[i] < existing[j])) {
      diffs->push_back(fresh[i] + ": missing (a fresh generation creates it)");
      ++i;
      continue;
    }
    if (i >= fresh.size() || existing[j] < fresh[i]) {
      diffs->push_back(existing[j] +
                       ": stale (a fresh generation does not create it)");
      ++j;
      continue;
    }
    std::string a, b;
    if (!ReadFile(fs::path(fresh_dir) / fresh[i], &a) ||
        !ReadFile(fs::path(existing_dir) / existing[j], &b)) {
      return false;
    }
    if (a != b) diffs->push_back(fresh[i] + ": content differs");
    ++i;
    ++j;
  }
  return true;
}

bool CheckGenerated(const std::string& root, const Graph& g,
                    const std::string& tpl_dir, const std::string& tpl_display,
                    CheckReport* report, const SchedPlan* sched_plan) {
  if (report == nullptr) return false;
  report->diffs.clear();
  report->meta_errors.clear();
  TemplateCatalog cat;
  EdgeManifest manifest;
  if (!PrepareGenerated(g, tpl_dir, "gen_graph --check", &cat, &manifest)) {
    return false;
  }
  const fs::path r(root);
  const fs::path dest = r / "generated";
  // GEN-09: explain a cross-format/cross-version tree before the byte diff.
  // An absent meta is reported by the diff as a missing file; a readable one
  // that cannot describe this generator is a migration diagnostic.
  gen_meta::Meta meta;
  if (gen_meta::Read(dest.string(), &meta)) {
    const std::string err = gen_meta::CompatibilityError(meta);
    if (!err.empty()) {
      report->meta_errors.push_back(err);
    } else {
      const std::string graph_hash = gen_meta::SourceGraphHash(g);
      if (meta.source_graph_hash != graph_hash) {
        report->meta_errors.push_back(
            "generated/.gen_meta.json: source graph hash " +
            meta.source_graph_hash + " does not match the current graph (" +
            graph_hash + "); the tree is stale -- regenerate with "
            "`--update generated`");
      }
    }
  } else {
    std::error_code meta_ec;
    if (fs::exists(dest / ".gen_meta.json", meta_ec)) {
      report->meta_errors.push_back(
          "generated/.gen_meta.json: unreadable; regenerate the tree with "
          "`--update generated` (generated/ is never patched by hand)");
    }
  }
  // --check never swaps: the fresh tree is staged next to generated/ and
  // diffed, so generated/ itself is byte-identical before and after.
  const fs::path tmp = StagingDir(r, "check");
  DirGuard tmp_guard{tmp};
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  if (ec) {
    std::fprintf(stderr, "gen_graph --check: mkdir tmp failed\n");
    return false;
  }
  if (!FillGeneratedTmp(tmp, g, tpl_dir, tpl_display, cat, manifest,
                        sched_plan)) {
    return false;
  }
  return DiffGeneratedTrees(tmp.string(), dest.string(), &report->diffs);
}
