#include "meta.h"

#include "util.h"

#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>

namespace gen_meta {

namespace {

namespace fs = std::filesystem;

constexpr const char* kMetaFile = ".gen_meta.json";

// Canonical semantic view of the graph. `source` and the per-item `line`
// fields are diagnostics only and never emitted, so they are excluded here.
std::string CanonicalGraph(const Graph& g) {
  std::ostringstream os;
  os << "graph|" << g.version << '|' << g.pool_n << '|' << g.ring_cap << '|'
     << g.shm_prefix << '|' << g.health_n << '\n';
  for (const Node& n : g.nodes) {
    os << "node|" << n.id << '|' << n.type << '|' << n.process << '|' << n.hz
       << '|';
    for (std::size_t i = 0; i < n.cpu_set.size(); ++i) {
      if (i != 0) os << ',';
      os << n.cpu_set[i];
    }
    os << '|' << n.sched_policy << '|' << n.priority << '|'
       << n.placement_fail << '|' << n.wait_inputs << '|' << n.exec << '|'
       << n.publish << '|' << n.deadline_ns << '|' << n.wcet_ns;
    if (n.threads > 1) {
      os << '|' << n.threads;
    }
    os << '\n';
  }
  for (const Edge& e : g.edges) {
    os << "edge|" << e.from << '|' << e.to << '|' << e.channel << '|'
       << e.cpp_type << '|' << e.pool << '|' << e.transport << '|' << e.shm
       << '|' << e.delivery << '|' << e.read << '|' << e.record_priority
       << '|' << (e.record ? 1 : 0) << '|' << (e.record_gate ? 1 : 0) << '|'
       << e.depth << '|'
       << (e.mp_gate ? 1 : 0) << '|' << (e.feedback ? 1 : 0) << '|'
       << e.delay_ns;
    if (e.lossless || e.max_lag_ms > 0 || e.before_ms > 0 || e.after_ms > 0 ||
        e.per_frame_write_ms > 0 || e.pin_budget > 0) {
      os << '|' << (e.lossless ? 1 : 0) << '|' << e.max_lag_ms << '|'
         << e.before_ms << '|' << e.after_ms << '|' << e.per_frame_write_ms
         << '|' << e.pin_budget;
    }
    if (!e.record_kind.empty() || e.bitrate_kbps != 0 || e.avg_frame_bytes != 0 ||
        e.max_frame_bytes != 0 || !e.record_shm.empty() || e.dynamic) {
      os << '|' << e.record_kind << '|' << e.bitrate_kbps << '|'
         << e.avg_frame_bytes << '|' << e.max_frame_bytes << '|' << e.record_shm
         << '|' << (e.dynamic ? 1 : 0);
    }
    os << '\n';
  }
  for (const Process& p : g.processes) {
    os << "process|" << p.id << '|' << p.role << '\n';
  }
  return os.str();
}

std::uint64_t Fnv1a64(const std::string& s) {
  std::uint64_t h = 1469598103934665603ULL;
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

std::string Hex64(std::uint64_t v) {
  std::ostringstream os;
  os << std::hex << std::setw(16) << std::setfill('0') << v;
  return os.str();
}

// Position just past "key" ':' in the JSON body, or npos.
std::size_t AfterKey(const std::string& text, const std::string& key) {
  const std::string quoted = "\"" + key + "\"";
  const std::size_t k = text.find(quoted);
  if (k == std::string::npos) return std::string::npos;
  const std::size_t colon = text.find(':', k + quoted.size());
  if (colon == std::string::npos) return std::string::npos;
  return colon + 1;
}

bool ReadString(const std::string& text, const std::string& key,
                std::string* out) {
  std::size_t pos = AfterKey(text, key);
  if (pos == std::string::npos) return false;
  pos = text.find('"', pos);
  if (pos == std::string::npos) return false;
  const std::size_t end = text.find('"', pos + 1);
  if (end == std::string::npos) return false;
  *out = text.substr(pos + 1, end - pos - 1);
  return true;
}

bool ReadInt(const std::string& text, const std::string& key, int* out) {
  std::size_t pos = AfterKey(text, key);
  if (pos == std::string::npos) return false;
  while (pos < text.size() &&
         (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
          text[pos] == '\r')) {
    ++pos;
  }
  bool negative = false;
  if (pos < text.size() && text[pos] == '-') {
    negative = true;
    ++pos;
  }
  if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') return false;
  int value = 0;
  while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
    value = value * 10 + (text[pos] - '0');
    ++pos;
  }
  *out = negative ? -value : value;
  return true;
}

}  // namespace

std::string SourceGraphHash(const Graph& g) {
  return Hex64(Fnv1a64(CanonicalGraph(g)));
}

Meta ForGraph(const Graph& g) {
  Meta m;
  m.source_graph_hash = SourceGraphHash(g);
  return m;
}

std::string Render(const Meta& m) {
  std::ostringstream os;
  os << "{\n";
  os << "  \"tool\": \"" << m.tool << "\",\n";
  os << "  \"generator_version\": \"" << m.generator_version << "\",\n";
  os << "  \"format_version\": " << m.format_version << ",\n";
  os << "  \"source_graph_hash\": \"" << m.source_graph_hash << "\"\n";
  os << "}\n";
  return os.str();
}

bool Parse(const std::string& text, Meta* out) {
  if (out == nullptr) return false;
  Meta m;
  if (!ReadString(text, "tool", &m.tool)) return false;
  if (!ReadString(text, "generator_version", &m.generator_version)) {
    return false;
  }
  if (!ReadInt(text, "format_version", &m.format_version)) return false;
  if (!ReadString(text, "source_graph_hash", &m.source_graph_hash)) {
    return false;
  }
  *out = m;
  return true;
}

bool Read(const std::string& generated_dir, Meta* out) {
  const fs::path p = fs::path(generated_dir) / kMetaFile;
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) return false;
  std::string body;
  if (!gen_util::ReadFile(p, &body)) return false;
  return Parse(body, out);
}

std::string CompatibilityError(const Meta& m) {
  if (m.tool != kTool) {
    return "generated/" + std::string(kMetaFile) + ": produced by tool '" +
           m.tool + "', this generator is '" + kTool +
           "'; regenerate the tree instead of mixing generators";
  }
  if (m.format_version != kFormatVersion) {
    return "generated/" + std::string(kMetaFile) +
           ": produced by output format v" + std::to_string(m.format_version) +
           ", this generator emits v" + std::to_string(kFormatVersion) +
           "; a format change requires a full `--update generated` "
           "regeneration (generated/ is never patched by hand)";
  }
  if (m.generator_version != kGeneratorVersion) {
    return "generated/" + std::string(kMetaFile) +
           ": produced by gen_graph " + m.generator_version +
           ", this generator is " + kGeneratorVersion +
           "; regenerate with `--update generated` to resync (output format "
           "v" + std::to_string(kFormatVersion) + " is unchanged)";
  }
  return {};
}

std::string VersionSummary() {
  std::string s = kTool;
  s += " ";
  s += kGeneratorVersion;
  s += " (output format ";
  s += std::to_string(kFormatVersion);
  s += ")";
  return s;
}

std::string BannerStamp() { return "// " + VersionSummary() + "\n"; }

}  // namespace gen_meta
