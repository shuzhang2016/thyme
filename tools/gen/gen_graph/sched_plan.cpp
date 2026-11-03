#include "sched_plan.h"

#include "capacity.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kNanosPerSecond = 1000000000ULL;

bool IsCyclic(const Node& n) { return n.exec.empty() || n.exec == "cyclic"; }
bool IsEvent(const Node& n) { return n.exec == "event"; }

uint64_t NominalPeriodNs(int hz) {
  if (hz <= 0) return 0;
  return kNanosPerSecond / static_cast<uint64_t>(hz);
}

std::size_t FindNodeIndex(const Graph& g, const std::string& id) {
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    if (g.nodes[i].id == id) return i;
  }
  return g.nodes.size();
}

std::string Join(const std::vector<std::string>& values, const char* sep) {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out += sep;
    out += values[i];
  }
  return out;
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  return out;
}

void AppendJsonStringArray(std::ostringstream& os,
                           const std::vector<std::string>& values,
                           const char* indent) {
  os << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) os << ", ";
    os << "\"" << JsonEscape(values[i]) << "\"";
  }
  os << "]";
  (void)indent;
}

std::string JsonBool(bool v) { return v ? "true" : "false"; }

// Duplicated from the yaml loader's TOPO traversal because rate.cpp keeps its
// own file-local helper; sched must not depend on rate derivation.
bool TopoSort(const Graph& g, std::vector<std::string>* order,
              std::vector<std::string>* errors) {
  std::vector<int> indeg(g.nodes.size(), 0);
  std::vector<std::vector<std::size_t>> adj(g.nodes.size());
  for (const Edge& e : g.edges) {
    if (e.feedback) continue;
    const std::size_t from = FindNodeIndex(g, e.from);
    const std::size_t to = FindNodeIndex(g, e.to);
    if (from == g.nodes.size() || to == g.nodes.size()) {
      errors->push_back("edge " + e.from + "->" + e.to + " (" + e.channel +
                        "): unknown node endpoint");
      continue;
    }
    adj[from].push_back(to);
    ++indeg[to];
  }
  std::vector<std::size_t> q;
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    if (indeg[i] == 0) q.push_back(i);
  }
  std::size_t head = 0;
  while (head < q.size()) {
    const std::size_t u = q[head++];
    order->push_back(g.nodes[u].id);
    for (const std::size_t v : adj[u]) {
      if (--indeg[v] == 0) q.push_back(v);
    }
  }
  if (order->size() != g.nodes.size()) {
    errors->push_back("graph still contains a directed cycle");
    return false;
  }
  return true;
}

// Non-feedback path from `start` to `goal` (inclusive), DFS in edge order.
bool FindPathDfs(const std::map<std::string, std::vector<std::string>>& adj,
                 const std::string& u, const std::string& goal,
                 std::vector<std::string>* stack,
                 std::vector<std::string>* visited) {
  stack->push_back(u);
  visited->push_back(u);
  if (u == goal) return true;
  const auto it = adj.find(u);
  if (it != adj.end()) {
    for (const std::string& v : it->second) {
      if (std::find(visited->begin(), visited->end(), v) != visited->end()) {
        continue;
      }
      if (FindPathDfs(adj, v, goal, stack, visited)) return true;
    }
  }
  stack->pop_back();
  return false;
}

bool FindPath(const Graph& g, const std::string& start,
              const std::string& goal, std::vector<std::string>* path) {
  path->clear();
  std::map<std::string, std::vector<std::string>> adj;
  for (const Edge& e : g.edges) {
    if (e.feedback) continue;
    adj[e.from].push_back(e.to);
  }
  std::vector<std::string> stack;
  std::vector<std::string> visited;
  if (FindPathDfs(adj, start, goal, &stack, &visited)) {
    *path = stack;
    return true;
  }
  return false;
}

}  // namespace

const char* SchedPrioritySourceName(SchedPrioritySource source) {
  switch (source) {
    case SchedPrioritySource::kExplicit: return "explicit";
    case SchedPrioritySource::kRateMonotonic: return "rate_monotonic";
    case SchedPrioritySource::kDefault: return "default";
  }
  return "default";
}

const char* SchedPriorityModeName(SchedPriorityMode mode) {
  return mode == SchedPriorityMode::kStrict ? "strict" : "best_effort";
}

const char* SchedAdmissionModeName(SchedAdmissionMode mode) {
  switch (mode) {
    case SchedAdmissionMode::kError: return "error";
    case SchedAdmissionMode::kWarn: return "warn";
    case SchedAdmissionMode::kOff: return "off";
  }
  return "error";
}

const char* SchedCapacityModeName(SchedCapacityMode mode) {
  return mode == SchedCapacityMode::kBestEffort ? "best_effort" : "error";
}

bool DeriveSchedPlan(const Graph& g, const SchedOptions& options,
                     SchedPlan* out) {
  if (out == nullptr) return false;
  out->options = options;
  out->nodes.clear();
  out->topo_order.clear();
  out->cores.clear();
  out->chains.clear();
  out->feedback_edges.clear();
  out->capacities.clear();
  out->loop_cuts.clear();
  out->notes.clear();
  out->warnings.clear();
  out->errors.clear();
  out->admission_status = "not_analyzed";
  out->capacity_status = "not_analyzed";

  // --- node skeleton -------------------------------------------------------
  out->nodes.reserve(g.nodes.size());
  for (const Node& n : g.nodes) {
    SchedNodePlan p;
    p.id = n.id;
    p.type = n.type;
    p.exec = IsEvent(n) ? "event" : "cyclic";
    p.hz = n.hz;
    p.rm_eligible = IsCyclic(n) && n.hz > 0;
    p.period_ns = NominalPeriodNs(n.hz);
    p.period_basis = IsCyclic(n) ? "hz_period" : "hz_message_rate";
    p.wcet_ns = n.wcet_ns;
    p.wcet_known = n.wcet_ns > 0;
    p.core = n.cpu_set.empty() ? -1 : n.cpu_set.front();
    p.multi_core = n.cpu_set.size() > 1;
    p.priority = kSchedDefaultPriority;
    p.priority_source = SchedPrioritySource::kDefault;
    out->nodes.push_back(std::move(p));
    if (p.multi_core) {
      out->warnings.push_back(
          "SCHED_WARN node " + n.id + ": cpu_set lists " +
          std::to_string(n.cpu_set.size()) +
          " CPUs; admission attributes the node to CPU " +
          std::to_string(p.core));
    }
  }

  // --- SCH-02: priority assignment ----------------------------------------
  // Explicit priorities win (validated at graph level to be 1..89). Cyclic
  // nodes without an explicit priority are ranked by declared rate: shorter
  // period = higher priority. Ties share a priority.
  std::vector<std::size_t> rm_nodes;
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    const Node& n = g.nodes[i];
    SchedNodePlan& p = out->nodes[i];
    if (n.priority >= 0) {
      p.priority = n.priority;
      p.priority_source = SchedPrioritySource::kExplicit;
      if (n.priority < kSchedPriorityMin || n.priority > kSchedPriorityMax) {
        out->errors.push_back("SCHED_ERROR node " + n.id + ": priority " +
                              std::to_string(n.priority) +
                              " is outside the RT priority range 1..99");
      } else if (n.priority >= kSchedReservedPriorityMin) {
        out->errors.push_back(
            "SCHED_ERROR node " + n.id + ": priority " +
            std::to_string(n.priority) +
            " is in the system-reserved band 90..99 (graph priorities must "
            "be 1..89)");
      }
    } else if (p.rm_eligible) {
      rm_nodes.push_back(i);
    } else {
      p.priority = kSchedDefaultPriority;
      p.priority_source = SchedPrioritySource::kDefault;
      if (options.priority_mode == SchedPriorityMode::kStrict) {
        out->errors.push_back(
            "SCHED_ERROR node " + n.id +
            ": no period for rate-monotonic priority (exec=event declares "
            "hz as messages/s); set an explicit priority or use "
            "--sched-priority=best-effort");
      } else {
        out->warnings.push_back(
            "SCHED_WARN node " + n.id +
            ": no period for rate-monotonic priority (exec=event declares "
            "hz as messages/s); keeping default priority " +
            std::to_string(kSchedDefaultPriority));
      }
    }
  }
  std::sort(rm_nodes.begin(), rm_nodes.end(),
            [&](std::size_t a, std::size_t b) {
              if (g.nodes[a].hz != g.nodes[b].hz) {
                return g.nodes[a].hz > g.nodes[b].hz;  // shorter period first
              }
              return g.nodes[a].id < g.nodes[b].id;
            });
  int rank = -1;
  int previous_hz = std::numeric_limits<int>::min();
  for (const std::size_t i : rm_nodes) {
    if (g.nodes[i].hz != previous_hz) {
      ++rank;
      previous_hz = g.nodes[i].hz;
    }
    int prio = kSchedGraphPriorityMax - rank;
    if (prio < kSchedGraphPriorityMin) prio = kSchedGraphPriorityMin;
    out->nodes[i].priority = prio;
    out->nodes[i].priority_source = SchedPrioritySource::kRateMonotonic;
  }

  // --- DAG order, feedback annotation -------------------------------------
  if (!TopoSort(g, &out->topo_order, &out->errors)) return true;

  for (const Edge& e : g.edges) {
    if (!e.feedback) continue;
    SchedFeedbackEdge f;
    f.from = e.from;
    f.to = e.to;
    f.channel = e.channel;
    f.delay_ns = e.delay_ns;
    std::vector<std::string> path;
    if (FindPath(g, e.to, e.from, &path)) {
      f.loop_nodes = path;
    } else {
      f.loop_nodes.push_back(e.to);
      if (e.from != e.to) f.loop_nodes.push_back(e.from);
    }
    out->feedback_edges.push_back(f);
    out->loop_cuts.push_back("loop unresolved at feedback edge " + e.from +
                             "->" + e.to + ":" + e.channel + " (delay " +
                             std::to_string(e.delay_ns) + " ns)");
    for (const std::string& id : f.loop_nodes) {
      const std::size_t idx = FindNodeIndex(g, id);
      if (idx < out->nodes.size()) out->nodes[idx].in_feedback_loop = true;
    }
  }

  // --- per-node phase/offset ----------------------------------------------
  std::map<std::string, std::size_t> index_of;
  for (std::size_t i = 0; i < g.nodes.size(); ++i) index_of[g.nodes[i].id] = i;

  for (const std::string& id : out->topo_order) {
    const std::size_t idx = index_of[id];
    SchedNodePlan& p = out->nodes[idx];
    uint64_t phase = 0;
    bool complete = true;
    for (const Edge& e : g.edges) {
      if (e.feedback || e.to != id) continue;
      const auto it = index_of.find(e.from);
      if (it == index_of.end()) continue;
      const SchedNodePlan& pred = out->nodes[it->second];
      const uint64_t cost = pred.wcet_known ? pred.wcet_ns : 0;
      const uint64_t cand = pred.phase_ns + cost;
      if (cand > phase) phase = cand;
      if (!pred.phase_complete || !pred.wcet_known) complete = false;
    }
    p.phase_ns = phase;
    p.phase_in_period_ns = p.period_ns > 0 ? phase % p.period_ns : 0;
    p.phase_complete = complete;
  }

  // --- SCH-05: per-core utilisation ---------------------------------------
  struct CoreAcc {
    std::vector<std::string> nodes;
    std::vector<std::string> unknown;
    double utilization = 0.0;
    std::size_t known = 0;
  };
  std::map<int, CoreAcc> by_core;  // ordered: -1 (unassigned) first
  bool any_cost_declared = false;
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    const SchedNodePlan& p = out->nodes[i];
    CoreAcc& acc = by_core[p.core];
    acc.nodes.push_back(p.id);
    if (!p.wcet_known || p.period_ns == 0) {
      acc.unknown.push_back(p.id);
    } else {
      acc.utilization += static_cast<double>(p.wcet_ns) /
                         static_cast<double>(p.period_ns);
      ++acc.known;
      any_cost_declared = true;
    }
  }

  bool any_applicable_core = false;
  bool overloaded_any = false;
  bool all_applicable_analyzed = true;
  bool any_applicable_partial = false;
  bool any_applicable_analyzed = false;
  for (const auto& kv : by_core) {
    SchedCorePlan core;
    core.core = kv.first;
    core.nodes = kv.second.nodes;
    core.unknown_nodes = kv.second.unknown;
    core.utilization = kv.second.utilization;
    core.admission_applicable = kv.first >= 0;
    if (core.admission_applicable) any_applicable_core = true;

    if (kv.second.known == 0) {
      core.status = "not_analyzed";
    } else if (!kv.second.unknown.empty()) {
      core.status = "partial";
    } else {
      core.status = "analyzed";
    }
    core.overloaded = core.utilization > kSchedAdmissionRejectUtilization;
    core.above_warn = core.utilization > kSchedAdmissionWarnUtilization;
    overloaded_any = overloaded_any || core.overloaded;

    if (core.admission_applicable) {
      if (core.status != "analyzed") all_applicable_analyzed = false;
      if (core.status == "partial") any_applicable_partial = true;
      if (core.status == "analyzed") any_applicable_analyzed = true;
    }
    out->cores.push_back(core);
  }

  if (options.admission_mode == SchedAdmissionMode::kOff) {
    out->admission_status = "disabled";
  } else {
    for (const SchedCorePlan& core : out->cores) {
      if (!core.admission_applicable) continue;
      const std::string where = "core " + std::to_string(core.core);
      if (core.overloaded) {
        const std::string msg =
            "SCHED admission " + where + ": sum(C_i/T_i) = " +
            std::to_string(core.utilization) + " > " +
            std::to_string(kSchedAdmissionRejectUtilization) + " (nodes: " +
            Join(core.nodes, ", ") + ")";
        if (options.admission_mode == SchedAdmissionMode::kError) {
          out->errors.push_back("SCHED_ERROR " + msg.substr(6));
        } else {
          out->warnings.push_back("SCHED_WARN " + msg.substr(6));
        }
      } else if (core.above_warn) {
        out->warnings.push_back(
            "SCHED_WARN admission " + where + ": sum(C_i/T_i) = " +
            std::to_string(core.utilization) + " > " +
            std::to_string(kSchedAdmissionWarnUtilization) +
            " (headroom warning; nodes: " + Join(core.nodes, ", ") + ")");
      }
      if (core.status == "not_analyzed" && any_cost_declared) {
        out->warnings.push_back(
            "SCHED_WARN admission " + where +
            ": not analyzed (no wcet_ns declared for: " +
            Join(core.unknown_nodes, ", ") + ")");
      } else if (core.status == "partial") {
        out->warnings.push_back(
            "SCHED_WARN admission " + where +
            ": partially analyzed; unknown wcet_ns for: " +
            Join(core.unknown_nodes, ", "));
      }
    }
    if (!any_applicable_core) {
      out->notes.push_back(
          "admission not analyzed: no node declares cpu_set, so there is no "
          "per-core C_i/T_i to attribute");
    }
    if (!any_cost_declared) {
      out->notes.push_back(
          "admission not analyzed: no node declares wcet_ns (costs are never "
          "assumed to be zero)");
    }

    if (overloaded_any) {
      out->admission_status = "overloaded";
    } else if (!any_applicable_core) {
      out->admission_status = "not_analyzed";
    } else if (all_applicable_analyzed) {
      out->admission_status = "ok";
    } else if (any_applicable_analyzed || any_applicable_partial) {
      out->admission_status = "partial";
    } else {
      out->admission_status = "not_analyzed";
    }
  }

  // --- SCH-12: queue / ring and BF-07 pin-window capacity ------------------
  bool any_capacity = false;
  bool insufficient_capacity = false;
  bool partial_capacity = false;
  bool best_effort_capacity = false;
  for (const Edge& e : g.edges) {
    const auto from_it = index_of.find(e.from);
    const auto to_it = index_of.find(e.to);
    if (from_it == index_of.end() || to_it == index_of.end()) continue;
    const Node& producer = g.nodes[from_it->second];
    const Node& consumer = g.nodes[to_it->second];
    const SchedNodePlan& consumer_plan = out->nodes[to_it->second];
    const uint64_t configured_capacity =
        e.depth > 0 ? static_cast<uint64_t>(e.depth)
                    : static_cast<uint64_t>(g.ring_cap);

    auto finish = [&](SchedCapacityPlan capacity,
                      const sched_capacity::Requirement& requirement) {
      any_capacity = true;
      capacity.known = requirement.known;
      if (requirement.known) {
        capacity.required_slots = requirement.slots;
        capacity.suggested_capacity = requirement.suggested_power_of_two;
        capacity.sufficient = configured_capacity >= requirement.slots;
        capacity.status = capacity.sufficient ? "ok" : "insufficient";
      } else {
        capacity.status = "partial";
        if (capacity.unknown_reason.empty()) {
          capacity.unknown_reason = "capacity arithmetic overflow";
        }
      }
      const std::string label = "capacity edge " + e.from + "->" + e.to +
                                " (" + e.channel + ", " + capacity.kind + ")";
      if (!capacity.known || !capacity.sufficient) {
        std::string msg = "SCHED " + label + ": ";
        if (!capacity.known) {
          msg += "not analyzed (" + capacity.unknown_reason +
                 "); unknown is not zero";
          partial_capacity = true;
        } else {
          msg += "capacity=" + std::to_string(configured_capacity) +
                 " < required_slots=" + std::to_string(requirement.slots) +
                 "; set depth/ring_cap >= " +
                 std::to_string(requirement.suggested_power_of_two) +
                 " (rate_hz=" + std::to_string(capacity.rate_hz) +
                 " lag_ns=" + std::to_string(capacity.lag_ns) +
                 " pin_budget=" + std::to_string(capacity.pin_budget) + ")";
          insufficient_capacity = true;
        }
        if (options.capacity_mode == SchedCapacityMode::kBestEffort) {
          capacity.best_effort = true;
          capacity.status = "best_effort";
          best_effort_capacity = true;
          out->warnings.push_back("SCHED_WARN " + msg.substr(6) +
                                  " [best_effort: unverified loss may occur]");
        } else {
          out->errors.push_back("SCHED_ERROR " + msg.substr(6));
        }
      }
      out->capacities.push_back(std::move(capacity));
    };

    if (e.lossless) {
      SchedCapacityPlan capacity;
      capacity.from = e.from;
      capacity.to = e.to;
      capacity.channel = e.channel;
      capacity.kind = "lossless";
      capacity.delivery = e.delivery;
      capacity.rate_hz = static_cast<uint32_t>(producer.hz);
      capacity.capacity = configured_capacity;
      uint64_t lag_ns = 0;
      bool lag_known = true;
      if (e.max_lag_ms > 0) {
        capacity.lag_basis = "max_lag_ms";
        if (e.max_lag_ms > std::numeric_limits<uint64_t>::max() /
                               sched_capacity::kNanosPerMillisecond) {
          lag_known = false;
        } else {
          lag_ns = e.max_lag_ms * sched_capacity::kNanosPerMillisecond;
        }
      } else {
        const uint64_t work_ns =
            consumer.deadline_ns > 0 ? consumer.deadline_ns : consumer.wcet_ns;
        capacity.lag_basis = consumer.deadline_ns > 0
                                 ? "consumer_period+deadline_ns"
                                 : "consumer_period+wcet_ns";
        if (work_ns == 0) {
          lag_known = false;
          capacity.unknown_reason =
              "consumer wcet_ns unknown; declare wcet_ns/deadline_ns or "
              "max_lag_ms";
        } else if (consumer_plan.period_ns == 0 ||
                   work_ns > std::numeric_limits<uint64_t>::max() -
                                 consumer_plan.period_ns) {
          lag_known = false;
        } else {
          lag_ns = consumer_plan.period_ns + work_ns;
        }
      }
      capacity.lag_ns = lag_ns;
      const sched_capacity::Requirement requirement =
          lag_known ? sched_capacity::ForRateAndLag(capacity.rate_hz, lag_ns, 0)
                    : sched_capacity::Requirement{};
      finish(std::move(capacity), requirement);
    }

    if (e.before_ms > 0) {
      SchedCapacityPlan capacity;
      capacity.from = e.from;
      capacity.to = e.to;
      capacity.channel = e.channel;
      capacity.kind = "pin_window";
      capacity.delivery = e.delivery;
      capacity.rate_hz = static_cast<uint32_t>(producer.hz);
      capacity.capacity = configured_capacity;
      capacity.pin_budget = e.pin_budget;
      capacity.lag_basis = "before_ms+after_ms+per_frame_write_ms";
      bool times_known = true;
      const uint64_t times_ms[3] = {e.before_ms, e.after_ms,
                                    e.per_frame_write_ms};
      uint64_t times_ns[3] = {0, 0, 0};
      for (std::size_t i = 0; i < 3; ++i) {
        if (times_ms[i] > std::numeric_limits<uint64_t>::max() /
                              sched_capacity::kNanosPerMillisecond) {
          times_known = false;
        } else {
          times_ns[i] = times_ms[i] * sched_capacity::kNanosPerMillisecond;
        }
      }
      const sched_capacity::Requirement requirement =
          times_known ? sched_capacity::ForPinWindow(
                            capacity.rate_hz, times_ns[0], times_ns[1],
                            times_ns[2], e.pin_budget)
                      : sched_capacity::Requirement{};
      capacity.lag_ns = requirement.known
                            ? times_ns[0] + times_ns[1] + times_ns[2]
                            : 0;
      finish(std::move(capacity), requirement);
    }
  }
  if (!any_capacity) {
    out->notes.push_back(
        "capacity not analyzed: no lossless or BF-07 pin-window edge declared");
  } else if (insufficient_capacity || partial_capacity) {
    out->capacity_status = best_effort_capacity ? "best_effort"
                           : insufficient_capacity ? "insufficient"
                                                   : "partial";
  } else {
    out->capacity_status = "ok";
  }

  // --- chain slack / end-to-end -------------------------------------------
  // Critical path per sink (DAG, feedback edges cut). A chain's deadline is
  // the tightest period/deadline on the path; slack is measured against the
  // structural completion time phase + WCET.
  std::vector<std::size_t> latency(g.nodes.size(), 0);
  std::vector<bool> latency_complete(g.nodes.size(), true);
  std::vector<int> pred(g.nodes.size(), -1);
  for (std::size_t i = 0; i < g.nodes.size(); ++i) {
    latency[i] = out->nodes[i].wcet_known ? out->nodes[i].wcet_ns : 0;
    latency_complete[i] = out->nodes[i].wcet_known;
  }
  for (const std::string& id : out->topo_order) {
    const std::size_t u = index_of[id];
    for (const Edge& e : g.edges) {
      if (e.feedback || e.from != id) continue;
      const auto it = index_of.find(e.to);
      if (it == index_of.end()) continue;
      const std::size_t v = it->second;
      const uint64_t own = out->nodes[v].wcet_known ? out->nodes[v].wcet_ns : 0;
      const uint64_t cand = latency[u] + own;
      const bool cand_complete =
          latency_complete[u] && out->nodes[v].wcet_known;
      // Keep the structural path even when every WCET on it is unknown (the
      // path is what the report annotates; completeness stays false). On an
      // equal-latency tie prefer the complete path so the marker is truthful.
      if (pred[v] < 0 || cand > latency[v] ||
          (cand == latency[v] && !latency_complete[v] && cand_complete)) {
        latency[v] = cand;
        pred[v] = static_cast<int>(u);
        latency_complete[v] = cand_complete;
      }
    }
  }

  std::vector<bool> has_out(g.nodes.size(), false);
  for (const Edge& e : g.edges) {
    if (e.feedback) continue;
    const auto it = index_of.find(e.from);
    if (it != index_of.end()) has_out[it->second] = true;
  }
  for (const std::string& id : out->topo_order) {
    const std::size_t sink = index_of[id];
    if (has_out[sink]) continue;
    SchedChainPlan chain;
    chain.sink = id;
    std::vector<std::size_t> rev;
    for (int cur = static_cast<int>(sink); cur >= 0;
         cur = pred[static_cast<std::size_t>(cur)]) {
      rev.push_back(static_cast<std::size_t>(cur));
      if (rev.size() > g.nodes.size()) break;  // defensive
    }
    for (auto it = rev.rbegin(); it != rev.rend(); ++it) {
      chain.nodes.push_back(g.nodes[*it].id);
    }
    chain.end_to_end_ns = latency[sink];
    chain.completion_ns = out->nodes[sink].phase_ns;
    if (out->nodes[sink].wcet_known) {
      chain.completion_ns += out->nodes[sink].wcet_ns;
    }
    chain.complete = latency_complete[sink] && out->nodes[sink].phase_complete;
    uint64_t deadline = std::numeric_limits<uint64_t>::max();
    bool deadline_known = true;
    for (const std::string& nid : chain.nodes) {
      const SchedNodePlan& p = out->nodes[index_of[nid]];
      uint64_t d = 0;
      if (IsEvent(g.nodes[index_of[nid]]) &&
          g.nodes[index_of[nid]].deadline_ns > 0) {
        d = g.nodes[index_of[nid]].deadline_ns;
      } else {
        d = p.period_ns;
      }
      if (d == 0) {
        deadline_known = false;
      } else if (d < deadline) {
        deadline = d;
      }
      if (!p.wcet_known) chain.unknown_nodes.push_back(nid);
    }
    if (deadline_known && deadline != std::numeric_limits<uint64_t>::max()) {
      chain.deadline_ns = deadline;
      chain.slack_known = chain.complete;
      if (chain.slack_known) {
        if (chain.completion_ns <= deadline) {
          chain.slack_ns =
              static_cast<int64_t>(deadline - chain.completion_ns);
        } else {
          const uint64_t over = chain.completion_ns - deadline;
          if (over >
              static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            chain.slack_ns = std::numeric_limits<int64_t>::min();
          } else {
            chain.slack_ns = -static_cast<int64_t>(over);
          }
        }
      }
    }
    out->chains.push_back(std::move(chain));
  }

  return true;
}

void ApplySchedPlan(Graph* g, const SchedPlan& plan) {
  if (g == nullptr) return;
  for (Node& n : g->nodes) {
    for (const SchedNodePlan& p : plan.nodes) {
      if (p.id != n.id) continue;
      if (p.priority_source == SchedPrioritySource::kExplicit ||
          p.priority_source == SchedPrioritySource::kRateMonotonic) {
        n.priority_effective = p.priority;
      }
      break;
    }
  }
}

void ApplyPhaseMode(Graph* g, SchedPlan* plan) {
  if (g == nullptr || plan == nullptr) return;
  if (!GraphAppliesPhase(*g)) return;
  for (Node& n : g->nodes) {
    const SchedNodePlan* sp = nullptr;
    for (const SchedNodePlan& p : plan->nodes) {
      if (p.id == n.id) {
        sp = &p;
        break;
      }
    }
    if (sp == nullptr) continue;
    n.phase_in_period_ns = sp->phase_in_period_ns;
    n.phase_complete = sp->phase_complete;
    if (n.exec == "event") continue;
    n.apply_phase = true;
    if (n.phase_complete) continue;
    n.phase_in_period_ns = 0;
    if (g->phase_mode == "require") {
      plan->errors.push_back(
          "SCHED_ERROR SCH-13: node '" + n.id +
          "' phase is incomplete (wcet/phase unknown) under "
          "phase_mode=require");
    } else {
      plan->warnings.push_back(
          "SCHED_WARN SCH-13: node '" + n.id +
          "' phase is incomplete; using offset 0");
    }
  }
}

std::string RenderSchedJson(const SchedPlan& plan) {
  std::ostringstream os;
  os << "{\n";
  os << "  \"schema\": \"thyme.gen_graph.sched.v1\",\n";
  os << "  \"policy\": {\n";
  os << "    \"priority_min\": " << kSchedPriorityMin << ",\n";
  os << "    \"priority_max\": " << kSchedPriorityMax << ",\n";
  os << "    \"reserved_min\": " << kSchedReservedPriorityMin << ",\n";
  os << "    \"reserved_max\": " << kSchedReservedPriorityMax << ",\n";
  os << "    \"graph_min\": " << kSchedGraphPriorityMin << ",\n";
  os << "    \"graph_max\": " << kSchedGraphPriorityMax << ",\n";
  os << "    \"default_priority\": " << kSchedDefaultPriority << ",\n";
  os << "    \"priority_mode\": \"" << SchedPriorityModeName(plan.options.priority_mode)
     << "\",\n";
  os << "    \"admission_mode\": \""
     << SchedAdmissionModeName(plan.options.admission_mode) << "\",\n";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6f", kSchedAdmissionRejectUtilization);
  os << "    \"admission_reject_above\": " << buf << ",\n";
  std::snprintf(buf, sizeof(buf), "%.6f", kSchedAdmissionWarnUtilization);
  os << "    \"admission_warn_above\": " << buf << ",\n";
  os << "    \"admission_status\": \"" << plan.admission_status << "\",\n";
  os << "    \"capacity_mode\": \""
     << SchedCapacityModeName(plan.options.capacity_mode) << "\",\n";
  os << "    \"capacity_status\": \"" << plan.capacity_status << "\"\n";
  os << "  },\n";

  os << "  \"topological_order\": ";
  AppendJsonStringArray(os, plan.topo_order, "");
  os << ",\n";

  os << "  \"nodes\": [\n";
  for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
    const SchedNodePlan& p = plan.nodes[i];
    os << "    {\n";
    os << "      \"id\": \"" << JsonEscape(p.id) << "\",\n";
    os << "      \"type\": \"" << JsonEscape(p.type) << "\",\n";
    os << "      \"exec\": \"" << p.exec << "\",\n";
    os << "      \"hz\": " << p.hz << ",\n";
    os << "      \"period_ns\": " << p.period_ns << ",\n";
    os << "      \"period_basis\": \"" << p.period_basis << "\",\n";
    os << "      \"rm_eligible\": " << JsonBool(p.rm_eligible) << ",\n";
    os << "      \"wcet_ns\": " << p.wcet_ns << ",\n";
    os << "      \"wcet_known\": " << JsonBool(p.wcet_known) << ",\n";
    os << "      \"priority\": " << p.priority << ",\n";
    os << "      \"priority_source\": \""
       << SchedPrioritySourceName(p.priority_source) << "\",\n";
    if (p.core < 0) {
      os << "      \"core\": null,\n";
    } else {
      os << "      \"core\": " << p.core << ",\n";
    }
    os << "      \"multi_core\": " << JsonBool(p.multi_core) << ",\n";
    os << "      \"phase_ns\": " << p.phase_ns << ",\n";
    os << "      \"phase_in_period_ns\": " << p.phase_in_period_ns << ",\n";
    os << "      \"phase_complete\": " << JsonBool(p.phase_complete) << ",\n";
    os << "      \"in_feedback_loop\": " << JsonBool(p.in_feedback_loop) << "\n";
    os << "    }" << (i + 1 == plan.nodes.size() ? "\n" : ",\n");
  }
  os << "  ],\n";

  os << "  \"feedback_edges\": [\n";
  for (std::size_t i = 0; i < plan.feedback_edges.size(); ++i) {
    const SchedFeedbackEdge& f = plan.feedback_edges[i];
    os << "    {\"from\": \"" << JsonEscape(f.from) << "\", \"to\": \""
       << JsonEscape(f.to) << "\", \"channel\": \"" << JsonEscape(f.channel)
       << "\", \"delay_ns\": " << f.delay_ns << ", \"loop_nodes\": ";
    AppendJsonStringArray(os, f.loop_nodes, "");
    os << "}" << (i + 1 == plan.feedback_edges.size() ? "\n" : ",\n");
  }
  os << "  ],\n";

  std::vector<std::string> loop_nodes;
  for (const SchedNodePlan& p : plan.nodes) {
    if (p.in_feedback_loop) loop_nodes.push_back(p.id);
  }
  os << "  \"cycles\": {\n";
  os << "    \"rejected\": false,\n";
  os << "    \"feedback_loop_nodes\": ";
  AppendJsonStringArray(os, loop_nodes, "");
  os << "\n  },\n";

  os << "  \"cores\": [\n";
  for (std::size_t i = 0; i < plan.cores.size(); ++i) {
    const SchedCorePlan& c = plan.cores[i];
    os << "    {\n";
    if (c.core < 0) {
      os << "      \"core\": null,\n";
    } else {
      os << "      \"core\": " << c.core << ",\n";
    }
    os << "      \"status\": \"" << c.status << "\",\n";
    std::snprintf(buf, sizeof(buf), "%.6f", c.utilization);
    os << "      \"utilization\": " << buf << ",\n";
    os << "      \"overloaded\": " << JsonBool(c.overloaded) << ",\n";
    os << "      \"above_warn\": " << JsonBool(c.above_warn) << ",\n";
    os << "      \"admission_applicable\": "
       << JsonBool(c.admission_applicable) << ",\n";
    os << "      \"nodes\": ";
    AppendJsonStringArray(os, c.nodes, "");
    os << ",\n      \"unknown_nodes\": ";
    AppendJsonStringArray(os, c.unknown_nodes, "");
    os << "\n    }" << (i + 1 == plan.cores.size() ? "\n" : ",\n");
  }
  os << "  ],\n";

  os << "  \"chains\": [\n";
  for (std::size_t i = 0; i < plan.chains.size(); ++i) {
    const SchedChainPlan& c = plan.chains[i];
    os << "    {\n";
    os << "      \"sink\": \"" << JsonEscape(c.sink) << "\",\n";
    os << "      \"nodes\": ";
    AppendJsonStringArray(os, c.nodes, "");
    os << ",\n";
    os << "      \"end_to_end_ns\": " << c.end_to_end_ns << ",\n";
    os << "      \"completion_ns\": " << c.completion_ns << ",\n";
    if (c.deadline_ns > 0) {
      os << "      \"deadline_ns\": " << c.deadline_ns << ",\n";
    } else {
      os << "      \"deadline_ns\": null,\n";
    }
    os << "      \"complete\": " << JsonBool(c.complete) << ",\n";
    os << "      \"slack_known\": " << JsonBool(c.slack_known) << ",\n";
    if (c.slack_known) {
      os << "      \"slack_ns\": " << c.slack_ns << ",\n";
    } else {
      os << "      \"slack_ns\": null,\n";
    }
    os << "      \"unknown_nodes\": ";
    AppendJsonStringArray(os, c.unknown_nodes, "");
    os << "\n    }" << (i + 1 == plan.chains.size() ? "\n" : ",\n");
  }
  os << "  ],\n";

  os << "  \"capacities\": [\n";
  for (std::size_t i = 0; i < plan.capacities.size(); ++i) {
    const SchedCapacityPlan& c = plan.capacities[i];
    os << "    {\n";
    os << "      \"from\": \"" << JsonEscape(c.from) << "\",\n";
    os << "      \"to\": \"" << JsonEscape(c.to) << "\",\n";
    os << "      \"channel\": \"" << JsonEscape(c.channel) << "\",\n";
    os << "      \"kind\": \"" << c.kind << "\",\n";
    os << "      \"delivery\": \"" << c.delivery << "\",\n";
    os << "      \"rate_hz\": " << c.rate_hz << ",\n";
    os << "      \"lag_ns\": " << c.lag_ns << ",\n";
    os << "      \"lag_basis\": \"" << c.lag_basis << "\",\n";
    os << "      \"pin_budget\": " << c.pin_budget << ",\n";
    os << "      \"capacity\": " << c.capacity << ",\n";
    os << "      \"required_slots\": " << c.required_slots << ",\n";
    os << "      \"suggested_capacity\": " << c.suggested_capacity << ",\n";
    os << "      \"known\": " << JsonBool(c.known) << ",\n";
    os << "      \"sufficient\": " << JsonBool(c.sufficient) << ",\n";
    os << "      \"best_effort\": " << JsonBool(c.best_effort) << ",\n";
    os << "      \"status\": \"" << c.status << "\",\n";
    if (c.unknown_reason.empty()) {
      os << "      \"unknown_reason\": null\n";
    } else {
      os << "      \"unknown_reason\": \"" << JsonEscape(c.unknown_reason)
         << "\"\n";
    }
    os << "    }" << (i + 1 == plan.capacities.size() ? "\n" : ",\n");
  }
  os << "  ],\n";

  auto array_field = [&](const char* name, const std::vector<std::string>& v,
                         bool last) {
    os << "  \"" << name << "\": ";
    AppendJsonStringArray(os, v, "");
    os << (last ? "\n" : ",\n");
  };
  array_field("notes", plan.notes, false);
  array_field("warnings", plan.warnings, false);
  array_field("errors", plan.errors, true);
  os << "}\n";
  return os.str();
}

std::string RenderSchedHuman(const SchedPlan& plan) {
  std::ostringstream os;
  os << "gen_graph --explain-sched (compile-time DAG only; no runtime "
        "executor)\n";
  os << "policy: RM priorities " << kSchedGraphPriorityMin << ".."
     << kSchedGraphPriorityMax << " (reserved "
     << kSchedReservedPriorityMin << ".." << kSchedReservedPriorityMax
     << "), default " << kSchedDefaultPriority << "; priority_mode="
     << SchedPriorityModeName(plan.options.priority_mode)
     << " admission_mode="
     << SchedAdmissionModeName(plan.options.admission_mode) << " (warn>"
     << kSchedAdmissionWarnUtilization << " reject>"
     << kSchedAdmissionRejectUtilization << ") capacity_mode="
     << SchedCapacityModeName(plan.options.capacity_mode) << "\n";
  os << "topological order: " << Join(plan.topo_order, " ") << "\n";
  os << "nodes:\n";
  os << "  " << "id" << " | exec | hz | period_ns | wcet_ns | prio | source"
     << " | phase_ns | core\n";
  for (const SchedNodePlan& p : plan.nodes) {
    os << "  " << p.id << " | " << p.exec << " | " << p.hz << " | "
       << p.period_ns << " | ";
    if (p.wcet_known) {
      os << p.wcet_ns;
    } else {
      os << "unknown";
    }
    os << " | " << p.priority << " | " << SchedPrioritySourceName(p.priority_source)
       << " | " << p.phase_ns << " | ";
    if (p.core < 0) {
      os << "unassigned";
    } else {
      os << p.core;
    }
    if (p.in_feedback_loop) os << " (feedback loop)";
    os << "\n";
  }
  os << "cores:\n";
  if (plan.cores.empty()) os << "  (none)\n";
  for (const SchedCorePlan& c : plan.cores) {
    os << "  ";
    if (c.core < 0) {
      os << "unassigned";
    } else {
      os << "core " << c.core;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", c.utilization);
    os << ": sum(C_i/T_i)=" << buf << " status=" << c.status;
    if (c.overloaded) os << " overloaded";
    else if (c.above_warn) os << " headroom-warning";
    os << " nodes=" << Join(c.nodes, ",");
    if (!c.unknown_nodes.empty()) {
      os << " unknown=" << Join(c.unknown_nodes, ",");
    }
    os << "\n";
  }
  os << "chains:\n";
  if (plan.chains.empty()) os << "  (none)\n";
  for (const SchedChainPlan& c : plan.chains) {
    os << "  sink " << c.sink << ": " << Join(c.nodes, " -> ")
       << " end_to_end=" << c.end_to_end_ns << " ns completion="
       << c.completion_ns << " ns deadline=";
    if (c.deadline_ns > 0) {
      os << c.deadline_ns << " ns";
    } else {
      os << "unknown";
    }
    os << " slack=";
    if (c.slack_known) {
      os << c.slack_ns << " ns";
    } else {
      os << "unknown";
    }
    if (!c.complete) os << " (incomplete: unknown WCET)";
    os << "\n";
  }
  os << "feedback:\n";
  if (plan.feedback_edges.empty()) os << "  (none)\n";
  for (const SchedFeedbackEdge& f : plan.feedback_edges) {
    os << "  " << f.from << " -> " << f.to << " (" << f.channel << ", delay "
       << f.delay_ns << " ns) loop: " << Join(f.loop_nodes, " -> ") << "\n";
  }
  os << "capacities:\n";
  if (plan.capacities.empty()) os << "  (none)\n";
  for (const SchedCapacityPlan& c : plan.capacities) {
    os << "  " << c.from << " -> " << c.to << " (" << c.channel << ", "
       << c.kind << "): rate_hz=" << c.rate_hz << " lag_ns=";
    if (c.lag_ns > 0) {
      os << c.lag_ns;
    } else {
      os << "unknown";
    }
    os << " lag_basis=" << c.lag_basis << " pin_budget=" << c.pin_budget
       << " capacity=" << c.capacity << " required_slots=";
    if (c.known) {
      os << c.required_slots << " suggested_capacity=" << c.suggested_capacity;
    } else {
      os << "unknown suggested_capacity=unknown";
    }
    os << " status=" << c.status;
    if (!c.unknown_reason.empty()) os << " reason=" << c.unknown_reason;
    os << "\n";
  }
  os << "admission: " << plan.admission_status
     << " capacity: " << plan.capacity_status << "\n";
  for (const std::string& n : plan.notes) os << "  note: " << n << "\n";
  for (const std::string& w : plan.warnings) os << "  " << w << "\n";
  for (const std::string& e : plan.errors) os << "  " << e << "\n";
  return os.str();
}

void PrintSchedDiagnostics(const SchedPlan& plan) {
  for (const std::string& e : plan.errors) {
    std::fprintf(stderr, "gen_graph: %s\n", e.c_str());
  }
  for (const std::string& w : plan.warnings) {
    std::fprintf(stderr, "%s\n", w.c_str());
  }
  // SCH-05: "not analyzed" is a real result, not a pass. Keep codegen
  // non-silent; --explain-sched carries the per-core detail.
  if (plan.admission_status == "not_analyzed" &&
      plan.options.admission_mode != SchedAdmissionMode::kOff) {
    std::fprintf(stderr,
                 "gen_graph: SCHED admission not analyzed; run "
                 "--explain-sched for per-core detail\n");
  }
}

int SchedExitCode(const SchedPlan& plan) {
  return plan.errors.empty() ? 0 : 2;
}

bool ParseSchedPriorityMode(const std::string& value, SchedPriorityMode* out) {
  if (value == "best-effort" || value == "best_effort") {
    *out = SchedPriorityMode::kBestEffort;
    return true;
  }
  if (value == "strict") {
    *out = SchedPriorityMode::kStrict;
    return true;
  }
  return false;
}

bool ParseSchedAdmissionMode(const std::string& value, SchedAdmissionMode* out) {
  if (value == "error") {
    *out = SchedAdmissionMode::kError;
    return true;
  }
  if (value == "warn") {
    *out = SchedAdmissionMode::kWarn;
    return true;
  }
  if (value == "off") {
    *out = SchedAdmissionMode::kOff;
    return true;
  }
  return false;
}

bool ParseSchedCapacityMode(const std::string& value, SchedCapacityMode* out) {
  if (value == "error") {
    *out = SchedCapacityMode::kError;
    return true;
  }
  if (value == "best_effort" || value == "best-effort") {
    *out = SchedCapacityMode::kBestEffort;
    return true;
  }
  return false;
}
