// test_mcap_verify.cpp — ECO-01: structural validation of the MCAP written by
// the thyme export path, independent of the round-trip payload assertions in
// test_mcap_export.
//
// The checks cover the invariants a reader such as Foxglove relies on:
// magics, record framing, summary/statistics consistency, Summary Offset group
// extents, footer/summary CRC, channel/schema references and per-channel
// message sequences. The file is deliberately unchunked; chunk/index records
// must be absent.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mcap_test_parser.h"
#include "thyme/check.h"
#include "thyme/record/mcap_export.h"
#include "thyme/record/thyme_bin.h"

using namespace thyme;

namespace {

constexpr uint64_t kBase = 1000000;
const char* kBinPath = "/tmp/thyme_mcap_verify_test.thyme.bin";
const char* kMcapPath = "/tmp/thyme_mcap_verify_test.mcap";

// Builds 2 data rows, one gap, one trigger and the header session.
void WriteRecordingWithEvents() {
  ThymeBinFileSink sink(kBinPath);
  CHECK(sink.ok());
  ThymeBinEncoder encoder(&sink);
  ThymeStructDesc descs[1] = {};
  descs[0].struct_id = 7;
  descs[0].contract_version = 2;
  descs[0].payload_size = 4;
  std::snprintf(descs[0].name, sizeof(descs[0].name), "Imu");
  CHECK(encoder.WriteHeader(0x1234, kBase, descs, 1));
  const uint8_t a[4] = {1, 2, 3, 4};
  const uint8_t a2[4] = {5, 6, 7, 8};
  CHECK(encoder.WriteData(1, 7, kBase + 100, 10, a, sizeof(a)));
  CHECK(encoder.WriteGap(7, kBase + 150, 3, 5));
  CHECK(encoder.WriteData(2, 7, kBase + 200, 11, a2, sizeof(a2)));
  CHECK(encoder.WriteTrigger(42, kBase + 300, 1000, 2000));
  CHECK(encoder.Finish());
}

const mcap_test::Channel* FindChannel(const mcap_test::Parsed& parsed,
                                      const std::string& topic) {
  for (const auto& channel : parsed.data_channels) {
    if (channel.topic == topic) return &channel;
  }
  return nullptr;
}

std::string TopicOf(const mcap_test::Parsed& parsed, uint16_t channel_id) {
  for (const auto& channel : parsed.data_channels) {
    if (channel.id == channel_id) return channel.topic;
  }
  return std::string();
}

bool IsRecordBoundary(const mcap_test::Parsed& parsed, uint64_t offset) {
  if (offset < parsed.header_end || offset >= parsed.closing_magic_offset) {
    return false;
  }
  std::size_t pos = static_cast<std::size_t>(offset);
  while (pos < parsed.file.size()) {
    const mcap_test::Record record = mcap_test::ReadRecord(parsed.file, pos);
    if (record.offset == offset) return true;
    pos = record.next_offset;
  }
  return false;
}

void ValidateStructure(const mcap_test::Parsed& parsed) {
  // ---- file framing ----------------------------------------------------
  CHECK(parsed.file.size() > 8);
  CHECK(std::memcmp(parsed.file.data(), mcap_test::kMagic, 8) == 0);
  CHECK(std::memcmp(parsed.file.data() + parsed.file.size() - 8,
                    mcap_test::kMagic, 8) == 0);
  CHECK(parsed.closing_magic_offset + 8 == parsed.file.size());

  // Walk the contiguous record stream: every record must land exactly on the
  // next declared boundary from Header through Footer.
  std::size_t pos = 8;
  std::size_t records = 0;
  uint64_t data_end = 0;
  uint64_t footer = 0;
  while (pos < parsed.closing_magic_offset) {
    const mcap_test::Record record = mcap_test::ReadRecord(parsed.file, pos);
    CHECK(record.next_offset > record.offset);
    if (record.opcode == mcap_test::kDataEnd) data_end = record.offset;
    if (record.opcode == mcap_test::kFooter) footer = record.offset;
    // Unchunked writer: no Chunk (0x06), Message Index (0x07), Chunk Index
    // (0x08), Attachment Index (0x09) or Metadata Index (0x0A) records.
    CHECK(record.opcode != 0x06 && record.opcode != 0x07 &&
          record.opcode != 0x08 && record.opcode != 0x09 &&
          record.opcode != 0x0A);
    pos = record.next_offset;
    ++records;
  }
  CHECK(pos == parsed.closing_magic_offset);
  CHECK(records > 6);
  CHECK(data_end != 0);
  CHECK(footer != 0);
  CHECK(data_end < footer);
  CHECK(parsed.data_end_offset == data_end);
  CHECK(parsed.footer_offset == footer);
  CHECK(parsed.footer_summary_start == parsed.summary_start);
  // Data End body is 4 bytes: the summary starts exactly after it.
  CHECK(parsed.summary_start == data_end + 9 + 4);

  // ---- CRCs ------------------------------------------------------------
  CHECK(parsed.data_end_crc == parsed.RecomputeDataCrc());
  CHECK(parsed.footer_summary_crc != 0);
  CHECK(parsed.footer_summary_crc == parsed.RecomputeSummaryCrc());

  // ---- schemas/channels ------------------------------------------------
  std::set<uint16_t> schema_ids;
  for (const auto& schema : parsed.data_schemas) {
    CHECK(schema.id != 0);
    CHECK(!schema.name.empty());
    CHECK(schema.encoding == "jsonschema");
    CHECK(schema_ids.insert(schema.id).second);
  }
  CHECK(parsed.summary_schemas.size() == parsed.data_schemas.size());

  std::set<uint16_t> channel_ids;
  std::set<std::string> topics;
  for (const auto& channel : parsed.data_channels) {
    CHECK(channel.id != 0);
    CHECK(!channel.topic.empty());
    CHECK(channel.schema_id == 0 || schema_ids.count(channel.schema_id) == 1);
    CHECK(channel_ids.insert(channel.id).second);
    CHECK(topics.insert(channel.topic).second);
  }
  CHECK(parsed.summary_channels.size() == parsed.data_channels.size());

  // ---- messages --------------------------------------------------------
  std::map<uint16_t, uint32_t> next_sequence;
  std::map<uint16_t, uint64_t> per_channel;
  uint64_t last_log_time = 0;
  for (const auto& message : parsed.messages) {
    CHECK(channel_ids.count(message.channel_id) == 1);
    const uint32_t expected = next_sequence[message.channel_id]++;
    CHECK(message.sequence == expected);
    CHECK(message.publish_time == message.log_time);
    CHECK(message.log_time >= last_log_time);
    last_log_time = message.log_time;
    ++per_channel[message.channel_id];
  }

  // ---- statistics ------------------------------------------------------
  CHECK(parsed.has_statistics);
  const auto& stats = parsed.statistics;
  CHECK(stats.message_count == parsed.messages.size());
  CHECK(stats.schema_count == parsed.data_schemas.size());
  CHECK(stats.channel_count == parsed.data_channels.size());
  CHECK(stats.attachment_count == 0);
  CHECK(stats.metadata_count == 0);
  CHECK(stats.chunk_count == 0);
  CHECK(stats.start_time == parsed.messages.front().log_time);
  CHECK(stats.end_time == parsed.messages.back().log_time);
  for (const auto& entry : stats.channel_message_counts) {
    CHECK(channel_ids.count(entry.first) == 1);
    CHECK(entry.second == per_channel[entry.first]);
  }
  for (const auto& entry : per_channel) {
    bool found = false;
    for (const auto& counted : stats.channel_message_counts) {
      if (counted.first == entry.first) {
        found = true;
        break;
      }
    }
    CHECK(found);
  }

  // ---- summary offset groups ------------------------------------------
  CHECK(!parsed.summary_offsets.empty());
  for (const auto& offset : parsed.summary_offsets) {
    CHECK(offset.group_start >= parsed.summary_start);
    CHECK(offset.group_start + offset.group_length <= parsed.footer_offset);
    CHECK(IsRecordBoundary(parsed, offset.group_start));
    std::size_t pos2 = static_cast<std::size_t>(offset.group_start);
    const std::size_t end =
        static_cast<std::size_t>(offset.group_start + offset.group_length);
    while (pos2 < end) {
      const mcap_test::Record record = mcap_test::ReadRecord(parsed.file, pos2);
      CHECK(record.opcode == offset.group_opcode);
      pos2 = record.next_offset;
    }
    CHECK(pos2 == end);
  }
}

void VerifyExportWithEvents() {
  WriteRecordingWithEvents();
  ThymeBinToMcapOptions options{};
  options.include_summary = true;
  options.data_section_crc = true;
  ThymeBinToMcapStats stats{};
  CHECK(ThymeBinToMcap(kBinPath, kMcapPath, options, &stats) ==
        ThymeBinExportStatus::kOk);
  CHECK(stats.writer_status == McapStatus::kOk);
  CHECK(stats.total_messages == 5);  // 2 data + gap + trigger + session

  const mcap_test::Parsed parsed = mcap_test::Parse(kMcapPath);
  ValidateStructure(parsed);
  CHECK(parsed.profile == "thyme");
  CHECK(parsed.library == "thyme-mcap");
  CHECK(FindChannel(parsed, "thyme/Imu") != nullptr);
  CHECK(FindChannel(parsed, "thyme/gap") != nullptr);
  CHECK(FindChannel(parsed, "thyme/trigger") != nullptr);
  CHECK(FindChannel(parsed, "thyme/session") != nullptr);

  // Foxglove-facing shape: the only encodings are the documented ones and
  // every raw data message keeps the recorded byte count.
  for (const auto& channel : parsed.data_channels) {
    CHECK(channel.message_encoding == "thyme" ||
          channel.message_encoding == "json");
  }
  for (const auto& message : parsed.messages) {
    if (TopicOf(parsed, message.channel_id) == "thyme/Imu") {
      CHECK(message.data.size() == 4);
    }
  }

  std::remove(kBinPath);
  std::remove(kMcapPath);
}

void VerifyExportWithoutSummary() {
  WriteRecordingWithEvents();
  ThymeBinToMcapOptions options{};
  options.include_summary = false;
  options.data_section_crc = false;
  ThymeBinToMcapStats stats{};
  CHECK(ThymeBinToMcap(kBinPath, kMcapPath, options, &stats) ==
        ThymeBinExportStatus::kOk);

  const mcap_test::Parsed parsed = mcap_test::Parse(kMcapPath);
  // No summary section: the footer points at the data end and there are no
  // Statistics/Summary Offset records. Foxglove still opens the file and
  // scans it linearly.
  CHECK(!parsed.has_statistics);
  CHECK(parsed.summary_offsets.empty());
  CHECK(parsed.footer_summary_start == 0);
  CHECK(parsed.footer_summary_offset_start == 0);
  CHECK(parsed.footer_summary_crc == 0);
  CHECK(parsed.summary_start == parsed.footer_offset);
  CHECK(parsed.data_end_crc == 0);
  CHECK(parsed.messages.size() == 5);
  CHECK(parsed.data_channels.size() == 4);

  std::remove(kBinPath);
  std::remove(kMcapPath);
}

}  // namespace

int main() {
  VerifyExportWithEvents();
  VerifyExportWithoutSummary();
  return 0;
}
