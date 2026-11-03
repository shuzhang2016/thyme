#pragma once
// PERF-02/SCH-02/SCH-05/SCH-11/SCH-12: scheduling plan derivation for gen_graph.
//
// One physical worker = one thread (docs/reference/scheduling-model.md). The
// caller projects SCH-09 logical sink nodes into workers before deriving:
//   * RM priorities for cyclic nodes (explicit priorities win),
//   * a DAG schedule report (topological order, phase/offset, per-core
//     utilisation, chain slack/end-to-end, feedback annotations),
//   * admission diagnostics (sum C_i/T_i per core).
//
// Report-only: DeriveSchedPlan never mutates the graph. ApplySchedPlan writes
// the derived priorities into Node::priority_effective for the emitters.

#include "graph.h"

#include <cstdint>
#include <string>
#include <vector>

// SCH-02 priority policy. The same constants are mirrored into the generated
// header (generated/sched_plan.h) so application code can assert them.
inline constexpr int kSchedPriorityMin = 1;
inline constexpr int kSchedPriorityMax = 99;
inline constexpr int kSchedReservedPriorityMin = 90;
inline constexpr int kSchedReservedPriorityMax = 99;
inline constexpr int kSchedGraphPriorityMin = 1;
inline constexpr int kSchedGraphPriorityMax = 89;
inline constexpr int kSchedDefaultPriority = 10;

// SCH-05 admission thresholds (documented in docs/reference/scheduling-model.md).
inline constexpr double kSchedAdmissionRejectUtilization = 1.0;
inline constexpr double kSchedAdmissionWarnUtilization = 0.9;

enum class SchedPriorityMode {
  // Cyclic nodes get RM priorities; an event node without an explicit
  // priority keeps the documented default 10 and a SCHED_WARN is reported.
  kBestEffort = 0,
  // Any node whose period cannot justify RM and has no explicit priority is a
  // generation error ("do not guess").
  kStrict = 1,
};

enum class SchedAdmissionMode {
  kError = 0,  // U > 1.0 fails generation (exit 2)
  kWarn = 1,   // U > 1.0 is a SCHED_WARN, generation continues
  kOff = 2,    // no admission diagnostics
};

enum class SchedCapacityMode {
  kError = 0,
  kBestEffort = 1,
};

struct SchedOptions {
  SchedPriorityMode priority_mode = SchedPriorityMode::kBestEffort;
  SchedAdmissionMode admission_mode = SchedAdmissionMode::kError;
  SchedCapacityMode capacity_mode = SchedCapacityMode::kError;
};

// Priority provenance, printed in the report and in generated/sched_plan.h.
enum class SchedPrioritySource {
  kExplicit = 0,
  kRateMonotonic = 1,
  kDefault = 2,
};

const char* SchedPrioritySourceName(SchedPrioritySource source);
const char* SchedPriorityModeName(SchedPriorityMode mode);
const char* SchedAdmissionModeName(SchedAdmissionMode mode);
const char* SchedCapacityModeName(SchedCapacityMode mode);

struct SchedNodePlan {
  std::string id;
  std::string type;
  std::string exec;  // "cyclic" | "event"
  int hz = 0;
  // RM eligibility: true only for cyclic nodes (event hz is messages/s, not a
  // period). period_ns is still the nominal inter-arrival used for admission.
  bool rm_eligible = false;
  uint64_t period_ns = 0;
  const char* period_basis = "hz_period";  // or "hz_message_rate"
  uint64_t wcet_ns = 0;
  bool wcet_known = false;
  int priority = kSchedDefaultPriority;
  SchedPrioritySource priority_source = SchedPrioritySource::kDefault;
  int core = -1;  // first cpu_set entry; -1 = unassigned
  bool multi_core = false;
  uint64_t phase_ns = 0;
  uint64_t phase_in_period_ns = 0;
  bool phase_complete = true;
  bool in_feedback_loop = false;
};

struct SchedCorePlan {
  int core = -1;  // -1 = nodes without cpu_set (not admission-checked)
  std::vector<std::string> nodes;
  std::vector<std::string> unknown_nodes;  // wcet_ns not declared
  // "analyzed" (all costs known), "partial" (some unknown), or
  // "not_analyzed" (no declared costs in this bucket).
  std::string status;
  double utilization = 0.0;
  bool overloaded = false;       // utilization > 1.0
  bool above_warn = false;       // utilization > 0.9
  bool admission_applicable = false;  // true only for a real core id
};

struct SchedChainPlan {
  std::string sink;
  std::vector<std::string> nodes;  // source -> sink (critical path)
  uint64_t end_to_end_ns = 0;      // sum of WCET along the path
  uint64_t completion_ns = 0;      // phase(sink) + WCET(sink)
  uint64_t deadline_ns = 0;        // min(period, explicit event deadline)
  bool complete = false;           // every WCET on the path is declared
  bool slack_known = false;
  int64_t slack_ns = 0;
  std::vector<std::string> unknown_nodes;
};

struct SchedFeedbackEdge {
  std::string from;
  std::string to;
  std::string channel;
  uint64_t delay_ns = 0;
  std::vector<std::string> loop_nodes;  // path to -> ... -> from
};

struct SchedCapacityPlan {
  std::string from;
  std::string to;
  std::string channel;
  std::string kind;  // lossless | pin_window
  std::string delivery;
  uint32_t rate_hz = 0;
  uint64_t lag_ns = 0;
  std::string lag_basis;
  uint64_t pin_budget = 0;
  uint64_t capacity = 0;
  uint64_t required_slots = 0;
  uint64_t suggested_capacity = 0;
  bool known = false;
  bool sufficient = false;
  bool best_effort = false;
  std::string status;  // ok | insufficient | partial | best_effort
  std::string unknown_reason;
};

struct SchedPlan {
  SchedOptions options;
  std::vector<SchedNodePlan> nodes;  // graph order
  std::vector<std::string> topo_order;
  std::vector<SchedCorePlan> cores;  // core id ascending, then unassigned (-1)
  std::vector<SchedChainPlan> chains;
  std::vector<SchedFeedbackEdge> feedback_edges;
  std::vector<SchedCapacityPlan> capacities;
  std::vector<std::string> loop_cuts;  // human-readable cut notes
  std::vector<std::string> notes;      // informational (not analyzed, ...)
  std::vector<std::string> warnings;   // SCHED_WARN ...
  std::vector<std::string> errors;     // SCHED_ERROR ...
  std::string admission_status =
      "not_analyzed";  // ok | ok_with_warnings | partial | not_analyzed |
                       // overloaded | disabled
  std::string capacity_status = "not_analyzed";
};

// Derive the full plan. Returns false only when the graph is structurally
// unusable (unknown node endpoint); diagnostics are in out->errors/warnings.
bool DeriveSchedPlan(const Graph& g, const SchedOptions& options,
                     SchedPlan* out);

// Writes derived priorities into Node::priority_effective for emitters.
void ApplySchedPlan(Graph* g, const SchedPlan& plan);
// SCH-13: copies phase_in_period_ns onto cyclic nodes when graph.phase_mode
// is best_effort/require. Incomplete phase under require is a SCHED_ERROR;
// best_effort warns and forces offset 0. Event nodes are ignored.
void ApplyPhaseMode(Graph* g, SchedPlan* plan);

// Byte-stable JSON (stdout contract for --explain-sched).
std::string RenderSchedJson(const SchedPlan& plan);
// Human-readable report (stderr side of --explain-sched).
std::string RenderSchedHuman(const SchedPlan& plan);
// Prints plan.errors + plan.warnings to stderr (codegen path).
void PrintSchedDiagnostics(const SchedPlan& plan);
// 2 when plan.errors is non-empty (generation / explain must fail), else 0.
int SchedExitCode(const SchedPlan& plan);

bool ParseSchedPriorityMode(const std::string& value, SchedPriorityMode* out);
bool ParseSchedAdmissionMode(const std::string& value, SchedAdmissionMode* out);
bool ParseSchedCapacityMode(const std::string& value, SchedCapacityMode* out);
