// replay_export.cpp — REP-09 package export (MCAP / NDJSON), sibling tool.
//
// Both formats stream the ReplayPackage timeline. Neither format is linked
// into the core libraries: the reader stays read-only and the replay player
// never sees an exporter. See replay_export.h for the format contract and
// docs/guides/replay.md for the user-facing description.

#include "replay_export.h"

#include "canonical.h"
#include "replay_json.h"
#include "self_desc.h"

#include "thyme/contracts/frame_header.h"
#include "thyme/record/mcap_writer.h"
#include "thyme/replay/replay_reader.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace thyme {
namespace replaycli {

namespace {

constexpr uint16_t kMcapPackageChannel = 1;
constexpr uint16_t kMcapDataSchemaBase = 1;
constexpr uint16_t kMcapGapSchema = 101;
constexpr uint16_t kMcapTriggerSchema = 102;
constexpr uint16_t kMcapSessionSchema = 103;
// Event channels are assigned after the data channels; ids stay < 128, the
// McapWriter capacity (kMcapMaxChannels).
constexpr uint32_t kMcapMaxExportedChannels = kMcapMaxChannels;

constexpr char kGapSchemaJson[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
    "\"title\":\"thyme.GapEvent\",\"type\":\"object\","
    "\"properties\":{\"event\":{\"type\":\"string\"},"
    "\"channel\":{\"type\":\"integer\"},"
    "\"struct_id\":{\"type\":\"integer\"},"
    "\"reason\":{\"type\":\"integer\"},"
    "\"missed_count\":{\"type\":\"integer\"}},"
    "\"required\":[\"event\"]}";

constexpr char kTriggerSchemaJson[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
    "\"title\":\"thyme.TriggerEvent\",\"type\":\"object\","
    "\"properties\":{\"event\":{\"type\":\"string\"},"
    "\"channel\":{\"type\":\"integer\"},"
    "\"trigger_id\":{\"type\":\"integer\"},"
    "\"pre_roll_ns\":{\"type\":\"integer\"},"
    "\"post_roll_ns\":{\"type\":\"integer\"}},"
    "\"required\":[\"event\"]}";

constexpr char kSessionSchemaJson[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
    "\"title\":\"thyme.SessionEvent\",\"type\":\"object\","
    "\"properties\":{\"event\":{\"type\":\"string\"},"
    "\"channel\":{\"type\":\"integer\"},"
    "\"session_id\":{\"type\":\"integer\"},"
    "\"reason\":{\"type\":\"integer\"}},"
    "\"required\":[\"event\"]}";

void AddU64(uint64_t a, uint64_t b, uint64_t* out) {
  *out = (b > UINT64_MAX - a) ? UINT64_MAX : a + b;
}

std::string StructName(const ThymeStructDesc& desc) {
  const std::size_t n = strnlen(desc.name, sizeof(desc.name));
  if (n == 0) {
    return std::string();
  }
  return std::string(desc.name, n);
}

const ThymeStructDesc* FindDesc(const replay::ReplayChannel& ch,
                                uint16_t struct_id) {
  for (const ThymeStructDesc& d : ch.registry) {
    if (d.struct_id == struct_id) {
      return &d;
    }
  }
  return nullptr;
}

std::string SanitizeTopicPart(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (const char c : in) {
    const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                      c == '.';
    out += keep ? c : '_';
  }
  return out;
}

std::string JsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (const char c : in) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

// One JSON document on one line: the Json emitter is pretty-printed, and
// NDJSON needs a single line per record (header/summary are small, so the
// compact copy is bounded).
std::string Compact(const std::string& pretty) {
  std::string out;
  out.reserve(pretty.size());
  for (const char c : pretty) {
    if (c != '\n') {
      out += c;
    }
  }
  return out;
}

void WriteBase64(std::ostream& out, const uint8_t* data, std::size_t len) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char buf[4096];
  std::size_t n = 0;
  std::size_t i = 0;
  auto flush_if_full = [&]() {
    if (n + 4 > sizeof(buf)) {
      out.write(buf, static_cast<std::streamsize>(n));
      n = 0;
    }
  };
  while (i + 3 <= len) {
    const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8) |
                       static_cast<uint32_t>(data[i + 2]);
    buf[n++] = kAlphabet[(v >> 18) & 0x3Fu];
    buf[n++] = kAlphabet[(v >> 12) & 0x3Fu];
    buf[n++] = kAlphabet[(v >> 6) & 0x3Fu];
    buf[n++] = kAlphabet[v & 0x3Fu];
    i += 3;
    flush_if_full();
  }
  if (i < len) {
    uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < len) {
      v |= static_cast<uint32_t>(data[i + 1]) << 8;
    }
    buf[n++] = kAlphabet[(v >> 18) & 0x3Fu];
    buf[n++] = kAlphabet[(v >> 12) & 0x3Fu];
    buf[n++] = (i + 1 < len) ? kAlphabet[(v >> 6) & 0x3Fu] : '=';
    buf[n++] = '=';
  }
  if (n != 0) {
    out.write(buf, static_cast<std::streamsize>(n));
  }
}

struct Window {
  bool has_start = false;
  uint64_t start_ns = 0;
  bool has_to = false;
  uint64_t to_ns = 0;
};

bool ResolveWindow(const replay::ReplayPackage& pkg,
                   const ExportOptions& opt, Window* w, std::string* err) {
  const uint64_t first = pkg.first_mono_ns();
  if (opt.has_from) {
    w->has_start = true;
    if (opt.from.absolute) {
      w->start_ns = opt.from.ns;
    } else {
      AddU64(first, opt.from.ns, &w->start_ns);
    }
  }
  if (opt.has_to) {
    w->has_to = true;
    if (opt.to.absolute) {
      w->to_ns = opt.to.ns;
    } else {
      AddU64(first, opt.to.ns, &w->to_ns);
    }
  }
  if (w->has_start && w->has_to && w->to_ns < w->start_ns) {
    *err = "--to is before --from on the package timeline";
    return false;
  }
  return true;
}

const char* KindName(replay::ReplayChannelKind kind) {
  switch (kind) {
    case replay::ReplayChannelKind::kMeta:
      return "meta";
    case replay::ReplayChannelKind::kSmall:
      return "small";
    case replay::ReplayChannelKind::kPinnedSlot:
      return "pinned_slot";
  }
  return "unknown";
}

void SeedChannelSummary(const replay::ReplayPackage& pkg, ExportSummary* s) {
  s->per_channel.clear();
  s->per_channel.reserve(pkg.channels().size());
  for (const replay::ReplayChannel& c : pkg.channels()) {
    ExportChannelSummary cs;
    cs.id = c.id;
    cs.name = c.name;
    cs.kind = KindName(c.kind);
    s->per_channel.push_back(std::move(cs));
  }
}

bool CheckFrame(const replay::ReplayEvent& ev, bool pinned_slot,
                const ExportOptions& opt, ExportSummary* s,
                std::string* err) {
  // Only raw frame segments carry a per-frame payload_crc; small/meta record
  // CRCs were already verified by ReplayPackage::Open().
  if (!pinned_slot || ev.kind != replay::ReplayKind::kData || !opt.verify_crc) {
    return true;
  }
  if (ev.payload_crc == 0) {
    s->crc_skipped += 1;
    return true;
  }
  s->crc_checked += 1;
  if (ev.payload == nullptr ||
      Crc32(ev.payload, ev.len) != ev.payload_crc) {
    s->crc_bad += 1;
    if (err->empty()) {
      char buf[192];
      std::snprintf(buf, sizeof(buf),
                    "frame CRC mismatch on channel %u (struct %u) at mono_ns "
                    "%llu",
                    static_cast<unsigned>(ev.channel),
                    static_cast<unsigned>(ev.struct_id),
                    static_cast<unsigned long long>(ev.mono_ns));
      *err = buf;
    }
    return false;
  }
  return true;
}

template <typename Fn>
bool ForEachEvent(replay::ReplayPackage& pkg, const Window& w, bool restart,
                  const std::string& redact_channel, std::string* err, Fn fn) {
  if (restart || w.has_start) {
    const replay::ReplayStatus sst =
        pkg.SeekMonoNs(w.has_start ? w.start_ns : 0);
    if (sst != replay::ReplayStatus::kOk) {
      *err = std::string("seek failed: ") + replay::ReplayStatusName(sst) +
             ": " + pkg.error();
      return false;
    }
  }
  replay::ReplayEvent ev;
  replay::ReplayStatus st = replay::ReplayStatus::kOk;
  while ((st = pkg.NextEvent(&ev)) == replay::ReplayStatus::kOk) {
    if (w.has_to && ev.mono_ns > w.to_ns) {
      return true;
    }
    // SEC-06: redact by channel at the event boundary, so every encoder
    // (raw/MCAP/NDJSON/canonical/self-describing) sees zeroed bytes.
    std::vector<uint8_t> redacted;
    if (!redact_channel.empty() &&
        pkg.channels()[ev.channel].name == redact_channel) {
      redacted.assign(ev.len, 0);
      ev.payload = redacted.data();
      // The payload CRC is over the bytes on disk, so it has to be recomputed
      // after redaction; otherwise the exporter's own CRC check rejects the
      // sanitized frame.
      ev.payload_crc = Crc32(redacted.data(), redacted.size());
    }
    if (!fn(ev)) {
      return false;
    }
  }
  if (st == replay::ReplayStatus::kEnd) {
    return true;
  }
  *err = std::string(replay::ReplayStatusName(st)) + ": " + pkg.error();
  return false;
}

void NoteEvent(const replay::ReplayEvent& ev, ExportSummary* s) {
  if (ev.kind == replay::ReplayKind::kData) {
    s->data_messages += 1;
    s->payload_bytes += ev.len;
  } else if (ev.kind == replay::ReplayKind::kGap) {
    s->gap_messages += 1;
  } else if (ev.kind == replay::ReplayKind::kTrigger) {
    s->trigger_messages += 1;
  } else if (ev.kind == replay::ReplayKind::kSession) {
    s->session_messages += 1;
  }
}

void NoteWindowTime(const replay::ReplayEvent& ev, ExportSummary* s) {
  if (s->events == 0) {
    s->first_mono_ns = ev.mono_ns;
    s->last_mono_ns = ev.mono_ns;
  } else {
    s->first_mono_ns = std::min(s->first_mono_ns, ev.mono_ns);
    s->last_mono_ns = std::max(s->last_mono_ns, ev.mono_ns);
  }
  s->events += 1;
}

// REC-27: in-tree exporter registry. Core stores opaque identity only; this
// table names the decoder. Real sensor profiles live out of tree — this list
// is intentionally tiny and used by the regression. Matching is exact
// (name + version + params_digest); a miss must never guess.
bool KnownPayloadProfile(const std::string& name, uint32_t version,
                         uint64_t digest) {
  return name == "inno-lidar/pointcloud@v2" && version == 1 && digest == 0;
}

const schema::PayloadProfileDesc* FindChannelProfile(
    const schema::PackageSchema& schema, const std::string& channel) {
  for (const schema::PayloadProfileDesc& p : schema.payload_profiles) {
    if (p.channel == channel) return &p;
  }
  return nullptr;
}

std::string ProfileDecodeError(const replay::ReplayPackage& pkg,
                               const ExportOptions& opt) {
  if (!opt.decode_profile) return {};
  if (pkg.schema_state() != schema::ReadState::kOk) return {};
  std::string err;
  for (const schema::PayloadProfileDesc& p : pkg.schema().payload_profiles) {
    if (p.profile.empty()) continue;
    if (KnownPayloadProfile(p.profile, p.version, p.params_digest)) continue;
    if (!err.empty()) err += "; ";
    err += "unknown payload profile '" + p.profile + "' version " +
           std::to_string(p.version) + " on channel " + p.channel +
           " (raw export kept)";
  }
  return err;
}

int FinishExport(ExportSummary* s, const std::string& decode_err) {
  if (!decode_err.empty()) {
    s->ok = false;
    s->error = decode_err;
    s->exit_code = kExitFailure;
    s->decode_refused = true;
    return kExitFailure;
  }
  s->ok = true;
  s->exit_code = kExitOk;
  return kExitOk;
}

std::string SchemaRefFor(const replay::ReplayPackage& pkg,
                         const ExportOptions& opt, uint16_t struct_id,
                         bool raw_frame) {
  if (raw_frame || pkg.schema_state() != schema::ReadState::kOk) {
    return std::string();
  }
  const char* base = opt.schema_out.empty() ? "schema.json" : opt.schema_out.c_str();
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%s#/records/%u", base,
                static_cast<unsigned>(struct_id));
  return buf;
}

// ---- MCAP -----------------------------------------------------------------

struct DataTopic {
  uint32_t channel_id = 0;
  uint16_t struct_id = 0;
  bool pinned_slot = false;
  std::string topic;
  std::string schema_name;
  std::string payload_profile;
  std::string struct_name;
  uint16_t contract_version = 0;
  uint32_t payload_size = 0;
  uint16_t schema_id = 0;
  uint16_t mcap_channel_id = 0;
  uint64_t messages = 0;
  uint64_t bytes = 0;
};

struct ChannelKinds {
  bool gap = false;
  bool trigger = false;
  bool session = false;
  uint16_t gap_channel = 0;
  uint16_t trigger_channel = 0;
  uint16_t session_channel = 0;
};

DataTopic* FindTopic(std::vector<DataTopic>& topics, uint32_t channel_id,
                     uint16_t struct_id) {
  for (DataTopic& t : topics) {
    if (t.channel_id == channel_id && t.struct_id == struct_id) {
      return &t;
    }
  }
  return nullptr;
}

int FailExport(const ExportOptions& opt, ExportSummary* s,
               const std::string& error, int exit_code) {
  s->ok = false;
  s->error = error;
  s->exit_code = exit_code;
  if (!opt.out_path.empty()) {
    struct stat st {};
    if (stat(opt.out_path.c_str(), &st) == 0) {
      s->removed_partial = std::remove(opt.out_path.c_str()) == 0;
    }
  }
  return exit_code;
}

void RenderExportJson(std::ostream& out, const ExportSummary& s) {
  Json j(out);
  j.BeginObject();
  j.KeyStr("tool", "thyme-replay");
  j.KeyStr("command", "export");
  j.KeyBool("ok", s.ok);
  j.KeyU64("exit_code", static_cast<uint64_t>(s.exit_code));
  if (s.error.empty()) {
    j.KeyNull("error");
  } else {
    j.KeyStr("error", s.error);
  }
  j.KeyStr("format", s.format);
  j.KeyStr("encode", s.encode);
  j.KeyU64("encoded_bytes", s.encoded_bytes);
  j.KeyU64("encode_ns", s.encode_ns);
  j.KeyStr("package", s.package_dir);
  j.KeyStr("out", s.out_path);
  j.KeyStr("schema_state", s.schema_state);
  if (s.schema_ref.empty()) {
    j.KeyNull("schema_ref");
  } else {
    j.KeyStr("schema_ref", s.schema_ref);
  }
  j.KeyU64("events", s.events);
  j.KeyU64("data", s.data_messages);
  j.KeyU64("gaps", s.gap_messages);
  j.KeyU64("triggers", s.trigger_messages);
  j.KeyU64("sessions", s.session_messages);
  j.KeyU64("payload_bytes", s.payload_bytes);
  j.KeyU64("crc_checked", s.crc_checked);
  j.KeyU64("crc_bad", s.crc_bad);
  j.KeyU64("crc_skipped", s.crc_skipped);
  j.KeyU64("first_mono_ns", s.first_mono_ns);
  j.KeyU64("last_mono_ns", s.last_mono_ns);
  j.KeyU64("schemas", s.schemas);
  j.KeyU64("channels", s.channels);
  j.KeyU64("output_bytes", s.output_bytes);
  j.KeyBool("removed_partial", s.removed_partial);
  j.KeyBool("decode_refused", s.decode_refused);
  j.Key("channel_counts");
  j.BeginArray();
  for (const ExportChannelSummary& c : s.per_channel) {
    j.BeginObject();
    j.KeyU64("id", c.id);
    j.KeyStr("name", c.name);
    j.KeyStr("kind", c.kind);
    j.KeyU64("messages", c.messages);
    j.KeyU64("bytes", c.bytes);
    j.EndObject();
  }
  j.EndArray();
  j.EndObject();
  out << '\n';
}

int RenderExportSummary(const ExportOptions& opt, const ExportSummary& s,
                        std::ostream& out) {
  if (opt.json) {
    RenderExportJson(out, s);
    return s.exit_code;
  }
  out << "export  : " << (s.ok ? "OK" : "FAILED") << "  format=" << s.format
      << "  out=" << s.out_path << '\n';
  if (!s.error.empty()) {
    out << "error   : " << s.error << '\n';
  }
  if (s.decode_refused) {
    out << "decode  : refused (raw export kept)\n";
  }
  out << "events  : " << s.events << " (data=" << s.data_messages
      << " gap=" << s.gap_messages << " trigger=" << s.trigger_messages
      << " session=" << s.session_messages << ")\n";
  out << "payload : " << FormatBytes(s.payload_bytes) << "  crc checked="
      << s.crc_checked << " bad=" << s.crc_bad << " skipped=" << s.crc_skipped
      << '\n';
  out << "span    : mono_ns " << s.first_mono_ns << " .. " << s.last_mono_ns
      << '\n';
  out << "schema  : " << s.schema_state;
  if (!s.schema_ref.empty()) {
    out << " (" << s.schema_ref << ')';
  }
  out << '\n';
  out << "output  : " << FormatBytes(s.output_bytes) << "  channels="
      << s.channels << " schemas=" << s.schemas << '\n';
  if (s.encode != "raw") {
    out << "encode  : " << s.encode << "  raw=" << FormatBytes(s.payload_bytes)
        << "  encoded=" << FormatBytes(s.encoded_bytes)
        << "  cpu_ns=" << s.encode_ns << '\n';
  }
  out << std::left << std::setw(3) << "ID" << ' ' << std::setw(14) << "NAME"
      << ' ' << std::setw(8) << "KIND" << std::right << std::setw(10)
      << "MESSAGES" << std::setw(12) << "BYTES" << '\n';
  for (const ExportChannelSummary& c : s.per_channel) {
    out << std::left << std::setw(3) << c.id << ' ' << std::setw(14) << c.name
        << ' ' << std::setw(8) << c.kind << std::right << std::setw(10)
        << c.messages << std::setw(12) << FormatBytes(c.bytes) << '\n';
  }
  return s.exit_code;
}

int ExportMcap(replay::ReplayPackage& pkg, const ExportOptions& opt,
               const Window& w, ExportSummary* s, std::ostream& out) {
  (void)out;
  const std::vector<replay::ReplayChannel>& channels = pkg.channels();
  std::vector<DataTopic> topics;
  std::vector<ChannelKinds> kinds(channels.size());
  std::string err;

  // Pass 1: discover the exact topics/event kinds inside the window and
  // validate per-frame CRCs before anything is written (no partial file for a
  // corrupt package).
  if (!ForEachEvent(pkg, w, /*restart=*/false, opt.redact_channel, &err,
                    [&](const replay::ReplayEvent& ev) {
        NoteWindowTime(ev, s);
        NoteEvent(ev, s);
        if (ev.channel >= channels.size()) {
          err = "event channel out of range";
          return false;
        }
        ChannelKinds& ck = kinds[ev.channel];
        if (ev.kind == replay::ReplayKind::kGap) {
          ck.gap = true;
        } else if (ev.kind == replay::ReplayKind::kTrigger) {
          ck.trigger = true;
        } else if (ev.kind == replay::ReplayKind::kSession) {
          ck.session = true;
        } else if (ev.kind == replay::ReplayKind::kData) {
          const bool pinned_slot =
              channels[ev.channel].kind ==
              replay::ReplayChannelKind::kPinnedSlot;
          if (!CheckFrame(ev, pinned_slot, opt, s, &err)) {
            return false;
          }
          if (FindTopic(topics, ev.channel, static_cast<uint16_t>(ev.struct_id)) == nullptr) {
            if (topics.size() >= 100) {
              err = "more than 100 data topics; MCAP export limit is 128 "
                    "schemas (see docs/guides/replay.md)";
              return false;
            }
            DataTopic t;
            t.channel_id = ev.channel;
            t.struct_id = static_cast<uint16_t>(ev.struct_id);
            const replay::ReplayChannel& ch = channels[ev.channel];
            t.pinned_slot = ch.kind == replay::ReplayChannelKind::kPinnedSlot;
            const std::string ch_part = SanitizeTopicPart(ch.name);
            const ThymeStructDesc* desc = FindDesc(ch, static_cast<uint16_t>(ev.struct_id));
            t.struct_name = desc != nullptr ? StructName(*desc) : std::string();
            if (t.struct_name.empty()) {
              t.struct_name = "struct_" + std::to_string(ev.struct_id);
            }
            if (t.pinned_slot) {
              t.topic = "thyme/" + ch_part + "/raw";
              t.schema_name = "thyme." + ch_part + ".raw_frame";
              if (opt.decode_profile &&
                  pkg.schema_state() == schema::ReadState::kOk) {
                const schema::PayloadProfileDesc* prof =
                    FindChannelProfile(pkg.schema(), ch.name);
                if (prof != nullptr && !prof->profile.empty() &&
                    KnownPayloadProfile(prof->profile, prof->version,
                                        prof->params_digest)) {
                  t.payload_profile = prof->profile;
                  t.schema_name = "thyme.profile." +
                                  SanitizeTopicPart(prof->profile);
                }
              }
            } else {
              t.topic = "thyme/" + ch_part + "/" +
                        SanitizeTopicPart(t.struct_name);
              t.schema_name = "thyme." + t.struct_name;
            }
            if (t.schema_name.empty()) {
              t.schema_name = "thyme.struct";
            }
            // Topic names are unique per (channel, struct) by construction;
            // disambiguate a duplicate struct name inside one channel.
            for (const DataTopic& other : topics) {
              if (other.topic == t.topic) {
                t.topic += "_" + std::to_string(ev.struct_id);
                break;
              }
            }
            if (desc != nullptr) {
              t.contract_version = desc->contract_version;
              t.payload_size = desc->payload_size;
            }
            topics.push_back(std::move(t));
          }
        }
        return true;
      })) {
    return FailExport(opt, s, err, kExitFailure);
  }

  // Channel numbering: 1 = package metadata; then data topics; then one event
  // channel per (channel, kind) actually present.
  uint32_t next_channel = kMcapPackageChannel + 1;
  uint16_t next_schema = kMcapDataSchemaBase;
  for (DataTopic& t : topics) {
    t.schema_id = next_schema++;
    t.mcap_channel_id = static_cast<uint16_t>(next_channel++);
  }
  for (ChannelKinds& ck : kinds) {
    if (ck.gap) ck.gap_channel = static_cast<uint16_t>(next_channel++);
    if (ck.trigger) ck.trigger_channel = static_cast<uint16_t>(next_channel++);
    if (ck.session) ck.session_channel = static_cast<uint16_t>(next_channel++);
  }
  if (next_channel > kMcapMaxExportedChannels || next_schema > 101) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "export needs %u MCAP channels/schemas; the writer limit is "
                  "%zu (see docs/guides/replay.md)",
                  static_cast<unsigned>(next_channel),
                  static_cast<std::size_t>(kMcapMaxExportedChannels));
    return FailExport(opt, s, buf, kExitFailure);
  }

  McapWriterOptions wopt{};
  wopt.profile = "thyme";
  wopt.library = "thyme-replay";
  wopt.include_summary = true;
  wopt.data_section_crc = true;
  wopt.buffer_bytes = opt.mcap_buffer_bytes;
  McapWriter writer;
  const McapStatus open_st = writer.Open(opt.out_path.c_str(), wopt);
  if (open_st != McapStatus::kOk) {
    return FailExport(opt, s,
                      std::string("cannot open MCAP output: ") +
                          (writer.error() != nullptr ? writer.error() : "error"),
                      kExitFailure);
  }
  bool writer_open = true;
  auto fail_writer = [&](const std::string& why) {
    if (writer_open) {
      (void)writer.Close();
      writer_open = false;
    }
    return FailExport(opt, s, why, kExitFailure);
  };

  const std::string package_ref =
      pkg.schema_state() == schema::ReadState::kOk
          ? (opt.schema_out.empty() ? "schema.json" : opt.schema_out)
          : std::string();
  const McapMetadataEntry package_meta[2] = {
      {"thyme.schema_ref", package_ref.c_str()},
      {"thyme.schema_state", schema::ReadStateName(pkg.schema_state())},
  };
  McapStatus st = writer.AddChannel(kMcapPackageChannel, 0, "thyme/package",
                                    "json", package_meta, 2);
  if (st != McapStatus::kOk) {
    return fail_writer("cannot register the package metadata channel");
  }
  std::vector<const schema::RecordDesc*> canonical_records;
  for (DataTopic& t : topics) {
    if (IsCanonicalEncode(opt.encode)) {
      if (t.pinned_slot || pkg.schema_state() != schema::ReadState::kOk) {
        return fail_writer(
            "canonical export needs a known field layout; big-frame payloads "
            "have none; use --encode=raw");
      }
      const schema::RecordDesc* rec =
          canonical::FindRecord(pkg.schema(), t.struct_id);
      if (rec == nullptr || rec->layout != schema::LayoutState::kKnown) {
        return fail_writer(
            "canonical export needs a known field layout for struct " +
            std::to_string(t.struct_id) + "; use --encode=raw");
      }
      canonical_records.push_back(rec);
      std::string schema_bytes;
      std::string serr;
      const char* schema_encoding = "protobuf";
      const char* message_encoding = "protobuf";
      if (opt.encode == ExportEncode::kProtobuf) {
        if (!canonical::RenderFileDescriptorSet(pkg.schema(), *rec,
                                                &schema_bytes, &serr)) {
          return fail_writer(serr);
        }
      } else {
        schema_bytes = canonical::RenderMsg(pkg.schema(), *rec);
        schema_encoding = "ros2msg";
        message_encoding = "cdr";
      }
      st = writer.AddSchema(t.schema_id, t.schema_name.c_str(), schema_encoding,
                            schema_bytes.data(), schema_bytes.size());
      if (st != McapStatus::kOk) {
        return fail_writer("cannot register a data schema");
      }
      const std::string schema_ref =
          SchemaRefFor(pkg, opt, t.struct_id, t.pinned_slot);
      char struct_text[8];
      char version_text[8];
      char size_text[8];
      char hash_text[32];
      std::snprintf(struct_text, sizeof(struct_text), "%u",
                    static_cast<unsigned>(t.struct_id));
      std::snprintf(version_text, sizeof(version_text), "%u",
                    static_cast<unsigned>(t.contract_version));
      std::snprintf(size_text, sizeof(size_text), "%u",
                    static_cast<unsigned>(t.payload_size));
      std::snprintf(hash_text, sizeof(hash_text), "%s",
                    schema::FormatManifestHash(rec->layout_hash).c_str());
      const char* canon_version = "1";
      const McapMetadataEntry meta[9] = {
          {"thyme.channel", channels[t.channel_id].name.c_str()},
          {"thyme.channel_kind", KindName(channels[t.channel_id].kind)},
          {"thyme.struct_id", struct_text},
          {"thyme.contract_version", version_text},
          {"thyme.payload_size", size_text},
          {"thyme.schema_ref", schema_ref.c_str()},
          {"thyme.payload_profile", t.payload_profile.c_str()},
          {"thyme.layout_hash", hash_text},
          {"thyme.canonical_version", canon_version},
      };
      st = writer.AddChannel(t.mcap_channel_id, t.schema_id, t.topic.c_str(),
                             message_encoding, meta, 9);
      if (st != McapStatus::kOk) {
        return fail_writer("cannot register a data channel");
      }
      continue;
    }
    char schema[1024];
    const int n = std::snprintf(
        schema, sizeof(schema),
        "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
        "\"title\":\"%s\","
        "\"description\":\"%s (channel %s, struct_id=%u, "
        "contract_version=%u, payload_size=%u). ThymeStructDesc carries no "
        "field layout; message bytes are the unmodified recorded payload and "
        "are not JSON.\","
        "\"type\":\"object\","
        "\"properties\":{\"data\":{\"type\":\"string\","
        "\"contentEncoding\":\"base64\"}},\"required\":[\"data\"]}",
        t.schema_name.c_str(),
        t.pinned_slot ? "Raw .raw frame segment" : "Raw thyme struct payload",
        SanitizeTopicPart(channels[t.channel_id].name).c_str(),
        static_cast<unsigned>(t.struct_id),
        static_cast<unsigned>(t.contract_version),
        static_cast<unsigned>(t.payload_size));
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(schema)) {
      return fail_writer("schema name too long");
    }
    st = writer.AddSchema(t.schema_id, t.schema_name.c_str(), "jsonschema",
                          schema, static_cast<std::size_t>(n));
    if (st != McapStatus::kOk) {
      return fail_writer("cannot register a data schema");
    }
    const std::string schema_ref =
        SchemaRefFor(pkg, opt, t.struct_id, t.pinned_slot);
    char struct_text[8];
    char version_text[8];
    char size_text[8];
    std::snprintf(struct_text, sizeof(struct_text), "%u",
                  static_cast<unsigned>(t.struct_id));
    std::snprintf(version_text, sizeof(version_text), "%u",
                  static_cast<unsigned>(t.contract_version));
    std::snprintf(size_text, sizeof(size_text), "%u",
                  static_cast<unsigned>(t.payload_size));
    const McapMetadataEntry meta[7] = {
        {"thyme.channel", channels[t.channel_id].name.c_str()},
        {"thyme.channel_kind", KindName(channels[t.channel_id].kind)},
        {"thyme.struct_id", struct_text},
        {"thyme.contract_version", version_text},
        {"thyme.payload_size", size_text},
        {"thyme.schema_ref", schema_ref.c_str()},
        {"thyme.payload_profile", t.payload_profile.c_str()},
    };
    st = writer.AddChannel(t.mcap_channel_id, t.schema_id, t.topic.c_str(),
                           t.pinned_slot ? "thyme.frame" : "thyme", meta, 7);
    if (st != McapStatus::kOk) {
      return fail_writer("cannot register a data channel");
    }
  }
  bool have_gap_schema = false;
  bool have_trigger_schema = false;
  bool have_session_schema = false;
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    const ChannelKinds& ck = kinds[i];
    const std::string ch_part = SanitizeTopicPart(channels[i].name);
    if (ck.gap) {
      if (!have_gap_schema) {
        if (writer.AddSchema(kMcapGapSchema, "thyme.GapEvent", "jsonschema",
                             kGapSchemaJson, std::strlen(kGapSchemaJson)) !=
            McapStatus::kOk) {
          return fail_writer("cannot register the gap schema");
        }
        have_gap_schema = true;
      }
      if (writer.AddChannel(ck.gap_channel, kMcapGapSchema,
                            ("thyme/" + ch_part + "/gap").c_str(),
                            "json") != McapStatus::kOk) {
        return fail_writer("cannot register a gap channel");
      }
    }
    if (ck.trigger) {
      if (!have_trigger_schema) {
        if (writer.AddSchema(kMcapTriggerSchema, "thyme.TriggerEvent",
                             "jsonschema", kTriggerSchemaJson,
                             std::strlen(kTriggerSchemaJson)) !=
            McapStatus::kOk) {
          return fail_writer("cannot register the trigger schema");
        }
        have_trigger_schema = true;
      }
      if (writer.AddChannel(ck.trigger_channel, kMcapTriggerSchema,
                            ("thyme/" + ch_part + "/trigger").c_str(),
                            "json") != McapStatus::kOk) {
        return fail_writer("cannot register a trigger channel");
      }
    }
    if (ck.session) {
      if (!have_session_schema) {
        if (writer.AddSchema(kMcapSessionSchema, "thyme.SessionEvent",
                             "jsonschema", kSessionSchemaJson,
                             std::strlen(kSessionSchemaJson)) !=
            McapStatus::kOk) {
          return fail_writer("cannot register the session schema");
        }
        have_session_schema = true;
      }
      if (writer.AddChannel(ck.session_channel, kMcapSessionSchema,
                            ("thyme/" + ch_part + "/session").c_str(),
                            "json") != McapStatus::kOk) {
        return fail_writer("cannot register a session channel");
      }
    }
  }

  // ABI-09/ABI-10 sidecar linkage as the first message: external tools do not
  // have to guess what schema.json means for these payloads.
  {
    std::ostringstream body;
    Json j(body);
    j.BeginObject();
    j.KeyStr("event", "package");
    j.KeyStr("format", "thyme-replay-mcap");
    j.KeyU64("version", 1);
    j.KeyStr("package", pkg.dir());
    j.Key("schema");
    j.BeginObject();
    if (pkg.schema_state() == schema::ReadState::kOk) {
      j.KeyStr("ref", package_ref);
      j.KeyStr("state", "ok");
      j.KeyU64("version", pkg.schema().version);
      j.KeyBool("partial", pkg.schema().partial);
      j.KeyU64("records", pkg.schema().records.size());
    } else {
      j.KeyNull("ref");
      j.KeyStr("state", schema::ReadStateName(pkg.schema_state()));
      j.KeyNull("version");
      j.KeyNull("records");
    }
    j.EndObject();
    j.Key("manifest");
    j.BeginObject();
    j.KeyBool("declared", pkg.manifest_declared());
    if (pkg.manifest_declared()) {
      j.KeyStr("hash", schema::FormatManifestHash(pkg.manifest_hash()));
      j.KeyU64("version", pkg.manifest_version());
    } else {
      j.KeyNull("hash");
      j.KeyNull("version");
    }
    j.EndObject();
    j.KeyU64("first_mono_ns", s->first_mono_ns);
    j.KeyU64("last_mono_ns", s->last_mono_ns);
    j.EndObject();
    const std::string text = body.str();
    // An empty window still gets one package message; stamp it at the package
    // start rather than at 0 so the MCAP timeline stays meaningful.
    const uint64_t stamp =
        s->events == 0 ? pkg.first_mono_ns() : s->first_mono_ns;
    st = writer.WriteMessage(kMcapPackageChannel, stamp, text.data(),
                             text.size());
    if (st != McapStatus::kOk) {
      return fail_writer("cannot write the package metadata message");
    }
  }

  // Pass 2: write. CRC was already validated in pass 1.
  std::string write_err;
  const bool wrote = ForEachEvent(pkg, w, /*restart=*/true,
                                  opt.redact_channel, &write_err,
                                  [&](const replay::ReplayEvent& ev) {
    if (ev.kind == replay::ReplayKind::kData) {
      DataTopic* t = FindTopic(topics, ev.channel, static_cast<uint16_t>(ev.struct_id));
      if (t == nullptr) {
        write_err = "internal: unseen data topic";
        return false;
      }
      const uint8_t* bytes = ev.payload;
      uint32_t nbytes = ev.len;
      std::string encoded;
      if (IsCanonicalEncode(opt.encode)) {
        const schema::RecordDesc* rec =
            pkg.schema_state() == schema::ReadState::kOk
                ? canonical::FindRecord(pkg.schema(), t->struct_id)
                : nullptr;
        if (rec == nullptr) {
          write_err = "canonical export needs a known field layout; use --encode=raw";
          return false;
        }
        const canonical::Encoding enc = opt.encode == ExportEncode::kCdr
                                            ? canonical::Encoding::kCdr
                                            : canonical::Encoding::kProtobuf;
        if (!canonical::Encode(enc, pkg.schema(), *rec, ev.payload, ev.len,
                               &encoded, &write_err)) {
          return false;
        }
        bytes = reinterpret_cast<const uint8_t*>(encoded.data());
        nbytes = static_cast<uint32_t>(encoded.size());
      }
      if (writer.WriteMessage(t->mcap_channel_id, ev.mono_ns, bytes, nbytes) !=
          McapStatus::kOk) {
        write_err = "MCAP message write failed";
        return false;
      }
      t->messages += 1;
      t->bytes += ev.len;
      return true;
    }
    if (ev.channel >= kinds.size()) {
      write_err = "event channel out of range";
      return false;
    }
    const ChannelKinds& ck = kinds[ev.channel];
    char json[320];
    int n = 0;
    uint16_t mcap_channel = 0;
    if (ev.kind == replay::ReplayKind::kGap && ck.gap) {
      mcap_channel = ck.gap_channel;
      n = std::snprintf(json, sizeof(json),
                        "{\"event\":\"gap\",\"channel\":%u,\"struct_id\":%u,"
                        "\"reason\":%u,\"missed_count\":%u}",
                        static_cast<unsigned>(ev.channel),
                        static_cast<unsigned>(ev.struct_id),
                        static_cast<unsigned>(ev.gap_reason),
                        static_cast<unsigned>(ev.source_sequence));
    } else if (ev.kind == replay::ReplayKind::kTrigger && ck.trigger) {
      mcap_channel = ck.trigger_channel;
      n = std::snprintf(json, sizeof(json),
                        "{\"event\":\"trigger\",\"channel\":%u,"
                        "\"trigger_id\":%llu,\"pre_roll_ns\":%llu,"
                        "\"post_roll_ns\":%llu}",
                        static_cast<unsigned>(ev.channel),
                        static_cast<unsigned long long>(ev.trigger_id),
                        static_cast<unsigned long long>(ev.pre_roll_ns),
                        static_cast<unsigned long long>(ev.post_roll_ns));
    } else if (ev.kind == replay::ReplayKind::kSession && ck.session) {
      mcap_channel = ck.session_channel;
      n = std::snprintf(json, sizeof(json),
                        "{\"event\":\"session\",\"channel\":%u,"
                        "\"session_id\":%llu,\"reason\":%u}",
                        static_cast<unsigned>(ev.channel),
                        static_cast<unsigned long long>(ev.session_id),
                        static_cast<unsigned>(ev.session_reason));
    } else {
      return true;  // unknown row kind: not part of the documented mapping
    }
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(json)) {
      write_err = "event JSON overflow";
      return false;
    }
    if (writer.WriteMessage(mcap_channel, ev.mono_ns, json,
                            static_cast<std::size_t>(n)) != McapStatus::kOk) {
      write_err = "MCAP message write failed";
      return false;
    }
    return true;
  });
  if (!wrote) {
    return fail_writer(write_err);
  }
  const McapStatus close_st = writer.Close();
  writer_open = false;
  if (close_st != McapStatus::kOk) {
    return FailExport(opt, s,
                      std::string("MCAP close failed: ") +
                          (writer.error() != nullptr ? writer.error() : "error"),
                      kExitFailure);
  }
  const uint64_t expected_messages = 1 + s->events;
  if (writer.message_count() != expected_messages) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "MCAP message count %llu != expected %llu",
                  static_cast<unsigned long long>(writer.message_count()),
                  static_cast<unsigned long long>(expected_messages));
    return FailExport(opt, s, buf, kExitFailure);
  }
  if (IsCanonicalEncode(opt.encode) && !canonical_records.empty()) {
    const std::string side = opt.out_path +
                             (opt.encode == ExportEncode::kCdr ? ".msg" : ".proto");
    std::ofstream schema_out(side, std::ios::binary | std::ios::trunc);
    if (!schema_out) {
      return FailExport(opt, s, "cannot write " + side, kExitFailure);
    }
    if (opt.encode == ExportEncode::kProtobuf) {
      schema_out << canonical::RenderProtoSet(pkg.schema(), canonical_records);
    } else {
      for (const schema::RecordDesc* rec : canonical_records) {
        schema_out << canonical::RenderMsg(pkg.schema(), *rec);
      }
    }
    schema_out.flush();
    if (!schema_out.good()) {
      return FailExport(opt, s, "cannot write " + side, kExitFailure);
    }
  }
  s->schemas = writer.schema_count();
  s->channels = writer.channel_count();
  struct stat ost {};
  s->output_bytes = stat(opt.out_path.c_str(), &ost) == 0
                        ? static_cast<uint64_t>(ost.st_size)
                        : writer.bytes_written();
  for (std::size_t i = 0; i < s->per_channel.size(); ++i) {
    for (const DataTopic& t : topics) {
      if (t.channel_id == i) {
        s->per_channel[i].messages += t.messages;
        s->per_channel[i].bytes += t.bytes;
      }
    }
  }
  return FinishExport(s, ProfileDecodeError(pkg, opt));
}

// ---- NDJSON ---------------------------------------------------------------

int ExportNdjson(replay::ReplayPackage& pkg, const ExportOptions& opt,
                 const Window& w, ExportSummary* s, std::ostream& out) {
  (void)out;
  const std::vector<replay::ReplayChannel>& channels = pkg.channels();
  const std::string package_ref =
      pkg.schema_state() == schema::ReadState::kOk
          ? (opt.schema_out.empty() ? "schema.json" : opt.schema_out)
          : std::string();
  std::ofstream f(opt.out_path, std::ios::binary | std::ios::trunc);
  if (!f) {
    return FailExport(opt, s, "cannot open NDJSON output " + opt.out_path,
                      kExitFailure);
  }
  {
    std::ostringstream body;
    Json j(body);
    j.BeginObject();
    j.KeyStr("record", "header");
    j.KeyStr("format", "thyme-replay-ndjson");
    j.KeyU64("version", 1);
    if (IsCanonicalEncode(opt.encode)) {
      j.KeyStr("canonical_encoding", ExportEncodeName(opt.encode));
      j.KeyU64("canonical_version", canonical::kVersion);
    }
    j.KeyStr("package", pkg.dir());
    j.Key("schema");
    j.BeginObject();
    j.KeyStr("state", schema::ReadStateName(pkg.schema_state()));
    if (pkg.schema_state() == schema::ReadState::kOk) {
      j.KeyStr("ref", package_ref);
      j.KeyU64("version", pkg.schema().version);
      j.KeyBool("partial", pkg.schema().partial);
      j.KeyU64("records", pkg.schema().records.size());
    } else {
      j.KeyNull("ref");
      j.KeyNull("version");
      j.KeyNull("records");
    }
    j.EndObject();
    j.Key("manifest");
    j.BeginObject();
    j.KeyBool("declared", pkg.manifest_declared());
    if (pkg.manifest_declared()) {
      j.KeyStr("hash", schema::FormatManifestHash(pkg.manifest_hash()));
      j.KeyU64("version", pkg.manifest_version());
    } else {
      j.KeyNull("hash");
      j.KeyNull("version");
    }
    j.EndObject();
    if (w.has_start || w.has_to) {
      j.Key("window");
      j.BeginObject();
      j.KeyBool("has_from", w.has_start);
      j.KeyU64("from_ns", w.start_ns);
      j.KeyBool("has_to", w.has_to);
      j.KeyU64("to_ns", w.to_ns);
      j.EndObject();
    } else {
      j.KeyNull("window");
    }
    j.Key("channels");
    j.BeginArray();
    for (const replay::ReplayChannel& c : channels) {
      j.BeginObject();
      j.KeyU64("id", c.id);
      j.KeyStr("name", c.name);
      j.KeyStr("kind", KindName(c.kind));
      j.Key("registry");
      j.BeginArray();
      for (const ThymeStructDesc& d : c.registry) {
        j.BeginObject();
        j.KeyU64("struct_id", d.struct_id);
        j.KeyStr("name", StructName(d));
        j.KeyU64("contract_version", d.contract_version);
        j.KeyU64("payload_size", d.payload_size);
        j.EndObject();
      }
      j.EndArray();
      j.EndObject();
    }
    j.EndArray();
    j.EndObject();
    f << Compact(body.str()) << '\n';
  }

  std::string err;
  const bool streamed = ForEachEvent(pkg, w, /*restart=*/false,
                                     opt.redact_channel, &err,
                                     [&](const replay::ReplayEvent& ev) {
    if (ev.channel >= channels.size()) {
      err = "event channel out of range";
      return false;
    }
    const replay::ReplayChannel& ch = channels[ev.channel];
    const bool ev_raw = ch.kind == replay::ReplayChannelKind::kPinnedSlot;
    if (!CheckFrame(ev, ev_raw, opt, s, &err)) {
      return false;
    }
    NoteWindowTime(ev, s);
    NoteEvent(ev, s);
    std::ostringstream head;
    head << "{\"record\":\"event\",\"t\":" << ev.mono_ns
         << ",\"channel\":" << ev.channel << ",\"channel_name\":\""
         << JsonEscape(ch.name) << "\",\"kind\":\"" << replay::ReplayKindName(ev.kind)
         << "\",\"struct_id\":" << ev.struct_id
         << ",\"record_sequence\":" << ev.record_sequence;
    if (ev.kind == replay::ReplayKind::kData) {
      const bool raw = ch.kind == replay::ReplayChannelKind::kPinnedSlot;
      std::string encoded;
      const uint8_t* bytes = ev.payload;
      uint32_t nbytes = ev.len;
      if (IsCanonicalEncode(opt.encode)) {
        if (raw || pkg.schema_state() != schema::ReadState::kOk) {
          err = "canonical export needs a known field layout; use --encode=raw";
          return false;
        }
        const schema::RecordDesc* rec =
            canonical::FindRecord(pkg.schema(), ev.struct_id);
        if (rec == nullptr) {
          err = "canonical export needs a known field layout for struct " +
                std::to_string(ev.struct_id) + "; use --encode=raw";
          return false;
        }
        const canonical::Encoding enc = opt.encode == ExportEncode::kCdr
                                            ? canonical::Encoding::kCdr
                                            : canonical::Encoding::kProtobuf;
        if (!canonical::Encode(enc, pkg.schema(), *rec, ev.payload, ev.len,
                               &encoded, &err)) {
          return false;
        }
        bytes = reinterpret_cast<const uint8_t*>(encoded.data());
        nbytes = static_cast<uint32_t>(encoded.size());
      }
      const std::string ref = SchemaRefFor(pkg, opt, static_cast<uint16_t>(ev.struct_id), raw);
      head << ",\"source_sequence\":" << ev.source_sequence
           << ",\"len\":" << nbytes << ",\"payload_kind\":\""
           << (IsCanonicalEncode(opt.encode)
                   ? ExportEncodeName(opt.encode)
                   : (raw ? "raw_frame" : "record"))
           << "\"";
      if (raw) {
        head << ",\"raw_offset\":" << ev.raw_offset
             << ",\"flags\":" << ev.frame_flags
             << ",\"payload_crc\":" << ev.payload_crc;
        if (opt.decode_profile &&
            pkg.schema_state() == schema::ReadState::kOk) {
          const schema::PayloadProfileDesc* prof =
              FindChannelProfile(pkg.schema(), ch.name);
          if (prof != nullptr && !prof->profile.empty()) {
            head << ",\"payload_profile\":\"" << JsonEscape(prof->profile)
                 << "\",\"profile_version\":" << prof->version;
          } else {
            head << ",\"payload_profile\":null";
          }
        }
      }
      head << ",\"schema_ref\":";
      if (ref.empty()) {
        head << "null";
      } else {
        head << '"' << JsonEscape(ref) << '"';
      }
      head << ",\"payload\":\"";
      f << head.str();
      WriteBase64(f, bytes, nbytes);
      f << "\"}\n";
    } else if (ev.kind == replay::ReplayKind::kGap) {
      head << ",\"reason\":" << ev.gap_reason
           << ",\"missed_count\":" << ev.source_sequence << "}\n";
      f << head.str();
    } else if (ev.kind == replay::ReplayKind::kTrigger) {
      head << ",\"trigger_id\":" << ev.trigger_id
           << ",\"pre_roll_ns\":" << ev.pre_roll_ns
           << ",\"post_roll_ns\":" << ev.post_roll_ns << "}\n";
      f << head.str();
    } else if (ev.kind == replay::ReplayKind::kSession) {
      head << ",\"session_id\":" << ev.session_id
           << ",\"reason\":" << ev.session_reason << "}\n";
      f << head.str();
    }
    if (!f.good()) {
      err = "NDJSON write failed";
      return false;
    }
    for (ExportChannelSummary& cs : s->per_channel) {
      if (cs.id == ev.channel) {
        cs.messages += 1;
        if (ev.kind == replay::ReplayKind::kData) {
          cs.bytes += ev.len;
        }
      }
    }
    return true;
  });

  if (!streamed) {
    f.close();
    return FailExport(opt, s, err, kExitFailure);
  }
  {
    std::ostringstream body;
    Json j(body);
    j.BeginObject();
    j.KeyStr("record", "summary");
    j.KeyBool("ok", true);
    j.KeyStr("package", pkg.dir());
    j.KeyU64("events", s->events);
    j.KeyU64("data", s->data_messages);
    j.KeyU64("gaps", s->gap_messages);
    j.KeyU64("triggers", s->trigger_messages);
    j.KeyU64("sessions", s->session_messages);
    j.KeyU64("payload_bytes", s->payload_bytes);
    j.KeyU64("crc_checked", s->crc_checked);
    j.KeyU64("crc_bad", s->crc_bad);
    j.KeyU64("crc_skipped", s->crc_skipped);
    j.KeyU64("first_mono_ns", s->first_mono_ns);
    j.KeyU64("last_mono_ns", s->last_mono_ns);
    j.EndObject();
    f << Compact(body.str()) << '\n';
  }
  f.flush();
  const bool good = f.good();
  f.close();
  if (!good) {
    return FailExport(opt, s, "NDJSON write failed", kExitFailure);
  }
  struct stat ost {};
  s->output_bytes = stat(opt.out_path.c_str(), &ost) == 0
                        ? static_cast<uint64_t>(ost.st_size)
                        : 0;
  return FinishExport(s, ProfileDecodeError(pkg, opt));
}

}  // namespace

const char* ExportFormatName(ExportFormat format) {
  switch (format) {
    case ExportFormat::kMcap:
      return "mcap";
    case ExportFormat::kNdjson:
      return "ndjson";
  }
  return "unknown";
}

const char* ExportEncodeName(ExportEncode encode) {
  switch (encode) {
    case ExportEncode::kRaw:
      return "raw";
    case ExportEncode::kCbor:
      return "cbor";
    case ExportEncode::kMsgpack:
      return "msgpack";
    case ExportEncode::kTlv:
      return "tlv";
    case ExportEncode::kProtobuf:
      return "protobuf";
    case ExportEncode::kCdr:
      return "cdr";
  }
  return "unknown";
}

bool ParseExportEncode(const std::string& text, ExportEncode* out) {
  if (out == nullptr) return false;
  if (text == "raw") {
    *out = ExportEncode::kRaw;
    return true;
  }
  if (text == "protobuf") {
    *out = ExportEncode::kProtobuf;
    return true;
  }
  if (text == "cdr") {
    *out = ExportEncode::kCdr;
    return true;
  }
  selfdesc::Encoding enc;
  if (!selfdesc::ParseEncoding(text, &enc)) return false;
  if (enc == selfdesc::Encoding::kCbor) *out = ExportEncode::kCbor;
  else if (enc == selfdesc::Encoding::kMsgpack) *out = ExportEncode::kMsgpack;
  else *out = ExportEncode::kTlv;
  return true;
}

bool ParseExportFormat(const std::string& text, ExportFormat* out) {
  if (text == "mcap") {
    *out = ExportFormat::kMcap;
    return true;
  }
  if (text == "ndjson") {
    *out = ExportFormat::kNdjson;
    return true;
  }
  return false;
}

selfdesc::Encoding ToSelfDesc(ExportEncode encode) {
  if (encode == ExportEncode::kMsgpack) return selfdesc::Encoding::kMsgpack;
  if (encode == ExportEncode::kTlv) return selfdesc::Encoding::kTlv;
  return selfdesc::Encoding::kCbor;
}

const schema::RecordDesc* FindStruct(const schema::PackageSchema& schema,
                                    uint32_t struct_id) {
  for (const schema::RecordDesc& r : schema.records) {
    if (r.struct_id == struct_id) return &r;
  }
  return nullptr;
}

bool EventToSelfDesc(const replay::ReplayPackage& pkg,
                     const replay::ReplayChannel& ch,
                     const replay::ReplayEvent& ev, selfdesc::Value* out,
                     std::string* err) {
  std::vector<std::pair<std::string, selfdesc::Value>> fields;
  auto put = [&](const char* key, selfdesc::Value value) {
    fields.emplace_back(key, std::move(value));
  };
  put("kind", selfdesc::Str(replay::ReplayKindName(ev.kind)));
  put("channel", selfdesc::Str(ch.name));
  put("mono_ns", selfdesc::U64(ev.mono_ns));
  if (ev.kind == replay::ReplayKind::kData) {
    put("sequence", selfdesc::U64(ev.record_sequence));
    if (ch.kind == replay::ReplayChannelKind::kPinnedSlot) {
      std::string blob;
      if (ev.len > 0 && ev.payload != nullptr) {
        blob.assign(reinterpret_cast<const char*>(ev.payload), ev.len);
      }
      put("payload", selfdesc::Bytes(std::move(blob)));
      const schema::PayloadProfileDesc* prof = nullptr;
      if (pkg.schema_state() == schema::ReadState::kOk) {
        prof = FindChannelProfile(pkg.schema(), ch.name);
      }
      if (prof != nullptr && !prof->profile.empty()) {
        put("profile", selfdesc::Str(prof->profile));
        put("profile_version", selfdesc::U64(prof->version));
      } else {
        put("profile", selfdesc::Null());
      }
      put("layout_hash", selfdesc::U64(0));
    } else {
      if (pkg.schema_state() != schema::ReadState::kOk) {
        *err = "self-describing export needs schema.json; use --encode=raw";
        return false;
      }
      const schema::RecordDesc* rec = FindStruct(pkg.schema(), ev.struct_id);
      if (rec == nullptr || rec->layout != schema::LayoutState::kKnown) {
        *err = "self-describing export needs a known field layout for struct " +
               std::to_string(ev.struct_id) + "; use --encode=raw";
        return false;
      }
      selfdesc::Value body;
      if (!selfdesc::FieldsFromRecord(pkg.schema(), *rec, ev.payload, ev.len,
                                     &body, err)) {
        return false;
      }
      put("struct", selfdesc::Str(rec->name));
      put("layout_hash", selfdesc::U64(rec->layout_hash));
      put("fields", std::move(body));
    }
  } else if (ev.kind == replay::ReplayKind::kGap) {
    put("reason", selfdesc::U64(ev.gap_reason));
    put("missed_count", selfdesc::U64(ev.source_sequence));
  } else if (ev.kind == replay::ReplayKind::kTrigger) {
    put("trigger_id", selfdesc::U64(ev.trigger_id));
    put("pre_roll_ns", selfdesc::U64(ev.pre_roll_ns));
    put("post_roll_ns", selfdesc::U64(ev.post_roll_ns));
  } else if (ev.kind == replay::ReplayKind::kSession) {
    put("session_id", selfdesc::U64(ev.session_id));
    put("reason", selfdesc::U64(ev.session_reason));
  }
  *out = selfdesc::Map(std::move(fields));
  return true;
}

int ExportSelfDesc(replay::ReplayPackage& pkg, const ExportOptions& opt,
                   const Window& w, ExportSummary* s) {
  FILE* fp = std::fopen(opt.out_path.c_str(), "wb+");
  if (fp == nullptr) {
    return FailExport(opt, s, "cannot create " + opt.out_path, kExitFailure);
  }
  const selfdesc::Encoding enc = ToSelfDesc(opt.encode);
  unsigned char hdr[16] = {};
  std::memcpy(hdr, selfdesc::kMagic, 8);
  hdr[8] = selfdesc::kVersion;
  hdr[9] = static_cast<unsigned char>(enc);
  if (std::fwrite(hdr, 1, 16, fp) != 16) {
    std::fclose(fp);
    return FailExport(opt, s, "cannot write selfdesc header", kExitFailure);
  }
  uint32_t count = 0;
  uint64_t encode_ns = 0;
  uint64_t encoded = 0;
  std::string err;
  const bool streamed = ForEachEvent(
      pkg, w, false, opt.redact_channel, &err, [&](const replay::ReplayEvent& ev) {
        if (ev.channel >= pkg.channels().size()) {
          err = "event channel out of range";
          return false;
        }
        const replay::ReplayChannel& ch = pkg.channels()[ev.channel];
        const bool raw = ch.kind == replay::ReplayChannelKind::kPinnedSlot;
        if (!CheckFrame(ev, raw, opt, s, &err)) return false;
        selfdesc::Value rec;
        if (!EventToSelfDesc(pkg, ch, ev, &rec, &err)) return false;
        std::string chunk;
        struct timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const uint64_t t0 = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
                            static_cast<uint64_t>(ts.tv_nsec);
        if (!selfdesc::EncodeValue(enc, rec, &chunk, &err)) return false;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const uint64_t t1 = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
                            static_cast<uint64_t>(ts.tv_nsec);
        encode_ns += t1 - t0;
        if (std::fwrite(chunk.data(), 1, chunk.size(), fp) != chunk.size()) {
          err = "selfdesc write failed";
          return false;
        }
        encoded += chunk.size();
        NoteWindowTime(ev, s);
        NoteEvent(ev, s);
        if (count == 0xFFFFFFFFu) {
          err = "selfdesc: too many records";
          return false;
        }
        ++count;
        for (ExportChannelSummary& cs : s->per_channel) {
          if (cs.id == ev.channel) {
            cs.messages += 1;
            if (ev.kind == replay::ReplayKind::kData) cs.bytes += ev.len;
          }
        }
        return true;
      });
  if (!streamed) {
    std::fclose(fp);
    return FailExport(opt, s, err, kExitFailure);
  }
  unsigned char nle[4] = {
      static_cast<unsigned char>(count),
      static_cast<unsigned char>(count >> 8),
      static_cast<unsigned char>(count >> 16),
      static_cast<unsigned char>(count >> 24)};
  if (std::fseek(fp, 12, SEEK_SET) != 0 || std::fwrite(nle, 1, 4, fp) != 4) {
    std::fclose(fp);
    return FailExport(opt, s, "cannot patch selfdesc count", kExitFailure);
  }
  std::fclose(fp);
  s->encode = ExportEncodeName(opt.encode);
  s->format = s->encode;
  s->encoded_bytes = 16 + encoded;
  s->encode_ns = encode_ns;
  s->output_bytes = s->encoded_bytes;
  s->ok = true;
  s->exit_code = kExitOk;
  return kExitOk;
}

int RunExportImpl(const ExportOptions& options, std::ostream& out,
                  ExportSummary* summary) {
  ExportSummary local;
  ExportSummary* s = summary != nullptr ? summary : &local;
  *s = ExportSummary{};
  s->format = ExportFormatName(options.format);
  s->encode = ExportEncodeName(options.encode);
  s->package_dir = options.package_dir;
  if (IsSelfDescribing(options.encode) && options.format_explicit) {
    s->error = "--encode and --format are mutually exclusive; "
               "mcap/ndjson stay raw";
    s->exit_code = kExitUsage;
    return kExitUsage;
  }
  s->out_path = options.out_path;
  if (options.out_path.empty()) {
    s->error = "export needs an output path (--out <file>)";
    s->exit_code = kExitUsage;
    return kExitUsage;
  }
  if (options.mcap_buffer_bytes < 256 ||
      options.mcap_buffer_bytes > 16u * 1024u * 1024u) {
    s->error = "--mcap-buffer-bytes must be 256..16777216";
    s->exit_code = kExitUsage;
    return kExitUsage;
  }
  replay::ReplayPackage pkg;
  const replay::ReplayStatus open_st = pkg.Open(options.package_dir);
  if (open_st != replay::ReplayStatus::kOk) {
    s->error = std::string(replay::ReplayStatusName(open_st)) + ": " +
               pkg.error();
    s->exit_code = kExitFailure;
    return kExitFailure;
  }
  std::string err;
  if (!CheckManifestIdentity(pkg.manifest_declared(), pkg.manifest_hash(),
                             options.have_expect_manifest_hash,
                             options.expect_manifest_hash, &err)) {
    s->error = err;
    s->exit_code = kExitFailure;
    return kExitFailure;
  }
  s->schema_state = schema::ReadStateName(pkg.schema_state());
  if (pkg.schema_state() == schema::ReadState::kOk) {
    s->schema_ref = options.schema_out.empty() ? "schema.json"
                                               : options.schema_out;
  }
  Window w;
  if (!ResolveWindow(pkg, options, &w, &err)) {
    s->error = err;
    s->exit_code = kExitUsage;
    return kExitUsage;
  }
  SeedChannelSummary(pkg, s);

  // Optional sidecar copy, so a consumer of the exported file has the contract
  // next to it. A package without a sidecar cannot satisfy --schema-out.
  if (!options.schema_out.empty()) {
    if (pkg.schema_state() != schema::ReadState::kOk) {
      s->error = "--schema-out: the package has no schema.json sidecar";
      s->exit_code = kExitFailure;
      return kExitFailure;
    }
    std::ifstream in(pkg.dir() + "/" + schema::kFileName, std::ios::binary);
    std::ofstream dst(options.schema_out, std::ios::binary | std::ios::trunc);
    if (!in || !dst) {
      s->error = "cannot copy schema sidecar to " + options.schema_out;
      s->exit_code = kExitFailure;
      return kExitFailure;
    }
    dst << in.rdbuf();
    dst.flush();
    if (!in.good() && !in.eof()) {
      s->error = "cannot read " + pkg.dir() + "/schema.json";
      s->exit_code = kExitFailure;
      return kExitFailure;
    }
    if (!dst.good()) {
      s->error = "cannot write " + options.schema_out;
      s->exit_code = kExitFailure;
      return kExitFailure;
    }
  }

  if (IsSelfDescribing(options.encode)) {
    return ExportSelfDesc(pkg, options, w, s);
  }
  return options.format == ExportFormat::kMcap
             ? ExportMcap(pkg, options, w, s, out)
             : ExportNdjson(pkg, options, w, s, out);
}

int RunExport(const ExportOptions& options, std::ostream& out,
              ExportSummary* summary) {
  ExportSummary local;
  ExportSummary* s = summary != nullptr ? summary : &local;
  (void)RunExportImpl(options, out, s);
  return RenderExportSummary(options, *s, out);
}

}  // namespace replaycli
}  // namespace thyme
