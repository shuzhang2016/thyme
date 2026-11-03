#include "yaml.h"
#include "thyme/contracts/abi.h"

#include <cstdio>
#include <string>

#define CHECK(c) \
  do { \
    if (!(c)) { \
      std::fprintf(stderr, "CHECK %s\n", #c); \
      std::abort(); \
    } \
  } while (0)

int main() {
  Graph g;
  CHECK(LoadGraphYamlFromString(
      R"(version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: tick
    type: tick
    hz: 10
    process: main
    cpu_set: [2]
    sched_policy: fifo
    priority: 40
    wait_inputs: period
edges: []
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 1
)",
      &g));
  CHECK(g.version == 1);
  CHECK(g.nodes.size() == 1);
  CHECK(g.nodes[0].wait_inputs == "period");

  Graph bad;
  CHECK(!LoadGraphYamlFromString("version: 1\n\tnodes:\n", &bad));

  CHECK(!LoadGraphYamlFromString(
      R"(version: 1
pool_n: 4
ring_cap: 6
nodes:
  - id: tick
    type: tick
    hz: 10
    process: main
edges: []
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 1
)",
      &bad));

  {
    Graph depth;
    CHECK(LoadGraphYamlFromString(
        R"(version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: prod
    type: t
    hz: 10
  - id: sink
    type: t
    hz: 10
    exec: event
edges:
  - from: prod
    to: sink
    channel: ch
    cpp_type: T
    pool: p
    transport: spsc
    depth: 16
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 2
)",
        &depth));
    CHECK(depth.edges[0].depth == 16);
    CHECK(depth.edges[0].delivery == "notify");
  }

  {
    // GEN-11: feedback + delay parse; feedback without delay and delay
    // without feedback are rejected.
    const char* kFbHead = R"(version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: a
    type: t
    hz: 10
  - id: b
    type: t
    hz: 10
edges:
  - from: a
    to: b
    channel: ch
    cpp_type: T
    pool: P
    transport: spsc
)";
    const char* kFbTail = R"(supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 2
)";
    Graph fb;
    CHECK(LoadGraphYamlFromString(
        std::string(kFbHead) + "    feedback: true\n    delay: 5000000\n" +
            kFbTail,
        &fb));
    CHECK(fb.edges[0].feedback);
    CHECK(fb.edges[0].delay_ns == 5000000);

    Graph no_delay;
    CHECK(!LoadGraphYamlFromString(
        std::string(kFbHead) + "    feedback: true\n" + kFbTail, &no_delay));

    Graph zero_delay;
    CHECK(!LoadGraphYamlFromString(
        std::string(kFbHead) + "    feedback: true\n    delay: 0\n" + kFbTail,
        &zero_delay));

    Graph no_feedback;
    CHECK(!LoadGraphYamlFromString(
        std::string(kFbHead) + "    delay: 5000000\n" + kFbTail, &no_feedback));

    Graph bad_delay;
    CHECK(!LoadGraphYamlFromString(
        std::string(kFbHead) + "    feedback: true\n    delay: 10ms\n" + kFbTail,
        &bad_delay));
  }

  {
    Graph phase_g;
    CHECK(LoadGraphYamlFromString(
        R"(version: 1
pool_n: 4
ring_cap: 8
phase_mode: best_effort
nodes:
  - id: tick
    type: tick
    hz: 10
    process: main
edges: []
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 1
)",
        &phase_g));
    CHECK(phase_g.phase_mode == "best_effort");
    Graph phase_bad;
    CHECK(!LoadGraphYamlFromString(
        "version: 1\npool_n: 4\nring_cap: 8\nphase_mode: maybe\n"
        "nodes:\n  - id: t\n    type: tick\n    hz: 10\n    process: main\n"
        "edges: []\n",
        &phase_bad));
  }

  Graph missing;
  CHECK(!LoadGraphYaml("/tmp/thyme_gg_nonexistent.yaml", &missing));

  std::printf("test_gen_graph_yaml_string ok\n");
  return 0;
}
