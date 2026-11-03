#pragma once

#include "catalog.h"
#include "graph.h"
#include "sched_plan.h"

#include <string>
#include <vector>

namespace gen_emit {
struct EdgeManifest;
}

bool EmitGenerated(const std::string& root, const Graph& g,
                   const std::string& tpl_dir, const std::string& tpl_display,
                   const SchedPlan* sched_plan = nullptr);

// GEN-05 --check: regenerate the full generated/ tree into a staging dir and
// byte-compare it against <root>/generated.
//
// GEN-09: the staged tree also carries `generated/.gen_meta.json` (tool,
// generator semantic version, output format version, source graph hash).
// `diffs` gets one "<relpath>: <reason>" entry per differing/missing/stale
// file; `meta_errors` gets a migration diagnostic when the checked-in tree
// was produced by a different tool, generator version, or output format.
// Both empty = the checked-in tree is up to date. Returns false on emit/IO
// failure.
struct CheckReport {
  std::vector<std::string> diffs;
  std::vector<std::string> meta_errors;
};

bool CheckGenerated(const std::string& root, const Graph& g,
                    const std::string& tpl_dir, const std::string& tpl_display,
                    CheckReport* report,
                    const SchedPlan* sched_plan = nullptr);

// Byte-compare two generated trees (relative paths, sorted; deterministic).
bool DiffGeneratedTrees(const std::string& fresh_dir,
                        const std::string& existing_dir,
                        std::vector<std::string>* diffs);

// Emit only generated/graph_ir.h into <root>/generated/ (for --update mp).
bool EmitGraphIrOnly(const std::string& root, const Graph& g,
                     const std::string& tpl_dir);

// Emit generated/graph_ir.h (the table read by hand-written src/mp).
// Writes into tmp_dir (a staging directory); the caller moves it into place.
bool EmitGraphIr(const std::string& tmp_dir, const Graph& g,
                 const std::string& tpl_dir, const gen_emit::EdgeManifest& m);

// Emit generated/shm_slots.h (kGraphShmKindCount + kGraphShmSlotSizes +
// kGraphCrossGates). Split out of graph_ir.h so graph_ir.h keeps only
// std headers and the per-kind sizeof expressions can carry the
// contracts/stub-types + LatestSlot includes.
bool EmitShmSlots(const std::string& tmp_dir, const Graph& g,
                  const std::string& tpl_dir, const gen_emit::EdgeManifest& m);

// Emit generated/graph_check.h (per-known-type channel contracts).
bool EmitGraphCheck(const std::string& tmp_dir, const Graph& g,
                    const std::string& tpl_dir);

// Emit generated/mp_unknown.h (helper RunRole calls for unknown node types).
bool EmitMpUnknown(const std::string& tmp_dir, const Graph& g,
                   const std::string& tpl_dir);

// Emit generated/mp_roles.h (single-point role dispatch: MakeKnownThread
// constructs the generated thread for a known node type from the graph tables,
// replacing the hand-written role if-chain).
bool EmitMpRoles(const std::string& tmp_dir, const Graph& g,
                 const TemplateCatalog& cat);

// Emit generated/mp_ports.h (per-node Ports structs + shared MpContext,
// converging the generated base ctors from 15-26 flat args to two groups).
bool EmitMpPorts(const std::string& tmp_dir, const Graph& g,
                 const TemplateCatalog& cat);

// Topic emitters (defined in emit_<topic>.cpp; used by EmitGenerated).
bool EmitWiringH(const std::string& tmp_dir, const Graph& g,
                 const std::string& tpl_dir);
bool EmitWiringCpp(const std::string& tmp_dir, const Graph& g,
                   const std::string& tpl_dir);
bool EmitRecordManifest(const std::string& tmp_dir, const Graph& g,
                        const TemplateCatalog& cat,
                        const gen_emit::EdgeManifest& m);
bool EmitTriggerBase(const std::string& tmp_dir, const Graph& g,
                     const TemplateCatalog& cat,
                     const gen_emit::EdgeManifest& m);
bool EmitRecordRegistry(const std::string& tmp_dir, const Graph& g,
                        const TemplateCatalog& cat,
                        const gen_emit::EdgeManifest& m);
bool EmitRecordTable(const std::string& tmp_dir, const Graph& g,
                     const TemplateCatalog& cat,
                     const gen_emit::EdgeManifest& m);
bool EmitIntegrationGuide(const std::string& tmp_dir, const Graph& g,
                          const TemplateCatalog& cat,
                          const std::string& tpl_display,
                          const gen_emit::EdgeManifest& m);
bool EmitRecordTapHelpers(const std::string& tmp_dir, const Graph& g,
                          const TemplateCatalog& cat,
                          const gen_emit::EdgeManifest& m);
bool EmitGraphRecordCheck(const std::string& tmp_dir, const Graph& g,
                          const TemplateCatalog& cat,
                          const gen_emit::EdgeManifest& m);
bool EmitReplayInject(const std::string& tmp_dir, const Graph& g,
                      const TemplateCatalog& cat,
                      const gen_emit::EdgeManifest& m);
bool EmitReplayStages(const std::string& tmp_dir, const Graph& g,
                      const TemplateCatalog& cat,
                      const gen_emit::EdgeManifest& m);

// Emit generated/sched_plan.h (PERF-02/SCH-02/SCH-11): the per-node period,
// effective priority, provenance, core and phase table, plus the priority
// range/reserved-band static asserts.
bool EmitSchedPlan(const std::string& tmp_dir, const Graph& g,
                   const SchedPlan& plan, const TemplateCatalog& cat);

// Emit generated/viz_ser.h (slot -> schema/serializer dispatch for viz_bridge).
// GEN-04: the contracts-JSON mode comes from cat.contracts_json_include
// (template contracts_json.txt), not from probing the project tree.
bool EmitVizSer(const std::string& tmp_dir, const Graph& g,
                const TemplateCatalog& cat, const gen_emit::EdgeManifest& m);
