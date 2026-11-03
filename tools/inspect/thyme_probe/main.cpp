// thyme_probe — read-only flow view over thyme SHM containers (ECO-09/ECO-11).
//
// The binary is deliberately not called `monitor`: the system already has an
// event-aggregation component under that name (chain health). This tool never
// subscribes, never writes a page, never takes a PinnedSlot lease and never
// advances a shared consumer cursor; see include/thyme/probe/probe.h.

#include "thyme/probe/abi_matrix.h"
#include "thyme/probe/probe.h"
#include "thyme/runtime/mp_session_identity_page.h"
#include "thyme/probe/schema_view.h"
#include "thyme/runtime/health_slot.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using thyme::probe::AbiCheck;
using thyme::probe::AbiReport;
using thyme::probe::EchoSample;
using thyme::probe::ExitCode;
using thyme::probe::KindName;
using thyme::probe::LatencyStats;
using thyme::probe::ManifestEntry;
using thyme::probe::Options;
using thyme::probe::ProbeApi;
using thyme::probe::RateStats;
using thyme::probe::RenderSchemaViewJson;
using thyme::probe::RenderSchemaViewText;
using thyme::probe::SchemaView;
using thyme::probe::RegionInfo;
using thyme::probe::Sample;
using thyme::probe::SampleOptions;
using thyme::probe::Status;
using thyme::probe::StatusName;

namespace {

constexpr uint64_t kMs = 1000000ULL;

void PrintUsage(FILE* out);
void PrintCommandUsage(FILE* out, const std::string& cmd);

std::string JStr(const std::string& v) {
  std::string o = "\"";
  for (char c : v) {
    switch (c) {
      case '"':
        o += "\\\"";
        break;
      case '\\':
        o += "\\\\";
        break;
      case '\n':
        o += "\\n";
        break;
      case '\r':
        o += "\\r";
        break;
      case '\t':
        o += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x",
                        static_cast<unsigned int>(
                            static_cast<unsigned char>(c)));
          o += buf;
        } else {
          o += c;
        }
        break;
    }
  }
  o += '"';
  return o;
}

std::string JNum(uint64_t v, bool none) {
  if (none || v == thyme::probe::kNone) return "null";
  return std::to_string(v);
}

std::string JU64Array(const std::vector<uint64_t>& values) {
  if (values.empty()) return "null";
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out += ",";
    out += std::to_string(values[i]);
  }
  return out + "]";
}

const char* JBool(bool v) { return v ? "true" : "false"; }

std::string FormatNs(uint64_t ns) {
  char buf[48];
  if (ns >= 1000000000ULL) {
    std::snprintf(buf, sizeof(buf), "%.3f s",
                  static_cast<double>(ns) / 1e9);
  } else if (ns >= 1000000ULL) {
    std::snprintf(buf, sizeof(buf), "%.3f ms",
                  static_cast<double>(ns) / 1e6);
  } else if (ns >= 1000ULL) {
    std::snprintf(buf, sizeof(buf), "%.3f us",
                  static_cast<double>(ns) / 1e3);
  } else {
    std::snprintf(buf, sizeof(buf), "%llu ns",
                  static_cast<unsigned long long>(ns));
  }
  return buf;
}

std::string FormatBytesPerS(double bps) {
  char buf[48];
  if (bps >= 1024.0 * 1024.0 * 1024.0) {
    std::snprintf(buf, sizeof(buf), "%.2f GiB/s", bps / (1024.0 * 1024.0 * 1024.0));
  } else if (bps >= 1024.0 * 1024.0) {
    std::snprintf(buf, sizeof(buf), "%.2f MiB/s", bps / (1024.0 * 1024.0));
  } else if (bps >= 1024.0) {
    std::snprintf(buf, sizeof(buf), "%.2f KiB/s", bps / 1024.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f B/s", bps);
  }
  return buf;
}

std::string WallClock() {
  struct timespec ts {};
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm {};
  gmtime_r(&ts.tv_sec, &tm);
  char buf[80];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L);
  return buf;
}

std::string HexDump(const std::vector<uint8_t>& bytes, std::size_t max_rows) {
  std::string out;
  const std::size_t rows = (bytes.size() + 15u) / 16u;
  const std::size_t shown = rows < max_rows ? rows : max_rows;
  for (std::size_t r = 0; r < shown; ++r) {
    char line[96];
    std::snprintf(line, sizeof(line), "  %08zx  ",
                  r * 16u);
    out += line;
    for (std::size_t c = 0; c < 16; ++c) {
      const std::size_t i = r * 16u + c;
      if (i < bytes.size()) {
        std::snprintf(line, sizeof(line), "%02x ",
                      static_cast<unsigned int>(bytes[i]));
      } else {
        std::snprintf(line, sizeof(line), "   ");
      }
      out += line;
    }
    out += " |";
    for (std::size_t c = 0; c < 16; ++c) {
      const std::size_t i = r * 16u + c;
      if (i < bytes.size()) {
        const unsigned char ch = bytes[i];
        out += (ch >= 0x20 && ch < 0x7f) ? static_cast<char>(ch) : '.';
      }
    }
    out += "|\n";
  }
  if (shown < rows) {
    out += "  ... (hexdump truncated)\n";
  }
  return out;
}

bool ParseU64(const std::string& s, uint64_t* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s.c_str(), &end, 0);
  if (end == nullptr || *end != '\0') return false;
  *out = static_cast<uint64_t>(v);
  return true;
}

bool ParseInt(const std::string& s, int* out) {
  uint64_t v = 0;
  if (!ParseU64(s, &v) || v > 0x7FFFFFFFULL) return false;
  *out = static_cast<int>(v);
  return true;
}

struct Cmd {
  std::vector<std::string> pos;
  bool json = false;
  bool exact = false;
  bool help = false;
  bool matrix = false;
  bool mismatches = false;
  std::string manifest;
  std::vector<std::string> prefixes;
  std::string filter;
  int count = -1;
  uint64_t period_ns = 0;
  uint64_t window_ns = 0;
  uint64_t duration_ns = 0;
  uint64_t timeout_ns = 0;
  uint32_t max_bytes = 0;

  bool Parse(int argc, char** argv, int start, std::string* err) {
    for (int i = start; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--json") {
        json = true;
      } else if (a == "--exact") {
        exact = true;
      } else if (a == "--matrix") {
        matrix = true;
      } else if (a == "--mismatches") {
        mismatches = true;
      } else if (a == "--help" || a == "-h") {
        help = true;
      } else if (a == "--manifest" || a == "--prefix" || a == "--filter" ||
                 a == "--count" || a == "--period" || a == "--window" ||
                 a == "--duration" || a == "--timeout" ||
                 a == "--max-bytes") {
        if (i + 1 >= argc) {
          *err = "missing value for " + a;
          return false;
        }
        const std::string v = argv[++i];
        if (a == "--manifest") {
          manifest = v;
        } else if (a == "--prefix") {
          prefixes.push_back(v);
        } else if (a == "--filter") {
          filter = v;
        } else if (a == "--count") {
          if (!ParseInt(v, &count)) {
            *err = "bad --count";
            return false;
          }
        } else if (a == "--period" || a == "--window" || a == "--duration" ||
                   a == "--timeout") {
          uint64_t ms = 0;
          if (!ParseU64(v, &ms)) {
            *err = "bad value for " + a;
            return false;
          }
          if (a == "--period") period_ns = ms * kMs;
          if (a == "--window") window_ns = ms * kMs;
          if (a == "--duration") duration_ns = ms * kMs;
          if (a == "--timeout") timeout_ns = ms * kMs;
        } else {  // --max-bytes
          uint64_t n = 0;
          if (!ParseU64(v, &n) || n > 0xFFFFFFFFULL) {
            *err = "bad --max-bytes";
            return false;
          }
          max_bytes = static_cast<uint32_t>(n);
        }
      } else if (!a.empty() && a[0] == '-') {
        *err = "unknown option " + a;
        return false;
      } else {
        pos.push_back(a);
      }
    }
    return true;
  }

  Options MakeOptions() const {
    Options o;
    if (!prefixes.empty()) o.prefixes = prefixes;
    o.manifest_path = manifest;
    return o;
  }

  Status LoadManifest(ProbeApi* api) const {
    if (manifest.empty()) return Status::kOk;
    return api->LoadManifest(manifest);
  }
};

int Fail(const std::string& msg) {
  std::fprintf(stderr, "thyme_probe: %s\n", msg.c_str());
  return 1;
}

// ABI-09: read a package's schema sidecar (a package dir or a schema.json
// path). No SHM access, no generated headers: the sidecar is the decoding
// contract. Absent (legacy package) -> exit 2; invalid/tampered -> exit 3.
int CmdSchema(const Cmd& c) {
  if (c.pos.size() != 1) {
    return Fail("schema needs exactly one package directory or schema.json");
  }
  const SchemaView view = thyme::probe::LoadSchemaView(c.pos[0]);
  if (c.json) {
    std::printf("%s\n", RenderSchemaViewJson(view).c_str());
  } else {
    std::fputs(RenderSchemaViewText(view).c_str(), stdout);
  }
  if (view.state == thyme::schema::ReadState::kOk) return 0;
  std::fprintf(stderr, "thyme_probe: schema %s: %s\n",
               thyme::schema::ReadStateName(view.state),
               view.error.empty() ? view.path.c_str() : view.error.c_str());
  return view.state == thyme::schema::ReadState::kAbsent ? 2 : 3;
}

std::string ArgName(const Cmd& c) {
  return c.pos.empty() ? std::string() : c.pos.front();
}

int CmdSession(const Cmd& c) {
  if (c.pos.size() != 1) {
    return Fail("usage: session <prefix>");
  }
  const std::string& prefix = c.pos[0];
  thyme::ShmRegion region;
  const thyme::MpSessionIdentityPage* page = nullptr;
  const thyme::SessionPageCheck check = thyme::OpenMpSessionIdentityPage(
      region, prefix.c_str(), 0, 0, 0, &page);
  if (check == thyme::SessionPageCheck::kMissing) {
    return Fail("no identity page for prefix '" + prefix +
                "' (pre-ARCH-17 session)");
  }
  if (check != thyme::SessionPageCheck::kOk || page == nullptr) {
    return Fail("identity page for prefix '" + prefix + "' is invalid");
  }
  const uint64_t config = page->config_hash.load(std::memory_order_acquire);
  const uint64_t graph = page->graph_hash.load(std::memory_order_acquire);
  const uint64_t start =
      page->supervisor_start_ns.load(std::memory_order_acquire);
  const uint32_t pid = page->supervisor_pid.load(std::memory_order_acquire);
  const uint32_t health = page->health_n.load(std::memory_order_acquire);
  if (c.json) {
    std::printf(
        "{\"schema\":1,\"prefix\":\"%s\",\"config_hash\":%llu,"
        "\"graph_hash\":%llu,\"supervisor_pid\":%u,"
        "\"supervisor_start_ns\":%llu,\"health_n\":%u}\n",
        prefix.c_str(), static_cast<unsigned long long>(config),
        static_cast<unsigned long long>(graph), pid,
        static_cast<unsigned long long>(start), health);
  } else {
    std::printf("prefix:            %s\n", prefix.c_str());
    std::printf("config_hash:       %llu\n",
                static_cast<unsigned long long>(config));
    std::printf("graph_hash:        %llu\n",
                static_cast<unsigned long long>(graph));
    std::printf("supervisor_pid:    %u\n", pid);
    std::printf("supervisor_start:  %llu\n",
                static_cast<unsigned long long>(start));
    std::printf("health_n:          %u\n", health);
  }
  return 0;
}

int CmdList(const Cmd& c) {
  ProbeApi api;
  api.SetOptions(c.MakeOptions());
  const Status mst = c.LoadManifest(&api);
  if (mst != Status::kOk) {
    return ExitCode(mst);
  }
  std::vector<RegionInfo> rows;
  const Status st = api.List(rows);
  if (st != Status::kOk) {
    std::fprintf(stderr,
                 "thyme_probe: no thyme SHM pages under /dev/shm (status %s)\n",
                 StatusName(st));
    return ExitCode(st);
  }
  if (!c.filter.empty()) {
    std::vector<RegionInfo> kept;
    for (const auto& r : rows) {
      if (r.name.find(c.filter) != std::string::npos) kept.push_back(r);
    }
    rows.swap(kept);
    if (rows.empty()) {
      std::fprintf(stderr, "thyme_probe: no page matches filter '%s'\n",
                   c.filter.c_str());
      return ExitCode(Status::kNotFound);
    }
  }
  if (c.json) {
    std::printf("[\n");
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const RegionInfo& r = rows[i];
      const std::string sid =
          r.has_session ? std::to_string(r.session_id) : std::string("null");
      std::printf(
          "  {\"name\":%s,\"kind\":%s,\"abi_version\":%u,\"compatible\":%s,"
          "\"slots\":%u,\"slot_bytes\":%u,\"stride\":%u,"
          "\"region_bytes\":%llu,\"head\":%s,\"tail\":%s,\"epoch\":%s,"
          "\"drop_count\":%s,\"reader_drop_slots\":%s,\"skip_count\":%s,"
          "\"steal_count\":%s,\"session_id\":%s,\"frame_shaped\":%s,"
          "\"geometry_source\":%s,\"note\":%s}%s\n",
          JStr(r.name).c_str(), JStr(KindName(r.kind)).c_str(),
          r.abi_version, JBool(r.compatible), r.slots, r.slot_bytes, r.stride,
          static_cast<unsigned long long>(r.region_bytes),
          JNum(r.head, false).c_str(), JNum(r.tail, false).c_str(),
          JNum(r.epoch, false).c_str(), JNum(r.drop_count, false).c_str(),
          JU64Array(r.reader_drop_slots).c_str(),
          JNum(r.skip_count, false).c_str(),
          JNum(r.steal_count, false).c_str(), sid.c_str(),
          JBool(r.frame_shaped), JStr(r.geometry_source).c_str(),
          JStr(r.note).c_str(), i + 1 < rows.size() ? "," : "");
    }
    std::printf("]\n");
    return 0;
  }
  std::printf("%-38s %-17s %5s %6s %9s %8s %14s %10s\n", "NAME", "KIND",
              "VER", "SLOTS", "SLOT_BYTES", "STRIDE", "HEAD", "DROP");
  for (const auto& r : rows) {
    char head[32];
    if (r.head == thyme::probe::kNone) {
      std::snprintf(head, sizeof(head), "-");
    } else {
      std::snprintf(head, sizeof(head), "%llu",
                    static_cast<unsigned long long>(r.head));
    }
    char drop[32];
    if (r.drop_count == thyme::probe::kNone) {
      std::snprintf(drop, sizeof(drop), "-");
    } else {
      std::snprintf(drop, sizeof(drop), "%llu",
                    static_cast<unsigned long long>(r.drop_count));
    }
    std::printf("%-38s %-17s %5u %6u %9u %8u %14s %10s%s\n", r.name.c_str(),
                KindName(r.kind), r.abi_version, r.slots, r.slot_bytes,
                r.stride, head, drop, r.compatible ? "" : "  [incompatible]");
  }
  std::vector<RegionInfo> notes;
  for (const auto& r : rows) {
    if (!r.note.empty()) notes.push_back(r);
  }
  for (const auto& r : notes) {
    std::printf("note: %s: %s\n", r.name.c_str(), r.note.c_str());
  }
  return 0;
}

int CmdInfo(const Cmd& c) {
  if (c.pos.empty()) return Fail("info requires a channel name");
  ProbeApi api;
  api.SetOptions(c.MakeOptions());
  const Status mst = c.LoadManifest(&api);
  if (mst != Status::kOk) return ExitCode(mst);
  RegionInfo r;
  const Status st = api.Info(ArgName(c), r);
  if (st == Status::kNotFound) {
    std::fprintf(stderr, "thyme_probe: no such thyme page: %s\n",
                 ArgName(c).c_str());
    return ExitCode(st);
  }
  if (c.json) {
    const std::string sid =
        r.has_session ? std::to_string(r.session_id) : std::string("null");
    std::printf(
        "{\"name\":%s,\"kind\":%s,\"abi_version\":%u,\"compatible\":%s,"
        "\"region_bytes\":%llu,\"slots\":%u,\"slot_bytes\":%u,"
        "\"stride\":%u,\"head\":%s,\"tail\":%s,\"epoch\":%s,"
        "\"drop_count\":%s,\"reader_drop_slots\":%s,\"skip_count\":%s,"
        "\"steal_count\":%s,\"session_id\":%s,\"workers\":%u,"
        "\"frame_shaped\":%s,\"geometry_source\":%s,\"note\":%s,"
        "\"structs\":[",
        JStr(r.name).c_str(), JStr(KindName(r.kind)).c_str(), r.abi_version,
        JBool(r.compatible), static_cast<unsigned long long>(r.region_bytes),
        r.slots, r.slot_bytes, r.stride, JNum(r.head, false).c_str(),
        JNum(r.tail, false).c_str(), JNum(r.epoch, false).c_str(),
        JNum(r.drop_count, false).c_str(),
        JU64Array(r.reader_drop_slots).c_str(), JNum(r.skip_count, false).c_str(),
        JNum(r.steal_count, false).c_str(), sid.c_str(),
        r.workers, JBool(r.frame_shaped), JStr(r.geometry_source).c_str(),
        JStr(r.note).c_str());
    for (std::size_t i = 0; i < r.structs.size(); ++i) {
      const auto& s = r.structs[i];
      std::printf("%s{\"id\":%u,\"bytes\":%u,\"name\":%s}",
                  i == 0 ? "" : ",", s.id, s.bytes, JStr(s.name).c_str());
    }
    std::printf("]}\n");
  } else {
    std::printf("name:            %s\n", r.name.c_str());
    std::printf("kind:            %s (version %u%s)\n", KindName(r.kind),
                r.abi_version, r.compatible ? "" : ", INCOMPATIBLE");
    std::printf("region_bytes:    %llu\n",
                static_cast<unsigned long long>(r.region_bytes));
    std::printf("slots:           %u\n", r.slots);
    std::printf("slot_bytes:      %u\n", r.slot_bytes);
    std::printf("stride:          %u\n", r.stride);
    if (r.head == thyme::probe::kNone) {
      std::printf("head:            -\n");
    } else {
      std::printf("head:            %llu\n",
                  static_cast<unsigned long long>(r.head));
    }
    if (r.tail != thyme::probe::kNone) {
      std::printf("tail:            %llu\n",
                  static_cast<unsigned long long>(r.tail));
    }
    if (r.epoch != thyme::probe::kNone) {
      std::printf("epoch:           %llu\n",
                  static_cast<unsigned long long>(r.epoch));
    }
    if (r.drop_count != thyme::probe::kNone) {
      std::printf("drop_count:      %llu\n",
                  static_cast<unsigned long long>(r.drop_count));
    }
    for (std::size_t i = 0; i < r.reader_drop_slots.size(); ++i) {
      std::printf("reader_drop_slots[%zu]: %llu\n", i,
                  static_cast<unsigned long long>(r.reader_drop_slots[i]));
    }
    if (r.skip_count != thyme::probe::kNone) {
      std::printf("pinned_skip:     %llu\n",
                  static_cast<unsigned long long>(r.skip_count));
    }
    if (r.steal_count != thyme::probe::kNone) {
      std::printf("lease_steal:     %llu\n",
                  static_cast<unsigned long long>(r.steal_count));
    }
    if (r.has_session) {
      std::printf("session_id:      %u (creator pid where documented)\n",
                  r.session_id);
    } else {
      std::printf("session_id:      not recorded on this page kind\n");
    }
    if (r.workers != 0) {
      std::printf("workers:         %u\n", r.workers);
    }
    std::printf("frame_shaped:    %s\n", r.frame_shaped ? "yes" : "no");
    std::printf("geometry_source: %s\n", r.geometry_source.c_str());
    if (!r.note.empty()) std::printf("note:            %s\n", r.note.c_str());
    if (!r.structs.empty()) {
      std::printf("structs (%zu):\n", r.structs.size());
      for (const auto& s : r.structs) {
        std::printf("  id=%-3u bytes=%-6u %s\n", s.id, s.bytes, s.name.c_str());
      }
    }
  }
  if (st != Status::kOk) {
    std::fprintf(stderr, "thyme_probe: %s: %s\n", r.name.c_str(),
                 StatusName(st));
  }
  return ExitCode(st);
}

int CmdWatch(const Cmd& c) {
  if (c.pos.empty()) return Fail("watch requires a channel name");
  ProbeApi api;
  api.SetOptions(c.MakeOptions());
  const Status mst = c.LoadManifest(&api);
  if (mst != Status::kOk) return ExitCode(mst);
  SampleOptions so;
  so.count = c.count > 0 ? c.count : 0;
  so.period_ns = c.period_ns != 0 ? c.period_ns : 1000ULL * kMs;
  so.max_duration_ns = c.duration_ns;
  uint64_t emitted = 0;
  const Status st = api.Watch(
      ArgName(c), so,
      [&c](const Sample& s) {
        if (c.json) {
          std::printf(
              "{\"seq\":%llu,\"ts_ns\":%llu,\"producer_ns\":%llu,"
              "\"size\":%u,\"rate_hz\":%.3f}\n",
              static_cast<unsigned long long>(s.sequence),
              static_cast<unsigned long long>(s.observed_ns),
              static_cast<unsigned long long>(s.producer_ns), s.size,
              s.rate_hz);
        } else {
          const std::string wall = WallClock();
          if (s.producer_ns != 0 && s.observed_ns > s.producer_ns) {
            std::printf("%s  seq=%-12llu size=%-6u rate=%-10.2fHz age=%s\n",
                        wall.c_str(),
                        static_cast<unsigned long long>(s.sequence), s.size,
                        s.rate_hz,
                        FormatNs(s.observed_ns - s.producer_ns).c_str());
          } else {
            std::printf("%s  seq=%-12llu size=%-6u rate=%-10.2fHz\n",
                        wall.c_str(),
                        static_cast<unsigned long long>(s.sequence), s.size,
                        s.rate_hz);
          }
        }
        return true;
      },
      &emitted);
  if (st != Status::kOk) {
    std::fprintf(stderr, "thyme_probe: watch: %s\n", StatusName(st));
  } else if (!c.json) {
    std::printf("-- %llu samples\n", static_cast<unsigned long long>(emitted));
  }
  return ExitCode(st);
}

int CmdHz(const Cmd& c) {
  if (c.pos.empty()) return Fail("hz requires a channel name");
  ProbeApi api;
  api.SetOptions(c.MakeOptions());
  const Status mst = c.LoadManifest(&api);
  if (mst != Status::kOk) return ExitCode(mst);
  RateStats stats;
  const Status st = api.Hz(ArgName(c), c.window_ns != 0 ? c.window_ns : 1000ULL * kMs,
                           c.period_ns != 0 ? c.period_ns : 100ULL * kMs,
                           &stats);
  if (st != Status::kOk) {
    std::fprintf(stderr, "thyme_probe: hz: %s\n", StatusName(st));
    return ExitCode(st);
  }
  if (c.json) {
    std::printf(
        "{\"channel\":%s,\"window_ns\":%llu,\"published\":%llu,"
        "\"hz_avg\":%.3f,\"hz_min\":%.3f,\"hz_max\":%.3f,"
        "\"jitter_p50_ns\":%llu,\"jitter_p99_ns\":%llu,"
        "\"jitter_max_ns\":%llu}\n",
        JStr(ArgName(c)).c_str(),
        static_cast<unsigned long long>(stats.window_ns),
        static_cast<unsigned long long>(stats.published), stats.hz_avg,
        stats.hz_min, stats.hz_max,
        static_cast<unsigned long long>(stats.jitter_p50_ns),
        static_cast<unsigned long long>(stats.jitter_p99_ns),
        static_cast<unsigned long long>(stats.jitter_max_ns));
  } else {
    std::printf("channel:   %s\n", ArgName(c).c_str());
    std::printf("window:    %.3f s (published %llu)\n", stats.window_s,
                static_cast<unsigned long long>(stats.published));
    std::printf("hz:        avg %.2f  min %.2f  max %.2f\n", stats.hz_avg,
                stats.hz_min, stats.hz_max);
    std::printf("jitter:    p50 %s  p99 %s  max %s\n",
                FormatNs(stats.jitter_p50_ns).c_str(),
                FormatNs(stats.jitter_p99_ns).c_str(),
                FormatNs(stats.jitter_max_ns).c_str());
  }
  return 0;
}

int CmdBw(const Cmd& c) {
  if (c.pos.empty()) return Fail("bw requires a channel name");
  ProbeApi api;
  api.SetOptions(c.MakeOptions());
  const Status mst = c.LoadManifest(&api);
  if (mst != Status::kOk) return ExitCode(mst);
  RateStats stats;
  const Status st = api.Bw(ArgName(c), c.window_ns != 0 ? c.window_ns : 1000ULL * kMs,
                           c.period_ns != 0 ? c.period_ns : 100ULL * kMs,
                           c.exact, &stats);
  if (st != Status::kOk) {
    std::fprintf(stderr, "thyme_probe: bw: %s\n", StatusName(st));
    return ExitCode(st);
  }
  if (c.json) {
    std::printf(
        "{\"channel\":%s,\"window_ns\":%llu,\"published\":%llu,"
        "\"bytes_per_s\":%.3f,\"bytes_per_s_min\":%.3f,"
        "\"bytes_per_s_max\":%.3f,\"hz_min\":%.3f,\"hz_max\":%.3f,"
        "\"jitter_p50_ns\":%llu,\"jitter_p99_ns\":%llu,"
        "\"exact_bytes\":%s}\n",
        JStr(ArgName(c)).c_str(),
        static_cast<unsigned long long>(stats.window_ns),
        static_cast<unsigned long long>(stats.published), stats.bytes_per_s,
        stats.bytes_per_s_min, stats.bytes_per_s_max, stats.hz_min,
        stats.hz_max,
        static_cast<unsigned long long>(stats.jitter_p50_ns),
        static_cast<unsigned long long>(stats.jitter_p99_ns),
        JBool(stats.exact_bytes));
  } else {
    std::printf("channel:   %s\n", ArgName(c).c_str());
    std::printf("window:    %.3f s (published %llu)\n", stats.window_s,
                static_cast<unsigned long long>(stats.published));
    std::printf("bandwidth: avg %s  min %s  max %s%s\n",
                FormatBytesPerS(stats.bytes_per_s).c_str(),
                FormatBytesPerS(stats.bytes_per_s_min).c_str(),
                FormatBytesPerS(stats.bytes_per_s_max).c_str(),
                stats.exact_bytes ? "  (exact per-frame lengths)"
                                 : "  (counter x slot capacity)");
    std::printf("jitter:    p50 %s  p99 %s\n",
                FormatNs(stats.jitter_p50_ns).c_str(),
                FormatNs(stats.jitter_p99_ns).c_str());
  }
  return 0;
}

void PrintHistogram(const LatencyStats& stats) {
  std::printf("histogram:\n");
  for (std::size_t i = 0; i < thyme::probe::Histogram::kBuckets; ++i) {
    const uint64_t upper = thyme::probe::Histogram::UpperNs(i);
    if (upper == thyme::probe::kNone) {
      std::printf("  %12s : %llu\n", "overflow",
                  static_cast<unsigned long long>(stats.histogram.count[i]));
    } else {
      std::printf("  %12s : %llu\n", FormatNs(upper).c_str(),
                  static_cast<unsigned long long>(stats.histogram.count[i]));
    }
  }
}

int CmdDelay(const Cmd& c) {
  if (c.pos.empty()) return Fail("delay requires a channel name");
  ProbeApi api;
  api.SetOptions(c.MakeOptions());
  const Status mst = c.LoadManifest(&api);
  if (mst != Status::kOk) return ExitCode(mst);
  LatencyStats stats;
  const Status st =
      api.Delay(ArgName(c), c.window_ns != 0 ? c.window_ns : 1000ULL * kMs,
                c.period_ns != 0 ? c.period_ns : 100ULL * kMs, &stats);
  if (st != Status::kOk) {
    std::fprintf(stderr, "thyme_probe: delay: %s\n", StatusName(st));
    if (st == Status::kBadGeometry) {
      std::fprintf(stderr,
                   "  (no producer timestamp: PinnedSlot carries one in its "
                   "slot header; ring/latest payloads need a manifest entry "
                   "with frame_header)\n");
    }
    return ExitCode(st);
  }
  if (c.json) {
    std::printf(
        "{\"channel\":%s,\"window_ns\":%llu,\"samples\":%llu,"
        "\"stamp_source\":%s,\"min_ns\":%llu,\"p50_ns\":%llu,"
        "\"p90_ns\":%llu,\"p99_ns\":%llu,\"max_ns\":%llu,\"buckets\":[",
        JStr(ArgName(c)).c_str(),
        static_cast<unsigned long long>(stats.window_ns),
        static_cast<unsigned long long>(stats.count),
        JStr(stats.stamp_source).c_str(),
        static_cast<unsigned long long>(stats.min_ns),
        static_cast<unsigned long long>(stats.p50_ns),
        static_cast<unsigned long long>(stats.p90_ns),
        static_cast<unsigned long long>(stats.p99_ns),
        static_cast<unsigned long long>(stats.max_ns));
    for (std::size_t i = 0; i < thyme::probe::Histogram::kBuckets; ++i) {
      std::printf("%s%llu", i == 0 ? "" : ",",
                  static_cast<unsigned long long>(stats.histogram.count[i]));
    }
    std::printf("]}\n");
  } else {
    std::printf("channel:      %s\n", ArgName(c).c_str());
    std::printf("window:       %.3f s (%llu observations)\n",
                static_cast<double>(stats.window_ns) / 1e9,
                static_cast<unsigned long long>(stats.count));
    std::printf("stamp source: %s\n", stats.stamp_source.c_str());
    std::printf("age:          min %s  p50 %s  p90 %s  p99 %s  max %s\n",
                FormatNs(stats.min_ns).c_str(), FormatNs(stats.p50_ns).c_str(),
                FormatNs(stats.p90_ns).c_str(), FormatNs(stats.p99_ns).c_str(),
                FormatNs(stats.max_ns).c_str());
    PrintHistogram(stats);
    std::printf(
        "note: age = now_mono - producer stamp at the observation point; it "
        "is not consumer latency.\n");
  }
  return 0;
}

int CmdEcho(const Cmd& c) {
  if (c.pos.empty()) return Fail("echo requires a channel name");
  ProbeApi api;
  api.SetOptions(c.MakeOptions());
  const Status mst = c.LoadManifest(&api);
  if (mst != Status::kOk) return ExitCode(mst);
  const int count = c.count > 0 ? c.count : 1;
  const uint32_t max_bytes = c.max_bytes != 0 ? c.max_bytes : 4096;
  const uint64_t timeout = c.timeout_ns != 0 ? c.timeout_ns : 2000ULL * kMs;
  std::vector<EchoSample> samples;
  const Status st = api.Echo(ArgName(c), count, max_bytes, timeout, &samples);
  if (st != Status::kOk) {
    std::fprintf(stderr, "thyme_probe: echo: %s\n", StatusName(st));
    return ExitCode(st);
  }
  for (const auto& s : samples) {
    if (c.json) {
      std::string hex;
      static const char* kHex = "0123456789abcdef";
      for (uint8_t b : s.bytes) {
        hex += kHex[(b >> 4) & 0xF];
        hex += kHex[b & 0xF];
      }
      const std::string worker =
          s.has_worker ? std::to_string(s.worker) : std::string("null");
      const std::string memory_flags =
          s.has_memory_flags ? std::to_string(s.memory_flags)
                             : std::string("null");
      const std::string memory_state =
          s.has_memory_flags
              ? JStr(thyme::MemoryHealthStateName(s.memory_flags))
              : std::string("null");
      std::printf(
          "{\"seq\":%llu,\"ts_ns\":%llu,\"producer_ns\":%llu,\"size\":%u,"
          "\"drop_count\":%s,\"worker\":%s,\"memory_flags\":%s,"
          "\"memory_state\":%s,\"truncated\":%s,"
          "\"struct_id\":%u,\"struct_name\":%s,\"has_header\":%s,\"header\":",
          static_cast<unsigned long long>(s.sequence),
          static_cast<unsigned long long>(s.observed_ns),
          static_cast<unsigned long long>(s.producer_ns), s.size,
          JNum(s.drop_count, false).c_str(), worker.c_str(), memory_flags.c_str(),
          memory_state.c_str(), JBool(s.truncated), s.struct_id,
          JStr(s.struct_name).c_str(), JBool(s.has_header));
      if (s.has_header) {
        std::printf(
            "{\"magic\":%u,\"version\":%u,\"size\":%u,\"session_id\":%llu,"
            "\"mono_ns\":%llu,\"sequence\":%u,\"valid\":%u,\"crc32\":%u},",
            s.header.magic, static_cast<unsigned int>(s.header.version),
            static_cast<unsigned int>(s.header.size),
            static_cast<unsigned long long>(s.header.session_id),
            static_cast<unsigned long long>(s.header.mono_ns),
            s.header.sequence, static_cast<unsigned int>(s.header.valid),
            s.header.crc32);
      } else {
        std::printf("null,");
      }
      std::printf("\"bytes\":\"%s\"}\n", hex.c_str());
    } else {
      std::printf("seq=%llu observed_ns=%llu producer_ns=%llu size=%u%s",
                  static_cast<unsigned long long>(s.sequence),
                  static_cast<unsigned long long>(s.observed_ns),
                  static_cast<unsigned long long>(s.producer_ns), s.size,
                  s.truncated ? " (truncated)" : "");
      if (s.drop_count != thyme::probe::kNone) {
        std::printf(" drop_count=%llu",
                    static_cast<unsigned long long>(s.drop_count));
      }
      if (s.has_worker) std::printf(" worker=%u", s.worker);
      if (s.has_memory_flags) {
        std::printf(" memory_flags=%u memory_state=%s", s.memory_flags,
                    thyme::MemoryHealthStateName(s.memory_flags));
      }
      if (!s.struct_name.empty() || s.struct_id != 0) {
        std::printf(" struct=%s(%u)", s.struct_name.c_str(), s.struct_id);
      }
      std::printf("\n");
      if (s.has_header) {
        std::printf(
            "  header: magic=0x%08x version=%u size=%u session_id=%llu "
            "mono_ns=%llu sequence=%u valid=%u crc32=0x%08x\n",
            s.header.magic, static_cast<unsigned int>(s.header.version),
            static_cast<unsigned int>(s.header.size),
            static_cast<unsigned long long>(s.header.session_id),
            static_cast<unsigned long long>(s.header.mono_ns),
            s.header.sequence, static_cast<unsigned int>(s.header.valid),
            s.header.crc32);
        std::printf(
            "  (live SHM payload: no schema attached; for a *package* use "
            "'thyme_probe schema <pkg>' (ABI-09 sidecar), hex follows the "
            "40-byte header)\n");
      } else {
        std::printf(
            "  (raw payload: no FrameHeader decoded; for a package payload "
            "see 'thyme_probe schema <pkg>')\n");
      }
      std::printf("%s", HexDump(s.bytes, 16).c_str());
    }
  }
  return 0;
}

int CmdAbi(const Cmd& c) {
  if (c.matrix) {
    std::printf("%s\n", thyme::probe::AbiMatrixJson().c_str());
    return 0;
  }
  ProbeApi api;
  api.SetOptions(c.MakeOptions());
  const Status mst = c.LoadManifest(&api);
  if (mst != Status::kOk) return ExitCode(mst);
  AbiReport report;
  const Status st = api.CheckAbi(&report);
  const int code = ExitCode(st);

  if (c.mismatches) {
    // Machine-readable for deploy scripts: one line per incompatible page.
    for (const AbiCheck& page : report.pages) {
      if (page.compatible) continue;
      std::printf("%s expected=%u actual=%u\n", page.name.c_str(),
                  static_cast<unsigned int>(page.expected),
                  static_cast<unsigned int>(page.actual));
    }
    return code;
  }
  if (c.json) {
    std::printf("%s\n", thyme::probe::AbiReportJson(report).c_str());
    return code;
  }

  std::printf("%-38s %-17s %-24s %6s %8s %s\n", "NAME", "KIND", "OBJECT",
              "ACTUAL", "EXPECTED", "RESULT");
  for (const AbiCheck& page : report.pages) {
    std::printf("%-38s %-17s %-24s %6u %8u %s\n", page.name.c_str(),
                KindName(page.kind), page.object.c_str(),
                static_cast<unsigned int>(page.actual),
                static_cast<unsigned int>(page.expected),
                page.compatible ? "ok" : "MISMATCH");
    if (!page.compatible && !page.note.empty()) {
      std::printf("%-38s %-17s %-24s %6s %8s %s\n", "", "", "", "", "",
                  page.note.c_str());
    }
  }
  if (!report.unverifiable.empty()) {
    std::printf("unverifiable:\n");
    for (const std::string& item : report.unverifiable) {
      std::printf("  %s\n", item.c_str());
    }
  }
  if (st == Status::kNotFound) {
    std::printf(
        "abi: no thyme SHM pages found (nothing is running, or check "
        "--prefix)\n");
    return code;
  }
  std::printf("abi: %u compatible, %u mismatch(es)\n",
              static_cast<unsigned int>(report.compatible_count),
              static_cast<unsigned int>(report.mismatch_count));
  if (report.mismatch_count != 0) {
    std::printf(
        "mixed versions cannot run together: stop producers, upgrade every\n"
        "peer, then rebuild the SHM session (docs/guides/upgrade.md).\n");
  }
  return code;
}

void PrintUsage(FILE* out) {
  std::fprintf(out,
               "thyme_probe - read-only flow view over thyme SHM containers\n"
               "\n"
               "usage: thyme_probe <command> [options]\n"
               "\n"
               "commands:\n"
               "  list [--json] [--filter PAT] [--prefix P]... [--manifest FILE]\n"
               "      enumerate thyme pages under /dev/shm (read-only)\n"
               "  info <name> [--json] [--manifest FILE]\n"
               "      geometry, counters, struct registry summary\n"
               "  watch <name> [--count N] [--period MS] [--duration MS] [--json]\n"
               "      sample the live value stream (NDJSON with --json)\n"
               "  hz <name> [--window MS] [--period MS] [--json]\n"
               "      publish rate, min/avg/max, jitter p50/p99\n"
               "  bw <name> [--window MS] [--period MS] [--exact] [--json]\n"
               "      payload bytes/s, min/avg/max, jitter p50/p99\n"
               "  delay <name> [--window MS] [--period MS] [--json]\n"
               "      age at observation: now_mono - producer stamp\n"
               "  echo <name> [--count N] [--max-bytes N] [--timeout MS] [--json]\n"
               "      decode/hexdump the newest payloads\n"
               "  schema <pkg|schema.json> [--json]\n"
               "      ABI-09 record schema sidecar: contracts, field layout,\n"
               "      manifest identity (no thyme headers needed to decode)\n"
               "  session <prefix> [--json]   ARCH-17 session identity page\n"
               "  abi [--json] [--matrix] [--mismatches] [--prefix P]... "
               "[--manifest FILE]\n"
               "      pre-flight ABI check of /dev/shm pages against this build;\n"
               "      --matrix prints the header-derived constants as JSON,\n"
               "      --mismatches prints 'name expected=N actual=N' lines\n"
               "  --check-abi            alias for 'abi'\n"
               "  --abi-matrix           alias for 'abi --matrix'\n"
               "  --help, <command> --help\n"
               "\n"
               "read-only: no subscription, no PinnedSlot lease, no shared cursor\n"
               "advance, no write, no unlink. The shared SPSC ring cursor is\n"
               "deliberately not used (TryPop writes tail); the probe re-reads\n"
               "the newest slot instead, so it can miss frames.\n"
               "\n"
               "delay is the age of the producer stamp at the observation\n"
               "point (producer CLOCK_MONOTONIC to probe read), not consumer\n"
               "latency.\n"
               "\n"
               "exit codes: 0 ok, 1 usage, 2 not found, 3 bad geometry/version,\n"
               "            4 permission, 5 no data/stale\n");
}

void PrintCommandUsage(FILE* out, const std::string& cmd) {
  if (cmd == "list") {
    std::fprintf(out,
                 "usage: thyme_probe list [--json] [--filter PAT] "
                 "[--prefix P]... [--manifest FILE]\n"
                 "--prefix matches /dev/shm entries without the leading '/' "
                 "(default: thyme_). The MP supervisor default is c9_.\n");
  } else if (cmd == "info") {
    std::fprintf(out,
                 "usage: thyme_probe info <name> [--json] [--manifest FILE]\n");
  } else if (cmd == "watch") {
    std::fprintf(out,
                 "usage: thyme_probe watch <name> [--count N] [--period MS] "
                 "[--duration MS] [--json]\n"
                 "--count 0 (default) samples until Ctrl-C or --duration.\n");
  } else if (cmd == "hz") {
    std::fprintf(out,
                 "usage: thyme_probe hz <name> [--window MS] [--period MS] "
                 "[--json]\n");
  } else if (cmd == "bw") {
    std::fprintf(out,
                 "usage: thyme_probe bw <name> [--window MS] [--period MS] "
                 "[--exact] [--json]\n");
  } else if (cmd == "delay") {
    std::fprintf(out,
                 "usage: thyme_probe delay <name> [--window MS] [--period MS] "
                 "[--json]\n"
                 "Prints age-at-observation percentiles and a histogram.\n");
  } else if (cmd == "echo") {
    std::fprintf(out,
                 "usage: thyme_probe echo <name> [--count N] [--max-bytes N] "
                 "[--timeout MS] [--json]\n");
  } else if (cmd == "schema") {
    std::fprintf(out,
                 "usage: thyme_probe schema <package-dir|schema.json> [--json]\n"
                 "Reads the ABI-09 sidecar written at package seal; field\n"
                 "offsets are absolute inside the recorded payload. Exit: 0\n"
                 "ok, 2 absent (pre-ABI-09 package), 3 invalid/tampered.\n");
  } else if (cmd == "session") {
    std::fprintf(out,
                 "usage: thyme_probe session <prefix> [--json]\n"
                 "Reads <prefix>_id (ARCH-17) and prints its identity fields.\n");
  } else if (cmd == "abi") {
    std::fprintf(out,
                 "usage: thyme_probe abi [--json] [--matrix] [--mismatches] "
                 "[--prefix P]... [--manifest FILE]\n"
                 "Compares every page List() reports against the ABI constants\n"
                 "compiled into this binary (read-only). --matrix prints those\n"
                 "constants as JSON; --mismatches prints one line per\n"
                 "incompatible page: '<name> expected=<n> actual=<n>'.\n"
                 "Exit: 0 compatible, 2 no pages found, 3 version/geometry\n"
                 "mismatch. Stale segments from old sessions are included;\n"
                 "see docs/guides/runbook.md section 2.\n");
  } else {
    PrintUsage(out);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage(stderr);
    return 1;
  }
  std::string cmd = argv[1];
  bool matrix_alias = false;
  if (cmd == "--check-abi") {
    cmd = "abi";
  } else if (cmd == "--abi-matrix") {
    cmd = "abi";
    matrix_alias = true;
  }
  if (cmd == "--help" || cmd == "-h" || cmd == "help") {
    PrintUsage(stdout);
    return 0;
  }
  Cmd c;
  std::string err;
  if (!c.Parse(argc, argv, 2, &err)) {
    std::fprintf(stderr, "thyme_probe: %s\n", err.c_str());
    PrintCommandUsage(stderr, cmd);
    return 1;
  }
  if (matrix_alias) c.matrix = true;
  if (c.help) {
    PrintCommandUsage(stdout, cmd);
    return 0;
  }
  if (cmd == "list") return CmdList(c);
  if (cmd == "info") return CmdInfo(c);
  if (cmd == "watch") return CmdWatch(c);
  if (cmd == "hz") return CmdHz(c);
  if (cmd == "bw") return CmdBw(c);
  if (cmd == "delay") return CmdDelay(c);
  if (cmd == "echo") return CmdEcho(c);
  if (cmd == "schema") return CmdSchema(c);
  if (cmd == "session") return CmdSession(c);
  if (cmd == "abi") return CmdAbi(c);
  std::fprintf(stderr, "thyme_probe: unknown command '%s'\n", cmd.c_str());
  PrintUsage(stderr);
  return 1;
}
