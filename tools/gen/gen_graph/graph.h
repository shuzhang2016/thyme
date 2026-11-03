#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Node {
  std::string id, type, process;
  int hz = 0;
  // 1-based yaml line of the list item (`- id: ...`), 0 when synthesized.
  // Diagnostics only: not part of the generated output.
  int line = 0;
  std::vector<int> cpu_set;
  std::string sched_policy;   // "", "fifo", "rr", "other"
  int priority = -1;          // -1 = unset (explicit yaml value)
  std::string placement_fail; // "", "best_effort", "require"
  std::string wait_inputs;    // "", "none", "period"
  std::string exec;           // "", "cyclic", "event" ("" = cyclic)
  std::string publish;        // "", "on_all_inputs", ...
  uint64_t deadline_ns = 0; // 0 = unset; exec=event dispatch budget only (M4.3)
  // PERF-02/SCH-02: declared worst-case execution time for this node's work
  // per activation. 0 = not declared (admission reports "not analyzed");
  // never treated as a zero-cost node.
  uint64_t wcet_ns = 0;
  // Filled by ApplySchedPlan before codegen (not part of the yaml surface).
  // -1 = no effective priority (runtime default 10 applies); otherwise the
  // value emitted into ThreadPlacement::priority.
  int priority_effective = -1;
  // SCH-13: filled by ApplyPhaseMode. apply_phase is true only for cyclic
  // nodes when graph.phase_mode is best_effort/require.
  uint64_t phase_in_period_ns = 0;
  bool phase_complete = false;
  bool apply_phase = false;
  // SCH-03: bounded messages per in-edge per Dispatch cycle (0 = unlimited).
  // exec=event only; enforced by EventThread's input-budget protocol.
  uint32_t input_budget = 0;
  // SCH-04: "", "skip", "degrade", "restart" ("" = skip). Event nodes
  // require deadline_ns > 0, otherwise the policy has no trigger.
  std::string overrun;
  // ADR-0016 per-tick transaction shape: "one" (default) = four-step Loop
  // (read inputs + borrow outputs, OnTick, commit/abort/release). "many" =
  // bounded OnProduce loop; only one of the two shapes applies per node.
  std::string produce;
  // SCH-09: static input shards for sink-only event nodes. shard_count is
  // internal physical-worker projection metadata, not a yaml surface key.
  uint32_t threads = 1;
  uint32_t shard_count = 1;
};

struct Edge {
  std::string from, to, channel, cpp_type, pool, transport, shm;
  // 1-based yaml line of the list item (`- from: ...`), 0 when synthesized.
  int line = 0;
  // Delivery primitive: "latest" | "notify" | "queue" | "broadcast". Empty in
  // yaml means derive from (from.process, to.process, to.exec); LoadGraphYaml
  // fills it. "broadcast" must be written explicitly (not derived).
  std::string delivery;
  std::string read;  // "", "latest", "follow"; meaningful on broadcast only
  std::string record_priority;  // "", "p0", "p1", "p2"
  // GEN-12 record manifest opt-out. Default true keeps every pre-GEN-12 graph's
  // recorded set unchanged. Unrecorded edges keep their in-process transport but
  // get no RecordStructId slot, tap, viz series, record gate, or arena bytes.
  // All edges sharing a channel must agree (validated).
  bool record = true;
  // Keep recording but do not require a frame for supervisor success when false.
  // Useful for legal-silent channels such as guardian estop release.
  bool record_gate = true;
  // OPS-13: "" and "mirror" stay on the aggregate mirror and get a
  // RecordStructId. "pinned_slot" and "h264" are separate recorder sources.
  // They are not Frame taps. gop_s is not a graph key.
  std::string record_kind;
  uint32_t bitrate_kbps = 0;      // required when record_kind is h264
  uint64_t avg_frame_bytes = 0;   // required when record_kind is pinned_slot
  uint64_t max_frame_bytes = 0;   // variable PinnedSlot slot capacity; fixed types may use sizeof(T)
  std::string record_shm;         // empty = "/<channel>"
  bool dynamic = false;           // force a local hub even with one subscriber
  int depth = 0;  // 0 = unset; use ring_cap. queue/notify/broadcast.
  // Supervisor success gate (S-D4): when true, this cross-process latest
  // edge gets an MpControl::cross_frames slot and the supervisor requires
  // >= 1 frame on it by end of run. Validated: latest + cross-process only.
  bool mp_gate = false;
  // GEN-11 feedback-loop escape hatch: a feedback edge closes a directed loop
  // on purpose; it is cut for DAG validation and rate derivation. Requires an
  // explicit delay > 0 (delay_ns); delay without feedback is rejected.
  bool feedback = false;
  uint64_t delay_ns = 0;  // 0 = unset
  bool lossless = false;
  uint64_t max_lag_ms = 0;  // 0 = derive consumer worst lag
  uint64_t before_ms = 0;   // BF-07 declarations; 0 = unset
  uint64_t after_ms = 0;
  uint64_t per_frame_write_ms = 0;
  uint64_t pin_budget = 0;
};

struct Process {
  std::string id, role;
  // 1-based yaml line of the list item, 0 when synthesized.
  int line = 0;
};

struct Graph {
  // Source name (yaml path, or "<yaml>" for string loads) used to prefix
  // diagnostics with `<source>:<line>:`. Never emitted.
  std::string source;
  int version = 0;
  int pool_n = 0;
  int ring_cap = 0;
  // SCH-13: "", "off", "best_effort", "require". Empty and off keep today's
  // first-wake (`now + period`). Not a node field: one policy per graph.
  std::string phase_mode;
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::vector<Process> processes;
  std::string shm_prefix;
  int health_n = 0;
};

inline bool EdgeRecordsBulk(const Edge& edge) {
  return edge.record_kind == "pinned_slot" || edge.record_kind == "h264";
}

inline bool IsValidPhaseMode(const std::string& v) {
  return v == "off" || v == "best_effort" || v == "require";
}
inline bool GraphAppliesPhase(const Graph& g) {
  return g.phase_mode == "best_effort" || g.phase_mode == "require";
}

inline std::string GraphLoc(const Graph& g, int line) {
  if (g.source.empty() || line <= 0) return "";
  return g.source + ":" + std::to_string(line) + ": ";
}
