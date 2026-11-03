#include "validate.h"

#include "util.h"
#include "thyme/contracts/abi.h"
#include "thyme/contracts/frame_header.h"

#include <cctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

// Contract cpp_type is emitted into thyme::Frame<...>: a plain or
// ::-qualified identifier (contract structs are never templates).
bool IsCppTypeName(const std::string& s) {
  if (s.empty()) return false;
  std::size_t start = 0;
  while (start <= s.size()) {
    const std::size_t sep = s.find("::", start);
    const std::string seg =
        (sep == std::string::npos) ? s.substr(start) : s.substr(start, sep - start);
    if (!gen_util::IsIdent(seg)) return false;
    if (sep == std::string::npos) return true;
    start = sep + 2;
  }
  return true;
}

// shm suffix lands in a POSIX shm name via MpName(prefix, suffix); keep it to
// portable name characters (no '/', no whitespace, no quotes).
bool IsShmSuffix(const std::string& s) {
  if (s.empty()) return false;
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (!std::isalnum(c) && ch != '_' && ch != '-') return false;
  }
  return true;
}

bool HasShmKind(const TemplateCatalog& cat, const std::string& cpp_type) {
  for (const auto& k : cat.shm_kinds) {
    if (k.cpp_type == cpp_type) return true;
  }
  return false;
}

std::string EdgeLabel(const Edge& e) {
  return e.from + "->" + e.to + " (" + e.channel + ")";
}

std::string Loc(const Graph& g, int line) { return GraphLoc(g, line); }

}  // namespace

bool ValidateGraphSemantics(const Graph& g, const TemplateCatalog* cat,
                            std::vector<std::string>* errors) {
  if (errors == nullptr) return false;
  errors->clear();

  // Unique node ids: emitters resolve nodes by first match (FindNodeById), so
  // a duplicate id silently aliases two nodes onto one health slot / member.
  // Node/process names land in generated C++ (class names, member names,
  // string literals), so they must be plain identifiers.
  std::set<std::string> node_ids;
  for (const auto& n : g.nodes) {
    if (!node_ids.insert(n.id).second) {
      errors->push_back(Loc(g, n.line) + "duplicate node id '" + n.id + "'");
    }
    if (!gen_util::IsIdent(n.type)) {
      errors->push_back(Loc(g, n.line) + "node " + n.id + ": type '" + n.type +
                        "' is not a valid C identifier (used for generated "
                        "class names)");
    }
    if (!n.process.empty() && !gen_util::IsIdent(n.process)) {
      errors->push_back(Loc(g, n.line) + "node " + n.id + ": process '" +
                        n.process + "' is not a valid C identifier");
    }
    // SCH-02: ThreadPlacement priorities are Linux RT priorities 1..99. The
    // top of the band (90..99) is reserved for system/watchdog threads, so a
    // graph may only claim 1..89 explicitly; rate-monotonic generation also
    // stays inside that band.
    if (n.priority >= 0) {
      if (n.priority < 1 || n.priority > 99) {
        errors->push_back(Loc(g, n.line) + "node " + n.id + ": priority " +
                          std::to_string(n.priority) +
                          " is outside the RT priority range 1..99");
      } else if (n.priority >= 90) {
        errors->push_back(Loc(g, n.line) + "node " + n.id + ": priority " +
                          std::to_string(n.priority) +
                          " is in the system-reserved band 90..99 "
                          "(graph priorities must be 1..89)");
      }
    }
    if (n.wcet_ns > 0 && n.wcet_ns > 1000000000ULL) {
      errors->push_back(Loc(g, n.line) + "node " + n.id + ": wcet_ns " +
                        std::to_string(n.wcet_ns) +
                        " exceeds 1 s; admission expects per-activation cost");
    }
    if (n.threads == 0 || n.threads > 64) {
      errors->push_back(Loc(g, n.line) + "node " + n.id +
                        ": threads must be in 1..64");
    }
    if (n.threads > 1 && n.exec != "event") {
      errors->push_back(Loc(g, n.line) + "node " + n.id +
                        ": threads > 1 requires exec=event");
    }
  }
  std::size_t physical_workers = 0;
  bool has_shards = false;
  for (const auto& n : g.nodes) {
    physical_workers += n.threads;
    has_shards = has_shards || n.shard_count > 1;
    if (n.threads == 1) {
      continue;
    }
    std::size_t inputs = 0;
    std::size_t outputs = 0;
    for (const auto& e : g.edges) {
      if (e.to == n.id) {
        ++inputs;
      }
      if (e.from == n.id) {
        ++outputs;
      }
    }
    if (outputs != 0) {
      errors->push_back(Loc(g, n.line) + "node " + n.id +
                        ": sharded node must be a sink (no out-edges)");
    }
    if (inputs == 0 || n.threads > inputs) {
      errors->push_back(Loc(g, n.line) + "node " + n.id +
                        ": threads must not exceed its non-zero in-edge count");
    }
    for (uint32_t shard = 0; shard < n.threads; ++shard) {
      const std::string id = n.id + "__s" + std::to_string(shard);
      if (node_ids.count(id) != 0) {
        errors->push_back(Loc(g, n.line) +
                          "physical worker name collision: " + id);
      }
    }
  }
  if (has_shards && g.health_n > 0 &&
      physical_workers > static_cast<std::size_t>(g.health_n)) {
    errors->push_back("supervisor.health_n is smaller than physical worker count " +
                      std::to_string(physical_workers));
  }
  if (has_shards && g.health_n > static_cast<int>(thyme::kAbiMaxThreads)) {
    errors->push_back("supervisor.health_n exceeds kAbiMaxThreads");
  }
  std::set<std::string> process_ids;
  for (const auto& p : g.processes) {
    if (!gen_util::IsIdent(p.id)) {
      errors->push_back(Loc(g, p.line) + "process id '" + p.id +
                        "' is not a valid C identifier");
    } else if (!process_ids.insert(p.id).second) {
      errors->push_back(Loc(g, p.line) + "duplicate process id '" + p.id + "'");
    }
  }

  // Edge endpoints must reference declared nodes; per-edge names must be
  // legal for the contexts they are emitted into.
  for (const auto& e : g.edges) {
    const std::string label = EdgeLabel(e);
    const std::string at = Loc(g, e.line);
    if (node_ids.find(e.from) == node_ids.end()) {
      errors->push_back(at + "edge " + label + ": from '" + e.from +
                        "' is not a declared node");
    }
    if (node_ids.find(e.to) == node_ids.end()) {
      errors->push_back(at + "edge " + label + ": to '" + e.to +
                        "' is not a declared node");
    }
    if (e.channel.empty()) {
      errors->push_back(at + "edge " + label + ": missing channel name");
    } else if (!gen_util::IsIdent(e.channel)) {
      errors->push_back(at + "edge " + label + ": channel name '" + e.channel +
                        "' is not a valid C identifier (used for generated "
                        "member names)");
    }
    if (e.cpp_type.empty()) {
      errors->push_back(at + "edge " + label + ": missing cpp_type");
    } else if (!IsCppTypeName(e.cpp_type)) {
      errors->push_back(at + "edge " + label + ": cpp_type '" + e.cpp_type +
                        "' is not a valid C++ type name");
    }
    if (!e.pool.empty() && !gen_util::IsIdent(e.pool)) {
      errors->push_back(at + "edge " + label + ": pool name '" + e.pool +
                        "' is not a valid C identifier");
    }
    if (!e.shm.empty() && !IsShmSuffix(e.shm)) {
      errors->push_back(at + "edge " + label + ": shm suffix '" + e.shm +
                        "' has illegal characters (allowed: [A-Za-z0-9_-]; "
                        "used in POSIX shm names)");
    }
  }

  // One channel name = one data contract: broadcast fan-out legally shares a
  // channel across edges, but every edge on it must carry the same cpp_type.
  {
    std::map<std::string, std::string> channel_type;
    for (const auto& e : g.edges) {
      const auto it = channel_type.emplace(e.channel, e.cpp_type);
      if (!it.second && it.first->second != e.cpp_type) {
        errors->push_back(Loc(g, e.line) + "edge " + EdgeLabel(e) +
                          ": channel '" + e.channel + "' carries both cpp_type '" +
                          it.first->second + "' and '" + e.cpp_type + "'");
      }
    }
  }

  // GEN-12: record is a property of the channel, not of one edge. Broadcast
  // fan-out shares a channel across edges; a mixed record flag would make the
  // manifest entry's presence depend on edge order, so reject it.
  {
    std::map<std::string, std::pair<bool, int>> channel_record;
    for (const auto& e : g.edges) {
      const auto it = channel_record.emplace(e.channel, std::make_pair(e.record, e.line));
      if (!it.second && it.first->second.first != e.record) {
        errors->push_back(Loc(g, e.line) + "edge " + EdgeLabel(e) +
                          ": channel '" + e.channel +
                          "' mixes record: true and record: false (record is "
                          "per channel; keep every edge on the channel "
                          "consistent)");
      }
    }
  }

  // Member-name collisions. Broadcast and generated local fan-out share one
  // producer endpoint; other duplicate channel names still collide in Ports.
  {
    const auto local_event_fanout = [&g](const Edge& edge) {
      if (!edge.shm.empty()) return false;
      const Node* from = nullptr;
      const Node* to = nullptr;
      for (const auto& node : g.nodes) {
        if (node.id == edge.from) from = &node;
        if (node.id == edge.to) to = &node;
      }
      return from != nullptr && to != nullptr && from->process == to->process &&
             to->exec == "event";
    };
    std::map<std::pair<std::string, std::string>, const Edge*> producer_ch;
    std::map<std::pair<std::string, std::string>, const Edge*> consumer_ch;
    for (const auto& e : g.edges) {
      if (e.delivery != "broadcast" && !local_event_fanout(e)) {
        const auto key = std::make_pair(e.from, e.channel);
        const auto it = producer_ch.emplace(key, &e);
        if (!it.second) {
          errors->push_back(
              Loc(g, e.line) + "edges " + EdgeLabel(*it.first->second) + " and " +
              EdgeLabel(e) + " share producer channel '" + e.channel +
              "' (generated Ports members collide; give each consumer its own "
              "channel or use delivery: broadcast fan-out)");
        }
      }
      const auto key = std::make_pair(e.to, e.channel);
      const auto it = consumer_ch.emplace(key, &e);
      if (!it.second) {
        errors->push_back(Loc(g, e.line) + "edges " +
                          EdgeLabel(*it.first->second) + " and " + EdgeLabel(e) +
                          " share consumer channel '" + e.channel +
                          "' (generated Ports/pipe members collide)");
      }
    }
  }

  // Missing shm kind is a hard error for delivery=latest shm edges: without a
  // kind the edge is silently dropped from kGraphShmChannels /
  // kGraphShmSlotSizes, so the supervisor never creates the LatestSlot and the
  // channel only fails at runtime. queue/broadcast rings are sized from
  // cpp_type directly and need no kind.
  if (cat != nullptr) {
    for (const auto& e : g.edges) {
      if (e.delivery != "latest" || e.shm.empty()) continue;
      const ContractSizeCat* size = FindContractSize(*cat, e.cpp_type);
      const bool pinned_slot = EdgeRecordsBulk(e) ||
          (size != nullptr && sizeof(thyme::FrameHeader) + size->bytes >
                                  thyme::kAbiMaxTransportBytes);
      if (pinned_slot) continue;
      if (!HasShmKind(*cat, e.cpp_type)) {
        errors->push_back(Loc(g, e.line) + "edge " + EdgeLabel(e) +
                          ": cpp_type '" + e.cpp_type +
                          "' has no shm kind in the template catalog (add a "
                          "'cpp_type|kind' row to shm_kinds.txt)");
      }
    }
  }

  // ARCH-19: oversized contracts route to PinnedSlot instead of being rejected.
  // Lossless is the incompatible part: lease overwrite and pinned-skip cannot
  // promise every frame, so reject that combination before any emitter runs.
  if (cat != nullptr) {
    const std::size_t kHeaderBytes = sizeof(thyme::FrameHeader);
    for (const auto& e : g.edges) {
      const ContractSizeCat* size = FindContractSize(*cat, e.cpp_type);
      const bool oversized =
          size != nullptr && kHeaderBytes + size->bytes > thyme::kAbiMaxTransportBytes;
      if (e.lossless && (oversized || EdgeRecordsBulk(e))) {
        errors->push_back(
            Loc(g, e.line) + "edge " + EdgeLabel(e) +
            ": lossless delivery is incompatible with PinnedSlot lease "
            "overwrite/pinned-skip");
      }
    }
  }

  return errors->empty();
}
