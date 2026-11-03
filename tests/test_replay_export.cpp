// test_replay_export.cpp — REP-09 export / republish.
//
// Uses the synthetic package fixture and exercises both documented formats:
//
//   A: MCAP export through the ECO-01 McapWriter — message counts, log times,
//      exact payload bytes, topic layout (raw frames vs recorded structs),
//      gap/trigger event messages, and the ABI-09 schema sidecar linkage
//      (channel metadata + package message) with --schema-out.
//   B: NDJSON export — header/event/summary records, base64 payload bytes and
//      timestamps byte-for-byte, schema_ref into the sidecar.
//   C: bounded-memory claim — a 256-byte MCAP writer buffer still exports
//      multi-hundred-KiB raw frames (McapWriter streams oversized messages
//      directly; neither format buffers the file).
//   D: window (--from/--to) and CRC policy behavior.
//   E: CLI contract: usage/exit codes, JSON shape, missing package and
//      --schema-out on a package without a sidecar.

#include "replay_synthetic_package.h"

#include "mcap_test_parser.h"
#include "replay_cli.h"
#include "replay_export.h"

#include "thyme/check.h"
#include "thyme/replay/replay_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

void RmTree(const std::string& path) {
  DIR* d = opendir(path.c_str());
  if (d == nullptr) {
    return;
  }
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') {
      continue;
    }
    const std::string sub = path + "/" + e->d_name;
    struct stat st {};
    if (stat(sub.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      RmTree(sub);
    } else {
      std::remove(sub.c_str());
    }
  }
  closedir(d);
  rmdir(path.c_str());
}

uint64_t FileSize(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
}

std::vector<uint8_t> B64Decode(const std::string& in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<uint8_t> out;
  out.reserve(in.size() * 3 / 4);
  uint32_t acc = 0;
  int bits = 0;
  for (const char c : in) {
    if (c == '=') break;
    const int v = val(c);
    if (v < 0) continue;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFFu));
    }
  }
  return out;
}

std::string JsonStrField(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const std::size_t p = line.find(needle);
  if (p == std::string::npos) {
    return std::string();
  }
  const std::size_t start = p + needle.size();
  const std::size_t end = line.find('"', start);
  if (end == std::string::npos) {
    return std::string();
  }
  return line.substr(start, end - start);
}

uint64_t JsonU64Field(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const std::size_t p = line.find(needle);
  if (p == std::string::npos) {
    return 0;
  }
  return std::strtoull(line.c_str() + p + needle.size(), nullptr, 10);
}

struct PkgEvent {
  uint64_t mono_ns = 0;
  uint32_t channel = 0;
  replay::ReplayKind kind = replay::ReplayKind::kData;
  uint32_t struct_id = 0;
  uint32_t len = 0;
  std::vector<uint8_t> bytes;
  std::string channel_name;
};

std::vector<PkgEvent> Collect(const std::string& dir) {
  replay::ReplayPackage pkg;
  CHECK(pkg.Open(dir) == replay::ReplayStatus::kOk);
  std::vector<PkgEvent> out;
  replay::ReplayEvent ev;
  while (pkg.NextEvent(&ev) == replay::ReplayStatus::kOk) {
    PkgEvent e;
    e.mono_ns = ev.mono_ns;
    e.channel = ev.channel;
    e.kind = ev.kind;
    e.struct_id = ev.struct_id;
    e.len = ev.len;
    if (ev.payload != nullptr && ev.len != 0) {
      e.bytes.assign(ev.payload, ev.payload + ev.len);
    }
    e.channel_name = pkg.channels()[ev.channel].name;
    out.push_back(std::move(e));
  }
  return out;
}

}  // namespace

int main() {
  replay_synth::Spec spec;
  spec.small_rows = 120;
  spec.frame_rows = 80;
  spec.schema = true;

  char root_buf[160];
  std::snprintf(root_buf, sizeof(root_buf), "/tmp/thyme_rep09_%ld",
                static_cast<long>(getpid()));
  const std::string root = root_buf;
  RmTree(root);
  CHECK(mkdir(root.c_str(), 0700) == 0);
  const std::string pkg = root + "/pkg";
  std::string err;
  CHECK(replay_synth::WritePackage(pkg, spec, &err));

  replay::ReplayPackage meta_pkg;
  CHECK(meta_pkg.Open(pkg) == replay::ReplayStatus::kOk);
  const std::vector<PkgEvent> events = Collect(pkg);
  uint64_t expected_data = 0;
  uint64_t expected_gap = 0;
  uint64_t expected_trigger = 0;
  uint64_t expected_bytes = 0;
  for (const PkgEvent& e : events) {
    if (e.kind == replay::ReplayKind::kData) {
      expected_data += 1;
      expected_bytes += e.len;
    } else if (e.kind == replay::ReplayKind::kGap) {
      expected_gap += 1;
    } else if (e.kind == replay::ReplayKind::kTrigger) {
      expected_trigger += 1;
    }
  }
  CHECK(expected_data == 4 + spec.small_rows + spec.frame_rows);
  CHECK(expected_gap == 1);
  CHECK(expected_trigger == 1);

  const std::string schema_copy = root + "/schema_copy.json";

  // ---- A: MCAP export ----
  const std::string mcap_path = root + "/out.mcap";
  {
    replaycli::ExportOptions opt;
    opt.package_dir = pkg;
    opt.out_path = mcap_path;
    opt.format = replaycli::ExportFormat::kMcap;
    opt.schema_out = schema_copy;
    opt.mcap_buffer_bytes = 256;  // force oversized messages through WriteDirect
    std::ostringstream out;
    replaycli::ExportSummary sum;
    const int mcap_rc = replaycli::RunExport(opt, out, &sum);
    if (mcap_rc != replaycli::kExitOk) {
      std::fprintf(stderr, "mcap export failed: %s\n%s\n", sum.error.c_str(),
                   out.str().c_str());
    }
    CHECK(mcap_rc == replaycli::kExitOk);
    CHECK(sum.ok);
    CHECK(sum.events == events.size());
    CHECK(sum.data_messages == expected_data);
    CHECK(sum.gap_messages == expected_gap);
    CHECK(sum.trigger_messages == expected_trigger);
    CHECK(sum.session_messages == 0);
    CHECK(sum.payload_bytes == expected_bytes);
    CHECK(sum.crc_checked == spec.frame_rows);
    CHECK(sum.crc_bad == 0);
    CHECK(sum.crc_skipped == 0);
    CHECK(sum.schema_state == "ok");
    CHECK(sum.schema_ref == schema_copy);
    CHECK(sum.first_mono_ns == events.front().mono_ns);
    CHECK(sum.last_mono_ns == events.back().mono_ns);
    CHECK(sum.output_bytes == FileSize(mcap_path));
    CHECK(sum.output_bytes > 50000);  // raw frames streamed, not buffered
    CHECK(FileSize(schema_copy) == FileSize(pkg + "/schema.json"));
    CHECK(out.str().find("export  : OK") != std::string::npos);
  }
  // ---- SEC-06: --redact <channel> zeroes only that channel's payload ----
  {
    replaycli::ExportOptions opt;
    opt.package_dir = pkg;
    opt.out_path = root + "/redact.mcap";
    opt.format = replaycli::ExportFormat::kMcap;
    opt.redact_channel = "cam_a";
    replaycli::ExportSummary sum;
    std::ostringstream out;
    CHECK(replaycli::RunExport(opt, out, &sum) == replaycli::kExitOk);

    const mcap_test::Parsed red = mcap_test::Parse(opt.out_path.c_str());
    uint16_t redact_id = 0;
    for (const mcap_test::Channel& c : red.data_channels) {
      if (c.topic == "thyme/cam_a/raw") {
        redact_id = c.id;
      }
    }
    CHECK(redact_id != 0);
    uint32_t zero_frames = 0;
    for (const mcap_test::Message& m : red.messages) {
      if (m.channel_id == redact_id) {
        ++zero_frames;
        for (const uint8_t b : m.data) CHECK(b == 0);
      }
    }
    CHECK(zero_frames > 0);
  }
  {
    mcap_test::Parsed parsed = mcap_test::Parse(mcap_path.c_str());
    CHECK(parsed.messages.size() == events.size() + 1);  // + package message
    CHECK(parsed.data_channels.size() == parsed.summary_channels.size());
    std::string pkg_topic;
    std::string raw_topic;
    for (const auto& c : parsed.data_channels) {
      if (c.topic == "thyme/package") {
        pkg_topic = c.topic;
        bool ref_ok = false;
        for (const auto& kv : c.metadata) {
          if (kv.first == "thyme.schema_ref" && kv.second == schema_copy) {
            ref_ok = true;
          }
          if (kv.first == "thyme.schema_state" && kv.second == "ok") {
            ref_ok = ref_ok && true;
          }
        }
        CHECK(ref_ok);
      }
      if (c.topic == "thyme/cam_a/raw") {
        raw_topic = c.topic;
        bool ref_ok = false;
        for (const auto& kv : c.metadata) {
          if (kv.first == "thyme.payload_size") {
            ref_ok = true;
          }
        }
        CHECK(ref_ok);
      }
    }
    CHECK(!pkg_topic.empty());
    CHECK(!raw_topic.empty());

    // Map message channel ids to topics.
    std::vector<std::string> topic_of(parsed.data_channels.size() + 1);
    for (const auto& c : parsed.data_channels) {
      CHECK(c.id < topic_of.size());
      topic_of[c.id] = c.topic;
    }
    uint64_t raw_messages = 0;
    uint64_t small_messages = 0;
    for (const auto& m : parsed.messages) {
      CHECK(m.channel_id < topic_of.size());
      if (topic_of[m.channel_id] == "thyme/cam_a/raw") {
        raw_messages += 1;
        // Compare with the package event at this timestamp.
        bool found = false;
        for (const PkgEvent& e : events) {
          if (e.kind == replay::ReplayKind::kData &&
              e.channel_name == "cam_a" && e.mono_ns == m.log_time &&
              e.len == m.data.size() &&
              std::memcmp(e.bytes.data(), m.data.data(), e.len) == 0) {
            found = true;
            break;
          }
        }
        CHECK(found);
      } else if (topic_of[m.channel_id] == "thyme/small/Pod16") {
        small_messages += 1;
      }
      if (topic_of[m.channel_id] == "thyme/package") {
        const std::string body(m.data.begin(), m.data.end());
        CHECK(body.find("\"format\":\"thyme-replay-mcap\"") !=
              std::string::npos);
        CHECK(body.find(schema_copy) != std::string::npos);
        CHECK(body.find("\"declared\":false") != std::string::npos);
      }
    }
    CHECK(raw_messages == spec.frame_rows);
    CHECK(small_messages == spec.small_rows);
    CHECK(parsed.statistics.message_count == parsed.messages.size());
    CHECK(parsed.statistics.start_time == events.front().mono_ns);
    CHECK(parsed.statistics.end_time == events.back().mono_ns);
  }

  // ---- B: NDJSON export ----
  const std::string ndjson_path = root + "/out.ndjson";
  {
    replaycli::ExportOptions opt;
    opt.package_dir = pkg;
    opt.out_path = ndjson_path;
    opt.format = replaycli::ExportFormat::kNdjson;
    opt.schema_out = schema_copy;
    std::ostringstream out;
    replaycli::ExportSummary sum;
    CHECK(replaycli::RunExport(opt, out, &sum) == replaycli::kExitOk);
    CHECK(sum.ok);
    CHECK(sum.events == events.size());
    CHECK(sum.payload_bytes == expected_bytes);
    CHECK(out.str().find("export  : OK") != std::string::npos);

    std::ifstream in(ndjson_path);
    CHECK(in.good());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty()) {
        lines.push_back(line);
      }
    }
    CHECK(lines.size() == 2 + events.size());
    CHECK(lines.front().find("\"record\":\"header\"") != std::string::npos);
    CHECK(lines.front().find(schema_copy) != std::string::npos);
    CHECK(lines.front().find("\"state\":\"ok\"") != std::string::npos);
    CHECK(lines.back().find("\"record\":\"summary\"") != std::string::npos);
    CHECK(lines.back().find("\"payload_bytes\":") != std::string::npos);

    std::size_t ev_index = 0;
    std::size_t data_seen = 0;
    for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
      const std::string& l = lines[i];
      CHECK(l.find("\"record\":\"event\"") != std::string::npos);
      CHECK(JsonU64Field(l, "t") == events[ev_index].mono_ns);
      CHECK(JsonU64Field(l, "channel") == events[ev_index].channel);
      const std::string kind = JsonStrField(l, "kind");
      CHECK(kind == replay::ReplayKindName(events[ev_index].kind));
      if (events[ev_index].kind == replay::ReplayKind::kData) {
        data_seen += 1;
        const std::size_t p = l.find("\"payload\":\"");
        CHECK(p != std::string::npos);
        const std::size_t start = p + std::strlen("\"payload\":\"");
        const std::size_t end = l.find('"', start);
        CHECK(end != std::string::npos);
        const std::vector<uint8_t> decoded =
            B64Decode(l.substr(start, end - start));
        CHECK(decoded == events[ev_index].bytes);
        if (events[ev_index].channel_name == "small") {
          CHECK(l.find("\"schema_ref\":\"" + schema_copy +
                       "#/records/0\"") != std::string::npos);
        }
        if (events[ev_index].channel_name == "cam_a") {
          CHECK(l.find("\"payload_kind\":\"raw_frame\"") !=
                std::string::npos);
          CHECK(l.find("\"schema_ref\":null") != std::string::npos);
        }
      }
      ev_index += 1;
    }
    CHECK(ev_index == events.size());
    CHECK(data_seen == expected_data);
  }

  // ---- C: bounded memory with a tiny MCAP buffer already exercised above; a
  // larger buffer must produce the same message set. ----
  {
    const std::string big = root + "/out_big_buffer.mcap";
    replaycli::ExportOptions opt;
    opt.package_dir = pkg;
    opt.out_path = big;
    opt.mcap_buffer_bytes = 1024u * 1024u;
    std::ostringstream out;
    replaycli::ExportSummary sum;
    CHECK(replaycli::RunExport(opt, out, &sum) == replaycli::kExitOk);
    mcap_test::Parsed small = mcap_test::Parse(mcap_path.c_str());
    mcap_test::Parsed large = mcap_test::Parse(big.c_str());
    CHECK(small.messages.size() == large.messages.size());
  }

  // ---- D: window and CRC policy ----
  {
    const std::size_t mid = events.size() / 2;
    const uint64_t from_abs = events[mid].mono_ns;
    const uint64_t to_abs = events[events.size() * 3 / 4].mono_ns;
    uint64_t expected = 0;
    for (const PkgEvent& e : events) {
      if (e.mono_ns >= from_abs && e.mono_ns <= to_abs) {
        expected += 1;
      }
    }
    replaycli::ExportOptions opt;
    opt.package_dir = pkg;
    opt.out_path = root + "/window.ndjson";
    opt.format = replaycli::ExportFormat::kNdjson;
    opt.has_from = true;
    opt.from.absolute = true;
    opt.from.ns = from_abs;
    opt.has_to = true;
    opt.to.absolute = true;
    opt.to.ns = to_abs;
    std::ostringstream out;
    replaycli::ExportSummary sum;
    CHECK(replaycli::RunExport(opt, out, &sum) == replaycli::kExitOk);
    CHECK(sum.events == expected);
    CHECK(sum.first_mono_ns == from_abs);
    CHECK(sum.last_mono_ns <= to_abs);

    replaycli::ExportOptions alt = opt;
    alt.out_path = root + "/window_nocrc.mcap";
    alt.format = replaycli::ExportFormat::kMcap;
    alt.verify_crc = false;
    std::ostringstream out2;
    replaycli::ExportSummary sum2;
    CHECK(replaycli::RunExport(alt, out2, &sum2) == replaycli::kExitOk);
    CHECK(sum2.crc_checked == 0);
    CHECK(sum2.crc_skipped == 0);
  }

  // ---- E: CLI contract ----
  {
    std::ostringstream out;
    std::ostringstream cli_err;
    CHECK(replaycli::RunCli({"export", pkg, "--format", "mcap"}, out, cli_err,
                            nullptr) == replaycli::kExitUsage);
    CHECK(replaycli::RunCli({"export", pkg, "--out", root + "/x.mcap",
                             "--format", "bogus"}, out, cli_err, nullptr) ==
          replaycli::kExitUsage);
    CHECK(replaycli::RunCli({"export", root + "/missing", "--out",
                             root + "/x.mcap"}, out, cli_err, nullptr) ==
          replaycli::kExitFailure);
    CHECK(replaycli::RunCli({"status"}, out, cli_err, nullptr) ==
          replaycli::kExitUsage);

    std::ostringstream jout;
    const std::string cli_out = root + "/cli.ndjson";
    CHECK(replaycli::RunCli({"export", pkg, "--out", cli_out, "--format",
                             "ndjson", "--json"}, jout, cli_err, nullptr) ==
          replaycli::kExitOk);
    CHECK(jout.str().find("\"command\":\"export\"") != std::string::npos);
    CHECK(jout.str().find("\"ok\":true") != std::string::npos);
    CHECK(jout.str().find("\"format\":\"ndjson\"") != std::string::npos);
    CHECK(jout.str().find("\"data\":") != std::string::npos);

    // ABI-10 expectation on a legacy package fails closed (no manifest).
    std::ostringstream eout;
    CHECK(replaycli::RunCli({"export", pkg, "--out", root + "/m.mcap",
                             "--expect-manifest-hash", "0x0000000000000001"},
                            eout, cli_err, nullptr) ==
          replaycli::kExitFailure);
    CHECK(!FileSize(root + "/m.mcap"));
  }

  // ---- F: --schema-out requires a real sidecar ----
  {
    replay_synth::Spec plain = spec;
    plain.schema = false;
    const std::string plain_pkg = root + "/plain";
    CHECK(replay_synth::WritePackage(plain_pkg, plain, &err));
    replaycli::ExportOptions opt;
    opt.package_dir = plain_pkg;
    opt.out_path = root + "/plain.ndjson";
    opt.format = replaycli::ExportFormat::kNdjson;
    opt.schema_out = root + "/plain_schema.json";
    std::ostringstream out;
    replaycli::ExportSummary sum;
    CHECK(replaycli::RunExport(opt, out, &sum) == replaycli::kExitFailure);
    CHECK(!sum.ok);
    CHECK(sum.error.find("no schema.json") != std::string::npos);
    CHECK(!FileSize(root + "/plain.ndjson"));

    // Without --schema-out a legacy package exports with schema absent.
    opt.schema_out.clear();
    std::ostringstream out2;
    replaycli::ExportSummary sum2;
    CHECK(replaycli::RunExport(opt, out2, &sum2) == replaycli::kExitOk);
    CHECK(sum2.schema_state == "absent");
    CHECK(sum2.schema_ref.empty());
    CHECK(out2.str().find("schema  : absent") != std::string::npos);
  }

  RmTree(root);
  return 0;
}
