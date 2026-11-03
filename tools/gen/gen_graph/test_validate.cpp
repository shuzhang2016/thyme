#include "validate.h"
#include "yaml.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define CHECK(c) \
  do { \
    if (!(c)) { \
      std::fprintf(stderr, "CHECK %s\n", #c); \
      std::abort(); \
    } \
  } while (0)

namespace {

const char* kHead = R"(version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: prod
    type: producer
    hz: 10
    process: p1
  - id: cons
    type: consumer
    hz: 10
    process: p2
  - id: cons2
    type: consumer
    hz: 10
    process: p2
)";

const char* kTail = R"(processes:
  - id: p1
    role: main
  - id: p2
    role: aux
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 3
)";

// A plain cross-process latest edge (shm, no delivery -> derived latest).
const char* kEdge = R"(edges:
  - from: prod
    to: cons
    channel: readings
    cpp_type: Reading
    pool: ReadingPool
    transport: spsc
    shm: _rd
)";

Graph Load(const std::string& text) {
  Graph g;
  CHECK(LoadGraphYamlFromString(text, &g));
  return g;
}

void ExpectClean(const std::string& text, const TemplateCatalog* cat) {
  Graph g = Load(text);
  std::vector<std::string> errors;
  if (!ValidateGraphSemantics(g, cat, &errors)) {
    for (const auto& e : errors) std::fprintf(stderr, "  %s\n", e.c_str());
    CHECK(false);
  }
}

void ExpectViolation(const std::string& text, const char* needle,
                     const TemplateCatalog* cat) {
  Graph g = Load(text);
  std::vector<std::string> errors;
  CHECK(!ValidateGraphSemantics(g, cat, &errors));
  bool found = false;
  for (const auto& e : errors) {
    if (e.find(needle) != std::string::npos) found = true;
  }
  if (!found) {
    for (const auto& e : errors) std::fprintf(stderr, "  %s\n", e.c_str());
    std::fprintf(stderr, "missing needle: %s\n", needle);
  }
  CHECK(found);
}

TemplateCatalog EmptyCatalog() { return TemplateCatalog{}; }

TemplateCatalog CatalogWithReadingKind() {
  TemplateCatalog cat;
  cat.shm_kinds.push_back(ShmKindCat{"Reading", "reading"});
  return cat;
}

}  // namespace

int main() {
  const TemplateCatalog empty_cat = EmptyCatalog();
  const TemplateCatalog reading_cat = CatalogWithReadingKind();
  const std::string base = std::string(kHead) + kEdge + kTail;

  // Positive: the base graph validates clean with no catalog (as-written
  // rules) and with a catalog that carries the shm kind.
  ExpectClean(base, nullptr);
  ExpectClean(base, &reading_cat);

  // Duplicate node id.
  ExpectViolation(std::string(kHead) +
                      "  - id: cons\n    type: consumer\n    hz: 10\n    process: p2\n" +
                      kEdge + kTail,
                  "duplicate node id 'cons'", nullptr);

  // Duplicate process id.
  ExpectViolation(std::string(kHead) + kEdge +
                      "processes:\n  - id: p1\n    role: main\n  - id: p1\n    role: aux\n" +
                      "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 3\n",
                  "duplicate process id 'p1'", nullptr);

  // Edge endpoints must reference declared nodes. (A dangling endpoint makes
  // the edge resolve as same-process at load, so these graphs carry no shm.)
  ExpectViolation(std::string(kHead) +
                      "edges:\n  - from: ghost\n    to: cons\n    channel: readings\n    cpp_type: Reading\n    pool: ReadingPool\n    transport: spsc\n" +
                      kTail,
                  "from 'ghost' is not a declared node", nullptr);
  ExpectViolation(std::string(kHead) +
                      "edges:\n  - from: prod\n    to: ghost\n    channel: readings\n    cpp_type: Reading\n    pool: ReadingPool\n    transport: spsc\n" +
                      kTail,
                  "to 'ghost' is not a declared node", nullptr);

  // One channel name = one cpp_type.
  ExpectViolation(std::string(kHead) + kEdge +
                      "  - from: prod\n    to: cons2\n    channel: readings\n    cpp_type: Other\n    transport: spsc\n    shm: _rd2\n" +
                      kTail,
                  "channel 'readings' carries both cpp_type 'Reading' and 'Other'",
                  nullptr);

  // Non-broadcast fan-out on one producer channel collides in generated Ports.
  ExpectViolation(std::string(kHead) + kEdge +
                      "  - from: prod\n    to: cons2\n    channel: readings\n    cpp_type: Reading\n    transport: spsc\n    shm: _rd2\n" +
                      kTail,
                  "share producer channel 'readings'", nullptr);

  // Two producers onto one consumer channel collide too.
  ExpectViolation(std::string(kHead) +
                      "  - id: prod2\n    type: producer\n    hz: 10\n    process: p1\n" +
                      kEdge +
                      "  - from: prod2\n    to: cons\n    channel: readings\n    cpp_type: Reading\n    transport: spsc\n    shm: _rd2\n" +
                      kTail,
                  "share consumer channel 'readings'", nullptr);

  // Identifier legality.
  ExpectViolation(std::string(kHead) +
                      "edges:\n  - from: prod\n    to: cons\n    channel: my-channel\n    cpp_type: Reading\n    transport: spsc\n    shm: _rd\n" +
                      kTail,
                  "channel name 'my-channel' is not a valid C identifier",
                  nullptr);
  ExpectViolation(std::string(kHead) +
                      "edges:\n  - from: prod\n    to: cons\n    channel: readings\n    transport: spsc\n    shm: _rd\n" +
                      kTail,
                  "missing cpp_type", nullptr);
  ExpectViolation(std::string(kHead) +
                      "edges:\n  - from: prod\n    to: cons\n    channel: readings\n    cpp_type: \"Foo Bar\"\n    transport: spsc\n    shm: _rd\n" +
                      kTail,
                  "cpp_type 'Foo Bar' is not a valid C++ type name", nullptr);
  ExpectViolation(std::string(kHead) +
                      "edges:\n  - from: prod\n    to: cons\n    channel: readings\n    cpp_type: Reading\n    transport: spsc\n    shm: \"_rd x\"\n" +
                      kTail,
                  "shm suffix '_rd x' has illegal characters", nullptr);
  ExpectViolation(
      "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n  - id: prod\n    type: \"my-producer\"\n    hz: 10\n    process: p1\nedges: []\n" +
          std::string(kTail),
      "type 'my-producer' is not a valid C identifier", nullptr);

  // ::-qualified cpp_type is legal.
  ExpectClean(std::string(kHead) +
                  "edges:\n  - from: prod\n    to: cons\n    channel: readings\n    cpp_type: ns::Reading\n    transport: spsc\n    shm: _rd\n" +
                  kTail,
              nullptr);

  // shm kind: delivery latest + shm without a catalog kind is a hard error
  // (the edge would be silently dropped from kGraphShmChannels); with the
  // kind present it validates clean. Without a catalog the rule is skipped.
  ExpectViolation(base, "cpp_type 'Reading' has no shm kind", &empty_cat);
  ExpectClean(base, nullptr);
  ExpectClean(base, &reading_cat);

  // queue / broadcast edges are sized from cpp_type directly: no kind needed.
  // (queue derives only for cross-process exec=event consumers.)
  ExpectClean(
      "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n"
      "  - id: prod\n    type: producer\n    hz: 10\n    process: p1\n"
      "  - id: ev\n    type: sink\n    hz: 10\n    process: p2\n    exec: event\n"
      "  - id: cyc\n    type: sink\n    hz: 10\n    process: p2\n"
      "edges:\n"
      "  - from: prod\n    to: ev\n    channel: readings\n    cpp_type: Reading\n    transport: spsc\n    shm: _rd\n"
      "  - from: prod\n    to: cyc\n    channel: tick\n    cpp_type: Tick\n    shm: _tick\n    delivery: broadcast\n" +
          std::string(kTail),
      &empty_cat);

  // Legal broadcast fan-out: same channel + shm suffix + cpp_type, one reader
  // per consumer.
  ExpectClean(std::string(kHead) +
                  "edges:\n  - from: prod\n    to: cons\n    channel: tick\n    cpp_type: Tick\n    shm: _tick\n    delivery: broadcast\n" +
                  "  - from: prod\n    to: cons2\n    channel: tick\n    cpp_type: Tick\n    shm: _tick\n    delivery: broadcast\n" +
                  kTail,
              &empty_cat);

  std::printf("test_gen_graph_validate ok\n");
  return 0;
}
