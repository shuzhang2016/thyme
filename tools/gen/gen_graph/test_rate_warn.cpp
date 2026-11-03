#include "rate.h"
#include "yaml.h"

#include <cstdio>
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

static bool HasWarning(const std::vector<std::string>& warnings,
                       const char* needle) {
  for (const std::string& w : warnings) {
    if (w.find(needle) != std::string::npos) return true;
  }
  return false;
}

int main() {
  Graph g;
  const std::string path =
      std::string(GEN_GRAPH_TESTDATA_DIR) + "/rate/product_cyclic.yaml";
  CHECK(LoadGraphYaml(path, &g));

  RateInfo info;
  std::vector<std::string> warnings;
  CHECK(DeriveRates(g, &info, &warnings));
  WarnHzDeviation(g, info, 0.20, &warnings);

  CHECK(info.node_frame_rate.at("fusion") == 10);
  CHECK(info.node_frame_rate.at("perception") == 10);
  CHECK(HasWarning(warnings, "node fusion"));
  CHECK(HasWarning(warnings, "node arcomm"));
  CHECK(!HasWarning(warnings, "node perception"));

  // SCH-03: two inputs at 10 Hz and 100 Hz on one event node -> a split-node
  // / input-budget diagnostic. Same-rate inputs do not warn.
  {
    Graph ev;
    Node slow;
    slow.id = "perception";
    slow.type = "perception";
    slow.hz = 10;
    Node fast;
    fast.id = "chassis";
    fast.type = "chassis";
    fast.hz = 100;
    Node sink;
    sink.id = "sink";
    sink.type = "sink";
    sink.hz = 100;
    sink.exec = "event";
    ev.nodes = {slow, fast, sink};
    Edge a;
    a.from = "perception";
    a.to = "sink";
    a.channel = "objects";
    Edge b;
    b.from = "chassis";
    b.to = "sink";
    b.channel = "state";
    ev.edges = {a, b};

    RateInfo ev_info;
    std::vector<std::string> ev_warn;
    CHECK(DeriveRates(ev, &ev_info, &ev_warn));
    WarnMixedEventRates(ev, ev_info, &ev_warn);
    CHECK(HasWarning(ev_warn, "SCH03_WARN event node sink"));
    CHECK(HasWarning(ev_warn, "split the node"));
    CHECK(HasWarning(ev_warn, "input_budget"));

    // Same rate: no diagnostic.
    ev.nodes[1].hz = 10;
    RateInfo same_info;
    std::vector<std::string> same_warn;
    CHECK(DeriveRates(ev, &same_info, &same_warn));
    WarnMixedEventRates(ev, same_info, &same_warn);
    CHECK(!HasWarning(same_warn, "SCH03_WARN"));
  }

  std::printf("test_gen_graph_rate_warn ok\n");
  return 0;
}
