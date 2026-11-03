#include "rate.h"
#include "dag.h"
#include "yaml.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(c) \
  do { \
    if (!(c)) { \
      std::fprintf(stderr, "CHECK %s\n", #c); \
      std::abort(); \
    } \
  } while (0)

#ifndef GEN_GRAPH_TESTDATA_DIR
#error "GEN_GRAPH_TESTDATA_DIR must be defined"
#endif

static int RateOf(const RateInfo& info, const char* id) {
  const auto it = info.node_publish_rate.find(id);
  if (it == info.node_publish_rate.end()) return -1;
  return it->second;
}

int main() {
  Graph g;
  const std::string path = std::string(GEN_GRAPH_TESTDATA_DIR) + "/rate/dummy_twin.yaml";
  CHECK(LoadGraphYaml(path, &g));

  RateInfo info;
  std::vector<std::string> warnings;
  CHECK(DeriveRates(g, &info, &warnings));

  CHECK(RateOf(info, "decision_ev") == 30);
  CHECK(RateOf(info, "planning_ev") == 30);
  CHECK(RateOf(info, "arcomm_ev") == 20);
  CHECK(RateOf(info, "fusion_ev") == 40);

  CHECK(info.node_frame_rate.at("fusion") == 10);
  CHECK(info.node_frame_rate.at("decision_ev") == 10);
  CHECK(info.edge_producer_rate.at("fusion->decision_ev:object_list_ev") == 10);

  {
    // GEN-11: the same 3-node loop, once bare (cycle detected) and once with
    // the closing edge marked feedback + delay (cut for the DAG; rate across
    // the cut annotated loop-unresolved).
    const char* kLoopHead = R"(version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: la
    type: t
    hz: 10
  - id: lb
    type: t
    hz: 20
  - id: lc
    type: t
    hz: 40
edges:
  - from: la
    to: lb
    channel: ch_ab
    cpp_type: T
    pool: P
    transport: spsc
  - from: lb
    to: lc
    channel: ch_bc
    cpp_type: T
    pool: P
    transport: spsc
  - from: lc
    to: la
    channel: ch_ca
    cpp_type: T
    pool: P
    transport: spsc
)";
    const char* kLoopTail = R"(supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 3
)";

    Graph cyc;
    CHECK(LoadGraphYamlFromString(std::string(kLoopHead) + kLoopTail, &cyc));
    std::vector<std::string> cycle;
    CHECK(FindCycle(cyc, &cycle));
    CHECK(!cycle.empty() && cycle.front() == cycle.back());
    // The 3-arg form also names the line of the loop-closing edge (GEN-07
    // diagnostics); the 2-arg form above must stay null-safe.
    int closing_line = 0;
    CHECK(FindCycle(cyc, &cycle, &closing_line));
    CHECK(closing_line > 0);

    Graph fb;
    CHECK(LoadGraphYamlFromString(
        std::string(kLoopHead) + "    feedback: true\n    delay: 10000000\n" +
            kLoopTail,
        &fb));
    CHECK(!FindCycle(fb, &cycle));

    RateInfo rinfo;
    std::vector<std::string> rwarn;
    CHECK(DeriveRates(fb, &rinfo, &rwarn));
    CHECK(rinfo.unresolved.empty());
    CHECK(rinfo.loop_cuts.size() == 1);
    CHECK(rinfo.loop_cuts[0].find("loop unresolved") != std::string::npos);
    CHECK(rinfo.loop_cuts[0].find("lc->la:ch_ca") != std::string::npos);
    // Cut at lc->la: la runs at its own hz, lb/lc follow the truncated chain.
    CHECK(rinfo.node_frame_rate.at("la") == 10);
    CHECK(rinfo.node_frame_rate.at("lb") == 10);
    CHECK(rinfo.node_frame_rate.at("lc") == 10);
  }

  std::printf("test_gen_graph_rate ok\n");
  return 0;
}
