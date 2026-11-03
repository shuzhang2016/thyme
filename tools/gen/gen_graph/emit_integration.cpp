#include "emit.h"

#include "emit_internal.h"
#include "meta.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace gen_emit;

namespace gen_emit {

void AppendChecklistItems(std::ostringstream& os, const std::string& checklist) {
  std::size_t start = 0;
  while (start < checklist.size()) {
    std::size_t sep = checklist.find(';', start);
    const std::size_t end = (sep == std::string::npos) ? checklist.size() : sep;
    std::string item = checklist.substr(start, end - start);
    while (!item.empty() &&
           std::isspace(static_cast<unsigned char>(item.front()))) {
      item.erase(item.begin());
    }
    while (!item.empty() &&
           std::isspace(static_cast<unsigned char>(item.back()))) {
      item.pop_back();
    }
    if (!item.empty()) os << "- [ ] " << item << "\n";
    if (sep == std::string::npos) break;
    start = sep + 1;
  }
}

}  // namespace gen_emit

bool EmitIntegrationGuide(const std::string& tmp_dir, const Graph& g,
                          const TemplateCatalog& cat,
                          const std::string& tpl_display,
                          const EdgeManifest& m) {
  std::ostringstream os;
  os << "<!-- Code generated for the " << cat.banner << ". DO NOT EDIT. -->\n";
  os << "<!-- " << gen_meta::VersionSummary() << " -->\n";
  os << "<!-- Hand integration: src/nodes/, include/.../contracts/, config/viz/ -->\n\n";
  os << "# Integration guide (this graph)\n\n";
  os << "Generated automatically from `graph.yaml` and the template catalog. "
        "The general workflow is described in "
        "`tools/gen/gen_graph/INTEGRATION.md` in the thyme repository; "
        "**this file covers only the manual steps and CLI parameters for the "
        "current graph.**\n\n";
  os << "Regenerate with:\n\n";
  os << "```bash\ngen_graph --in graph.yaml --out . --update generated";
  if (tpl_display.empty()) {
    os << " [--template-dir <dir>]\n```\n\n";
  } else {
    // Show only the dir name so the doc is identical whether the caller
    // passed a relative ("template") or absolute (".../template") path —
    // golden comparisons must not depend on the invocation cwd.
    os << " --template-dir " << std::filesystem::path(tpl_display).filename().string()
       << "\n```\n\n";
  }

  os << "## Graph summary\n\n";
  os << "| Item | Value |\n|------|-------|\n";
  os << "| nodes | " << g.nodes.size() << " |\n";
  os << "| edges | " << m.edges.size() << " |\n";
  os << "| processes | " << g.processes.size() << " |\n";
  os << "| pool_n | " << g.pool_n << " |\n";
  os << "| ring_cap | " << g.ring_cap << " |\n";
  os << "| health_n | " << g.health_n << " |\n";
  os << "| record topics | " << m.recorded.size()
     << " (`kRecordRegistryCount`) |\n";
  os << "| shm_prefix | `" << g.shm_prefix << "` |\n\n";

  os << "`hz`: a **cyclic** node's value is its loop period in Hz (`1/period`); "
        "an **event** node's value is a lower bound on expected steady-state "
        "messages/s (the same order as `NoteHandled`), **not** a cyclic period "
        "and not `channels ×` upstream declared hz. The supervisor's "
        "`MinLoopsForProcess` uses the process **min hz** × seconds / 4, so a "
        "single node is not required to saturate production.\n\n";

  os << "## Generated vs manual\n\n";
  os << "| Category | Path | Notes |\n|----------|------|-------|\n";
  os << "| Topology glue | `generated/wiring.*`, `generated/*_generated.h` | Regenerate; do not edit by hand |\n";
  os << "| Record manifest | `generated/record_manifest.h` | Ordered recorded-channel table (id/hash); inspect the diff after regeneration |\n";
  os << "| Record registry | `generated/record_registry.h`, `record_table.h`, "
        "`record_tap_helpers.h` | struct_id / sizeof / RecordTap* |\n";
  os << "| Replay | `generated/replay_inject.h`, `replay_stages.h` | "
        "`ReplayInjectOne` / stage filtering |\n";
  os << "| Multi-process tables | `generated/graph_ir.h` | SHM suffixes and process ownership |\n";
  os << "| Contract types | `include/.../contracts/` (template provides stubs) | "
        "**Edit payload evolution here**; regeneration is usually unnecessary |\n";
  os << "| Algorithm + SDK seam | `src/nodes/<id>.cpp` | **Implement Loop() and SDK integration here** |\n";
  os << "| Viz configuration | `config/viz/viz.yaml` | Channel allowlist (see below) |\n";
  os << "| Recorder/control binaries | `tools/recorder/`, `tools/recorderctl/` | "
        "Thin shells; the logic lives in `thyme::recorder` |\n\n";

  os << "## Node and SDK integration (manual)\n\n";
  os << "Rule (all nodes): inside `Loop()` use **acquire → write slot → publish → release inputs**; "
        "never pass raw SDK pointers through a ring; call `RecordTap*` only **after publish** "
        "(if there is an output).\n\n";

  for (const auto& n : g.nodes) {
    const KnownCat* known = FindKnown(cat, n.type);
    const IntegrationHintCat* hint = FindIntegrationHint(cat, n.type);
    const std::string cls =
        known ? known->cls : (PascalCase(n.type) + "Thread");

    os << "### `" << n.id << "` — " << cls << "\n\n";
    os << "| Property | Value |\n|----------|-------|\n";
    os << "| type | `" << n.type << "` |\n";
    os << "| Hz | " << n.hz << " |\n";
    os << "| process | `" << n.process << "` |\n";
    if (!n.wait_inputs.empty())
      os << "| wait_inputs | `" << n.wait_inputs << "` |\n";
    if (hint != nullptr && !hint->sdk_label.empty())
      os << "| SDK / role | " << hint->sdk_label << " |\n";
    os << "| Hand-written file | `src/nodes/" << n.id << ".cpp` |\n";
    os << "| Generated header | `generated/" << n.id << "_generated.h` |\n\n";

    os << "**Input edges (consume):**\n\n";
    bool any_in = false;
    for (const auto& r : m.edges) {
      if (r.edge->to != n.id) continue;
      any_in = true;
      os << "- `" << r.edge->channel << "` ← `" << r.edge->from << "` · `"
         << r.edge->cpp_type << "` · pool `" << r.edge->pool << "`";
      if (!r.edge->shm.empty()) os << " · shm `" << r.edge->shm << "`";
      os << "\n";
    }
    if (!any_in) os << "- (no data-edge inputs)\n";
    for (const auto& x : cat.extras) {
      if (ExtraBufListed(x.read_types, n.type)) {
        os << "- (optional) read `" << x.cpp_type << "` latest slot\n";
      }
      if (ExtraBufListed(x.write_types, n.type)) {
        os << "- (optional) write `" << x.cpp_type << "`\n";
      }
    }
    os << "\n";

    os << "**Output edges (produce + record tap):**\n\n";
    bool any_out = false;
    bool any_record_out = false;
    for (const auto& r : m.edges) {
      if (r.edge->from != n.id) continue;
      any_out = true;
      os << "- `" << r.edge->channel << "` → `" << r.edge->to << "` · `"
         << r.edge->cpp_type << "`";
      if (r.recorded) {
        any_record_out = true;
        os << " · `RecordTap" << PascalCase(r.edge->channel)
           << "(page, pod)`";
      } else {
        os << " · `record: false` (no RecordTap / registry slot generated)";
      }
      os << "\n";
    }
    if (!any_out) os << "- (no output data edge)\n";
    os << "\n";

    os << "**Catalog contracts (preflight / graph_check):**\n\n";
    bool any_contract = false;
    for (const auto& c : cat.contracts) {
      if (c.type != n.type) continue;
      any_contract = true;
      os << "- [ ] " << (c.is_out ? "Output" : "Input") << " `" << c.channel
         << "` is connected in graph.yaml\n";
    }
    if (!any_contract)
      os << "- (the template declares no contracts; yaml edges are authoritative)\n";
    os << "\n";

    os << "**Manual checklist:**\n\n";
    os << "- [ ] Implement `" << cls << "::Loop()`\n";
    if (any_record_out) {
      os << "- [ ] For each output, call the matching `RecordTap*` after `publish()` "
            "(see `generated/record_tap_helpers.h`)\n";
    }
    if (hint != nullptr && !hint->checklist.empty()) {
      AppendChecklistItems(os, hint->checklist);
    } else if (known != nullptr && known->flags.find("noworker") != std::string::npos) {
      os << "- [ ] Aggregate `HealthSlot`; no SDK and no record tap\n";
    } else if (known == nullptr) {
      os << "- [ ] Unknown type: define the SDK seam and I/O yourself\n";
    } else {
      os << "- [ ] Materialize SDK output into a pool slot (`WriteInto` / memcpy); "
            "lifetime is limited to this `Loop()`\n";
    }
    os << "\n";
  }

  os << "## Recording — topics for this graph\n\n";
  os << "See `generated/record_registry.h` for the registry. "
        "The recorder CLI is documented in `tools/gen/gen_graph/README.md` "
        "(recorder-cli section) in the thyme repository.\n\n";

  if (m.recorded.empty()) {
    os << "This graph has **no recorded channels**, so `kRecordRegistryCount=0`; "
          "the recorder still compiles but the mirror has no data topics. "
          "(Edges with `record: false` still use their in-process transport; they "
          "simply do not enter the manifest.)\n\n";
  } else {
    os << "Topic order defines `RecordStructId`: recorded channels are sorted by "
          "channel name (see `generated/record_manifest.h`).\n\n";
    os << "### Topic table\n\n";
    os << "| id | channel | cpp_type | producer | process | priority |\n";
    os << "|----|---------|----------|----------|---------|----------|\n";
    for (std::size_t i = 0; i < m.recorded.size(); ++i) {
      const auto& r = m.recorded[i];
      os << "| " << i << " | `" << r.edge->channel << "` | `" << r.edge->cpp_type
         << "` | `" << r.edge->from << "` | `" << r.from_process << "` | "
         << r.record_label << " |\n";
    }
    os << "\n";

    os << "### Example commands (replace `<pid>` / paths)\n\n";
    os << "**Continuous recording (single process or attach to a shared mirror):**\n\n";
    os << "```bash\n";
    os << "# The default mirror name is project-specific; for the MP dummy it is /c9_<supervisor_pid>_rec\n";
    os << "recorder --shm /thyme_rec_<pid> --out /data/sess.thyme.bin --seconds 300\n";
    os << "```\n\n";

    os << "**Filter by topic** (channel or cpp_type names, comma-separated):\n\n";
    os << "```bash\nrecorder --shm /thyme_rec_<pid> --out /data/subset.bin \\\n";
    os << "  --topics ";
    for (std::size_t i = 0; i < m.recorded.size(); ++i) {
      if (i) os << ",";
      os << m.recorded[i].edge->channel;
    }
    os << " --seconds 60\n```\n\n";

    os << "**Triggered recording plus control plane:**\n\n";
    os << "```bash\n";
    os << "recorder --shm /c9_<pid>_rec --trigger --out-dir /data/triggers \\\n";
    os << "  --pre 30 --post 60 --settle-ms 500 \\\n";
    os << "  --control-socket /tmp/thyme_recorder.sock --seconds 86400\n\n";
    os << "thyme-recorderctl --socket /tmp/thyme_recorder.sock trigger\n";
    os << "```\n\n";
    os << "On the module side, `record_trigger::Request(reason)` goes through the "
          "supervisor's `--relay-recorder-socket` (same socket path). **Do not** "
          "connect directly to the recorder from `Loop()`.\n\n";
  }

  os << "## Replay — stages for this graph\n\n";
  if (m.recorded.empty()) {
    os << "No inject stages.\n\n";
  } else {
    std::set<std::string> stages;
    for (const auto& r : m.recorded) stages.insert(r.edge->to);
    os << "Allowed `--inject-stage` values (consumer node ids): ";
    bool first = true;
    for (const auto& s : stages) {
      if (!first) os << ", ";
      first = false;
      os << "`" << s << "`";
    }
    os << ". See `generated/replay_stages.h` for the mapping.\n\n";
    os << "```bash\n";
    os << "replay_tool --bin /data/sess.thyme.bin --inject-stage fusion --as-fast\n";
    os << "ad_stack --replay /data/sess.thyme.bin  # default: inject into paused producers only\n";
    os << "replay_tool --bin /data/sess.thyme.bin --inject-all --as-fast  # full bin replay\n";
    os << "```\n\n";
  }

  const bool has_cross_shm =
      g.processes.size() > 1 || !m.shm_edges.empty();
  if (has_cross_shm) {
    os << "## Multi-process (--mp) — this graph\n\n";
    os << "| process | role |\n|---------|------|\n";
    for (const auto& p : g.processes)
      os << "| `" << p.id << "` | `" << p.role << "` |\n";
    os << "\n**Cross-process SHM edges** (suffixes from yaml; table in `generated/graph_ir.h`):\n\n";
    bool any_shm = false;
    for (const auto& r : m.shm_edges) {
      any_shm = true;
      os << "- `" << r.edge->channel << "` · suffix `" << r.edge->shm << "` · "
         << r.edge->from << " (" << (r.from != nullptr ? r.from->process : "?")
         << ") → " << r.edge->to << " ("
         << (r.to != nullptr ? r.to->process : "?") << ")\n";
    }
    if (!any_shm) os << "- (single-process graph or no shm suffix)\n";
    os << "\n```bash\n";
    os << "ad_stack --mp --seconds 2\n";
    os << "ad_stack --mp --record --seconds 2\n";
    os << "# mirror: /c9_<supervisor_pid>_rec\n";
    os << "ad_stack --mp --record --relay-recorder-socket /tmp/thyme_recorder.sock\n";
    os << "```\n\n";
  }

  os << "## Visualization (viz_bridge) — channel allowlist for this graph\n\n";
  os << "Enable these in `channels:` in `config/viz/viz.yaml`. Field-level decoding is "
        "provided by codegen (`generated/viz_ser.h` plus contract `contracts_json.h`), "
        "so the PC side no longer needs a user script:\n\n";
  os << "```yaml\nchannels:\n";
  for (const auto& r : m.recorded) os << "  - " << r.edge->channel << "\n";
  os << "```\n\n";
  os << "```bash\nviz_bridge --shm /thyme_rec_<pid> --config config/viz/viz.yaml\n```\n\n";

  os << "## Integration verification (suggested for this graph)\n\n";
  os << "- [ ] `cmake --build` passes after `gen_graph --update generated`\n";
  os << "- [ ] No static_assert failures in `generated/graph_check.h`\n";
  if (!m.recorded.empty()) {
    os << "- [ ] Every producer calls `RecordTap*`\n";
    os << "- [ ] `recorder --shm ... --out /tmp/t.bin --seconds 5` produces a non-empty bin\n";
  }
  if (has_cross_shm) os << "- [ ] `ad_stack --mp --seconds 2` exits 0\n";
  os << "- [ ] Run the project's CTest cases related to record/mp/viz\n";

  return WriteFile(fs::path(tmp_dir) / "integration.md", os.str());
}
