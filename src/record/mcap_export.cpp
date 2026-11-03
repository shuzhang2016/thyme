#include "thyme/record/mcap_export.h"

#include <cstdio>
#include <cstring>

namespace thyme {

namespace {

constexpr uint16_t kDataChannelBase = 16;
constexpr uint16_t kDataSchemaBase = 1;
constexpr uint16_t kGapChannel = 1;
constexpr uint16_t kTriggerChannel = 2;
constexpr uint16_t kSessionChannel = 3;
constexpr uint16_t kGapSchema = 101;
constexpr uint16_t kTriggerSchema = 102;
constexpr uint16_t kSessionSchema = 103;

constexpr char kGapSchemaJson[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
    "\"title\":\"thyme.GapEvent\",\"type\":\"object\","
    "\"properties\":{\"event\":{\"type\":\"string\"},"
    "\"struct_id\":{\"type\":\"integer\"},"
    "\"reason\":{\"type\":\"integer\"},"
    "\"missed_count\":{\"type\":\"integer\"}},"
    "\"required\":[\"event\"]}";

constexpr char kTriggerSchemaJson[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
    "\"title\":\"thyme.TriggerEvent\",\"type\":\"object\","
    "\"properties\":{\"event\":{\"type\":\"string\"},"
    "\"trigger_id\":{\"type\":\"integer\"},"
    "\"pre_roll_ns\":{\"type\":\"integer\"},"
    "\"post_roll_ns\":{\"type\":\"integer\"}},"
    "\"required\":[\"event\"]}";

constexpr char kSessionSchemaJson[] =
    "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
    "\"title\":\"thyme.SessionEvent\",\"type\":\"object\","
    "\"properties\":{\"event\":{\"type\":\"string\"},"
    "\"session_id\":{\"type\":\"integer\"},"
    "\"reason\":{\"type\":\"integer\"},"
    "\"device_id\":{\"type\":\"integer\"},"
    "\"start_mono_ns\":{\"type\":\"integer\"},"
    "\"file_abi_version\":{\"type\":\"integer\"}},"
    "\"required\":[\"event\"]}";

std::size_t BoundedLength(const char* s, std::size_t cap) {
  if (s == nullptr) return 0;
  std::size_t n = 0;
  while (n < cap && s[n] != '\0') ++n;
  return n;
}

void SanitizeTopicChars(const char* s, std::size_t len, char* out,
                        std::size_t cap) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < len && n + 1 < cap; ++i) {
    const char c = s[i];
    const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '/' ||
                      c == '-' || c == '.';
    out[n++] = keep ? c : '_';
  }
  out[n] = '\0';
}

void JsonEscape(const char* s, std::size_t len, char* out, std::size_t cap) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < len && n + 1 < cap; ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c == '"' || c == '\\') {
      if (n + 2 >= cap) break;
      out[n++] = '\\';
      out[n++] = static_cast<char>(c);
    } else if (c < 0x20u) {
      const int wrote = std::snprintf(out + n, cap - n, "\\u%04x",
                                      static_cast<unsigned>(c));
      if (wrote <= 0) break;
      n += static_cast<std::size_t>(wrote);
    } else {
      out[n++] = static_cast<char>(c);
    }
  }
  out[n] = '\0';
}

struct DataChannel {
  uint16_t struct_id = 0;
  uint16_t channel_id = 0;
};

void FillStatsError(ThymeBinToMcapStats* stats, McapStatus status) {
  if (stats != nullptr) stats->writer_status = status;
}

}  // namespace

ThymeBinExportStatus ThymeBinToMcap(ThymeBinReader& reader, McapWriter& writer,
                                    const ThymeBinToMcapOptions& options,
                                    ThymeBinToMcapStats* stats) {
  ThymeBinToMcapStats local{};
  ThymeBinToMcapStats& out = (stats != nullptr) ? *stats : local;
  out = ThymeBinToMcapStats{};
  out.output_opened = writer.is_open();

  const auto& registry = reader.registry();
  if (registry.size() > kMcapMaxChannels - kDataChannelBase) {
    FillStatsError(&out, McapStatus::kOutOfCapacity);
    return ThymeBinExportStatus::kWriterError;
  }

  DataChannel channels[64];
  char topics[64][96];
  const std::size_t channel_count = registry.size();
  std::size_t topic_count = 0;

  for (std::size_t i = 0; i < channel_count; ++i) {
    const ThymeStructDesc& desc = registry[i];
    const std::size_t name_len = BoundedLength(desc.name, sizeof(desc.name));
    char safe[64];
    char topic[96];
    if (name_len == 0) {
      (void)std::snprintf(topic, sizeof(topic), "thyme/struct_%u",
                          static_cast<unsigned>(desc.struct_id));
    } else {
      SanitizeTopicChars(desc.name, name_len, safe, sizeof(safe));
      (void)std::snprintf(topic, sizeof(topic), "thyme/%s", safe);
    }
    bool duplicate = false;
    for (std::size_t j = 0; j < topic_count; ++j) {
      if (std::strcmp(topics[j], topic) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      const std::size_t used = std::strlen(topic);
      (void)std::snprintf(topic + used, sizeof(topic) - used, "_%u",
                          static_cast<unsigned>(desc.struct_id));
    }
    const std::size_t topic_len = std::strlen(topic);
    std::memcpy(topics[topic_count], topic, topic_len + 1);
    ++topic_count;

    char escaped[96];
    JsonEscape(desc.name, name_len, escaped, sizeof(escaped));
    char schema[768];
    const int schema_len = std::snprintf(
        schema, sizeof(schema),
        "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
        "\"title\":\"thyme.%s\","
        "\"description\":\"Raw thyme struct payload (id=%u, "
        "contract_version=%u, payload_size=%u). ThymeStructDesc carries no "
        "field layout; message bytes are the unmodified .thyme.bin payload "
        "and are not JSON.\","
        "\"type\":\"object\","
        "\"properties\":{\"data\":{\"type\":\"string\","
        "\"contentEncoding\":\"base64\"}},"
        "\"required\":[\"data\"]}",
        escaped, static_cast<unsigned>(desc.struct_id),
        static_cast<unsigned>(desc.contract_version),
        static_cast<unsigned>(desc.payload_size));
    if (schema_len <= 0 || static_cast<std::size_t>(schema_len) >= sizeof(schema)) {
      FillStatsError(&out, McapStatus::kInvalidArgument);
      return ThymeBinExportStatus::kWriterError;
    }

    const uint16_t schema_id =
        static_cast<uint16_t>(kDataSchemaBase + i);
    const uint16_t channel_id =
        static_cast<uint16_t>(kDataChannelBase + i);
    McapStatus status =
        writer.AddSchema(schema_id, topic, "jsonschema", schema,
                         static_cast<std::size_t>(schema_len));
    if (status != McapStatus::kOk) {
      FillStatsError(&out, status);
      return ThymeBinExportStatus::kWriterError;
    }

    char id_text[8];
    char version_text[8];
    char size_text[8];
    (void)std::snprintf(id_text, sizeof(id_text), "%u",
                        static_cast<unsigned>(desc.struct_id));
    (void)std::snprintf(version_text, sizeof(version_text), "%u",
                        static_cast<unsigned>(desc.contract_version));
    (void)std::snprintf(size_text, sizeof(size_text), "%u",
                        static_cast<unsigned>(desc.payload_size));
    const McapMetadataEntry metadata[3] = {
        {"thyme.struct_id", id_text},
        {"thyme.contract_version", version_text},
        {"thyme.payload_size", size_text},
    };
    status = writer.AddChannel(channel_id, schema_id, topic, "thyme", metadata,
                               3);
    if (status != McapStatus::kOk) {
      FillStatsError(&out, status);
      return ThymeBinExportStatus::kWriterError;
    }
    channels[i].struct_id = desc.struct_id;
    channels[i].channel_id = channel_id;
  }

  if (options.emit_events) {
    struct EventChannel {
      uint16_t schema_id;
      uint16_t channel_id;
      const char* topic;
      const char* schema_name;
      const char* schema_json;
    };
    const EventChannel events[3] = {
        {kGapSchema, kGapChannel, "thyme/gap", "thyme.GapEvent",
         kGapSchemaJson},
        {kTriggerSchema, kTriggerChannel, "thyme/trigger",
         "thyme.TriggerEvent", kTriggerSchemaJson},
        {kSessionSchema, kSessionChannel, "thyme/session",
         "thyme.SessionEvent", kSessionSchemaJson},
    };
    for (const auto& event : events) {
      McapStatus status =
          writer.AddSchema(event.schema_id, event.schema_name, "jsonschema",
                           event.schema_json, std::strlen(event.schema_json));
      if (status != McapStatus::kOk) {
        FillStatsError(&out, status);
        return ThymeBinExportStatus::kWriterError;
      }
      status = writer.AddChannel(event.channel_id, event.schema_id,
                                 event.topic, "json", nullptr, 0);
      if (status != McapStatus::kOk) {
        FillStatsError(&out, status);
        return ThymeBinExportStatus::kWriterError;
      }
    }
  }

  const ThymeBinFileHeader& header = reader.header();
  uint64_t last_time_ns = header.start_mono_ns;
  out.channels = writer.channel_count();

  if (options.emit_events && options.emit_header_session) {
    char json[256];
    const int n = std::snprintf(
        json, sizeof(json),
        "{\"event\":\"session_start\",\"device_id\":%u,\"session_id\":%llu,"
        "\"start_mono_ns\":%llu,\"file_abi_version\":%u}",
        static_cast<unsigned>(header.device_id),
        static_cast<unsigned long long>(header.session_id),
        static_cast<unsigned long long>(header.start_mono_ns),
        static_cast<unsigned>(header.file_abi_version));
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(json)) {
      FillStatsError(&out, McapStatus::kInvalidArgument);
      return ThymeBinExportStatus::kWriterError;
    }
    const McapStatus status = writer.WriteMessage(
        kSessionChannel, header.start_mono_ns, json, static_cast<std::size_t>(n));
    if (status != McapStatus::kOk) {
      FillStatsError(&out, status);
      return ThymeBinExportStatus::kWriterError;
    }
    ++out.session_messages;
    ++out.total_messages;
  }

  ThymeBinDataEntry entry{};
  for (;;) {
    const ThymeBinReader::ReadResult result = reader.ReadNext(entry);
    if (result == ThymeBinReader::ReadResult::kEof) break;
    if (result == ThymeBinReader::ReadResult::kCorrupt) {
      out.corrupt_offset = reader.corrupt_offset();
      out.corrupt_crc_expected = reader.corrupt_crc_expected();
      out.corrupt_crc_actual = reader.corrupt_crc_actual();
      return ThymeBinExportStatus::kInputCorrupt;
    }
    ++out.input_records;

    McapStatus status = McapStatus::kOk;
    switch (entry.type) {
      case ThymeBinEntryType::kData: {
        uint16_t channel_id = 0;
        bool found = false;
        for (std::size_t i = 0; i < channel_count; ++i) {
          if (channels[i].struct_id == entry.struct_id) {
            channel_id = channels[i].channel_id;
            found = true;
            break;
          }
        }
        if (!found) return ThymeBinExportStatus::kUnknownStruct;
        const uint64_t log_time = entry.mono_ns;
        status = writer.WriteMessage(channel_id, log_time, entry.payload.data(),
                                     entry.payload.size());
        if (status == McapStatus::kOk) {
          ++out.data_messages;
          ++out.total_messages;
          if (entry.mono_ns > last_time_ns) last_time_ns = entry.mono_ns;
        }
        break;
      }
      case ThymeBinEntryType::kGap: {
        if (!options.emit_events) break;
        char json[256];
        const int n = std::snprintf(
            json, sizeof(json),
            "{\"event\":\"gap\",\"struct_id\":%u,\"reason\":%u,"
            "\"missed_count\":%u}",
            static_cast<unsigned>(entry.struct_id),
            static_cast<unsigned>(entry.reserved),
            static_cast<unsigned>(entry.source_sequence));
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(json)) {
          return ThymeBinExportStatus::kInvalidArgument;
        }
        status = writer.WriteMessage(kGapChannel, entry.mono_ns, json,
                                     static_cast<std::size_t>(n));
        if (status == McapStatus::kOk) {
          ++out.gap_messages;
          ++out.total_messages;
          if (entry.mono_ns > last_time_ns) last_time_ns = entry.mono_ns;
        }
        break;
      }
      case ThymeBinEntryType::kTrigger: {
        if (!options.emit_events) break;
        char json[256];
        const int n = std::snprintf(
            json, sizeof(json),
            "{\"event\":\"trigger\",\"trigger_id\":%llu,"
            "\"pre_roll_ns\":%llu,\"post_roll_ns\":%llu}",
            static_cast<unsigned long long>(entry.record_sequence),
            static_cast<unsigned long long>(entry.pre_roll_ns),
            static_cast<unsigned long long>(entry.post_roll_ns));
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(json)) {
          return ThymeBinExportStatus::kInvalidArgument;
        }
        status = writer.WriteMessage(kTriggerChannel, entry.mono_ns, json,
                                     static_cast<std::size_t>(n));
        if (status == McapStatus::kOk) {
          ++out.trigger_messages;
          ++out.total_messages;
          if (entry.mono_ns > last_time_ns) last_time_ns = entry.mono_ns;
        }
        break;
      }
      case ThymeBinEntryType::kSession: {
        if (!options.emit_events) break;
        char json[256];
        const int n = std::snprintf(
            json, sizeof(json),
            "{\"event\":\"session_change\",\"session_id\":%llu,"
            "\"reason\":%u}",
            static_cast<unsigned long long>(entry.record_sequence),
            static_cast<unsigned>(entry.reserved));
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(json)) {
          return ThymeBinExportStatus::kInvalidArgument;
        }
        status = writer.WriteMessage(kSessionChannel, last_time_ns, json,
                                     static_cast<std::size_t>(n));
        if (status == McapStatus::kOk) {
          ++out.session_messages;
          ++out.total_messages;
        }
        break;
      }
      default:
        return ThymeBinExportStatus::kInputCorrupt;
    }
    if (status != McapStatus::kOk) {
      FillStatsError(&out, status);
      return ThymeBinExportStatus::kWriterError;
    }
  }

  out.channels = writer.channel_count();
  return ThymeBinExportStatus::kOk;
}

ThymeBinExportStatus ThymeBinToMcap(const char* in_path, const char* out_path,
                                    const ThymeBinToMcapOptions& options,
                                    ThymeBinToMcapStats* stats) {
  if (in_path == nullptr || out_path == nullptr) {
    return ThymeBinExportStatus::kInvalidArgument;
  }
  if (stats != nullptr) *stats = ThymeBinToMcapStats{};
  ThymeBinReader reader(in_path);
  if (!reader.ok()) return ThymeBinExportStatus::kInputOpen;

  McapWriterOptions writer_options{};
  writer_options.profile = "thyme";
  writer_options.library = "thyme-mcap";
  writer_options.include_summary = options.include_summary;
  writer_options.data_section_crc = options.data_section_crc;
  McapWriter writer;
  const McapStatus open_status = writer.Open(out_path, writer_options);
  if (open_status != McapStatus::kOk) {
    FillStatsError(stats, open_status);
    return ThymeBinExportStatus::kWriterError;
  }
  const ThymeBinExportStatus result =
      ThymeBinToMcap(reader, writer, options, stats);
  const McapStatus close_status = writer.Close();
  if (result == ThymeBinExportStatus::kOk &&
      close_status != McapStatus::kOk) {
    FillStatsError(stats, close_status);
    return ThymeBinExportStatus::kWriterError;
  }
  return result;
}

}  // namespace thyme
