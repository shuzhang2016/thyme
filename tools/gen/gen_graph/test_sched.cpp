// PERF-02/SCH-02/SCH-05/SCH-11/SCH-12 unit tests: RM priority policy, explicit
// override, event "no period" behavior, admission status (unknown costs are
// never treated as zero), byte-stable JSON, chains and feedback annotation.
#include "capacity.h"
#include "sched_plan.h"
#include "yaml.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define CHECK(c) \
  do { \
    if (!(c)) { \
      std::fprintf(stderr, "CHECK %s (%s:%d)\n", #c, __FILE__, __LINE__); \
      std::abort(); \
    } \
  } while (0)

namespace {

Graph Load(const std::string& yaml) {
  Graph g;
  if (!LoadGraphYamlFromString(yaml, &g, "test.yaml")) {
    std::fprintf(stderr, "failed to load test graph:\n%s\n", yaml.c_str());
    std::abort();
  }
  return g;
}

const SchedNodePlan& PlanNode(const SchedPlan& plan, const std::string& id) {
  for (const SchedNodePlan& n : plan.nodes) {
    if (n.id == id) return n;
  }
  std::fprintf(stderr, "missing node %s\n", id.c_str());
  std::abort();
}

std::string Header() {
  return "version: 1\npool_n: 4\nring_cap: 8\nprocesses:\n"
         "  - id: main\n    role: main\nsupervisor:\n"
         "  shm_prefix: \"/c9_%pid\"\n  health_n: 8\n";
}

void TestRmOrderingAndExplicitOverride() {
  const std::string yaml =
      Header() +
      "nodes:\n"
      "  - id: fast_explicit\n"
      "    type: t\n"
      "    hz: 100\n"
      "    process: main\n"
      "    priority: 50\n"
      "  - id: fast\n"
      "    type: t\n"
      "    hz: 100\n"
      "    process: main\n"
      "  - id: mid\n"
      "    type: t\n"
      "    hz: 50\n"
      "    process: main\n"
      "  - id: slow\n"
      "    type: t\n"
      "    hz: 10\n"
      "    process: main\n"
      "  - id: ev\n"
      "    type: t\n"
      "    hz: 100\n"
      "    process: main\n"
      "    exec: event\n"
      "edges: []\n";
  const Graph g = Load(yaml);
  SchedPlan plan;
  SchedOptions opt;
  CHECK(DeriveSchedPlan(g, opt, &plan));
  CHECK(plan.errors.empty());

  const SchedNodePlan& e = PlanNode(plan, "fast_explicit");
  CHECK(e.priority == 50);
  CHECK(e.priority_source == SchedPrioritySource::kExplicit);

  // RM ranks only the nodes without an explicit priority; the explicit node
  // does not consume a rank.
  CHECK(PlanNode(plan, "fast").priority == 89);
  CHECK(PlanNode(plan, "mid").priority == 88);
  CHECK(PlanNode(plan, "slow").priority == 87);
  CHECK(PlanNode(plan, "fast").priority_source ==
        SchedPrioritySource::kRateMonotonic);

  // Event hz is messages/s, not a period: no guess, documented default 10.
  CHECK(PlanNode(plan, "ev").priority == kSchedDefaultPriority);
  CHECK(PlanNode(plan, "ev").priority_source == SchedPrioritySource::kDefault);
  CHECK(!PlanNode(plan, "ev").rm_eligible);
  CHECK(!plan.warnings.empty());
}

void TestStrictEventRejects() {
  const std::string yaml =
      Header() +
      "nodes:\n"
      "  - id: ev\n"
      "    type: t\n"
      "    hz: 100\n"
      "    process: main\n"
      "    exec: event\n"
      "edges: []\n";
  const Graph g = Load(yaml);
  SchedPlan plan;
  SchedOptions opt;
  opt.priority_mode = SchedPriorityMode::kStrict;
  CHECK(DeriveSchedPlan(g, opt, &plan));
  CHECK(!plan.errors.empty());
  CHECK(SchedExitCode(plan) == 2);
}

void TestPriorityRangeRejected() {
  const std::string yaml =
      Header() +
      "nodes:\n"
      "  - id: a\n"
      "    type: t\n"
      "    hz: 100\n"
      "    process: main\n"
      "    priority: 95\n"
      "edges: []\n";
  const Graph g = Load(yaml);
  SchedPlan plan;
  CHECK(DeriveSchedPlan(g, SchedOptions{}, &plan));
  CHECK(!plan.errors.empty());
  CHECK(plan.errors[0].find("system-reserved band 90..99") != std::string::npos);
  CHECK(SchedExitCode(plan) == 2);
}

void TestAdmissionUnknownAndOverload() {
  // Both nodes on core 0, costs declared: U = 0.5 + 0.05 = 0.55.
  const std::string ok_yaml =
      Header() +
      "nodes:\n"
      "  - id: a\n"
      "    type: t\n"
      "    hz: 100\n"
      "    process: main\n"
      "    cpu_set: [0]\n"
      "    wcet_ns: 5000000\n"
      "  - id: b\n"
      "    type: t\n"
      "    hz: 10\n"
      "    process: main\n"
      "    cpu_set: [0]\n"
      "    wcet_ns: 5000000\n"
      "edges: []\n";
  {
    const Graph g = Load(ok_yaml);
    SchedPlan plan;
    CHECK(DeriveSchedPlan(g, SchedOptions{}, &plan));
    CHECK(plan.errors.empty());
    CHECK(plan.admission_status == "ok");
    CHECK(plan.cores.size() == 1);
    CHECK(plan.cores[0].status == "analyzed");
    CHECK(plan.cores[0].utilization > 0.54 &&
          plan.cores[0].utilization < 0.56);
  }

  // Unknown cost on one node -> partial, never a silent pass.
  const std::string unknown_yaml =
      Header() +
      "nodes:\n"
      "  - id: a\n"
      "    type: t\n"
      "    hz: 100\n"
      "    process: main\n"
      "    cpu_set: [0]\n"
      "    wcet_ns: 5000000\n"
      "  - id: b\n"
      "    type: t\n"
      "    hz: 10\n"
      "    process: main\n"
      "    cpu_set: [0]\n"
      "edges: []\n";
  {
    const Graph g = Load(unknown_yaml);
    SchedPlan plan;
    CHECK(DeriveSchedPlan(g, SchedOptions{}, &plan));
    CHECK(plan.admission_status == "partial");
    CHECK(plan.cores.size() == 1);
    CHECK(plan.cores[0].status == "partial");
    CHECK(plan.cores[0].unknown_nodes.size() == 1);
    CHECK(plan.cores[0].unknown_nodes[0] == "b");
  }

  // No costs at all -> not_analyzed (costs are never assumed to be zero).
  {
    const std::string yaml =
        Header() +
        "nodes:\n"
        "  - id: a\n"
        "    type: t\n"
        "    hz: 100\n"
        "    process: main\n"
        "    cpu_set: [0]\n"
        "edges: []\n";
    const Graph g = Load(yaml);
    SchedPlan plan;
    CHECK(DeriveSchedPlan(g, SchedOptions{}, &plan));
    CHECK(plan.admission_status == "not_analyzed");
    CHECK(plan.cores.size() == 1);
    CHECK(plan.cores[0].status == "not_analyzed");
  }

  // U > 1.0 -> generation error in the default admission mode.
  const std::string overload_yaml =
      Header() +
      "nodes:\n"
      "  - id: a\n"
      "    type: t\n"
      "    hz: 100\n"
      "    process: main\n"
      "    cpu_set: [0]\n"
      "    wcet_ns: 20000000\n"
      "edges: []\n";
  {
    const Graph g = Load(overload_yaml);
    SchedPlan plan;
    CHECK(DeriveSchedPlan(g, SchedOptions{}, &plan));
    CHECK(plan.admission_status == "overloaded");
    CHECK(!plan.errors.empty());
    CHECK(SchedExitCode(plan) == 2);

    SchedPlan warn_plan;
    SchedOptions warn;
    warn.admission_mode = SchedAdmissionMode::kWarn;
    CHECK(DeriveSchedPlan(g, warn, &warn_plan));
    CHECK(warn_plan.admission_status == "overloaded");
    CHECK(warn_plan.errors.empty());
    CHECK(!warn_plan.warnings.empty());
    CHECK(SchedExitCode(warn_plan) == 0);
  }
}

void TestChainAndFeedback() {
  const std::string chain_yaml =
      Header() +
      "nodes:\n"
      "  - id: a\n"
      "    type: t\n"
      "    hz: 100\n"
      "    process: main\n"
      "    cpu_set: [0]\n"
      "    wcet_ns: 1000000\n"
      "  - id: b\n"
      "    type: t\n"
      "    hz: 50\n"
      "    process: main\n"
      "    cpu_set: [0]\n"
      "    wcet_ns: 2000000\n"
      "edges:\n"
      "  - from: a\n"
      "    to: b\n"
      "    channel: ch\n"
      "    cpp_type: T\n"
      "    pool: P\n"
      "    transport: spsc\n";
  {
    const Graph g = Load(chain_yaml);
    SchedPlan plan;
    CHECK(DeriveSchedPlan(g, SchedOptions{}, &plan));
    CHECK(plan.chains.size() == 1);
    const SchedChainPlan& c = plan.chains[0];
    CHECK(c.sink == "b");
    CHECK(c.nodes.size() == 2 && c.nodes[0] == "a" && c.nodes[1] == "b");
    CHECK(c.end_to_end_ns == 3000000ULL);
    CHECK(c.completion_ns == 3000000ULL);  // b starts after a's WCET
    CHECK(c.deadline_ns == 10000000ULL);  // a's period (tightest)
    CHECK(c.slack_known);
    CHECK(c.slack_ns == 7000000);
  }

  const std::string feedback_yaml =
      Header() +
      "nodes:\n"
      "  - id: la\n"
      "    type: t\n"
      "    hz: 10\n"
      "    process: main\n"
      "  - id: lb\n"
      "    type: t\n"
      "    hz: 20\n"
      "    process: main\n"
      "  - id: lc\n"
      "    type: t\n"
      "    hz: 40\n"
      "    process: main\n"
      "edges:\n"
      "  - from: la\n    to: lb\n    channel: ab\n    cpp_type: T\n"
      "    pool: P\n    transport: spsc\n"
      "  - from: lb\n    to: lc\n    channel: bc\n    cpp_type: T\n"
      "    pool: P\n    transport: spsc\n"
      "  - from: lc\n    to: la\n    channel: ca\n    cpp_type: T\n"
      "    pool: P\n    transport: spsc\n    feedback: true\n"
      "    delay: 1000\n";
  {
    const Graph g = Load(feedback_yaml);
    SchedPlan plan;
    CHECK(DeriveSchedPlan(g, SchedOptions{}, &plan));
    CHECK(plan.errors.empty());
    CHECK(plan.feedback_edges.size() == 1);
    CHECK(plan.feedback_edges[0].from == "lc");
    CHECK(plan.feedback_edges[0].to == "la");
    CHECK(plan.feedback_edges[0].loop_nodes.size() == 3);
    CHECK(PlanNode(plan, "la").in_feedback_loop);
    CHECK(PlanNode(plan, "lb").in_feedback_loop);
    CHECK(PlanNode(plan, "lc").in_feedback_loop);
  }
}

void TestDeterministicJsonAndApply() {
  const std::string yaml =
      Header() +
      "nodes:\n"
      "  - id: a\n"
      "    type: t\n"
      "    hz: 100\n"
      "    process: main\n"
      "    cpu_set: [0]\n"
      "    wcet_ns: 1000000\n"
      "  - id: b\n"
      "    type: t\n"
      "    hz: 10\n"
      "    process: main\n"
      "    cpu_set: [0]\n"
      "    wcet_ns: 2000000\n"
      "edges:\n"
      "  - from: a\n    to: b\n    channel: ch\n    cpp_type: T\n"
      "    pool: P\n    transport: spsc\n";
  Graph g = Load(yaml);
  SchedPlan p1;
  SchedPlan p2;
  CHECK(DeriveSchedPlan(g, SchedOptions{}, &p1));
  CHECK(DeriveSchedPlan(g, SchedOptions{}, &p2));
  const std::string j1 = RenderSchedJson(p1);
  const std::string j2 = RenderSchedJson(p2);
  CHECK(j1 == j2);
  CHECK(!j1.empty());
  CHECK(j1.find("\"schema\": \"thyme.gen_graph.sched.v1\"") != std::string::npos);

  ApplySchedPlan(&g, p1);
  for (const Node& n : g.nodes) {
    if (n.id == "a") CHECK(n.priority_effective == 89);
    if (n.id == "b") CHECK(n.priority_effective == 88);
  }
}

void TestCapacityFormula() {
  const sched_capacity::Requirement lossless =
      sched_capacity::ForRateAndLag(100, 60000000, 0);
  CHECK(lossless.known);
  CHECK(lossless.slots == 6);
  CHECK(lossless.suggested_power_of_two == 8);

  const sched_capacity::Requirement pins = sched_capacity::ForPinWindow(
      10, 100000000, 50000000, 10000000, 2);
  CHECK(pins.known);
  CHECK(pins.slots == 4);
  CHECK(pins.suggested_power_of_two == 4);

  CHECK(!sched_capacity::ForRateAndLag(0, 1000, 0).known);
  CHECK(!sched_capacity::ForRateAndLag(100, 0, 0).known);
  CHECK(!sched_capacity::ForRateAndLag(UINT32_MAX, UINT64_MAX, 1).known);
}

void TestLosslessAndPinAdmission() {
  const std::string prefix =
      Header() +
      "nodes:\n"
      "  - id: producer\n"
      "    type: t\n"
      "    hz: 100\n"
      "    process: main\n"
      "    wcet_ns: 1000000\n"
      "  - id: consumer\n"
      "    type: t\n"
      "    hz: 20\n"
      "    process: main\n"
      "    exec: event\n"
      "    wcet_ns: 10000000\n"
      "edges:\n"
      "  - from: producer\n"
      "    to: consumer\n"
      "    channel: frames\n"
      "    cpp_type: T\n"
      "    pool: P\n"
      "    transport: spsc\n"
      "    lossless: true\n"
      "    depth: 4\n";
  {
    const Graph g = Load(prefix);
    SchedPlan plan;
    CHECK(DeriveSchedPlan(g, SchedOptions{}, &plan));
    CHECK(plan.capacity_status == "insufficient");
    CHECK(plan.capacities.size() == 1);
    CHECK(plan.capacities[0].required_slots == 6);
    CHECK(plan.capacities[0].suggested_capacity == 8);
    CHECK(SchedExitCode(plan) == 2);

    SchedOptions best_effort;
    best_effort.capacity_mode = SchedCapacityMode::kBestEffort;
    SchedPlan downgraded;
    CHECK(DeriveSchedPlan(g, best_effort, &downgraded));
    CHECK(downgraded.capacity_status == "best_effort");
    CHECK(downgraded.capacities[0].best_effort);
    CHECK(SchedExitCode(downgraded) == 0);
  }
  {
    const Graph g = Load(prefix + "    max_lag_ms: 40\n");
    SchedPlan plan;
    CHECK(DeriveSchedPlan(g, SchedOptions{}, &plan));
    CHECK(plan.errors.empty());
    CHECK(plan.capacity_status == "ok");
    CHECK(plan.capacities[0].lag_ns == 40000000);
    CHECK(plan.capacities[0].required_slots == 4);
  }

  const std::string pin_yaml =
      Header() +
      "nodes:\n"
      "  - id: producer\n"
      "    type: t\n"
      "    hz: 10\n"
      "    process: main\n"
      "    wcet_ns: 1000000\n"
      "  - id: consumer\n"
      "    type: t\n"
      "    hz: 10\n"
      "    process: main\n"
      "    exec: event\n"
      "    wcet_ns: 1000000\n"
      "edges:\n"
      "  - from: producer\n"
      "    to: consumer\n"
      "    channel: frames\n"
      "    cpp_type: T\n"
      "    pool: P\n"
      "    transport: spsc\n"
      "    depth: 2\n"
      "    before_ms: 100\n"
      "    after_ms: 50\n"
      "    per_frame_write_ms: 10\n"
      "    pin_budget: 2\n";
  const Graph g = Load(pin_yaml);
  SchedPlan plan;
  CHECK(DeriveSchedPlan(g, SchedOptions{}, &plan));
  CHECK(plan.capacities.size() == 1);
  CHECK(plan.capacities[0].kind == "pin_window");
  CHECK(plan.capacities[0].required_slots == 4);
  CHECK(plan.capacities[0].pin_budget == 2);
  CHECK(plan.capacity_status == "insufficient");
}

void TestLosslessUnknownIsNotZero() {
  const std::string yaml =
      Header() +
      "nodes:\n"
      "  - id: producer\n"
      "    type: t\n"
      "    hz: 100\n"
      "    process: main\n"
      "    wcet_ns: 1000000\n"
      "  - id: consumer\n"
      "    type: t\n"
      "    hz: 20\n"
      "    process: main\n"
      "    exec: event\n"
      "edges:\n"
      "  - from: producer\n"
      "    to: consumer\n"
      "    channel: frames\n"
      "    cpp_type: T\n"
      "    pool: P\n"
      "    transport: spsc\n"
      "    lossless: true\n";
  const Graph g = Load(yaml);
  SchedPlan plan;
  CHECK(DeriveSchedPlan(g, SchedOptions{}, &plan));
  CHECK(plan.capacity_status == "partial");
  CHECK(!plan.capacities[0].known);
  CHECK(plan.capacities[0].required_slots == 0);
  CHECK(plan.capacities[0].unknown_reason.find("wcet_ns unknown") !=
        std::string::npos);
  CHECK(SchedExitCode(plan) == 2);
}

void TestPhaseMode() {
  const std::string chain =
      Header() +
      "nodes:\n"
      "  - id: src\n"
      "    type: t\n"
      "    hz: 100\n"
      "    process: main\n"
      "    wcet_ns: 1000000\n"
      "  - id: dst\n"
      "    type: t\n"
      "    hz: 50\n"
      "    process: main\n"
      "    wcet_ns: 2000000\n"
      "  - id: ev\n"
      "    type: t\n"
      "    hz: 10\n"
      "    process: main\n"
      "    exec: event\n"
      "edges:\n"
      "  - from: src\n    to: dst\n    channel: ch\n    cpp_type: T\n"
      "    pool: P\n    transport: spsc\n";
  {
    Graph g = Load(chain);
    CHECK(g.phase_mode.empty());
    SchedPlan plan;
    CHECK(DeriveSchedPlan(g, SchedOptions{}, &plan));
    ApplySchedPlan(&g, plan);
    ApplyPhaseMode(&g, &plan);
    CHECK(plan.errors.empty());
    for (const Node& n : g.nodes) CHECK(!n.apply_phase);
  }
  {
    Graph g = Load(std::string("phase_mode: best_effort\n") + chain);
    CHECK(g.phase_mode == "best_effort");
    SchedPlan plan;
    CHECK(DeriveSchedPlan(g, SchedOptions{}, &plan));
    ApplySchedPlan(&g, plan);
    ApplyPhaseMode(&g, &plan);
    CHECK(plan.errors.empty());
    CHECK(PlanNode(plan, "dst").phase_complete);
    CHECK(PlanNode(plan, "dst").phase_in_period_ns == 1000000);
    for (const Node& n : g.nodes) {
      if (n.id == "src" || n.id == "dst") {
        CHECK(n.apply_phase);
        CHECK(n.phase_complete);
      }
      if (n.id == "ev") CHECK(!n.apply_phase);
    }
  }
  {
    const std::string incomplete =
        Header() +
        "phase_mode: require\n"
        "nodes:\n"
        "  - id: src\n"
        "    type: t\n"
        "    hz: 100\n"
        "    process: main\n"
        "  - id: dst\n"
        "    type: t\n"
        "    hz: 50\n"
        "    process: main\n"
        "edges:\n"
        "  - from: src\n    to: dst\n    channel: ch\n    cpp_type: T\n"
        "    pool: P\n    transport: spsc\n";
    Graph g = Load(incomplete);
    SchedPlan plan;
    CHECK(DeriveSchedPlan(g, SchedOptions{}, &plan));
    ApplyPhaseMode(&g, &plan);
    CHECK(SchedExitCode(plan) == 2);
    CHECK(!plan.errors.empty());
    CHECK(plan.errors[0].find("SCH-13") != std::string::npos);
    CHECK(plan.errors[0].find("dst") != std::string::npos);
  }
  {
    const std::string incomplete =
        Header() +
        "phase_mode: best_effort\n"
        "nodes:\n"
        "  - id: src\n"
        "    type: t\n"
        "    hz: 100\n"
        "    process: main\n"
        "  - id: dst\n"
        "    type: t\n"
        "    hz: 50\n"
        "    process: main\n"
        "edges:\n"
        "  - from: src\n    to: dst\n    channel: ch\n    cpp_type: T\n"
        "    pool: P\n    transport: spsc\n";
    Graph g = Load(incomplete);
    SchedPlan plan;
    CHECK(DeriveSchedPlan(g, SchedOptions{}, &plan));
    ApplyPhaseMode(&g, &plan);
    CHECK(plan.errors.empty());
    CHECK(!plan.warnings.empty());
    bool saw = false;
    for (const Node& n : g.nodes) {
      if (n.id == "dst") {
        CHECK(n.apply_phase);
        CHECK(!n.phase_complete);
        CHECK(n.phase_in_period_ns == 0);
        saw = true;
      }
    }
    CHECK(saw);
  }
  {
    Graph bad;
    CHECK(!LoadGraphYamlFromString(
        Header() + "phase_mode: maybe\nnodes:\n  - id: a\n    type: t\n"
                   "    hz: 10\n    process: main\nedges: []\n",
        &bad));
  }
}

}  // namespace

int main() {
  TestRmOrderingAndExplicitOverride();
  TestStrictEventRejects();
  TestPriorityRangeRejected();
  TestAdmissionUnknownAndOverload();
  TestChainAndFeedback();
  TestDeterministicJsonAndApply();
  TestCapacityFormula();
  TestLosslessAndPinAdmission();
  TestLosslessUnknownIsNotZero();
  TestPhaseMode();
  std::printf("test_gen_graph_sched OK\n");
  return 0;
}
