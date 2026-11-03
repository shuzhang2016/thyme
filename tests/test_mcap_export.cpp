// test_mcap_export.cpp — REC-06 round-trip: build a .thyme.bin with data,
// gap, trigger and session entries, export it, then parse the MCAP back with
// the independent test parser and compare payloads/timestamps/topics.

#include "mcap_test_parser.h"
#include "thyme/check.h"
#include "thyme/record/mcap_export.h"
#include "thyme/record/thyme_bin.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace thyme;

namespace {

constexpr uint64_t kBase = 1000000;

const char* kBinPath = "/tmp/thyme_mcap_export_test.thyme.bin";
const char* kMcapPath = "/tmp/thyme_mcap_export_test.mcap";

uint32_t SessionCrc(uint64_t new_session_id, uint16_t reason) {
  uint32_t c = 0xFFFFFFFFu;
  const uint8_t type_byte = static_cast<uint8_t>(ThymeBinEntryType::kSession);
  c = Crc32Update(c, &type_byte, 1);
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&new_session_id),
                  sizeof(new_session_id));
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&reason), sizeof(reason));
  return c ^ 0xFFFFFFFFu;
}

void WriteSessionRecord(ThymeBinSink& sink, uint64_t new_session_id,
                        uint16_t reason) {
  const uint8_t type_byte = static_cast<uint8_t>(ThymeBinEntryType::kSession);
  uint8_t pad[3] = {};
  uint16_t reserved = 0;
  const uint32_t crc = SessionCrc(new_session_id, reason);
  CHECK(sink.Write(&type_byte, 1));
  CHECK(sink.Write(pad, sizeof(pad)));
  CHECK(sink.Write(&new_session_id, sizeof(new_session_id)));
  CHECK(sink.Write(&reason, sizeof(reason)));
  CHECK(sink.Write(&reserved, sizeof(reserved)));
  CHECK(sink.Write(&crc, sizeof(crc)));
}

ThymeStructDesc MakeDesc(uint16_t struct_id, const char* name,
                         uint32_t payload_size) {
  ThymeStructDesc desc{};
  desc.struct_id = struct_id;
  desc.contract_version = 2;
  desc.payload_size = payload_size;
  std::snprintf(desc.name, sizeof(desc.name), "%s", name);
  return desc;
}

void WriteMainRecording(const char* path) {
  ThymeBinFileSink sink(path);
  CHECK(sink.ok());
  ThymeBinEncoder encoder(&sink);
  const ThymeStructDesc descs[2] = {MakeDesc(7, "Imu", 4),
                                    MakeDesc(9, "Pose", 8)};
  CHECK(encoder.WriteHeader(0x1234, kBase, descs, 2));

  const uint8_t a[4] = {1, 2, 3, 4};
  const uint8_t b[8] = {10, 11, 12, 13, 14, 15, 16, 17};
  const uint8_t a2[4] = {5, 6, 7, 8};
  CHECK(encoder.WriteData(1, 7, kBase + 100, 10, a, sizeof(a)));
  CHECK(encoder.WriteData(2, 9, kBase + 150, 11, b, sizeof(b)));
  CHECK(encoder.WriteData(3, 7, kBase + 200, 12, a2, sizeof(a2)));
  CHECK(encoder.WriteGap(7, kBase + 250, 3, 5));
  CHECK(encoder.WriteTrigger(42, kBase + 300, 1000, 2000));
  WriteSessionRecord(sink, 0x5678, 2);
  CHECK(encoder.Finish());
}

void WriteUnknownStructRecording(const char* path) {
  ThymeBinWriter writer(path);
  CHECK(writer.ok());
  const ThymeStructDesc desc = MakeDesc(7, "Imu", 4);
  CHECK(writer.WriteHeader(1, kBase, &desc, 1));
  const uint8_t payload[4] = {1, 2, 3, 4};
  CHECK(writer.WriteData(1, 9, kBase + 10, 0, payload, sizeof(payload)));
}

void WriteDuplicateNameRecording(const char* path) {
  ThymeBinWriter writer(path);
  CHECK(writer.ok());
  const ThymeStructDesc descs[2] = {MakeDesc(7, "Imu", 4),
                                    MakeDesc(9, "Imu", 4)};
  CHECK(writer.WriteHeader(1, kBase, descs, 2));
  const uint8_t payload[4] = {1, 2, 3, 4};
  CHECK(writer.WriteData(1, 7, kBase + 10, 0, payload, sizeof(payload)));
  CHECK(writer.WriteData(2, 9, kBase + 20, 1, payload, sizeof(payload)));
}

const mcap_test::Channel* FindChannel(const mcap_test::Parsed& parsed,
                                      const std::string& topic) {
  for (const auto& channel : parsed.data_channels) {
    if (channel.topic == topic) return &channel;
  }
  return nullptr;
}

std::string PayloadString(const mcap_test::Message& message) {
  return std::string(reinterpret_cast<const char*>(message.data.data()),
                     message.data.size());
}

std::vector<const mcap_test::Message*> MessagesOn(
    const mcap_test::Parsed& parsed, uint16_t channel_id) {
  std::vector<const mcap_test::Message*> out;
  for (const auto& message : parsed.messages) {
    if (message.channel_id == channel_id) out.push_back(&message);
  }
  return out;
}

bool Contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

void PokeByte(const char* path, long offset, uint8_t value) {
  std::FILE* fp = std::fopen(path, "r+b");
  CHECK(fp != nullptr);
  CHECK(std::fseek(fp, offset, SEEK_SET) == 0);
  CHECK(std::fwrite(&value, 1, 1, fp) == 1);
  std::fclose(fp);
}

void RoundTripTest() {
  WriteMainRecording(kBinPath);

  ThymeBinToMcapOptions options{};
  options.include_summary = true;
  options.data_section_crc = true;
  ThymeBinToMcapStats stats{};
  CHECK(ThymeBinToMcap(kBinPath, kMcapPath, options, &stats) ==
        ThymeBinExportStatus::kOk);
  CHECK(stats.data_messages == 3);
  CHECK(stats.gap_messages == 1);
  CHECK(stats.trigger_messages == 1);
  CHECK(stats.session_messages == 2);  // header session + session change
  CHECK(stats.total_messages == 7);
  CHECK(stats.channels == 5);
  CHECK(stats.writer_status == McapStatus::kOk);

  const mcap_test::Parsed parsed = mcap_test::Parse(kMcapPath);
  CHECK(parsed.profile == "thyme");
  CHECK(parsed.library == "thyme-mcap");
  CHECK(parsed.data_schemas.size() == 5);
  CHECK(parsed.data_channels.size() == 5);
  CHECK(parsed.messages.size() == 7);
  CHECK(parsed.data_end_crc == parsed.RecomputeDataCrc());
  CHECK(parsed.has_statistics);
  CHECK(parsed.statistics.message_count == 7);
  CHECK(parsed.statistics.schema_count == 5);
  CHECK(parsed.statistics.channel_count == 5);
  CHECK(parsed.statistics.start_time == kBase);
  CHECK(parsed.statistics.end_time == kBase + 300);

  const mcap_test::Channel* imu = FindChannel(parsed, "thyme/Imu");
  const mcap_test::Channel* pose = FindChannel(parsed, "thyme/Pose");
  const mcap_test::Channel* gap = FindChannel(parsed, "thyme/gap");
  const mcap_test::Channel* trigger = FindChannel(parsed, "thyme/trigger");
  const mcap_test::Channel* session = FindChannel(parsed, "thyme/session");
  CHECK(imu != nullptr && pose != nullptr && gap != nullptr &&
        trigger != nullptr && session != nullptr);
  CHECK(imu->message_encoding == "thyme");
  CHECK(gap->message_encoding == "json");
  CHECK(trigger->message_encoding == "json");
  CHECK(session->message_encoding == "json");

  // Channel metadata carries the thyme struct descriptor identity.
  bool saw_id = false;
  for (const auto& entry : imu->metadata) {
    if (entry.first == "thyme.struct_id") {
      CHECK(entry.second == "7");
      saw_id = true;
    }
    if (entry.first == "thyme.contract_version") CHECK(entry.second == "2");
    if (entry.first == "thyme.payload_size") CHECK(entry.second == "4");
  }
  CHECK(saw_id);

  const auto imu_messages = MessagesOn(parsed, imu->id);
  const auto pose_messages = MessagesOn(parsed, pose->id);
  CHECK(imu_messages.size() == 2);
  CHECK(pose_messages.size() == 1);

  const uint8_t a[4] = {1, 2, 3, 4};
  const uint8_t b[8] = {10, 11, 12, 13, 14, 15, 16, 17};
  const uint8_t a2[4] = {5, 6, 7, 8};
  CHECK(imu_messages[0]->log_time == kBase + 100);
  CHECK(imu_messages[0]->publish_time == kBase + 100);
  CHECK(imu_messages[0]->data.size() == sizeof(a));
  CHECK(std::memcmp(imu_messages[0]->data.data(), a, sizeof(a)) == 0);
  CHECK(imu_messages[1]->log_time == kBase + 200);
  CHECK(imu_messages[1]->data.size() == sizeof(a2));
  CHECK(std::memcmp(imu_messages[1]->data.data(), a2, sizeof(a2)) == 0);
  CHECK(pose_messages[0]->log_time == kBase + 150);
  CHECK(pose_messages[0]->data.size() == sizeof(b));
  CHECK(std::memcmp(pose_messages[0]->data.data(), b, sizeof(b)) == 0);

  const auto gap_messages = MessagesOn(parsed, gap->id);
  CHECK(gap_messages.size() == 1);
  CHECK(gap_messages[0]->log_time == kBase + 250);
  const std::string gap_json = PayloadString(*gap_messages[0]);
  CHECK(Contains(gap_json, "\"struct_id\":7"));
  CHECK(Contains(gap_json, "\"reason\":3"));
  CHECK(Contains(gap_json, "\"missed_count\":5"));

  const auto trigger_messages = MessagesOn(parsed, trigger->id);
  CHECK(trigger_messages.size() == 1);
  CHECK(trigger_messages[0]->log_time == kBase + 300);
  const std::string trigger_json = PayloadString(*trigger_messages[0]);
  CHECK(Contains(trigger_json, "\"trigger_id\":42"));
  CHECK(Contains(trigger_json, "\"pre_roll_ns\":1000"));
  CHECK(Contains(trigger_json, "\"post_roll_ns\":2000"));

  const auto session_messages = MessagesOn(parsed, session->id);
  CHECK(session_messages.size() == 2);
  CHECK(session_messages[0]->log_time == kBase);
  const std::string start_json = PayloadString(*session_messages[0]);
  CHECK(Contains(start_json, "session_start"));
  CHECK(Contains(start_json, "\"session_id\":4660"));  // 0x1234
  CHECK(Contains(start_json, "\"device_id\":0"));
  CHECK(Contains(start_json, "\"file_abi_version\":2"));
  CHECK(session_messages[1]->log_time == kBase + 300);
  const std::string change_json = PayloadString(*session_messages[1]);
  CHECK(Contains(change_json, "session_change"));
  CHECK(Contains(change_json, "\"session_id\":22136"));  // 0x5678
  CHECK(Contains(change_json, "\"reason\":2"));

  // Data schema is the honest opaque-payload JSON schema.
  const mcap_test::Schema& imu_schema = parsed.data_schemas[0];
  CHECK(imu_schema.encoding == "jsonschema");
  const std::string schema_text(
      reinterpret_cast<const char*>(imu_schema.data.data()),
      imu_schema.data.size());
  CHECK(Contains(schema_text, "contentEncoding"));
  CHECK(Contains(schema_text, "ThymeStructDesc carries no field layout"));

  // Independent of the parser's own invariant checks, every record must land
  // exactly on its declared boundary (walk from Header to closing magic).
  std::size_t pos = parsed.header_end;
  std::size_t records = 0;
  while (pos < parsed.closing_magic_offset) {
    const mcap_test::Record record = mcap_test::ReadRecord(parsed.file, pos);
    CHECK(record.next_offset > record.offset);
    pos = record.next_offset;
    ++records;
  }
  CHECK(pos == parsed.closing_magic_offset);
  CHECK(records > 10);

  std::remove(kBinPath);
  std::remove(kMcapPath);
}

void NoEventsTest() {
  WriteMainRecording(kBinPath);
  ThymeBinToMcapOptions options{};
  options.emit_events = false;
  ThymeBinToMcapStats stats{};
  CHECK(ThymeBinToMcap(kBinPath, kMcapPath, options, &stats) ==
        ThymeBinExportStatus::kOk);
  CHECK(stats.data_messages == 3);
  CHECK(stats.gap_messages == 0);
  CHECK(stats.trigger_messages == 0);
  CHECK(stats.session_messages == 0);
  CHECK(stats.channels == 2);

  const mcap_test::Parsed parsed = mcap_test::Parse(kMcapPath);
  CHECK(parsed.messages.size() == 3);
  CHECK(parsed.data_channels.size() == 2);
  CHECK(FindChannel(parsed, "thyme/gap") == nullptr);
  CHECK(FindChannel(parsed, "thyme/trigger") == nullptr);
  CHECK(FindChannel(parsed, "thyme/session") == nullptr);

  std::remove(kBinPath);
  std::remove(kMcapPath);
}

void DuplicateNameTest() {
  WriteDuplicateNameRecording(kBinPath);
  ThymeBinToMcapStats stats{};
  CHECK(ThymeBinToMcap(kBinPath, kMcapPath, {}, &stats) ==
        ThymeBinExportStatus::kOk);
  const mcap_test::Parsed parsed = mcap_test::Parse(kMcapPath);
  CHECK(FindChannel(parsed, "thyme/Imu") != nullptr);
  CHECK(FindChannel(parsed, "thyme/Imu_9") != nullptr);
  std::remove(kBinPath);
  std::remove(kMcapPath);
}

void UnknownStructTest() {
  WriteUnknownStructRecording(kBinPath);
  ThymeBinToMcapStats stats{};
  CHECK(ThymeBinToMcap(kBinPath, kMcapPath, {}, &stats) ==
        ThymeBinExportStatus::kUnknownStruct);
  std::remove(kBinPath);
  std::remove(kMcapPath);
}

void CorruptInputTest() {
  WriteMainRecording(kBinPath);
  // First data record starts after the 32-byte header and two descriptors;
  // its payload starts 32 bytes into the record.
  // No manifest -> the writer keeps the legacy 40 B registry (file v2).
  const long record_offset = static_cast<long>(
      sizeof(ThymeBinFileHeader) + 2 * kThymeStructDescLegacySize);
  const long payload_offset = record_offset + 32;
  PokeByte(kBinPath, payload_offset, 0xEEu);

  ThymeBinToMcapStats stats{};
  CHECK(ThymeBinToMcap(kBinPath, kMcapPath, {}, &stats) ==
        ThymeBinExportStatus::kInputCorrupt);
  CHECK(stats.corrupt_offset == static_cast<uint64_t>(record_offset));
  CHECK(stats.corrupt_crc_expected != stats.corrupt_crc_actual);
  std::remove(kBinPath);
  std::remove(kMcapPath);
}

void MissingInputTest() {
  CHECK(ThymeBinToMcap("/tmp/thyme_mcap_does_not_exist.bin", kMcapPath, {},
                       nullptr) == ThymeBinExportStatus::kInputOpen);
  CHECK(ThymeBinToMcap(static_cast<const char*>(nullptr), kMcapPath, {},
                       nullptr) == ThymeBinExportStatus::kInvalidArgument);
}

}  // namespace

int main() {
  RoundTripTest();
  NoEventsTest();
  DuplicateNameTest();
  UnknownStructTest();
  CorruptInputTest();
  MissingInputTest();
  return 0;
}
