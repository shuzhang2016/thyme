// replay_cli.cpp — argument parsing, read-only commands (list/info/verify),
// JSON/text rendering for thyme-replay. Playback lives in replay_runner.cpp.

#include "replay_cli.h"

#include "replay_export.h"
#include "replay_json.h"
#include "replay_repair.h"
#include "replay_status.h"

#include "thyme/contracts/frame_header.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/replay/replay_reader.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <sys/stat.h>

namespace thyme {
namespace replaycli {

namespace {

std::string Trim(const std::string& s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) {
    ++b;
  }
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) {
    --e;
  }
  return s.substr(b, e - b);
}

bool ParseU64(const std::string& text, uint64_t* out) {
  if (text.empty() || text[0] == '-' || text[0] == '+') {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0') {
    return false;
  }
  *out = static_cast<uint64_t>(v);
  return true;
}

// Parses a (possibly fractional) decimal number, no sign.
bool ParseDouble(const std::string& text, double* out) {
  if (text.empty()) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (errno != 0 || end == text.c_str() || *end != '\0' || !(v >= 0.0) ||
      !std::isfinite(v)) {
    return false;
  }
  *out = v;
  return true;
}

bool ScaleToNs(const std::string& number, double scale, uint64_t* out) {
  double v = 0.0;
  if (!ParseDouble(number, &v)) {
    return false;
  }
  const double ns = v * scale;
  if (!(ns >= 0.0) || ns > static_cast<double>(std::numeric_limits<uint64_t>::max())) {
    return false;
  }
  *out = static_cast<uint64_t>(ns + 0.5);
  return true;
}

bool ParseClock(const std::string& text, uint64_t* out) {
  // HH:MM:SS[.mmm] (hour count may exceed 24; minutes/seconds < 60).
  const std::size_t c1 = text.find(':');
  if (c1 == std::string::npos) {
    return false;
  }
  const std::size_t c2 = text.find(':', c1 + 1);
  if (c2 == std::string::npos) {
    return false;
  }
  uint64_t hours = 0;
  uint64_t minutes = 0;
  double seconds = 0.0;
  if (!ParseU64(text.substr(0, c1), &hours) ||
      !ParseU64(text.substr(c1 + 1, c2 - c1 - 1), &minutes) ||
      !ParseDouble(text.substr(c2 + 1), &seconds)) {
    return false;
  }
  if (minutes >= 60 || seconds >= 60.0) {
    return false;
  }
  const double total_s =
      static_cast<double>(hours) * 3600.0 + static_cast<double>(minutes) * 60.0 +
      seconds;
  const double ns = total_s * 1e9;
  if (!(ns >= 0.0) || ns > static_cast<double>(std::numeric_limits<uint64_t>::max())) {
    return false;
  }
  *out = static_cast<uint64_t>(ns + 0.5);
  return true;
}

bool SplitOnce(const std::string& text, char sep, std::string* left,
               std::string* right) {
  const std::size_t p = text.find(sep);
  if (p == std::string::npos) {
    return false;
  }
  *left = text.substr(0, p);
  *right = text.substr(p + 1);
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

uint64_t FileSize(const std::string& path) {
  struct stat st {};
  if (path.empty() || stat(path.c_str(), &st) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(st.st_size);
}

// ---- report building ----------------------------------------------------

void FillChannelReport(const replay::ReplayChannel& ch, ChannelReport* out) {
  out->id = ch.id;
  out->name = ch.name;
  out->kind = KindName(ch.kind);
  out->structs.clear();
  out->structs.reserve(ch.registry.size());
  for (const ThymeStructDesc& d : ch.registry) {
    StructReport s;
    s.struct_id = d.struct_id;
    s.name = d.name;  // bounded char[]; NUL-terminated by every writer
    s.channel = d.channel;
    s.record_key = ThymeStructDescKey(d);
    s.contract_version = d.contract_version;
    s.payload_size = d.payload_size;
    s.layout_hash = d.layout_hash;
    out->structs.push_back(std::move(s));
  }
  out->records = ch.record_count;
  out->data = ch.data_count;
  out->gaps = ch.gap_count;
  out->first_mono_ns = ch.first_mono_ns;
  out->last_mono_ns = ch.last_mono_ns;
  out->indexed = ch.has_footer_index;
  out->max_payload_bytes = ch.max_data_len;
  out->min_payload_bytes = ch.min_data_len;
  out->tbin_path = ch.tbin_path;
  out->raw_path = ch.raw_path;
  out->tbin_bytes = FileSize(ch.tbin_path);
  out->raw_bytes = FileSize(ch.raw_path);
  out->data_offset = ch.data_offset;
  out->records_end_offset = ch.records_end_offset;
  out->first_record_offset = ch.first_record_offset;
  out->last_record_offset = ch.last_record_offset;
  out->seek_entries = ch.seek_entries;
  out->seek_stride = ch.seek_stride;
  out->record_crc_checked = ch.record_crc_checked;
  out->index_crc_checked = ch.index_crc_checked;
  out->frame_crc_declared = ch.frame_crc_declared;
  out->frame_crc_absent = ch.frame_crc_absent;
  out->first_raw_offset = ch.first_raw_offset;
  out->last_raw_end = ch.last_raw_end;
}

bool OpenReport(const std::string& dir, PackageReport* report) {
  report->package_dir = dir;
  replay::ReplayPackage pkg;
  const replay::ReplayStatus st = pkg.Open(dir);
  if (st != replay::ReplayStatus::kOk) {
    report->ok = false;
    report->error = std::string(replay::ReplayStatusName(st)) + ": " +
                    pkg.error();
    return false;
  }
  report->ok = true;
  report->wal_present = pkg.wal_present();
  report->package_complete = pkg.package_complete();
  report->schema_state = schema::ReadStateName(pkg.schema_state());
  report->schema_error = pkg.schema_error();
  report->schema_partial = pkg.schema().partial;
  report->schema_version = pkg.schema().version;
  report->schema_records = static_cast<uint32_t>(pkg.schema().records.size());
  report->manifest_declared = pkg.manifest_declared();
  report->manifest_hash = pkg.manifest_hash();
  report->manifest_version = pkg.manifest_version();
  report->first_mono_ns = pkg.first_mono_ns();
  report->last_mono_ns = pkg.last_mono_ns();
  report->duration_ns = report->last_mono_ns >= report->first_mono_ns
                            ? report->last_mono_ns - report->first_mono_ns
                            : 0;
  report->channels.clear();
  report->channels.reserve(pkg.channels().size());
  report->events = 0;
  for (const replay::ReplayChannel& ch : pkg.channels()) {
    ChannelReport cr;
    FillChannelReport(ch, &cr);
    report->events += ch.record_count;
    report->channels.push_back(cr);
  }
  return true;
}

// One event pass: data bytes per channel (info) and per-frame CRC (verify).
bool ScanEvents(const std::string& dir, bool verify_crc, PackageReport* report,
                std::string* err) {
  replay::ReplayPackage pkg;
  const replay::ReplayStatus open_st = pkg.Open(dir);
  if (open_st != replay::ReplayStatus::kOk) {
    *err = std::string(replay::ReplayStatusName(open_st)) + ": " + pkg.error();
    return false;
  }
  bool last_valid = false;
  uint64_t last_mono = 0;
  report->events = 0;
  report->data_bytes = 0;
  report->mono_regressions = 0;
  for (ChannelReport& cr : report->channels) {
    cr.data_bytes = 0;
    cr.crc_checked = 0;
    cr.crc_ok = 0;
    cr.crc_bad = 0;
    cr.crc_unchecked = 0;
    cr.min_frame_len = 0;
    cr.max_frame_len = 0;
  }
  replay::ReplayEvent ev;
  replay::ReplayStatus st = replay::ReplayStatus::kOk;
  while ((st = pkg.NextEvent(&ev)) == replay::ReplayStatus::kOk) {
    report->events += 1;
    if (ev.channel >= report->channels.size()) {
      *err = "event channel out of range";
      return false;
    }
    ChannelReport& cr = report->channels[ev.channel];
    if (last_valid && ev.mono_ns < last_mono) {
      report->mono_regressions += 1;
    }
    last_mono = ev.mono_ns;
    last_valid = true;
    if (ev.kind != replay::ReplayKind::kData) {
      continue;
    }
    report->data_bytes += ev.len;
    cr.data_bytes += ev.len;
    if (!verify_crc) {
      continue;
    }
    if (ev.channel >= pkg.channels().size() ||
        pkg.channels()[ev.channel].kind !=
            replay::ReplayChannelKind::kPinnedSlot) {
      continue;  // record CRCs for .tbin rows are checked by Open()
    }
    if (ev.len > 0 && (cr.min_frame_len == 0 || ev.len < cr.min_frame_len)) {
      cr.min_frame_len = ev.len;
    }
    if (ev.len > cr.max_frame_len) {
      cr.max_frame_len = ev.len;
    }
    if (ev.payload_crc == 0) {
      cr.crc_unchecked += 1;
      continue;
    }
    cr.crc_checked += 1;
    if (ev.payload == nullptr ||
        Crc32(ev.payload, ev.len) != ev.payload_crc) {
      cr.crc_bad += 1;
      if (err->empty()) {
        *err = "frame CRC mismatch on channel " + std::to_string(ev.channel) +
               " (" + pkg.channels()[ev.channel].name + ") at mono_ns " +
               std::to_string(ev.mono_ns) + " offset " +
               std::to_string(ev.raw_offset);
      }
    } else {
      cr.crc_ok += 1;
    }
  }
  if (st != replay::ReplayStatus::kEnd) {
    *err = std::string(replay::ReplayStatusName(st)) + ": " + pkg.error();
    return false;
  }
  return true;
}

void DeriveRates(PackageReport* report) {
  const double duration_s =
      static_cast<double>(report->duration_ns) / 1000000000.0;
  for (ChannelReport& cr : report->channels) {
    if (duration_s > 0.0) {
      cr.rate_hz = static_cast<double>(cr.data) / duration_s;
      cr.bytes_per_s = static_cast<double>(cr.data_bytes) / duration_s;
    }
    if (cr.max_frame_len == 0) {
      cr.max_frame_len = cr.max_payload_bytes;
    }
    if (cr.min_frame_len == 0) {
      cr.min_frame_len = cr.max_payload_bytes;
    }
  }
}

// ---- text rendering -----------------------------------------------------

void PrintChannelTable(const PackageReport& r, bool with_rate,
                       std::ostream& out) {
  out << std::left << std::setw(3) << "ID" << ' ' << std::setw(14) << "NAME"
      << ' ' << std::setw(8) << "KIND" << std::right << std::setw(8)
      << "RECORDS" << std::setw(8) << "DATA" << std::setw(7) << "GAPS"
      << "  " << std::left << std::setw(12) << "SEEK" << std::right
      << std::setw(12) << "MAX_PAYLOAD";
  if (with_rate) {
    out << std::setw(12) << "BYTES" << std::setw(11) << "RATE_HZ"
        << std::setw(12) << "BYTES/S";
  }
  out << '\n';
  for (const ChannelReport& c : r.channels) {
    out << std::left << std::setw(3) << c.id << ' ' << std::setw(14) << c.name
        << ' ' << std::setw(8) << c.kind << std::right << std::setw(8)
        << c.records << std::setw(8) << c.data << std::setw(7) << c.gaps << "  "
        << std::left << std::setw(12)
        << ((c.indexed ? "footer:" : "scan:") + std::to_string(c.seek_entries))
        << std::right << std::setw(12) << FormatBytes(c.max_payload_bytes);
    if (with_rate) {
      out << std::setw(12) << FormatBytes(c.data_bytes) << std::setw(11)
          << std::fixed << std::setprecision(1) << c.rate_hz << std::setw(12)
          << FormatBytes(static_cast<uint64_t>(c.bytes_per_s + 0.5));
    }
    out << '\n';
  }
}

// ABI-09/ABI-10 identity shared by the JSON renderers.
void AddIdentityJson(Json& j, const PackageReport& r) {
  j.KeyStr("schema", r.schema_state);
  if (r.schema_error.empty()) {
    j.KeyNull("schema_error");
  } else {
    j.KeyStr("schema_error", r.schema_error);
  }
  j.KeyU64("schema_version", r.schema_version);
  j.KeyBool("schema_partial", r.schema_partial);
  j.KeyU64("schema_records", r.schema_records);
  j.KeyBool("manifest_declared", r.manifest_declared);
  if (r.manifest_declared) {
    j.KeyStr("record_manifest_hash",
             schema::FormatManifestHash(r.manifest_hash));
    j.KeyU64("record_manifest_version", r.manifest_version);
  } else {
    j.KeyNull("record_manifest_hash");
    j.KeyNull("record_manifest_version");
  }
}

void PrintHeader(const PackageReport& r, std::ostream& out) {
  out << "package : " << r.package_dir << '\n';
  out << "wal     : "
      << (r.wal_present ? (r.package_complete ? "present (sealed)"
                                              : "present (UNSEALED)")
                        : "missing")
      << '\n';
  out << "span    : " << FormatDurationNs(r.duration_ns) << " (mono_ns "
      << r.first_mono_ns << " .. " << r.last_mono_ns << ")\n";
  out << "schema  : ";
  if (r.schema_state == "ok") {
    out << "present (v" << r.schema_version << ", "
        << (r.schema_partial ? "partial" : "sealed") << ", "
        << r.schema_records << " record(s))";
  } else {
    out << r.schema_state;
    if (!r.schema_error.empty()) out << " (" << r.schema_error << ")";
  }
  out << '\n';
  out << "manifest: ";
  if (r.manifest_declared) {
    out << schema::FormatManifestHash(r.manifest_hash)
        << " version=" << r.manifest_version;
  } else {
    out << "absent (legacy package: no ABI-10 manifest block)";
  }
  out << '\n';
  out << "channels: " << r.channels.size();
  if (r.events != 0) {
    out << "   events: " << r.events;
  }
  out << '\n';
}

// ABI-11: per-channel registry identity exposed by list/info. Legacy
// packages report empty keys and a null layout_hash (map by struct_id).
void AddStructRegistryJson(Json& j, const ChannelReport& c) {
  j.Key("structs");
  j.BeginArray();
  for (const StructReport& s : c.structs) {
    j.BeginObject();
    j.KeyU64("struct_id", s.struct_id);
    j.KeyStr("name", s.name);
    if (s.channel.empty()) {
      j.KeyNull("channel");
    } else {
      j.KeyStr("channel", s.channel);
    }
    if (s.record_key.empty()) {
      j.KeyNull("record_key");
    } else {
      j.KeyStr("record_key", s.record_key);
    }
    j.KeyU64("contract_version", s.contract_version);
    j.KeyU64("payload_size", s.payload_size);
    if (s.layout_hash == schema::kLayoutHashNone) {
      j.KeyNull("layout_hash");
    } else {
      j.KeyStr("layout_hash", schema::FormatManifestHash(s.layout_hash));
    }
    j.EndObject();
  }
  j.EndArray();
}

// REP-06: seek/index, offset and geometry metadata shared by list/info/verify.
// The CRC object states what was actually verified at Open (record CRCs and the
// REC-04 index footer); per-frame payload CRC results are verify-only and are
// emitted by RenderVerifyJson.
void AddChannelSeekJson(Json& j, const ChannelReport& c) {
  j.Key("seek");
  j.BeginObject();
  j.KeyStr("index", c.indexed ? "footer" : "scan");
  j.KeyBool("indexed", c.indexed);
  j.KeyU64("entries", c.seek_entries);
  j.KeyU64("stride", c.seek_stride);
  j.KeyStr("complexity", "O(log n)");
  j.EndObject();
  j.Key("offsets");
  j.BeginObject();
  j.KeyU64("tbin_data", c.data_offset);
  j.KeyU64("records_end", c.records_end_offset);
  j.KeyU64("first_record", c.first_record_offset);
  j.KeyU64("last_record", c.last_record_offset);
  j.KeyU64("records_bytes",
           c.records_end_offset >= c.data_offset
               ? c.records_end_offset - c.data_offset
               : 0);
  j.KeyU64("first_raw", c.first_raw_offset);
  j.KeyU64("last_raw_end", c.last_raw_end);
  j.EndObject();
  j.Key("geometry");
  j.BeginObject();
  j.KeyU64("min_payload_bytes", c.min_payload_bytes);
  j.KeyU64("max_payload_bytes", c.max_payload_bytes);
  j.KeyU64("tbin_bytes", c.tbin_bytes);
  j.KeyU64("raw_bytes", c.raw_bytes);
  j.EndObject();
  j.Key("crc");
  j.BeginObject();
  j.KeyStr("records",
           c.record_crc_checked == c.records ? "ok" : "unchecked");
  j.KeyU64("record_checked", c.record_crc_checked);
  j.KeyStr("index", c.index_crc_checked != 0 ? "ok" : "absent");
  j.KeyU64("frames_declared", c.frame_crc_declared);
  j.KeyU64("frames_absent", c.frame_crc_absent);
  j.EndObject();
}

void RenderList(const PackageReport& r, bool json, std::ostream& out) {
  if (!json) {
    PrintHeader(r, out);
    PrintChannelTable(r, false, out);
    return;
  }
  Json j(out);
  j.BeginObject();
  j.KeyStr("tool", "thyme-replay");
  j.KeyStr("command", "list");
  j.KeyStr("package", r.package_dir);
  j.KeyBool("ok", r.ok);
  j.KeyBool("wal_present", r.wal_present);
  j.KeyBool("package_complete", r.package_complete);
  AddIdentityJson(j, r);
  j.KeyU64("first_mono_ns", r.first_mono_ns);
  j.KeyU64("last_mono_ns", r.last_mono_ns);
  j.KeyU64("duration_ns", r.duration_ns);
  j.KeyU64("events", r.events);
  j.Key("channels");
  j.BeginArray();
  for (const ChannelReport& c : r.channels) {
    j.BeginObject();
    j.KeyU64("id", c.id);
    j.KeyStr("name", c.name);
    j.KeyStr("kind", c.kind);
    j.KeyU64("records", c.records);
    j.KeyU64("data", c.data);
    j.KeyU64("gaps", c.gaps);
    AddStructRegistryJson(j, c);
    j.KeyU64("first_mono_ns", c.first_mono_ns);
    j.KeyU64("last_mono_ns", c.last_mono_ns);
    j.KeyBool("indexed", c.indexed);
    j.KeyU64("max_payload_bytes", c.max_payload_bytes);
    j.KeyU64("tbin_bytes", c.tbin_bytes);
    j.KeyU64("raw_bytes", c.raw_bytes);
    j.KeyStr("tbin", c.tbin_path);
    if (c.raw_path.empty()) {
      j.KeyNull("raw");
    } else {
      j.KeyStr("raw", c.raw_path);
    }
    AddChannelSeekJson(j, c);
    j.EndObject();
  }
  j.EndArray();
  j.EndObject();
  out << '\n';
}

void RenderInfo(const PackageReport& r, std::ostream& out) {
  PrintHeader(r, out);
  out << "bytes   : " << FormatBytes(r.data_bytes) << " (data payload)\n";
  PrintChannelTable(r, true, out);
}

void RenderInfoJson(const PackageReport& r, std::ostream& out) {
  Json j(out);
  j.BeginObject();
  j.KeyStr("tool", "thyme-replay");
  j.KeyStr("command", "info");
  j.KeyStr("package", r.package_dir);
  j.KeyBool("ok", r.ok);
  j.KeyBool("wal_present", r.wal_present);
  j.KeyBool("package_complete", r.package_complete);
  AddIdentityJson(j, r);
  j.KeyU64("first_mono_ns", r.first_mono_ns);
  j.KeyU64("last_mono_ns", r.last_mono_ns);
  j.KeyU64("duration_ns", r.duration_ns);
  j.KeyU64("events", r.events);
  j.KeyU64("data_bytes", r.data_bytes);
  j.Key("channels");
  j.BeginArray();
  for (const ChannelReport& c : r.channels) {
    j.BeginObject();
    j.KeyU64("id", c.id);
    j.KeyStr("name", c.name);
    j.KeyStr("kind", c.kind);
    j.KeyU64("records", c.records);
    j.KeyU64("data", c.data);
    j.KeyU64("gaps", c.gaps);
    AddStructRegistryJson(j, c);
    j.KeyU64("bytes", c.data_bytes);
    j.KeyDouble("rate_hz", c.rate_hz);
    j.KeyDouble("bytes_per_s", c.bytes_per_s);
    j.KeyU64("min_payload_bytes", c.min_payload_bytes);
    j.KeyU64("max_payload_bytes", c.max_payload_bytes);
    j.KeyBool("indexed", c.indexed);
    AddChannelSeekJson(j, c);
    j.EndObject();
  }
  j.EndArray();
  j.EndObject();
  out << '\n';
}

void RenderVerify(const PackageReport& r, bool ok, const std::string& error,
                  std::ostream& out) {
  PrintHeader(r, out);
  std::size_t bad_channels = 0;
  for (const ChannelReport& c : r.channels) {
    if (c.crc_bad != 0) {
      ++bad_channels;
    }
  }
  out << "verify  : " << (ok ? "OK" : "FAILED") << '\n';
  if (!error.empty()) {
    out << "error   : " << error << '\n';
  }
  out << "order   : " << (r.mono_regressions == 0 ? "monotonic" : "REGRESSIONS")
      << " (" << r.mono_regressions << ")\n";
  out << "records : .tbin record CRCs validated for every channel at open\n";
  out << "frames  : \"checked\" = payload_crc verified, \"skipped\" = recorder "
         "CRC disabled, \"bad\" = mismatch\n";
  out << std::left << std::setw(3) << "ID" << ' ' << std::setw(14) << "NAME"
      << ' ' << std::setw(8) << "KIND" << std::right << std::setw(8) << "DATA"
      << std::setw(8) << "BYTES" << std::setw(10) << "CHECKED"
      << std::setw(10) << "SKIPPED" << std::setw(7) << "BAD" << std::setw(10)
      << "MIN_LEN" << std::setw(10) << "MAX_LEN" << std::setw(13) << "SEEK"
      << '\n';
  for (const ChannelReport& c : r.channels) {
    out << std::left << std::setw(3) << c.id << ' ' << std::setw(14) << c.name
        << ' ' << std::setw(8) << c.kind << std::right << std::setw(8) << c.data
        << std::setw(8) << c.data_bytes << std::setw(10) << c.crc_checked
        << std::setw(10) << c.crc_unchecked << std::setw(7) << c.crc_bad
        << std::setw(10) << c.min_frame_len << std::setw(10) << c.max_frame_len
        << std::setw(13)
        << ((c.indexed ? "footer:" : "scan:") + std::to_string(c.seek_entries))
        << '\n';
  }
  if (bad_channels != 0) {
    out << "corrupt : " << bad_channels << " channel(s)\n";
  }
}

void RenderVerifyJson(const PackageReport& r, bool ok, const std::string& error,
                      std::ostream& out) {
  Json j(out);
  j.BeginObject();
  j.KeyStr("tool", "thyme-replay");
  j.KeyStr("command", "verify");
  j.KeyStr("package", r.package_dir);
  j.KeyBool("ok", ok);
  // ARCH-16: replicate the lifecycle WAL state in the JSON audit as well.
  {
    triggered::PackageLifecycleState life{};
    if (triggered::ReadPackageLifecycle(r.package_dir, &life)) {
      j.KeyStr("lifecycle", triggered::PackageLifecycleName(life.state));
      if (life.abort_reason != triggered::PackageAbortReason::kNone) {
        j.KeyStr("abort_reason",
                 triggered::PackageAbortReasonName(life.abort_reason));
      }
      j.KeyBool("lifecycle_legacy", life.legacy);
    } else {
      j.KeyStr("lifecycle", "unknown");
    }
  }
  if (error.empty()) {
    j.KeyNull("error");
  } else {
    j.KeyStr("error", error);
  }
  j.KeyBool("wal_present", r.wal_present);
  j.KeyBool("package_complete", r.package_complete);
  AddIdentityJson(j, r);
  j.KeyU64("events", r.events);
  j.KeyU64("mono_regressions", r.mono_regressions);
  j.Key("channels");
  j.BeginArray();
  for (const ChannelReport& c : r.channels) {
    j.BeginObject();
    j.KeyU64("id", c.id);
    j.KeyStr("name", c.name);
    j.KeyStr("kind", c.kind);
    j.KeyU64("data", c.data);
    j.KeyU64("bytes", c.data_bytes);
    j.KeyU64("crc_checked", c.crc_checked);
    j.KeyU64("crc_ok", c.crc_ok);
    j.KeyU64("crc_bad", c.crc_bad);
    j.KeyU64("crc_unchecked", c.crc_unchecked);
    j.KeyU64("min_frame_len", c.min_frame_len);
    j.KeyU64("max_frame_len", c.max_frame_len);
    j.KeyBool("indexed", c.indexed);
    AddChannelSeekJson(j, c);
    j.EndObject();
  }
  j.EndArray();
  j.EndObject();
  out << '\n';
}

// ---- argv parsing -------------------------------------------------------

bool TakeValue(const std::vector<std::string>& args, std::size_t* i,
               const char* name, std::string* out, std::ostream& err) {
  if (*i + 1 >= args.size()) {
    err << "thyme-replay: " << name << " requires a value\n";
    return false;
  }
  *i += 1;
  *out = args[*i];
  return true;
}

}  // namespace

std::string FormatNs(uint64_t ns) { return std::to_string(ns); }

std::string FormatDurationNs(uint64_t ns) {
  std::ostringstream o;
  if (ns >= 1000000000ULL) {
    o << std::fixed << std::setprecision(3)
      << (static_cast<double>(ns) / 1e9) << " s";
  } else if (ns >= 1000000ULL) {
    o << std::fixed << std::setprecision(3)
      << (static_cast<double>(ns) / 1e6) << " ms";
  } else if (ns >= 1000ULL) {
    o << std::fixed << std::setprecision(3)
      << (static_cast<double>(ns) / 1e3) << " us";
  } else {
    o << ns << " ns";
  }
  return o.str();
}

std::string FormatBytes(uint64_t bytes) {
  std::ostringstream o;
  if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
    o << std::fixed << std::setprecision(2)
      << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << " GiB";
  } else if (bytes >= 1024ULL * 1024ULL) {
    o << std::fixed << std::setprecision(2)
      << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MiB";
  } else if (bytes >= 1024ULL) {
    o << std::fixed << std::setprecision(2)
      << (static_cast<double>(bytes) / 1024.0) << " KiB";
  } else {
    o << bytes << " B";
  }
  return o.str();
}

bool ParseTimeSpec(const std::string& raw, TimeSpec* out, std::string* err) {
  const std::string text = Trim(raw);
  if (text.empty()) {
    *err = "empty time value";
    return false;
  }
  TimeSpec spec;
  if (text[0] == '@') {
    if (!ParseU64(text.substr(1), &spec.ns)) {
      *err = "bad absolute mono_ns: " + text;
      return false;
    }
    spec.absolute = true;
    *out = spec;
    return true;
  }
  if (text.find(':') != std::string::npos) {
    if (!ParseClock(text, &spec.ns)) {
      *err = "bad clock value (want HH:MM:SS.mmm): " + text;
      return false;
    }
    *out = spec;
    return true;
  }
  struct Suffix {
    const char* text;
    double scale;
  };
  static const Suffix kSuffixes[] = {
      {"ns", 1.0},      {"us", 1e3},      {"ms", 1e6},
      {"s", 1e9},       {"m", 60e9},      {"h", 3600e9},
  };
  for (const Suffix& s : kSuffixes) {
    const std::size_t n = std::strlen(s.text);
    if (text.size() > n && text.compare(text.size() - n, n, s.text) == 0) {
      if (!ScaleToNs(text.substr(0, text.size() - n), s.scale, &spec.ns)) {
        *err = "bad time value: " + text;
        return false;
      }
      *out = spec;
      return true;
    }
  }
  if (!ParseU64(text, &spec.ns)) {
    *err = "bad time value (want ns, 1.5s, HH:MM:SS.mmm or @mono_ns): " + text;
    return false;
  }
  *out = spec;
  return true;
}

bool ParsePinnedSlotTarget(const std::string& text, TargetSpec* out,
                         std::string* err) {
  std::string left;
  std::string right;
  if (!SplitOnce(text, '=', &left, &right) || Trim(left).empty() ||
      Trim(right).empty()) {
    *err = "bad --pinned_slot target (want <channel>=<shm-name>): " + text;
    return false;
  }
  TargetSpec spec;
  spec.raw = text;
  spec.channel = Trim(left);
  spec.shm_name = Trim(right);
  *out = spec;
  return true;
}

bool ParseSmallTarget(const std::string& text, TargetSpec* out,
                      std::string* err) {
  std::string left;
  std::string right;
  if (!SplitOnce(text, '=', &left, &right) || Trim(left).empty() ||
      Trim(right).empty()) {
    *err =
        "bad --small target (want <channel>:<struct_id>=<ring>:<id> or "
        "<channel>=<ring>): " +
        text;
    return false;
  }
  TargetSpec spec;
  spec.raw = text;
  std::string chan;
  std::string sid;
  if (!SplitOnce(Trim(left), ':', &chan, &sid)) {
    // REP-11 auto-map form: --small <channel>=<ring>. The ring name is a
    // POSIX shm name; a trailing ":<id>" is not meaningful here (the id is
    // resolved from the key), so reject it instead of guessing.
    if (right.find(':') != std::string::npos) {
      *err = "bad --small auto-map target (want <channel>=<ring>): " + text;
      return false;
    }
    spec.channel = Trim(left);
    spec.shm_name = Trim(right);
    spec.auto_map = true;
    *out = spec;
    return true;
  }
  if (Trim(chan).empty()) {
    *err = "bad --small target left side (want <channel>:<struct_id>): " + text;
    return false;
  }
  // The ring name is a POSIX shm name; the target struct id follows the last
  // ':' so a name containing ':' still parses (shm names rarely do).
  const std::size_t p = right.rfind(':');
  if (p == std::string::npos) {
    *err = "bad --small target right side (want <ring>:<target-struct-id>): " +
           text;
    return false;
  }
  const std::string ring = Trim(right.substr(0, p));
  const std::string tid = Trim(right.substr(p + 1));
  uint64_t sid_v = 0;
  uint64_t tid_v = 0;
  if (ring.empty() || !ParseU64(Trim(sid), &sid_v) || !ParseU64(tid, &tid_v) ||
      sid_v > 0xFFFFULL || tid_v > 0xFFFFULL) {
    *err = "bad --small target (ids must be 0..65535): " + text;
    return false;
  }
  spec.channel = Trim(chan);
  spec.shm_name = ring;
  spec.struct_id = static_cast<uint16_t>(sid_v);
  spec.target_struct_id = static_cast<uint16_t>(tid_v);
  *out = spec;
  return true;
}

const ChannelReport* PackageReport::Find(const std::string& name) const {
  for (const ChannelReport& c : channels) {
    if (c.name == name) {
      return &c;
    }
  }
  return nullptr;
}

bool CheckManifestIdentity(bool declared, uint64_t hash, bool have_expect,
                           uint64_t expect_hash, std::string* err) {
  if (!have_expect) return true;
  if (!declared) {
    *err = "no ABI-10 record manifest in the package (legacy); cannot verify "
           "against the expected hash " +
           schema::FormatManifestHash(expect_hash);
    return false;
  }
  if (hash != expect_hash) {
    *err = "record-manifest mismatch: package=" +
           schema::FormatManifestHash(hash) + " expected=" +
           schema::FormatManifestHash(expect_hash) +
           " (RecordStructId values are not comparable; refusing to decode)";
    return false;
  }
  return true;
}

bool CheckExpectedManifest(const PackageReport& report, bool have_expect,
                           uint64_t expect_hash, std::string* err) {
  return CheckManifestIdentity(report.manifest_declared, report.manifest_hash,
                               have_expect, expect_hash, err);
}

namespace {

void PrintReportError(const char* command, const std::string& dir,
                      const std::string& error, bool json, std::ostream& out) {
  if (json) {
    Json j(out);
    j.BeginObject();
    j.KeyStr("tool", "thyme-replay");
    j.KeyStr("command", command);
    j.KeyStr("package", dir);
    j.KeyBool("ok", false);
    j.KeyStr("error", error);
    j.EndObject();
    out << '\n';
  } else {
    out << "thyme-replay: cannot read package " << dir << ": " << error << '\n';
  }
}

}  // namespace

int RunList(const std::string& dir, bool json, std::ostream& out,
            PackageReport* report, bool have_expect_manifest,
            uint64_t expect_manifest_hash) {
  PackageReport local;
  PackageReport* r = report != nullptr ? report : &local;
  *r = PackageReport{};
  const bool ok = OpenReport(dir, r);
  if (!ok) {
    PrintReportError("list", dir, r->error, json, out);
    return kExitFailure;
  }
  std::string merr;
  // ABI-10: refuse before rendering/decoding anything under a different
  // RecordStructId assignment.
  if (!CheckExpectedManifest(*r, have_expect_manifest, expect_manifest_hash,
                             &merr)) {
    r->ok = false;
    r->error = merr;
    PrintReportError("list", dir, merr, json, out);
    return kExitFailure;
  }
  RenderList(*r, json, out);
  return kExitOk;
}

int RunInfo(const std::string& dir, bool json, std::ostream& out,
            PackageReport* report, bool have_expect_manifest,
            uint64_t expect_manifest_hash) {
  PackageReport local;
  PackageReport* r = report != nullptr ? report : &local;
  *r = PackageReport{};
  if (!OpenReport(dir, r)) {
    PrintReportError("info", dir, r->error, json, out);
    return kExitFailure;
  }
  std::string merr;
  if (!CheckExpectedManifest(*r, have_expect_manifest, expect_manifest_hash,
                             &merr)) {
    r->ok = false;
    r->error = merr;
    PrintReportError("info", dir, merr, json, out);
    return kExitFailure;
  }
  std::string err;
  if (!ScanEvents(dir, /*verify_crc=*/false, r, &err)) {
    r->ok = false;
    r->error = err;
    PrintReportError("info", dir, err, json, out);
    return kExitFailure;
  }
  DeriveRates(r);
  if (json) {
    RenderInfoJson(*r, out);
  } else {
    RenderInfo(*r, out);
  }
  return kExitOk;
}

int RunVerify(const std::string& dir, bool json, std::ostream& out,
              PackageReport* report, bool have_expect_manifest,
              uint64_t expect_manifest_hash) {
  PackageReport local;
  PackageReport* r = report != nullptr ? report : &local;
  *r = PackageReport{};
  if (!OpenReport(dir, r)) {
    PrintReportError("verify", dir, r->error, json, out);
    return kExitFailure;
  }
  std::string merr;
  if (!CheckExpectedManifest(*r, have_expect_manifest, expect_manifest_hash,
                             &merr)) {
    // Refuse before the per-frame CRC pass: nothing is decoded under an
    // unverified id assignment.
    r->ok = false;
    r->error = merr;
    if (json) {
      RenderVerifyJson(*r, false, merr, out);
    } else {
      RenderVerify(*r, false, merr, out);
    }
    return kExitFailure;
  }
  std::string err;
  bool ok = ScanEvents(dir, /*verify_crc=*/true, r, &err);
  for (const ChannelReport& c : r->channels) {
    if (c.crc_bad != 0) {
      ok = false;
    }
  }
  if (r->mono_regressions != 0) {
    ok = false;
    if (err.empty()) {
      err = "timeline is not monotonic in mono_ns";
    }
  }
  if (!r->wal_present) {
    // Informational only: a package without a journal is still readable.
  }
  r->ok = ok;
  r->error = err;
  if (json) {
    RenderVerifyJson(*r, ok, err, out);
  } else {
    RenderVerify(*r, ok, err, out);
  }
  return ok ? kExitOk : kExitFailure;
}

int RunSchema(const std::string& dir, bool json, std::ostream& out,
              PackageReport* report) {
  PackageReport local;
  PackageReport* r = report != nullptr ? report : &local;
  *r = PackageReport{};
  replay::ReplayPackage pkg;
  const replay::ReplayStatus st = pkg.Open(dir);
  if (st != replay::ReplayStatus::kOk) {
    r->ok = false;
    r->error = std::string(replay::ReplayStatusName(st)) + ": " + pkg.error();
    PrintReportError("schema", dir, r->error, json, out);
    return kExitFailure;
  }
  const schema::PackageSchema& sc = pkg.schema();
  if (pkg.schema_state() == schema::ReadState::kAbsent) {
    r->ok = false;
    r->error = "no " + std::string(schema::kFileName) +
               " (pre-ABI-09 package: no field schema)";
    PrintReportError("schema", dir, r->error, json, out);
    return kExitFailure;
  }
  r->ok = true;
  r->schema_state = schema::ReadStateName(pkg.schema_state());
  r->schema_partial = sc.partial;
  r->schema_version = sc.version;
  r->schema_records = static_cast<uint32_t>(sc.records.size());
  r->manifest_declared = pkg.manifest_declared();
  r->manifest_hash = pkg.manifest_hash();
  r->manifest_version = pkg.manifest_version();
  if (json) {
    Json j(out);
    j.BeginObject();
    j.KeyStr("tool", "thyme-replay");
    j.KeyStr("command", "schema");
    j.KeyStr("package", dir);
    j.KeyBool("ok", true);
    j.KeyU64("schema_version", sc.version);
    j.KeyBool("partial", sc.partial);
    AddIdentityJson(j, *r);
    j.Key("records");
    j.BeginArray();
    for (const schema::RecordDesc& rec : sc.records) {
      j.BeginObject();
      j.KeyU64("struct_id", rec.struct_id);
      j.KeyStr("name", rec.name);
      j.KeyU64("contract_version", rec.contract_version);
      j.KeyU64("payload_size", rec.payload_size);
      j.KeyStr("layout", rec.layout == schema::LayoutState::kKnown ? "known"
                                                                  : "unavailable");
      j.KeyU64("contract_bytes", rec.contract_bytes);
      j.Key("fields");
      j.BeginArray();
      for (const schema::FieldDesc& f : rec.fields) {
        j.BeginObject();
        j.KeyStr("name", f.name);
        j.KeyStr("type", f.type);
        j.KeyU64("offset", f.offset);
        j.KeyU64("size", f.size);
        j.KeyU64("count", f.count);
        j.KeyU64("stride", f.stride);
        j.EndObject();
      }
      j.EndArray();
      j.EndObject();
    }
    j.EndArray();
    j.EndObject();
    out << '\n';
  } else {
    out << "package  : " << dir << '\n';
    out << schema::RenderSchemaText(sc);
  }
  return kExitOk;
}

namespace {

const char* KindLabel(integrity::PackageKind kind) {
  return kind == integrity::PackageKind::kContinuous ? "continuous" : "trigger";
}

const char* FileState(const integrity::FileStatus& f) {
  if (!f.present) return "MISSING";
  if (!f.size_ok || (f.hash_checked && !f.hash_ok)) return "MISMATCH";
  if (!f.hash_checked) return "UNVERIFIED(payload)";
  return "OK";
}

void RenderIntegrity(const std::string& dir,
                     const integrity::VerifyResult& r, bool ok,
                     std::ostream& out) {
  out << "package  : " << dir << '\n';
  out << "manifest : ";
  switch (r.state) {
    case integrity::ManifestState::kAbsent:
      out << "absent\n";
      break;
    case integrity::ManifestState::kInvalid:
      out << "INVALID\n";
      break;
    case integrity::ManifestState::kOk:
      out << "present (" << (r.signed_manifest ? "signed hmac-sha256"
                                               : "unsigned")
          << ")\n";
      break;
  }
  // ARCH-16: the lifecycle WAL is the only source of truth for the package
  // state; print it even when the integrity manifest is absent, because that
  // is exactly the case an auditor needs to distinguish (sealed vs aborted).
  {
    triggered::PackageLifecycleState life{};
    out << "lifecycle: ";
    if (triggered::ReadPackageLifecycle(dir, &life)) {
      out << triggered::PackageLifecycleName(life.state);
      if (life.abort_reason != triggered::PackageAbortReason::kNone) {
        out << " ("
            << triggered::PackageAbortReasonName(life.abort_reason) << ')';
      }
      if (life.legacy) out << " [legacy]";
      out << '\n';
    } else {
      out << "unknown (no lifecycle WAL)\n";
    }
  }
  if (r.state == integrity::ManifestState::kOk) {
    out << "kind     : " << KindLabel(r.info.kind) << '\n';
    out << "mode     : " << integrity::ManifestModeName(r.info.manifest_mode)
        << '\n';
    out << "seal_ns  : " << r.info.seal_time_ns << '\n';
    out << "session  : " << r.info.session_id << '\n';
    out << "partial  : "
        << (r.partial ? "yes (invalidated/aborted package; manifest covers "
                        "the surviving files)"
                      : "no")
        << '\n';
    if (r.info.kind == integrity::PackageKind::kContinuous) {
      out << "slice    : seq=" << r.info.slice_seq
          << " begin=" << r.info.begin_ns << " end=" << r.info.end_ns << '\n';
    } else {
      out << "trigger  : id=" << r.info.trigger_id << " t0=" << r.info.t0_ns
          << " before=" << r.info.before_ns << " after=" << r.info.after_ns
          << '\n';
    }
  }
  if (r.state == integrity::ManifestState::kOk) {
    out << "chain    : " << (r.chain_ok ? "OK" : "FAILED") << '\n';
    out << "files    : " << r.files_ok << " ok, " << r.files_missing
        << " missing, " << r.files_mismatch << " mismatched, "
        << r.files_unlisted << " unlisted, " << r.files_unverified
        << " unverified payload\n";
    out << "signature: ";
    if (!r.signed_manifest) {
      out << "absent (unsigned)\n";
    } else if (!r.signature_checked) {
      out << "unchecked (--key required)\n";
    } else {
      out << (r.signature_ok ? "OK" : "FAILED") << '\n';
    }
  }
  const std::size_t kMaxRows = 512;
  std::size_t rows = 0;
  for (const integrity::FileStatus& f : r.files) {
    if (rows++ == kMaxRows) {
      out << "file     : ... " << (r.files.size() - kMaxRows)
          << " more entries\n";
      break;
    }
    out << "file     : " << std::left << std::setw(8) << FileState(f)
        << ' ' << std::right << f.path << " (" << f.actual_size << " B";
    if (f.present && (!f.size_ok || (f.hash_checked && !f.hash_ok))) {
      out << ", manifest " << f.manifest_size << " B";
    }
    out << ")\n";
  }
  for (const std::string& u : r.unlisted) {
    out << "file     : UNLISTED " << u << '\n';
  }
  if (!r.error.empty()) out << "error    : " << r.error << '\n';
  out << "integrity: ";
  if (ok) {
    out << "OK";
    if (r.info.manifest_mode != integrity::ManifestMode::kFull) {
      out << "(" << integrity::ManifestModeName(r.info.manifest_mode)
          << "); UNVERIFIED(payload)";
    }
  } else if (r.state == integrity::ManifestState::kOk && r.partial &&
             r.intact()) {
    out << "PARTIAL (not sealed; declared scope intact)";
    if (r.info.manifest_mode != integrity::ManifestMode::kFull) {
      out << "; UNVERIFIED(payload)";
    }
  } else {
    out << "FAILED";
  }
  out << '\n';
}

void RenderIntegrityJson(const std::string& dir,
                         const integrity::VerifyResult& r, bool ok,
                         std::ostream& out) {
  Json j(out);
  j.BeginObject();
  j.KeyStr("tool", "thyme-replay");
  j.KeyStr("command", "integrity");
  j.KeyStr("package", dir);
  j.KeyBool("ok", ok);
  const char* state = r.state == integrity::ManifestState::kOk
                          ? "present"
                          : (r.state == integrity::ManifestState::kAbsent
                                 ? "absent"
                                 : "invalid");
  j.KeyStr("manifest", state);
  // ARCH-16: surface the lifecycle WAL state in the integrity JSON too.
  {
    triggered::PackageLifecycleState life{};
    if (triggered::ReadPackageLifecycle(dir, &life)) {
      j.KeyStr("lifecycle", triggered::PackageLifecycleName(life.state));
      if (life.abort_reason != triggered::PackageAbortReason::kNone) {
        j.KeyStr("abort_reason",
                 triggered::PackageAbortReasonName(life.abort_reason));
      }
      j.KeyBool("lifecycle_legacy", life.legacy);
    } else {
      j.KeyStr("lifecycle", "unknown");
    }
  }
  if (r.error.empty()) {
    j.KeyNull("error");
  } else {
    j.KeyStr("error", r.error);
  }
  j.KeyStr("kind", KindLabel(r.info.kind));
  j.KeyStr("integrity_mode", integrity::ManifestModeName(r.info.manifest_mode));
  j.KeyBool("partial", r.partial);
  j.KeyBool("intact", r.state == integrity::ManifestState::kOk && r.intact());
  j.KeyU64("seal_time_ns", r.info.seal_time_ns);
  j.KeyU64("session_id", r.info.session_id);
  j.KeyU64("trigger_id", r.info.trigger_id);
  j.KeyU64("t0_ns", r.info.t0_ns);
  j.KeyU64("before_ns", r.info.before_ns);
  j.KeyU64("after_ns", r.info.after_ns);
  j.KeyU64("slice_seq", r.info.slice_seq);
  j.KeyU64("begin_ns", r.info.begin_ns);
  j.KeyU64("end_ns", r.info.end_ns);
  j.KeyBool("signed", r.signed_manifest);
  j.KeyBool("signature_present", r.signature_present);
  j.KeyBool("signature_checked", r.signature_checked);
  j.KeyBool("signature_ok", r.signature_ok);
  j.KeyBool("chain_ok", r.chain_ok);
  j.KeyU64("files_ok", r.files_ok);
  j.KeyU64("files_missing", r.files_missing);
  j.KeyU64("files_mismatch", r.files_mismatch);
  j.KeyU64("files_unlisted", r.files_unlisted);
  j.KeyU64("files_unverified", r.files_unverified);
  j.Key("files");
  j.BeginArray();
  for (const integrity::FileStatus& f : r.files) {
    j.BeginObject();
    j.KeyStr("path", f.path);
    j.KeyStr("state", FileState(f));
    j.KeyU64("size", f.actual_size);
    j.KeyU64("manifest_size", f.manifest_size);
    j.KeyBool("hash_checked", f.hash_checked);
    j.KeyBool("hash_ok", f.hash_ok);
    j.KeyStr("sha256", f.sha256_hex);
    j.EndObject();
  }
  j.EndArray();
  j.Key("unlisted");
  j.BeginArray();
  for (const std::string& u : r.unlisted) j.Str(u);
  j.EndArray();
  j.EndObject();
  out << '\n';
}

}  // namespace

int RunIntegrity(const std::string& dir, bool json,
                 const std::string& key_path, std::ostream& out,
                 integrity::VerifyResult* result) {
  integrity::VerifyResult local;
  integrity::VerifyResult* r = result != nullptr ? result : &local;
  *r = integrity::VerifyPackageManifest(dir, key_path);
  const bool ok = r->ok();
  if (json) {
    RenderIntegrityJson(dir, *r, ok, out);
  } else {
    RenderIntegrity(dir, *r, ok, out);
  }
  return ok ? kExitOk : kExitFailure;
}

void PrintUsage(std::ostream& out, std::ostream& err) {
  out <<
      "usage: thyme-replay <command> <package-dir> [options]\n"
      "\n"
      "commands:\n"
      "  list   <pkg>   channels, kinds, counts, span, geometry, index "
      "status, completeness\n"
      "  info   <pkg>   package summary with per-channel rate and bytes\n"
      "  schema <pkg>   ABI-09 record schema sidecar (contracts + field layout)\n"
      "  play   <pkg>   replay the package into live targets\n"
      "  verify <pkg>   read-only integrity check (non-zero exit on "
      "corruption)\n"
      "  integrity <pkg>  SEC-03/REC-18 evidence manifest: per-file sha256, hash\n"
      "                   chain, optional HMAC signature; sealed -> OK,\n"
      "                   .incomplete with intact survivors -> PARTIAL, else\n"
      "                   FAILED (non-zero exit on tamper/missing/absent)\n"
      "  status [--status-file <path>] [--json]\n"
      "                 live snapshot or final summary of a play run\n"
      "  export <pkg> --out <file> [--format mcap|ndjson]\n"
      "                 [--encode raw|cbor|msgpack|tlv|protobuf|cdr]\n"
      "                 stream the merged timeline to an external format;\n"
      "                 mcap reuses the ECO-01 writer (Foxglove), ndjson is a\n"
      "                 generic base64 republish with schema sidecar refs;\n"
      "                 --encode other than raw writes a self-describing file\n"
      "  repair <input.tbin> --out <output.tbin> [--json]\n"
      "                 salvage one .tbin valid prefix; no resync, no overwrite\n"
      "\n"
      "integrity:\n"
      "  --integrity           verify also checks the evidence manifest "
      "(verify only)\n"
      "  --key <path>          HMAC-SHA256 key file for a signed manifest\n"
      "\n"
      "record manifest (ABI-10):\n"
      "  --expect-manifest-hash <0x...>\n"
      "                        require the package's record manifest to match\n"
      "                        this build; list/info/verify exit 1 and play\n"
      "                        refuses to inject on a mismatch. A legacy\n"
      "                        package without a manifest also fails closed.\n"
      "\n"
      "play targets (explicit or key-resolved; never name-guessed):\n"
      "  --pinned_slot <channel>=<shm-name>\n"
      "  --small <channel>:<struct_id>=<shm-ring-name>:<target-struct-id>\n"
      "  --small <channel>=<shm-ring-name>\n"
      "                        REP-11 topology map: resolve every struct of\n"
      "                        the package channel by (record_key,\n"
      "                        contract_version, payload_size, layout_hash)\n"
      "  --allow-missing       with the auto-map form, skip a package channel\n"
      "                        or struct the target topology does not have\n"
      "                        (default: hard error; tuple mismatches always\n"
      "                        fail, never silently remap)\n"
      "  --check-compat        print the resolved mapping table and exit\n"
      "  --dry-run             resolve targets and print mappings, do not\n"
      "                        create/attach/inject\n"
      "  big-frame targets are created by the tool (it is the producer) and\n"
      "  left in place after the run; small-struct rings must already exist\n"
      "  and are attached read-write (never created or clobbered)\n"
      "  --strict-targets      fail when a data channel has no target "
      "(exit 3)\n"
      "  --force               replace an existing big-frame channel (a live\n"
      "                        replay gate is never overridden)\n"
      "  --no-gate             do NOT hold the replay producer gate (unsafe:\n"
      "                        real producers may double-write; legacy peers)\n"
      "  --pinned_slot-slots <n>  target ring depth for created channels "
      "(default 64)\n"
      "  --no-verify-crc       skip per-frame CRC verification before inject\n"
      "  --preserve-sequences  keep recorded producer sequence numbers\n"
      "                        (default: renumber per target from 0)\n"
      "\n"
      "playback:\n"
      "  --timeline=absolute   replay explicit packages or continuous_* slices\n"
      "                        on their recorded timestamps (default)\n"
      "  --timeline=stitch     join slices with one measured sample period\n"
      "  --rate <r>            speed multiplier (default 1.0)\n"
      "  --as-fast             disable pacing\n"
      "  --pause               start paused (tty: r resume, s step, p pause, "
      "q quit;\n"
      "                        no tty: waits for SIGINT/SIGTERM)\n"
      "  --step                deliver exactly one event, then stop\n"
      "  --seek <t>            start at timeline offset t\n"
      "  --from <t>            same as --seek\n"
      "  --to <t>              stop before the first event after t "
      "(inclusive)\n"
      "  --status-file <path>  publish JSON status snapshots (and the final\n"
      "                        summary) while playing, atomically\n"
      "  --status-interval-ms <n>  live status period (default 200)\n"
      "\n"
      "interactive tty controls (with --pause):\n"
      "  p pause   r resume   s step   q quit   rate <r>   seek <t>\n"
      "\n"
      "export:\n"
      "  --format mcap|ndjson     output format (default mcap)\n"
      "  --encode raw|cbor|msgpack|tlv|protobuf|cdr\n"
      "                           raw (default) keeps mcap/ndjson bytes.\n"
      "                           cbor/msgpack/tlv are self-describing files\n"
      "                           and are not combinable with --format.\n"
      "                           protobuf/cdr encode inside mcap/ndjson from\n"
      "                           a known field layout.\n"
      "  --schema-out <path>      copy <pkg>/schema.json and reference it\n"
      "  --redact <channel>       SEC-06: zero this channel's payload on export\n"
      "  --mcap-buffer-bytes <n>  bounded writer buffer (default 65536)\n"
      "  --from/--to              export window (same time forms as play)\n"
      "  --no-verify-crc          skip per-frame payload CRC validation\n"
      "  --decode-profile         REC-27: resolve payload profiles; unknown\n"
      "                           names refuse decode but keep the raw export\n"
      "\n"
      "time values: <int> = ns offset, 250ms / 1.5s / 2m / 1h, "
      "HH:MM:SS.mmm, @<mono_ns> absolute\n"
      "all commands accept --json\n"
      "\n"
      "exit codes: 0 ok, 1 failure/verification error, 2 usage, "
      "3 target configuration error\n";
  (void)err;
}

namespace {

struct ParsedCommand {
  std::string command;
  std::string package_dir;
  bool json = false;
  bool verify_integrity = false;
  std::string key_path;
  bool have_expect_manifest_hash = false;
  uint64_t expect_manifest_hash = 0;
  PlayOptions play;
  // REP-08 status document path for play/status.
  std::string status_file;
  uint32_t status_interval_ms = 200;
  // REP-09 export options.
  std::string out_path;
  ExportFormat export_format = ExportFormat::kMcap;
  ExportEncode export_encode = ExportEncode::kRaw;
  bool format_explicit = false;
  std::string schema_out;
  uint32_t mcap_buffer_bytes = 64u * 1024u;
  bool no_verify_crc = false;
  bool decode_profile = false;
  // SEC-06: channel name whose payload is zeroed on export.
  std::string redact_channel;
};

bool ParseArgs(const std::vector<std::string>& args, ParsedCommand* pc,
               std::ostream& err) {
  if (args.empty()) {
    err << "thyme-replay: missing command\n";
    return false;
  }
  pc->command = args[0];
  if (pc->command == "--help" || pc->command == "-h" || pc->command == "help") {
    pc->command = "help";
    return true;
  }
  if (pc->command != "list" && pc->command != "info" &&
      pc->command != "play" && pc->command != "verify" &&
      pc->command != "integrity" && pc->command != "schema" &&
      pc->command != "status" && pc->command != "export" &&
      pc->command != "repair") {
    err << "thyme-replay: unknown command '" << pc->command << "'\n";
    return false;
  }
  std::vector<std::string> positionals;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    std::string value;
    if (a == "--json") {
      pc->json = true;
    } else if (a == "--expect-manifest-hash") {
      if (!TakeValue(args, &i, "--expect-manifest-hash", &value, err)) {
        return false;
      }
      if (!schema::ParseManifestHash(value, &pc->expect_manifest_hash)) {
        err << "thyme-replay: --expect-manifest-hash wants 0x<16 hex digits>"
            << " (got " << value << ")\n";
        return false;
      }
      pc->have_expect_manifest_hash = true;
    } else if (a == "--integrity") {
      pc->verify_integrity = true;
    } else if (a == "--key") {
      if (!TakeValue(args, &i, "--key", &value, err)) {
        return false;
      }
      pc->key_path = value;
    } else if (a == "--help" || a == "-h") {
      pc->command = "help";
      return true;
    } else if (a == "--pinned_slot") {
      if (!TakeValue(args, &i, "--pinned_slot", &value, err)) {
        return false;
      }
      TargetSpec spec;
      std::string e;
      if (!ParsePinnedSlotTarget(value, &spec, &e)) {
        err << "thyme-replay: " << e << '\n';
        return false;
      }
      pc->play.pinned_slots.push_back(spec);
    } else if (a == "--small") {
      if (!TakeValue(args, &i, "--small", &value, err)) {
        return false;
      }
      TargetSpec spec;
      std::string e;
      if (!ParseSmallTarget(value, &spec, &e)) {
        err << "thyme-replay: " << e << '\n';
        return false;
      }
      pc->play.smalls.push_back(spec);
    } else if (a == "--rate") {
      if (!TakeValue(args, &i, "--rate", &value, err)) {
        return false;
      }
      double rate = 0.0;
      if (!ParseDouble(value, &rate) || !(rate > 0.0)) {
        err << "thyme-replay: --rate must be > 0 (got " << value << ")\n";
        return false;
      }
      pc->play.rate = rate;
    } else if (a.rfind("--timeline=", 0) == 0) {
      pc->play.timeline = a.substr(11);
      if (pc->play.timeline != "absolute" && pc->play.timeline != "stitch") {
        err << "thyme-replay: --timeline must be absolute or stitch (got "
            << pc->play.timeline << ")\n";
        return false;
      }
    } else if (a == "--as-fast") {
      pc->play.as_fast = true;
    } else if (a == "--pause") {
      pc->play.start_paused = true;
    } else if (a == "--step") {
      pc->play.step_once = true;
    } else if (a == "--seek" || a == "--from") {
      if (!TakeValue(args, &i, a.c_str(), &value, err)) {
        return false;
      }
      TimeSpec ts;
      std::string e;
      if (!ParseTimeSpec(value, &ts, &e)) {
        err << "thyme-replay: " << e << '\n';
        return false;
      }
      if (a == "--seek") {
        pc->play.has_seek = true;
        pc->play.seek = ts;
      } else {
        pc->play.has_from = true;
        pc->play.from = ts;
      }
    } else if (a == "--to") {
      if (!TakeValue(args, &i, "--to", &value, err)) {
        return false;
      }
      TimeSpec ts;
      std::string e;
      if (!ParseTimeSpec(value, &ts, &e)) {
        err << "thyme-replay: " << e << '\n';
        return false;
      }
      pc->play.has_to = true;
      pc->play.to = ts;
    } else if (a == "--strict-targets") {
      pc->play.strict_targets = true;
    } else if (a == "--allow-missing") {
      pc->play.allow_missing = true;
    } else if (a == "--check-compat") {
      pc->play.check_compat = true;
      pc->play.dry_run = true;
    } else if (a == "--dry-run") {
      pc->play.dry_run = true;
    } else if (a == "--force") {
      pc->play.force = true;
    } else if (a == "--no-gate") {
      pc->play.gate = false;
    } else if (a == "--preserve-sequences") {
      pc->play.preserve_sequences = true;
    } else if (a == "--no-verify-crc") {
      pc->play.verify_crc = false;
      pc->no_verify_crc = true;
    } else if (a == "--decode-profile") {
      pc->decode_profile = true;
    } else if (a == "--pinned_slot-slots") {
      if (!TakeValue(args, &i, "--pinned_slot-slots", &value, err)) {
        return false;
      }
      uint64_t n = 0;
      if (!ParseU64(value, &n) || n == 0 || n > 65536) {
        err << "thyme-replay: --pinned_slot-slots must be 1..65536 (got " << value
            << ")\n";
        return false;
      }
      pc->play.pinned_slot_slots = static_cast<uint32_t>(n);
    } else if (a == "--status-file") {
      if (!TakeValue(args, &i, "--status-file", &value, err)) {
        return false;
      }
      pc->status_file = value;
    } else if (a == "--status-interval-ms") {
      if (!TakeValue(args, &i, "--status-interval-ms", &value, err)) {
        return false;
      }
      uint64_t n = 0;
      if (!ParseU64(value, &n) || n == 0 || n > 3600000) {
        err << "thyme-replay: --status-interval-ms must be 1..3600000 (got "
            << value << ")\n";
        return false;
      }
      pc->status_interval_ms = static_cast<uint32_t>(n);
    } else if (a == "--out") {
      if (!TakeValue(args, &i, "--out", &value, err)) {
        return false;
      }
      pc->out_path = value;
    } else if (a == "--format") {
      if (!TakeValue(args, &i, "--format", &value, err)) {
        return false;
      }
      if (!ParseExportFormat(value, &pc->export_format)) {
        err << "thyme-replay: --format must be mcap or ndjson (got " << value
            << ")\n";
        return false;
      }
      pc->format_explicit = true;
    } else if (a == "--encode") {
      if (!TakeValue(args, &i, "--encode", &value, err)) {
        return false;
      }
      if (!ParseExportEncode(value, &pc->export_encode)) {
        err << "thyme-replay: --encode must be raw, cbor, msgpack, tlv, "
               "protobuf or cdr (got "
            << value << ")\n";
        return false;
      }
    } else if (a == "--schema-out") {
      if (!TakeValue(args, &i, "--schema-out", &value, err)) {
        return false;
      }
      pc->schema_out = value;
    } else if (a == "--redact") {
      if (!TakeValue(args, &i, "--redact", &pc->redact_channel, err)) {
        return false;
      }
    } else if (a == "--mcap-buffer-bytes") {
      if (!TakeValue(args, &i, "--mcap-buffer-bytes", &value, err)) {
        return false;
      }
      uint64_t n = 0;
      if (!ParseU64(value, &n) || n < 256 || n > 16u * 1024u * 1024u) {
        err << "thyme-replay: --mcap-buffer-bytes must be 256..16777216 (got "
            << value << ")\n";
        return false;
      }
      pc->mcap_buffer_bytes = static_cast<uint32_t>(n);
    } else if (!a.empty() && a[0] == '-') {
      err << "thyme-replay: unknown option '" << a << "'\n";
      return false;
    } else {
      positionals.push_back(a);
    }
  }
  if (pc->command == "help") {
    return true;
  }
  if (pc->command == "status") {
    if (!positionals.empty()) {
      err << "thyme-replay: status takes no package directory (it reads the "
             "play --status-file document)\n";
      return false;
    }
    if (pc->status_file.empty()) {
      err << "thyme-replay: status needs --status-file <path>\n";
      return false;
    }
    return true;
  }
  if (positionals.empty() ||
      (pc->command != "play" && positionals.size() != 1)) {
    if (pc->command == "play") {
      err << "thyme-replay: play needs one or more package directories\n";
    } else {
      err << "thyme-replay: " << pc->command << " needs exactly one "
          << (pc->command == "repair" ? "input .tbin" : "package directory")
          << " (got " << positionals.size() << ")\n";
    }
    return false;
  }
  if ((pc->command == "export" || pc->command == "repair") &&
      pc->out_path.empty()) {
    err << "thyme-replay: " << pc->command << " needs --out <file>\n";
    return false;
  }
  pc->package_dir = positionals[0];
  pc->play.package_dir = positionals[0];
  if (pc->command == "play" && positionals.size() > 1) {
    pc->play.package_dirs = positionals;
  }
  pc->play.json = pc->json;
  pc->play.have_expect_manifest_hash = pc->have_expect_manifest_hash;
  pc->play.expect_manifest_hash = pc->expect_manifest_hash;
  pc->play.status_file = pc->status_file;
  pc->play.status_interval_ms = pc->status_interval_ms;
  if (pc->play.has_seek && pc->play.has_from) {
    err << "thyme-replay: --seek and --from are aliases; use one\n";
    return false;
  }
  return true;
}

}  // namespace

int RunCli(const std::vector<std::string>& args, std::ostream& out,
           std::ostream& err, PlayControl* control) {
  ParsedCommand pc;
  if (!ParseArgs(args, &pc, err)) {
    PrintUsage(err, err);
    return kExitUsage;
  }
  if (pc.command == "help") {
    PrintUsage(out, err);
    return kExitOk;
  }
  if (pc.command == "list") {
    return RunList(pc.package_dir, pc.json, out, nullptr,
                   pc.have_expect_manifest_hash, pc.expect_manifest_hash);
  }
  if (pc.command == "info") {
    return RunInfo(pc.package_dir, pc.json, out, nullptr,
                   pc.have_expect_manifest_hash, pc.expect_manifest_hash);
  }
  if (pc.command == "schema") {
    return RunSchema(pc.package_dir, pc.json, out, nullptr);
  }
  if (pc.command == "verify") {
    int rc = RunVerify(pc.package_dir, pc.json, out, nullptr,
                       pc.have_expect_manifest_hash, pc.expect_manifest_hash);
    if (pc.verify_integrity) {
      const int irc = RunIntegrity(pc.package_dir, pc.json, pc.key_path, out,
                                   nullptr);
      if (irc != kExitOk) rc = irc;
    }
    return rc;
  }
  if (pc.command == "integrity") {
    return RunIntegrity(pc.package_dir, pc.json, pc.key_path, out, nullptr);
  }
  if (pc.command == "status") {
    return RunStatus(pc.status_file, pc.json, out);
  }
  if (pc.command == "repair") {
    return RunRepair(pc.package_dir, pc.out_path, pc.json, out, nullptr);
  }
  if (pc.command == "export") {
    ExportOptions opt;
    opt.package_dir = pc.package_dir;
    opt.out_path = pc.out_path;
    opt.format = pc.export_format;
    opt.encode = pc.export_encode;
    opt.format_explicit = pc.format_explicit;
    opt.verify_crc = !pc.no_verify_crc;
    opt.has_from = pc.play.has_from || pc.play.has_seek;
    opt.from = pc.play.has_from ? pc.play.from : pc.play.seek;
    opt.has_to = pc.play.has_to;
    opt.to = pc.play.to;
    opt.schema_out = pc.schema_out;
    opt.mcap_buffer_bytes = pc.mcap_buffer_bytes;
    opt.json = pc.json;
    opt.have_expect_manifest_hash = pc.have_expect_manifest_hash;
    opt.expect_manifest_hash = pc.expect_manifest_hash;
    opt.decode_profile = pc.decode_profile;
    opt.redact_channel = pc.redact_channel;
    ExportSummary summary;
    return RunExport(opt, out, &summary);
  }
  PlaySummary summary;
  return RunPlay(pc.play, control, out, &summary);
}

}  // namespace replaycli
}  // namespace thyme
