// PERF-02/SCH-02/SCH-11: emit generated/sched_plan.h.
//
// The table is the generated, read-only view of the plan that produced the
// ThreadPlacement priorities: per node period, effective priority,
// provenance, analysis core and phase. Application/tests can assert against
// it without re-parsing graph.yaml. The priority range and reserved-band
// invariants are compile-time checked here (GEN-08 conventions).
#include "emit_internal.h"
#include "sched_plan.h"

#include <cstddef>
#include <sstream>
#include <string>

using namespace gen_emit;

namespace {

const char* SourceToken(SchedPrioritySource source) {
  switch (source) {
    case SchedPrioritySource::kExplicit: return "Explicit";
    case SchedPrioritySource::kRateMonotonic: return "RateMonotonic";
    case SchedPrioritySource::kDefault: return "Default";
  }
  return "Default";
}

}  // namespace

bool EmitSchedPlan(const std::string& tmp_dir, const Graph& g,
                   const SchedPlan& plan, const TemplateCatalog& cat) {
  (void)g;
  std::ostringstream os;
  os << BannerHeader(cat);
  os << "#pragma once\n\n";
  os << "#include <cstddef>\n";
  os << "#include <cstdint>\n\n";
  os << "// SCH-02 priority policy (mirrors tools/gen/gen_graph/sched_plan.h):\n";
  os << "//   RT priorities are 1..99; graph/RM priorities use 1..89 and the\n";
  os << "//   system-reserved band 90..99 is never emitted for a node.\n";
  os << "inline constexpr int kSchedPriorityMin = " << kSchedPriorityMin
     << ";\n";
  os << "inline constexpr int kSchedPriorityMax = " << kSchedPriorityMax
     << ";\n";
  os << "inline constexpr int kSchedReservedPriorityMin = "
     << kSchedReservedPriorityMin << ";\n";
  os << "inline constexpr int kSchedReservedPriorityMax = "
     << kSchedReservedPriorityMax << ";\n";
  os << "inline constexpr int kSchedGraphPriorityMin = "
     << kSchedGraphPriorityMin << ";\n";
  os << "inline constexpr int kSchedGraphPriorityMax = "
     << kSchedGraphPriorityMax << ";\n";
  os << "inline constexpr int kSchedDefaultPriority = "
     << kSchedDefaultPriority << ";\n\n";

  os << "enum class SchedPrioritySource : uint8_t {\n";
  os << "  Explicit = 0,      // graph.yaml `priority`, wins over RM\n";
  os << "  RateMonotonic = 1, // derived: shorter declared period = higher\n";
  os << "  Default = 2,       // no period (event hz is messages/s); no guess\n";
  os << "};\n\n";

  // Field order is chosen so the layout is identical on 32/64-bit targets
  // (uint64 fields start at offset 8 on both); the asserts below pin it.
  os << "struct SchedPlanRow {\n";
  os << "  const char* node;    // 0\n";
  os << "  uint64_t period_ns;  // 8  (nominal; event hz = messages/s)\n";
  os << "  uint64_t wcet_ns;    // 16 (0 = unknown)\n";
  os << "  uint64_t phase_ns;   // 24\n";
  os << "  int32_t hz;          // 32\n";
  os << "  int32_t priority;    // 36\n";
  os << "  int32_t core;        // 40 (-1 = unassigned)\n";
  os << "  SchedPrioritySource source;  // 44\n";
  os << "  uint8_t reserved[3]; // 45..47\n";
  os << "};\n\n";

  os << "inline constexpr SchedPlanRow kGraphSchedPlan[] = {\n";
  for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
    const SchedNodePlan& p = plan.nodes[i];
    os << "  {\"" << p.id << "\", " << p.period_ns << "ULL, " << p.wcet_ns
       << "ULL, " << p.phase_ns << "ULL, " << p.hz << ", " << p.priority
       << ", " << p.core << ", SchedPrioritySource::" << SourceToken(p.priority_source)
       << ", {0, 0, 0}}";
    os << (i + 1 == plan.nodes.size() ? "\n" : ",\n");
  }
  os << "};\n";
  os << "inline constexpr std::size_t kGraphSchedPlanCount =\n";
  os << "    sizeof(kGraphSchedPlan) / sizeof(kGraphSchedPlan[0]);\n\n";

  os << "static_assert(kGraphSchedPlanCount > 0u,\n";
  os << "              \"sched_plan must have one row per graph node\");\n";
  os << "static_assert(sizeof(SchedPrioritySource) == 1u,\n";
  os << "              \"SchedPrioritySource must stay one byte\");\n";
  os << "static_assert(sizeof(SchedPlanRow) == 48u,\n";
  os << "              \"SchedPlanRow layout changed (32/64-bit lock)\");\n";
  os << "static_assert(offsetof(SchedPlanRow, node) == 0u,\n";
  os << "              \"SchedPlanRow::node must be first\");\n";
  os << "static_assert(offsetof(SchedPlanRow, period_ns) == 8u,\n";
  os << "              \"SchedPlanRow::period_ns offset drift\");\n";
  os << "static_assert(offsetof(SchedPlanRow, hz) == 32u,\n";
  os << "              \"SchedPlanRow::hz offset drift\");\n";
  os << "static_assert(offsetof(SchedPlanRow, source) == 44u,\n";
  os << "              \"SchedPlanRow::source offset drift\");\n";
  os << "static_assert(kSchedReservedPriorityMin == kSchedGraphPriorityMax + 1,\n";
  os << "              \"graph priority band must end before the reserved "
        "band\");\n";
  for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
    os << "static_assert(kGraphSchedPlan[" << i
       << "].priority >= kSchedPriorityMin &&\n";
    os << "                  kGraphSchedPlan[" << i
       << "].priority <= kSchedPriorityMax,\n";
    os << "              \"node priority outside 1..99\");\n";
    os << "static_assert(kGraphSchedPlan[" << i
       << "].source == SchedPrioritySource::Default ||\n";
    os << "                  kGraphSchedPlan[" << i
       << "].priority <= kSchedGraphPriorityMax,\n";
    os << "              \"graph priority must stay out of the system-reserved "
          "band\");\n";
  }
  return WriteFile(fs::path(tmp_dir) / "sched_plan.h", os.str());
}
