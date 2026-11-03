// test_trigger_config_doc.cpp — DOC-08 config-schema documentation sync.
//
// The machine-readable audit table in docs/guides/trigger-config.md (between
// the DOC-08-AUDIT-BEGIN/END markers) is cross-checked against the parser:
//
//   1. A maximal config exercising every section is parsed and rendered with
//      DumpConfig; the dump's (section, key) set must equal the table's
//      `implemented` rows exactly (both directions). The dump is the parser's
//      effective-key seam: TRG-17 requires every accepted key to materialise
//      there, so a key added to the parser without DumpConfig/docs fails here.
//   2. `deprecated` rows must still parse (with a warning) and must NOT appear
//      in the dump (only the canonical spelling does).
//   3. `rejected`/`future` rows must be parse errors when injected into their
//      section (no accepted-but-ignored keys, and no documented-but-
//      unimplemented key shown without its marker).
//   4. Example boot check: every shipped example .ini under
//      THYME_EXAMPLE_CONFIG_DIR must load through the real parser/validation
//      path (LoadCenterConfig) and its effective dump must round-trip.
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "thyme/check.h"
#include "thyme/record/trigger_config.h"

using namespace thyme;

namespace {

struct DocRow {
  std::string section;
  std::string key;
  std::string status;
};

std::string Trim(const std::string &s) {
  const std::size_t a = s.find_first_not_of(" \t`");
  if (a == std::string::npos) return "";
  const std::size_t b = s.find_last_not_of(" \t`");
  return s.substr(a, b - a + 1);
}

std::vector<DocRow> ReadAuditTable(const char *path) {
  std::ifstream in(path);
  CHECK(in.good());
  std::vector<DocRow> rows;
  bool in_audit = false;
  bool in_schema = false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.find("DOC-08-AUDIT-BEGIN") != std::string::npos) {
      in_audit = true;
      continue;
    }
    if (line.find("DOC-08-AUDIT-END") != std::string::npos) {
      in_audit = false;
      continue;
    }
    // ARCH-13 keeps per-section schema blocks outside the audit block; a key
    // documented only there is still documented, so scan those too instead of
    // stopping at DOC-08-AUDIT-END.
    if (line.find("TRIGGER-CONFIG-SCHEMA-") != std::string::npos) {
      in_schema = line.find("-BEGIN") != std::string::npos;
      continue;
    }
    if (!in_audit && !in_schema) continue;
    if (line.empty() || line.front() != '|')
      continue;
    std::vector<std::string> cells;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, '|'))
      cells.push_back(Trim(cell));
    // Leading/trailing '|' produce empty edge cells.
    if (cells.size() >= 5 && cells.front().empty()) {
      cells.erase(cells.begin());
    }
    if (cells.size() < 4)
      continue;
    if (cells[0] == "section" || cells[0].find("---") != std::string::npos) {
      continue;
    }
    rows.push_back(DocRow{cells[0], cells[1], cells[2]});
  }
  // The markers must exist and at least one row must be parsed; a silently
  // empty table would make the two-directional check vacuous.
  CHECK(!rows.empty());
  return rows;
}

std::string NormalizeSection(const std::string &header) {
  const std::string name = header.substr(1, header.size() - 2);
  for (const char *prefix : {"channel.", "source.", "quota."}) {
    const std::string p(prefix);
    if (name.compare(0, p.size(), p) == 0) return p + "*";
  }
  return name;
}

using KeySet = std::set<std::pair<std::string, std::string>>;

KeySet DumpKeys(const std::string &dump) {
  KeySet out;
  std::stringstream ss(dump);
  std::string line;
  std::string section;
  while (std::getline(ss, line)) {
    if (line.empty() || line.front() == '#')
      continue;
    if (line.front() == '[') {
      section = NormalizeSection(line);
      continue;
    }
    const std::size_t eq = line.find('=');
    CHECK(eq != std::string::npos);
    CHECK(!section.empty());
    out.insert({section, line.substr(0, eq)});
  }
  return out;
}

// Every implemented key set, all sections present, all cross-checks
// satisfiable (selectors match channels, slice fits the quota, wait_idle has
// a real [chassis], audit path differs from both sockets).
const char *kMaximal = R"(
[trigger_center]
socket = /tmp/thyme_doc_trigger.sock
max_concurrent_captures = 1
tick_ms = 2
socket_poll_budget_us = 250
max_trigger_rate_hz = 20000
dedup_window_entries = 2048
dedup_window_ms = 30000
default_source = manual
allow_empty_sources = true
crash_priority = 100

[security]
allowed_uids = 1000, 1001
allow_root = true
require_peer_pid = false
max_priority_hint = 500
allow_legacy_unsequenced = true
allow_legacy_v2 = true
allow_legacy_control = true
signing_key = /tmp/thyme_doc_key
audit_log = /tmp/thyme_doc.audit
control_audit_log = /tmp/thyme_doc.control.audit
audit_max_bytes = 1048576
audit_rotate_keep = 3
audit_fsync_interval_s = 1.5

[recorder]
output_dir = /tmp/thyme_doc_out
socket = /tmp/thyme_doc_recorder.sock
pending_bytes_quota = 1048576
flush_policy = realtime
idle_hold_s = 5
idle_fallback = assume_idle
io_rate_limit_mbps = 50
disk_quota_mb = 4096
high_reserved_mb = 128
incomplete_policy = delete_after_age
incomplete_max_age_s = 60

[channel./h264_front]
kind = h264_stream
name = h264_front
rate_hz = 30
avg_frame_bytes = 100000
bitrate_kbps = 4000
crc = package

[channel./thyme_recr_can]
kind = mirror_ring
name = can_chassis
record_channels = can_chassis
rate_hz = 50
avg_frame_bytes = 128
bitrate_kbps = 0
crc = frame

[source.manual]
channels = /h264_front, /thyme_recr_can
before_s = 10
after_s = 5
priority = 10
mute_s = 1
flush_policy = idle
quota_class = manual

[quota.manual]
max_triggers_per_hour = 10

[chassis]
page_shm = /thyme_chassis
struct_id = 1
epb_offset = 8
speed_offset = 16
speed_threshold = 0.5

[continuous]
auto_arm = false
channels = small_struct:*
slice_mb = 512
slice_s = 60
wait_idle = true
demo_idle = false
rate_check = warn
)";

std::string ProbeHeader(const std::string &section) {
  if (section == "channel.*") return "[channel./h264_front]";
  if (section == "source.*") return "[source.manual]";
  if (section == "quota.*") return "[quota.manual]";
  return "[" + section + "]";
}

// kMaximal with `key = value` injected at the top of the given section.
std::string Inject(const std::string &section, const std::string &key,
                   const std::string &value) {
  std::string text(kMaximal);
  const std::string header = ProbeHeader(section);
  const std::size_t at = text.find(header);
  CHECK(at != std::string::npos);
  text.insert(at + header.size(), "\n" + key + " = " + value);
  return text;
}

}  // namespace

int main() {
  const std::vector<DocRow> rows = ReadAuditTable(THYME_DOC_TRIGGER_CONFIG);
  CHECK(!rows.empty());

  trigger_cfg::CenterConfig cfg;
  std::string err, warn;
  CHECK(trigger_cfg::ParseCenterConfig(kMaximal, &cfg, &err, &warn));
  const KeySet dump = DumpKeys(trigger_cfg::DumpConfig(cfg));

  std::set<std::pair<std::string, std::string>> implemented;
  int n_implemented = 0, n_deprecated = 0, n_rejected = 0, n_future = 0;
  for (const DocRow &row : rows) {
    if (row.status == "implemented") {
      ++n_implemented;
      if (dump.count({row.section, row.key}) == 0) {
        std::fprintf(stderr,
                     "doc table marks [%s] %s implemented but it is missing "
                     "from the effective DumpConfig output\n",
                     row.section.c_str(), row.key.c_str());
        CHECK(false);
      }
      implemented.insert({row.section, row.key});
    } else if (row.status == "deprecated") {
      ++n_deprecated;
      // Still parses (with a warning); the dump shows only the canonical key.
      trigger_cfg::CenterConfig probe;
      std::string perr, pwarn;
      CHECK(trigger_cfg::ParseCenterConfig(Inject(row.section, row.key, "true"),
                                           &probe, &perr, &pwarn));
      CHECK(!pwarn.empty());
      const KeySet probe_dump = DumpKeys(trigger_cfg::DumpConfig(probe));
      CHECK(probe_dump.count({row.section, row.key}) == 0);
    } else if (row.status == "rejected" || row.status == "future") {
      if (row.status == "rejected")
        ++n_rejected;
      if (row.status == "future")
        ++n_future;
      // Must be a parse error — never accepted-but-ignored.
      trigger_cfg::CenterConfig probe;
      std::string perr;
      if (trigger_cfg::ParseCenterConfig(Inject(row.section, row.key, "1"),
                                         &probe, &perr, nullptr)) {
        std::fprintf(stderr,
                     "doc table marks [%s] %s as %s but the parser accepts "
                     "it\n",
                     row.section.c_str(), row.key.c_str(), row.status.c_str());
        CHECK(false);
      }
    } else {
      std::fprintf(stderr, "unknown audit status %s for [%s] %s\n",
                   row.status.c_str(), row.section.c_str(), row.key.c_str());
      CHECK(false);
    }
  }

  // Reverse direction: every effective parser key is documented.
  for (const auto &sk : dump) {
    if (implemented.count(sk) == 0) {
      std::fprintf(stderr,
                   "parser accepts [%s] %s (effective dump) but the doc audit "
                   "table does not list it as implemented\n",
                   sk.first.c_str(), sk.second.c_str());
      CHECK(false);
    }
  }
  std::printf(
      "audit table: %d implemented, %d deprecated, %d rejected, "
      "%d future; dump keys: %zu\n",
      n_implemented, n_deprecated, n_rejected, n_future, dump.size());

  // Example boot check: every shipped example .ini must load through the real
  // parser + validation path, and its effective dump must round-trip.
  int examples = 0;
  for (const auto &entry :
       std::filesystem::directory_iterator(THYME_EXAMPLE_CONFIG_DIR)) {
    if (entry.path().extension() != ".ini") continue;
    ++examples;
    trigger_cfg::CenterConfig ex;
    std::string lerr, lwarn;
    if (!trigger_cfg::LoadCenterConfig(entry.path().c_str(), &ex, &lerr,
                                       &lwarn)) {
      std::fprintf(stderr, "shipped example %s fails to load: %s\n",
                   entry.path().c_str(), lerr.c_str());
      CHECK(false);
    }
    const std::string dump1 = trigger_cfg::DumpConfig(ex);
    trigger_cfg::CenterConfig ex2;
    std::string rerr;
    if (!trigger_cfg::ParseCenterConfig(dump1, &ex2, &rerr, nullptr)) {
      std::fprintf(stderr, "dump of %s does not re-parse: %s\n",
                   entry.path().c_str(), rerr.c_str());
      CHECK(false);
    }
    CHECK(trigger_cfg::DumpConfig(ex2) == dump1);
    std::printf("example %s: loads OK, dump round-trips\n",
                entry.path().filename().c_str());
  }
  CHECK(examples > 0);

  std::puts("test_trigger_config_doc: OK");
  return 0;
}
