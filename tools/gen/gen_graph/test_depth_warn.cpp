#include "depth.h"
#include "rate.h"
#include "yaml.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#define CHECK(c) \
  do { \
    if (!(c)) { \
      std::fprintf(stderr, "CHECK %s\n", #c); \
      std::abort(); \
    } \
  } while (0)

#ifndef DEPTH_SUGGEST_FIXTURE
#error "DEPTH_SUGGEST_FIXTURE must be defined"
#endif

static bool HasWarning(const std::vector<std::string>& warnings,
                       const char* prefix) {
  for (const std::string& w : warnings) {
    if (w.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

static bool ReadFixture(const char* path, std::string* out) {
  std::ifstream in(path);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

int main() {
  std::string fixture;
  CHECK(ReadFixture(DEPTH_SUGGEST_FIXTURE, &fixture));
  CHECK(fixture.find("\"out\": 8") != std::string::npos);

  CHECK(NextPow2(1) == 1);
  CHECK(NextPow2(2) == 2);
  CHECK(NextPow2(5) == 8);
  CHECK(NextPow2(8) == 8);

  CHECK(SuggestDepth(30, 20, 3) == 8);
  CHECK(SuggestDepth(10, 40, 3) == 2);
  CHECK(SuggestDepth(10, 30, 2) == 2);

  Graph shallow;
  CHECK(LoadGraphYamlFromString(
      R"(version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: prod
    type: t
    hz: 30
    process: p1
  - id: sink
    type: t
    hz: 20
    process: p2
    exec: event
edges:
  - from: prod
    to: sink
    channel: ch
    cpp_type: T
    transport: spsc
    shm: _q
    delivery: queue
    depth: 2
processes:
  - id: p1
    role: p1
  - id: p2
    role: p2
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 2
)",
      &shallow));

  RateInfo info;
  std::vector<std::string> warnings;
  CHECK(DeriveRates(shallow, &info, &warnings));
  WarnDepthBelowSuggest(shallow, info, &warnings);
  CHECK(HasWarning(warnings, "DEPTH_WARN"));
  CHECK(warnings[0].find("depth=2") != std::string::npos);
  CHECK(warnings[0].find("suggested=8") != std::string::npos);

  Graph ok;
  CHECK(LoadGraphYamlFromString(
      R"(version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: prod
    type: t
    hz: 30
    process: p1
  - id: sink
    type: t
    hz: 20
    process: p2
    exec: event
edges:
  - from: prod
    to: sink
    channel: ch
    cpp_type: T
    transport: spsc
    shm: _q
    delivery: queue
    depth: 8
processes:
  - id: p1
    role: p1
  - id: p2
    role: p2
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 2
)",
      &ok));
  warnings.clear();
  CHECK(DeriveRates(ok, &info, &warnings));
  WarnDepthBelowSuggest(ok, info, &warnings);
  CHECK(!HasWarning(warnings, "DEPTH_WARN"));

  std::printf("test_gen_graph_depth_warn ok\n");
  return 0;
}
