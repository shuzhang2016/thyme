#include "mcap_test_parser.h"
#include "replay_synthetic_package.h"

#include "thyme/check.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

std::string JsonString(const std::string& value) {
  std::string out = "\"";
  const char hex[] = "0123456789abcdef";
  for (const unsigned char c : value) {
    if (c == '\"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20u) {
      out += "\\u00";
      out.push_back(hex[c >> 4u]);
      out.push_back(hex[c & 0x0Fu]);
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  out.push_back('\"');
  return out;
}

std::string Hex(const std::vector<uint8_t>& bytes) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const uint8_t byte : bytes) {
    stream << std::setw(2) << static_cast<uint32_t>(byte);
  }
  return stream.str();
}

void Prepare(const std::string& root) {
  CHECK(mkdir(root.c_str(), 0700) == 0);
  replay_synth::Spec spec;
  spec.small_rows = 9;
  spec.frame_rows = 12;
  spec.schema = true;
  std::string error;
  CHECK(replay_synth::WritePackage(root + "/pkg", spec, &error));
}

void Dump(const std::string& mcap_path, const std::string& json_path) {
  const mcap_test::Parsed parsed = mcap_test::Parse(mcap_path.c_str());
  std::vector<mcap_test::Schema> schemas = parsed.data_schemas;
  std::vector<mcap_test::Channel> channels = parsed.data_channels;
  std::sort(schemas.begin(), schemas.end(),
            [](const mcap_test::Schema& a, const mcap_test::Schema& b) {
              return a.id < b.id;
            });
  std::sort(channels.begin(), channels.end(),
            [](const mcap_test::Channel& a, const mcap_test::Channel& b) {
              return a.id < b.id;
            });

  std::map<uint16_t, std::string> topics;
  std::map<std::string, uint64_t> topic_message_counts;
  for (const mcap_test::Channel& channel : channels) {
    topics[channel.id] = channel.topic;
    topic_message_counts[channel.topic] = 0;
  }
  for (const mcap_test::Message& message : parsed.messages) {
    ++topic_message_counts[topics[message.channel_id]];
  }

  std::ofstream out(json_path);
  CHECK(out.good());
  out << "{\n  \"header\": {\"profile\": " << JsonString(parsed.profile)
      << ", \"library\": " << JsonString(parsed.library) << "},\n";
  out << "  \"schemas\": [\n";
  for (std::size_t i = 0; i < schemas.size(); ++i) {
    const mcap_test::Schema& schema = schemas[i];
    out << "    {\"id\": " << schema.id
        << ", \"name\": " << JsonString(schema.name)
        << ", \"encoding\": " << JsonString(schema.encoding)
        << ", \"data_hex\": \"" << Hex(schema.data) << "\"}";
    out << (i + 1u == schemas.size() ? "\n" : ",\n");
  }
  out << "  ],\n  \"channels\": [\n";
  for (std::size_t i = 0; i < channels.size(); ++i) {
    const mcap_test::Channel& channel = channels[i];
    out << "    {\"id\": " << channel.id
        << ", \"schema_id\": " << channel.schema_id
        << ", \"topic\": " << JsonString(channel.topic)
        << ", \"message_encoding\": " << JsonString(channel.message_encoding)
        << "}";
    out << (i + 1u == channels.size() ? "\n" : ",\n");
  }
  out << "  ],\n  \"messages\": [\n";
  for (std::size_t i = 0; i < parsed.messages.size(); ++i) {
    const mcap_test::Message& message = parsed.messages[i];
    out << "    {\"channel_id\": " << message.channel_id
        << ", \"sequence\": " << message.sequence
        << ", \"log_time\": " << message.log_time
        << ", \"publish_time\": " << message.publish_time << "}";
    out << (i + 1u == parsed.messages.size() ? "\n" : ",\n");
  }
  out << "  ],\n  \"message_count\": " << parsed.messages.size() << ",\n";
  out << "  \"topic_message_counts\": {";
  std::size_t topic_index = 0;
  for (const auto& entry : topic_message_counts) {
    out << (topic_index++ == 0 ? "" : ", ") << JsonString(entry.first) << ": "
        << entry.second;
  }
  out << "},\n  \"statistics\": {\n";
  const mcap_test::Statistics& stats = parsed.statistics;
  CHECK(parsed.has_statistics);
  out << "    \"message_count\": " << stats.message_count << ",\n"
      << "    \"schema_count\": " << stats.schema_count << ",\n"
      << "    \"channel_count\": " << stats.channel_count << ",\n"
      << "    \"attachment_count\": " << stats.attachment_count << ",\n"
      << "    \"metadata_count\": " << stats.metadata_count << ",\n"
      << "    \"chunk_count\": " << stats.chunk_count << ",\n"
      << "    \"start_time\": " << stats.start_time << ",\n"
      << "    \"end_time\": " << stats.end_time << ",\n"
      << "    \"channel_message_counts\": {";
  for (std::size_t i = 0; i < stats.channel_message_counts.size(); ++i) {
    const auto& entry = stats.channel_message_counts[i];
    out << (i == 0 ? "" : ", ") << JsonString(std::to_string(entry.first))
        << ": " << entry.second;
  }
  out << "}\n  }\n}\n";
  CHECK(out.good());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "prepare") {
    Prepare(argv[2]);
    return 0;
  }
  if (argc == 4 && std::string(argv[1]) == "dump") {
    Dump(argv[2], argv[3]);
    return 0;
  }
  std::fprintf(
      stderr,
      "usage: eco12_mcap_fixture prepare DIR | dump FILE.mcap FILE.json\n");
  return 2;
}
