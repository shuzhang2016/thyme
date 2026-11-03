// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include <cstddef>
#include <cstdint>

// SCH-02 priority policy (mirrors tools/gen/gen_graph/sched_plan.h):
//   RT priorities are 1..99; graph/RM priorities use 1..89 and the
//   system-reserved band 90..99 is never emitted for a node.
inline constexpr int kSchedPriorityMin = 1;
inline constexpr int kSchedPriorityMax = 99;
inline constexpr int kSchedReservedPriorityMin = 90;
inline constexpr int kSchedReservedPriorityMax = 99;
inline constexpr int kSchedGraphPriorityMin = 1;
inline constexpr int kSchedGraphPriorityMax = 89;
inline constexpr int kSchedDefaultPriority = 10;

enum class SchedPrioritySource : uint8_t {
  Explicit = 0,      // graph.yaml `priority`, wins over RM
  RateMonotonic = 1, // derived: shorter declared period = higher
  Default = 2,       // no period (event hz is messages/s); no guess
};

struct SchedPlanRow {
  const char* node;    // 0
  uint64_t period_ns;  // 8  (nominal; event hz = messages/s)
  uint64_t wcet_ns;    // 16 (0 = unknown)
  uint64_t phase_ns;   // 24
  int32_t hz;          // 32
  int32_t priority;    // 36
  int32_t core;        // 40 (-1 = unassigned)
  SchedPrioritySource source;  // 44
  uint8_t reserved[3]; // 45..47
};

inline constexpr SchedPlanRow kGraphSchedPlan[] = {
  {"prod", 20000000ULL, 1000000ULL, 0ULL, 50, 89, 0, SchedPrioritySource::RateMonotonic, {0, 0, 0}},
  {"sink__s0", 20000000ULL, 1000000ULL, 1000000ULL, 50, 40, 0, SchedPrioritySource::Explicit, {0, 0, 0}},
  {"sink__s1", 20000000ULL, 1000000ULL, 1000000ULL, 50, 40, 0, SchedPrioritySource::Explicit, {0, 0, 0}}
};
inline constexpr std::size_t kGraphSchedPlanCount =
    sizeof(kGraphSchedPlan) / sizeof(kGraphSchedPlan[0]);

static_assert(kGraphSchedPlanCount > 0u,
              "sched_plan must have one row per graph node");
static_assert(sizeof(SchedPrioritySource) == 1u,
              "SchedPrioritySource must stay one byte");
static_assert(sizeof(SchedPlanRow) == 48u,
              "SchedPlanRow layout changed (32/64-bit lock)");
static_assert(offsetof(SchedPlanRow, node) == 0u,
              "SchedPlanRow::node must be first");
static_assert(offsetof(SchedPlanRow, period_ns) == 8u,
              "SchedPlanRow::period_ns offset drift");
static_assert(offsetof(SchedPlanRow, hz) == 32u,
              "SchedPlanRow::hz offset drift");
static_assert(offsetof(SchedPlanRow, source) == 44u,
              "SchedPlanRow::source offset drift");
static_assert(kSchedReservedPriorityMin == kSchedGraphPriorityMax + 1,
              "graph priority band must end before the reserved band");
static_assert(kGraphSchedPlan[0].priority >= kSchedPriorityMin &&
                  kGraphSchedPlan[0].priority <= kSchedPriorityMax,
              "node priority outside 1..99");
static_assert(kGraphSchedPlan[0].source == SchedPrioritySource::Default ||
                  kGraphSchedPlan[0].priority <= kSchedGraphPriorityMax,
              "graph priority must stay out of the system-reserved band");
static_assert(kGraphSchedPlan[1].priority >= kSchedPriorityMin &&
                  kGraphSchedPlan[1].priority <= kSchedPriorityMax,
              "node priority outside 1..99");
static_assert(kGraphSchedPlan[1].source == SchedPrioritySource::Default ||
                  kGraphSchedPlan[1].priority <= kSchedGraphPriorityMax,
              "graph priority must stay out of the system-reserved band");
static_assert(kGraphSchedPlan[2].priority >= kSchedPriorityMin &&
                  kGraphSchedPlan[2].priority <= kSchedPriorityMax,
              "node priority outside 1..99");
static_assert(kGraphSchedPlan[2].source == SchedPrioritySource::Default ||
                  kGraphSchedPlan[2].priority <= kSchedGraphPriorityMax,
              "graph priority must stay out of the system-reserved band");
