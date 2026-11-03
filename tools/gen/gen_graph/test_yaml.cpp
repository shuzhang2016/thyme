#include "yaml.h"
#include "thyme/contracts/abi.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK %s\n", #c); std::abort(); } } while (0)
int main() {
  const char* ok = "/tmp/thyme_gg_ok.yaml";
  {
    std::ofstream f(ok);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: tick\n    type: tick\n    hz: 10\n    process: main\n"
         "    cpu_set: [2]\n    sched_policy: fifo\n    priority: 40\n    wait_inputs: period\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  Graph g;
  CHECK(LoadGraphYaml(ok, &g));
  CHECK(g.version == 1);
  CHECK(g.nodes.size() == 1);
  CHECK(g.nodes[0].type == "tick");
  CHECK(g.shm_prefix == "/c9_%pid");
  CHECK(g.nodes[0].cpu_set.size()==1 && g.nodes[0].cpu_set[0]==2);
  CHECK(g.nodes[0].wait_inputs=="period");
  CHECK(g.nodes[0].input_budget == 0);  // default: unlimited
  CHECK(g.nodes[0].overrun.empty());    // default: skip

  // SCH-03/SCH-04 node keys: parses on exec=event; rejected where the
  // mechanism does not exist (cyclic input_budget, event policy w/o budget).
  const char* sched_keys = "/tmp/thyme_gg_sched_keys.yaml";
  {
    std::ofstream f(sched_keys);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: ev\n    type: ev\n    hz: 100\n    process: main\n"
         "    exec: event\n    deadline_ns: 2000000\n    overrun: degrade\n"
         "    input_budget: 3\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  Graph sk;
  CHECK(LoadGraphYaml(sched_keys, &sk));
  CHECK(sk.nodes[0].input_budget == 3);
  CHECK(sk.nodes[0].overrun == "degrade");

  const char* bad_overrun = "/tmp/thyme_gg_bad_overrun.yaml";
  {
    std::ofstream f(bad_overrun);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: ev\n    type: ev\n    hz: 10\n"
         "    process: main\n    exec: event\n    overrun: yolo\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  CHECK(!LoadGraphYaml(bad_overrun, &sk));

  const char* overrun_no_deadline = "/tmp/thyme_gg_overrun_no_dl.yaml";
  {
    std::ofstream f(overrun_no_deadline);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: ev\n    type: ev\n    hz: 10\n"
         "    process: main\n    exec: event\n    overrun: restart\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  CHECK(!LoadGraphYaml(overrun_no_deadline, &sk));

  const char* cyclic_budget = "/tmp/thyme_gg_cyclic_budget.yaml";
  {
    std::ofstream f(cyclic_budget);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: c\n    type: c\n    hz: 10\n"
         "    process: main\n    input_budget: 2\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  CHECK(!LoadGraphYaml(cyclic_budget, &sk));

  const char* tab = "/tmp/thyme_gg_tab.yaml";
  { std::ofstream f(tab); f << "version: 1\n\tnodes:\n"; }
  Graph bad;
  CHECK(!LoadGraphYaml(tab, &bad));

  const char* unk = "/tmp/thyme_gg_unk.yaml";
  {
    std::ofstream f(unk);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: p\n    type: passthrough\n    hz: 10\n    process: main\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  Graph u;
  CHECK(LoadGraphYaml(unk, &u));
  CHECK(u.nodes[0].type == "passthrough");

  const char* bad_sched = "/tmp/thyme_gg_bad_sched.yaml";
  {
    std::ofstream f(bad_sched);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: tick\n    type: tick\n    hz: 10\n    process: main\n"
         "    sched_policy: fifoish\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  Graph bs;
  CHECK(!LoadGraphYaml(bad_sched, &bs));

  const char* bad_hz = "/tmp/thyme_gg_bad_hz.yaml";
  {
    std::ofstream f(bad_hz);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: tick\n    type: tick\n    hz: abc\n    process: main\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  CHECK(!LoadGraphYaml(bad_hz, &bs));

  const char* zero_hz = "/tmp/thyme_gg_zero_hz.yaml";
  {
    std::ofstream f(zero_hz);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: tick\n    type: tick\n    hz: 0\n    process: main\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  CHECK(!LoadGraphYaml(zero_hz, &bs));

  const char* bad_key = "/tmp/thyme_gg_bad_key.yaml";
  {
    std::ofstream f(bad_key);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - idd: tick\n    type: tick\n    hz: 10\n    process: main\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  CHECK(!LoadGraphYaml(bad_key, &bs));

  const char* bad_ring = "/tmp/thyme_gg_bad_ring.yaml";
  {
    std::ofstream f(bad_ring);
    f << "version: 1\npool_n: 4\nring_cap: 6\n"
         "nodes:\n  - id: tick\n    type: tick\n    hz: 10\n    process: main\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  CHECK(!LoadGraphYaml(bad_ring, &bs));

  const char* high_hz = "/tmp/thyme_gg_high_hz.yaml";
  {
    std::ofstream f(high_hz);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: tick\n    type: tick\n    hz: 10001\n    process: main\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  CHECK(!LoadGraphYaml(high_hz, &bs));

  const char* max_hz = "/tmp/thyme_gg_max_hz.yaml";
  {
    std::ofstream f(max_hz);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: tick\n    type: tick\n    hz: 10000\n    process: main\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  CHECK(LoadGraphYaml(max_hz, &bs));
  CHECK(bs.nodes[0].hz == 10000);

  const char* bad_pool_n = "/tmp/thyme_gg_bad_pool_n.yaml";
  {
    std::ofstream f(bad_pool_n);
    f << "version: 1\npool_n: 2147483648\nring_cap: 8\n"
         "nodes:\n  - id: tick\n    type: tick\n    hz: 10\n    process: main\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  CHECK(!LoadGraphYaml(bad_pool_n, &bs));

  const char* bad_ring_int = "/tmp/thyme_gg_bad_ring_int.yaml";
  {
    std::ofstream f(bad_ring_int);
    f << "version: 1\npool_n: 4\nring_cap: 2147483648\n"
         "nodes:\n  - id: tick\n    type: tick\n    hz: 10\n    process: main\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  CHECK(!LoadGraphYaml(bad_ring_int, &bs));

  const char* bad_health_n = "/tmp/thyme_gg_bad_health_n.yaml";
  {
    std::ofstream f(bad_health_n);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: tick\n    type: tick\n    hz: 10\n    process: main\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2147483648\n";
  }
  CHECK(!LoadGraphYaml(bad_health_n, &bs));

  const char* editor_style = "/tmp/thyme_gg_editor_style.yaml";
  {
    std::ofstream f(editor_style);
    f << "version: 1\n"
         "pool_n: 4\n"
         "ring_cap: 8\n"
         "nodes:\n"
         "  - id: tick\n"
         "    type: tick\n"
         "    hz: 10\n"
         "    process: main\n"
         "edges:\n"
         "  - from: tick\n"
         "    to: tick\n"
         "    channel: ch\n"
         "    cpp_type: T\n"
         "    pool: P\n"
         "    transport: spsc\n"
         "    record_priority: p0\n"
         "    record: false\n"
         "  - from: tick\n"
         "    to: tick\n"
         "    channel: ch2\n"
         "    cpp_type: T\n"
         "    pool: P\n"
         "    transport: spsc\n"
         "processes:\n"
         "  - id: main\n"
         "    role: main\n"
         "supervisor:\n"
         "  shm_prefix: /c9_%pid\n"
         "  health_n: 1\n";
  }
  Graph ed;
  CHECK(LoadGraphYaml(editor_style, &ed));
  CHECK(ed.shm_prefix == "/c9_%pid");
  CHECK(ed.edges.size() == 2);
  CHECK(ed.edges[0].record_priority == "p0");
  // GEN-12: record defaults to true; `record: false` opts the channel out.
  CHECK(!ed.edges[0].record);
  CHECK(ed.edges[1].record);
  CHECK(ed.nodes[0].wait_inputs.empty());

  // exec: event parses; default (absent) is empty (= cyclic).
  const char* evt = "/tmp/thyme_gg_event.yaml";
  {
    std::ofstream f(evt);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: prod\n    type: producer\n    hz: 10\n    process: main\n"
         "  - id: sink\n    type: sink\n    hz: 10\n    process: main\n    exec: event\n"
         "edges:\n  - from: prod\n    to: sink\n    channel: readings\n"
         "    cpp_type: Reading\n    transport: spsc\n"
         "processes:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  Graph ev;
  CHECK(LoadGraphYaml(evt, &ev));
  CHECK(ev.nodes[0].exec.empty());
  CHECK(ev.nodes[1].exec == "event");

  const char* bad_exec = "/tmp/thyme_gg_bad_exec.yaml";
  {
    std::ofstream f(bad_exec);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: tick\n    type: tick\n    hz: 10\n    process: main\n"
         "    exec: sporadic\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  CHECK(!LoadGraphYaml(bad_exec, &bs));

  // event nodes are driven by their input pipes: wait_inputs is cyclic-only.
  const char* evt_wait = "/tmp/thyme_gg_event_wait.yaml";
  {
    std::ofstream f(evt_wait);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: sink\n    type: sink\n    hz: 10\n    process: main\n"
         "    exec: event\n    wait_inputs: period\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  CHECK(!LoadGraphYaml(evt_wait, &bs));

  // M4.3: optional deadline_ns on exec=event nodes only.
  const char* evt_dl = "/tmp/thyme_gg_event_deadline.yaml";
  {
    std::ofstream f(evt_dl);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: sink\n    type: sink\n    hz: 10\n    process: main\n"
         "    exec: event\n    deadline_ns: 5000000\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  Graph evdl;
  CHECK(LoadGraphYaml(evt_dl, &evdl));
  CHECK(evdl.nodes[0].deadline_ns == 5000000ULL);

  const char* cyc_dl = "/tmp/thyme_gg_cyc_deadline.yaml";
  {
    std::ofstream f(cyc_dl);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: tick\n    type: tick\n    hz: 10\n    process: main\n"
         "    deadline_ns: 1000000\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  CHECK(!LoadGraphYaml(cyc_dl, &bs));

  // shm edges into an event node are allowed when cross-process (delivery
  // derives to queue → ShmSpscRing). Same-process event edges are notify
  // and must not carry shm.
  const char* evt_shm = "/tmp/thyme_gg_event_shm.yaml";
  {
    std::ofstream f(evt_shm);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: prod\n    type: producer\n    hz: 10\n    process: p1\n"
         "  - id: sink\n    type: sink\n    hz: 10\n    process: p2\n    exec: event\n"
         "edges:\n  - from: prod\n    to: sink\n    channel: readings\n"
         "    cpp_type: Reading\n    transport: shm\n    shm: _rd\n"
         "processes:\n  - id: p1\n    role: p1\n  - id: p2\n    role: p2\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  CHECK(LoadGraphYaml(evt_shm, &bs));
  CHECK(bs.edges.size() == 1);
  CHECK(bs.edges[0].delivery == "queue");
  CHECK(bs.edges[0].shm == "_rd");

  // Graphs with only shm and no delivery still load: delivery is derived.
  const char* derive_latest = "/tmp/thyme_gg_derive_latest.yaml";
  {
    std::ofstream f(derive_latest);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: prod\n    type: producer\n    hz: 10\n    process: p1\n"
         "  - id: sink\n    type: sink\n    hz: 10\n    process: p2\n"
         "edges:\n  - from: prod\n    to: sink\n    channel: readings\n"
         "    cpp_type: Reading\n    pool: ReadingPool\n    transport: spsc\n"
         "    shm: _rd\n"
         "processes:\n  - id: p1\n    role: p1\n  - id: p2\n    role: p2\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  CHECK(LoadGraphYaml(derive_latest, &bs));
  CHECK(bs.edges[0].delivery == "latest");

  // Same-process → event derives notify (no shm).
  const char* derive_notify = "/tmp/thyme_gg_derive_notify.yaml";
  {
    std::ofstream f(derive_notify);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: prod\n    type: producer\n    hz: 10\n    process: main\n"
         "  - id: sink\n    type: sink\n    hz: 10\n    process: main\n"
         "    exec: event\n"
         "edges:\n  - from: prod\n    to: sink\n    channel: readings\n"
         "    cpp_type: Reading\n    transport: spsc\n"
         "processes:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  CHECK(LoadGraphYaml(derive_notify, &bs));
  CHECK(bs.edges[0].delivery == "notify");

  // Explicit delivery must match derivation.
  const char* bad_delivery = "/tmp/thyme_gg_bad_delivery.yaml";
  {
    std::ofstream f(bad_delivery);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: prod\n    type: producer\n    hz: 10\n    process: main\n"
         "  - id: sink\n    type: sink\n    hz: 10\n    process: main\n"
         "    exec: event\n"
         "edges:\n  - from: prod\n    to: sink\n    channel: readings\n"
         "    cpp_type: Reading\n    transport: spsc\n    delivery: queue\n"
         "processes:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  CHECK(!LoadGraphYaml(bad_delivery, &bs));

  // Same shm suffix cannot map to two layouts (latest vs queue).
  const char* shm_layout = "/tmp/thyme_gg_shm_layout.yaml";
  {
    std::ofstream f(shm_layout);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: a\n    type: t\n    hz: 10\n    process: p1\n"
         "  - id: b\n    type: t\n    hz: 10\n    process: p2\n"
         "  - id: c\n    type: t\n    hz: 10\n    process: p2\n    exec: event\n"
         "edges:\n  - from: a\n    to: b\n    channel: x\n"
         "    cpp_type: T\n    pool: P\n    transport: spsc\n    shm: _x\n"
         "  - from: a\n    to: c\n    channel: y\n"
         "    cpp_type: T\n    transport: spsc\n    shm: _x\n"
         "processes:\n  - id: p1\n    role: p1\n  - id: p2\n    role: p2\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 3\n";
  }
  CHECK(!LoadGraphYaml(shm_layout, &bs));

  // EventThread supports placement (same ThreadPlacement contract as
  // CyclicThread), so cpu_set/sched_policy/priority load fine on exec=event.
  const char* evt_cpu = "/tmp/thyme_gg_event_cpu.yaml";
  {
    std::ofstream f(evt_cpu);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: sink\n    type: sink\n    hz: 10\n    process: main\n"
         "    exec: event\n    cpu_set: [2]\n"
         "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 1\n";
  }
  CHECK(LoadGraphYaml(evt_cpu, &bs));

  // A cross-process edge without a shm suffix silently dead-ends (each role
  // would drain its own local transport): rejected for any target, cyclic or
  // event.
  const char* xproc_no_shm = "/tmp/thyme_gg_xproc_no_shm.yaml";
  {
    std::ofstream f(xproc_no_shm);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: prod\n    type: producer\n    hz: 10\n    process: p1\n"
         "  - id: sink\n    type: sink\n    hz: 10\n    process: p2\n"
         "edges:\n  - from: prod\n    to: sink\n    channel: readings\n"
         "    cpp_type: Reading\n    pool: ReadingPool\n    transport: spsc\n"
         "processes:\n  - id: p1\n    role: p1\n  - id: p2\n    role: p2\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  CHECK(!LoadGraphYaml(xproc_no_shm, &bs));
  {
    std::ofstream f(xproc_no_shm);
    f << "version: 1\npool_n: 4\nring_cap: 8\n"
         "nodes:\n  - id: prod\n    type: producer\n    hz: 10\n    process: p1\n"
         "  - id: sink\n    type: sink\n    hz: 10\n    process: p2\n"
         "    exec: event\n"
         "edges:\n  - from: prod\n    to: sink\n    channel: readings\n"
         "    cpp_type: Reading\n    transport: spsc\n"
         "processes:\n  - id: p1\n    role: p1\n  - id: p2\n    role: p2\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  CHECK(!LoadGraphYaml(xproc_no_shm, &bs));

  // Per-process worker ceiling: every node runs one thread in its process app
  // and takes one health slot, so a process cannot hold more than
  // kAbiMaxThreads nodes (cyclic or event). The limit derives from the ABI
  // constant, not a literal, so it tracks any kAbiMaxThreads bump.
  {
    std::string y = "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n";
    for (std::size_t i = 0; i < thyme::kAbiMaxThreads; ++i) {
      y += "  - id: w" + std::to_string(i) +
           "\n    type: t\n    hz: 10\n    process: main\n";
    }
    y += "  - id: sink\n    type: t\n    hz: 10\n    process: main\n"
         "    exec: event\n";
    y += "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 99\n";
    Graph ov;
    CHECK(!LoadGraphYamlFromString(y, &ov));
  }

  // Exactly at the ceiling loads.
  {
    std::string y = "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n";
    for (std::size_t i = 0; i < thyme::kAbiMaxThreads; ++i) {
      y += "  - id: w" + std::to_string(i) +
           "\n    type: t\n    hz: 10\n    process: main\n";
    }
    y += "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 99\n";
    Graph at;
    CHECK(LoadGraphYamlFromString(y, &at));
    CHECK(at.nodes.size() == thyme::kAbiMaxThreads);
  }

  // The same over-ceiling total spread across two processes stays legal: the
  // ceiling is per process.
  {
    const std::size_t half = thyme::kAbiMaxThreads / 2 + 1;
    std::string y = "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n";
    for (std::size_t i = 0; i < half; ++i) {
      y += "  - id: a" + std::to_string(i) +
           "\n    type: t\n    hz: 10\n    process: main\n";
      y += "  - id: b" + std::to_string(i) +
           "\n    type: t\n    hz: 10\n    process: aux\n";
    }
    y += "edges: []\nprocesses:\n  - id: main\n    role: main\n"
         "  - id: aux\n    role: aux\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 99\n";
    Graph sp;
    CHECK(LoadGraphYamlFromString(y, &sp));
    CHECK(sp.nodes.size() > thyme::kAbiMaxThreads);
  }

  // M5.3: depth on queue/notify edges; power-of-two; not on latest.
  const char* depth_notify = "/tmp/thyme_gg_depth_notify.yaml";
  {
    std::ofstream f(depth_notify);
    f << "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n"
         "  - id: prod\n    type: t\n    hz: 10\n"
         "  - id: sink\n    type: t\n    hz: 10\n    exec: event\n"
         "edges:\n"
         "  - from: prod\n    to: sink\n    channel: ch\n    cpp_type: T\n"
         "    pool: p\n    transport: spsc\n    depth: 16\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  {
    Graph gn;
    CHECK(LoadGraphYaml(depth_notify, &gn));
    CHECK(gn.edges.size() == 1);
    CHECK(gn.edges[0].depth == 16);
    CHECK(gn.edges[0].delivery == "notify");
  }
  const char* depth_bad_pow2 = "/tmp/thyme_gg_depth_bad_pow2.yaml";
  {
    std::ofstream f(depth_bad_pow2);
    f << "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n"
         "  - id: prod\n    type: t\n    hz: 10\n"
         "  - id: sink\n    type: t\n    hz: 10\n    exec: event\n"
         "edges:\n"
         "  - from: prod\n    to: sink\n    channel: ch\n    cpp_type: T\n"
         "    pool: p\n    transport: spsc\n    depth: 6\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  {
    Graph bad2;
    CHECK(!LoadGraphYaml(depth_bad_pow2, &bad2));
  }
  const char* depth_on_latest = "/tmp/thyme_gg_depth_latest.yaml";
  {
    std::ofstream f(depth_on_latest);
    f << "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n"
         "  - id: a\n    type: t\n    hz: 10\n"
         "  - id: b\n    type: t\n    hz: 10\n"
         "edges:\n"
         "  - from: a\n    to: b\n    channel: ch\n    cpp_type: T\n"
         "    pool: p\n    transport: spsc\n    depth: 16\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  {
    Graph bad3;
    CHECK(!LoadGraphYaml(depth_on_latest, &bad3));
  }

  // broadcast: two cross-process edges may share shm; omitted depth → ring_cap.
  const char* bc_share = "/tmp/thyme_gg_broadcast_share.yaml";
  {
    std::ofstream f(bc_share);
    f << "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n"
         "  - id: prod\n    type: t\n    hz: 10\n    process: p1\n"
         "  - id: a\n    type: t\n    hz: 10\n    process: p2\n"
         "  - id: b\n    type: t\n    hz: 10\n    process: p3\n"
         "edges:\n"
         "  - from: prod\n    to: a\n    channel: ch\n    cpp_type: T\n"
         "    shm: _x\n    delivery: broadcast\n"
         "  - from: prod\n    to: b\n    channel: ch\n    cpp_type: T\n"
         "    shm: _x\n    delivery: broadcast\n"
         "processes:\n  - id: p1\n    role: p1\n  - id: p2\n    role: p2\n"
         "  - id: p3\n    role: p3\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 3\n";
  }
  {
    Graph gshare;
    CHECK(LoadGraphYaml(bc_share, &gshare));
    CHECK(gshare.edges.size() == 2);
    CHECK(gshare.edges[0].delivery == "broadcast");
    CHECK(gshare.edges[1].delivery == "broadcast");
    CHECK(gshare.edges[0].depth == 0);
    CHECK(gshare.edges[1].depth == 0);
    CHECK(gshare.ring_cap == 8);
    CHECK(gshare.edges[0].read.empty());
    CHECK(gshare.edges[1].read.empty());
  }

  // Shared shm: depth:16 vs omit with ring_cap:8 → Cap mismatch.
  const char* bc_cap_bad = "/tmp/thyme_gg_broadcast_cap_bad.yaml";
  {
    std::ofstream f(bc_cap_bad);
    f << "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n"
         "  - id: prod\n    type: t\n    hz: 10\n    process: p1\n"
         "  - id: a\n    type: t\n    hz: 10\n    process: p2\n"
         "  - id: b\n    type: t\n    hz: 10\n    process: p3\n"
         "edges:\n"
         "  - from: prod\n    to: a\n    channel: ch\n    cpp_type: T\n"
         "    shm: _x\n    delivery: broadcast\n    depth: 16\n"
         "  - from: prod\n    to: b\n    channel: ch\n    cpp_type: T\n"
         "    shm: _x\n    delivery: broadcast\n"
         "processes:\n  - id: p1\n    role: p1\n  - id: p2\n    role: p2\n"
         "  - id: p3\n    role: p3\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 3\n";
  }
  {
    Graph badc;
    CHECK(!LoadGraphYaml(bc_cap_bad, &badc));
  }

  // Shared shm: depth:8 + omit with ring_cap:8 → same effective Cap, ok.
  const char* bc_cap_ok = "/tmp/thyme_gg_broadcast_cap_ok.yaml";
  {
    std::ofstream f(bc_cap_ok);
    f << "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n"
         "  - id: prod\n    type: t\n    hz: 10\n    process: p1\n"
         "  - id: a\n    type: t\n    hz: 10\n    process: p2\n"
         "  - id: b\n    type: t\n    hz: 10\n    process: p3\n"
         "edges:\n"
         "  - from: prod\n    to: a\n    channel: ch\n    cpp_type: T\n"
         "    shm: _x\n    delivery: broadcast\n    depth: 8\n"
         "  - from: prod\n    to: b\n    channel: ch\n    cpp_type: T\n"
         "    shm: _x\n    delivery: broadcast\n"
         "processes:\n  - id: p1\n    role: p1\n  - id: p2\n    role: p2\n"
         "  - id: p3\n    role: p3\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 3\n";
  }
  {
    Graph gcap;
    CHECK(LoadGraphYaml(bc_cap_ok, &gcap));
    CHECK(gcap.edges[0].depth == 8);
    CHECK(gcap.edges[1].depth == 0);
  }

  // Same-process broadcast is rejected (IPC-only this round).
  const char* bc_same = "/tmp/thyme_gg_broadcast_sameproc.yaml";
  {
    std::ofstream f(bc_same);
    f << "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n"
         "  - id: prod\n    type: t\n    hz: 10\n    process: main\n"
         "  - id: sink\n    type: t\n    hz: 10\n    process: main\n"
         "edges:\n"
         "  - from: prod\n    to: sink\n    channel: ch\n    cpp_type: T\n"
         "    shm: _x\n    delivery: broadcast\n"
         "processes:\n  - id: main\n    role: main\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  {
    Graph bads;
    CHECK(!LoadGraphYaml(bc_same, &bads));
  }

  // broadcast without shm is rejected.
  const char* bc_no_shm = "/tmp/thyme_gg_broadcast_no_shm.yaml";
  {
    std::ofstream f(bc_no_shm);
    f << "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n"
         "  - id: prod\n    type: t\n    hz: 10\n    process: p1\n"
         "  - id: sink\n    type: t\n    hz: 10\n    process: p2\n"
         "edges:\n"
         "  - from: prod\n    to: sink\n    channel: ch\n    cpp_type: T\n"
         "    delivery: broadcast\n"
         "processes:\n  - id: p1\n    role: p1\n  - id: p2\n    role: p2\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  {
    Graph badn;
    CHECK(!LoadGraphYaml(bc_no_shm, &badn));
  }

  // broadcast + non-empty pool is rejected (payload lives in the ring).
  const char* bc_pool = "/tmp/thyme_gg_broadcast_pool.yaml";
  {
    std::ofstream f(bc_pool);
    f << "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n"
         "  - id: prod\n    type: t\n    hz: 10\n    process: p1\n"
         "  - id: sink\n    type: t\n    hz: 10\n    process: p2\n"
         "edges:\n"
         "  - from: prod\n    to: sink\n    channel: ch\n    cpp_type: T\n"
         "    pool: FooPool\n    shm: _x\n    delivery: broadcast\n"
         "processes:\n  - id: p1\n    role: p1\n  - id: p2\n    role: p2\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  {
    Graph badp;
    CHECK(!LoadGraphYaml(bc_pool, &badp));
  }

  // read is broadcast-only.
  const char* read_on_latest = "/tmp/thyme_gg_read_on_latest.yaml";
  {
    std::ofstream f(read_on_latest);
    f << "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n"
         "  - id: a\n    type: t\n    hz: 10\n"
         "  - id: b\n    type: t\n    hz: 10\n"
         "edges:\n"
         "  - from: a\n    to: b\n    channel: ch\n    cpp_type: T\n"
         "    pool: p\n    transport: spsc\n    delivery: latest\n"
         "    read: follow\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  {
    Graph badr;
    CHECK(!LoadGraphYaml(read_on_latest, &badr));
  }

  // broadcast + mp_gate is rejected.
  const char* bc_mp = "/tmp/thyme_gg_broadcast_mp_gate.yaml";
  {
    std::ofstream f(bc_mp);
    f << "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n"
         "  - id: prod\n    type: t\n    hz: 10\n    process: p1\n"
         "  - id: sink\n    type: t\n    hz: 10\n    process: p2\n"
         "edges:\n"
         "  - from: prod\n    to: sink\n    channel: ch\n    cpp_type: T\n"
         "    shm: _x\n    delivery: broadcast\n    mp_gate: true\n"
         "processes:\n  - id: p1\n    role: p1\n  - id: p2\n    role: p2\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  {
    Graph badm;
    CHECK(!LoadGraphYaml(bc_mp, &badm));
  }

  // queue and broadcast cannot share a shm suffix.
  const char* bc_queue = "/tmp/thyme_gg_broadcast_queue_shm.yaml";
  {
    std::ofstream f(bc_queue);
    f << "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n"
         "  - id: a\n    type: t\n    hz: 10\n    process: p1\n"
         "  - id: b\n    type: t\n    hz: 10\n    process: p2\n"
         "  - id: c\n    type: t\n    hz: 10\n    process: p2\n    exec: event\n"
         "edges:\n"
         "  - from: a\n    to: b\n    channel: ch\n    cpp_type: T\n"
         "    shm: _x\n    delivery: broadcast\n"
         "  - from: a\n    to: c\n    channel: y\n    cpp_type: T\n"
         "    transport: spsc\n    shm: _x\n"
         "processes:\n  - id: p1\n    role: p1\n  - id: p2\n    role: p2\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 3\n";
  }
  {
    Graph badq;
    CHECK(!LoadGraphYaml(bc_queue, &badq));
  }

  // 9 readers on one suffix exceeds kAbiMaxSpmcReaders.
  {
    const char* too_many = "/tmp/thyme_gg_broadcast_9readers.yaml";
    std::ofstream f(too_many);
    f << "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n"
         "  - id: prod\n    type: t\n    hz: 10\n    process: p0\n";
    for (std::size_t i = 0; i < thyme::kAbiMaxSpmcReaders + 1; ++i) {
      f << "  - id: c" << i << "\n    type: t\n    hz: 10\n    process: p"
        << (i + 1) << "\n";
    }
    f << "edges:\n";
    for (std::size_t i = 0; i < thyme::kAbiMaxSpmcReaders + 1; ++i) {
      f << "  - from: prod\n    to: c" << i << "\n    channel: ch\n"
           "    cpp_type: T\n    shm: _x\n    delivery: broadcast\n";
    }
    f << "processes:\n  - id: p0\n    role: p0\n";
    for (std::size_t i = 0; i < thyme::kAbiMaxSpmcReaders + 1; ++i) {
      f << "  - id: p" << (i + 1) << "\n    role: p" << (i + 1) << "\n";
    }
    f << "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 99\n";
    f.close();
    Graph too;
    CHECK(!LoadGraphYaml(too_many, &too));
  }

  // Explicit broadcast is allowed even when DeriveDelivery would be latest.
  const char* bc_explicit = "/tmp/thyme_gg_broadcast_explicit.yaml";
  {
    std::ofstream f(bc_explicit);
    f << "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n"
         "  - id: prod\n    type: t\n    hz: 10\n    process: p1\n"
         "  - id: sink\n    type: t\n    hz: 10\n    process: p2\n"
         "edges:\n"
         "  - from: prod\n    to: sink\n    channel: ch\n    cpp_type: T\n"
         "    shm: _x\n    delivery: broadcast\n    read: follow\n"
         "processes:\n  - id: p1\n    role: p1\n  - id: p2\n    role: p2\n"
         "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n";
  }
  {
    Graph gex;
    CHECK(LoadGraphYaml(bc_explicit, &gex));
    CHECK(gex.edges.size() == 1);
    CHECK(gex.edges[0].delivery == "broadcast");
    CHECK(gex.edges[0].read == "follow");
  }

  return 0;
}
