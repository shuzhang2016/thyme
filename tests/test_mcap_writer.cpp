// test_mcap_writer.cpp — REC-06 structural + error-path tests for the
// streaming MCAP writer, including an allocation sentinel around the
// per-message path.

#include "mcap_test_parser.h"
#include "thyme/check.h"
#include "thyme/record/mcap_writer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace thyme;

namespace {

std::atomic<bool> g_armed{false};
std::atomic<std::uint64_t> g_allocs{0};

void* Allocate(std::size_t n) {
  if (g_armed.load(std::memory_order_relaxed)) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
  }
  void* p = std::malloc(n == 0 ? 1 : n);
  if (p == nullptr) std::abort();
  return p;
}

}  // namespace

void* operator new(std::size_t n) { return Allocate(n); }
void* operator new[](std::size_t n) { return Allocate(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

void* operator new(std::size_t n, std::align_val_t alignment) {
  if (g_armed.load(std::memory_order_relaxed)) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
  }
  void* p = nullptr;
  const std::size_t align = static_cast<std::size_t>(alignment);
  if (::posix_memalign(&p, align, n == 0 ? 1 : n) != 0) std::abort();
  return p;
}
void* operator new[](std::size_t n, std::align_val_t alignment) {
  return operator new(n, alignment);
}
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}

namespace {

const char* kPath = "/tmp/thyme_mcap_writer_test.mcap";
const char* kNoSummaryPath = "/tmp/thyme_mcap_writer_nosummary.mcap";
const char* kPipePath = "/tmp/thyme_mcap_writer_pipe.mcap";
const char* kNoAllocPath = "/tmp/thyme_mcap_writer_noalloc.mcap";

const mcap_test::Channel* FindChannel(const mcap_test::Parsed& p,
                                      const std::string& topic) {
  for (const auto& channel : p.data_channels) {
    if (channel.topic == topic) return &channel;
  }
  return nullptr;
}

void StructuralTest() {
  McapWriterOptions options{};
  options.profile = "thyme-test";
  options.library = "thyme-mcap-test";
  options.include_summary = true;
  options.data_section_crc = true;
  options.buffer_bytes = 512;

  McapWriter writer;
  CHECK(writer.Open(kPath, options) == McapStatus::kOk);
  CHECK(writer.AddSchema(1, "test/Demo", "jsonschema", "{\"x\":1}", 7) ==
        McapStatus::kOk);
  CHECK(writer.AddSchema(2, "test/Empty", "jsonschema", nullptr, 0) ==
        McapStatus::kOk);

  const McapMetadataEntry metadata[1] = {{"origin", "unit-test"}};
  CHECK(writer.AddChannel(1, 1, "topic/a", "thyme", metadata, 1) ==
        McapStatus::kOk);
  CHECK(writer.AddChannel(2, 2, "topic/b", "json") == McapStatus::kOk);

  // Recoverable validation errors must not poison the writer.
  CHECK(writer.AddSchema(1, "dup", "jsonschema", "x", 1) ==
        McapStatus::kDuplicateId);
  CHECK(writer.AddChannel(3, 99, "topic/c", "thyme") ==
        McapStatus::kUnknownSchema);
  CHECK(writer.WriteMessage(99, 0, "x", 1) == McapStatus::kUnknownChannel);
  CHECK(writer.WriteMessage(1, 0, nullptr, 1) ==
        McapStatus::kInvalidArgument);
  CHECK(writer.status() == McapStatus::kOk);

  const uint8_t small[3] = {1, 2, 3};
  CHECK(writer.WriteMessage(1, 1000, small, sizeof(small)) == McapStatus::kOk);
  CHECK(writer.WriteMessage(1, 1000, nullptr, 0) == McapStatus::kOk);
  uint8_t big[600];
  for (std::size_t i = 0; i < sizeof(big); ++i) {
    big[i] = static_cast<uint8_t>(i & 0xFFu);
  }
  CHECK(writer.WriteMessage(2, 900, big, sizeof(big)) == McapStatus::kOk);
  uint8_t later[700];
  std::memset(later, 7, sizeof(later));
  CHECK(writer.WriteMessage(1, 2000, later, sizeof(later)) == McapStatus::kOk);
  CHECK(writer.message_count() == 4);
  CHECK(writer.schema_count() == 2);
  CHECK(writer.channel_count() == 2);
  CHECK(writer.Close() == McapStatus::kOk);
  CHECK(writer.Close() == McapStatus::kOk);  // idempotent
  CHECK(writer.is_closed());

  const mcap_test::Parsed p = mcap_test::Parse(kPath);
  CHECK(p.profile == "thyme-test");
  CHECK(p.library == "thyme-mcap-test");
  CHECK(p.data_schemas.size() == 2);
  CHECK(p.data_schemas[0].id == 1);
  CHECK(p.data_schemas[0].name == "test/Demo");
  CHECK(p.data_schemas[0].encoding == "jsonschema");
  CHECK(p.data_schemas[0].data.size() == 7);
  CHECK(p.data_schemas[1].id == 2);
  CHECK(p.data_schemas[1].data.empty());
  CHECK(p.summary_schemas.size() == p.data_schemas.size());
  for (std::size_t i = 0; i < p.data_schemas.size(); ++i) {
    CHECK(p.summary_schemas[i].id == p.data_schemas[i].id);
    CHECK(p.summary_schemas[i].name == p.data_schemas[i].name);
    CHECK(p.summary_schemas[i].data == p.data_schemas[i].data);
  }

  CHECK(p.data_channels.size() == 2);
  CHECK(p.data_channels[0].id == 1);
  CHECK(p.data_channels[0].schema_id == 1);
  CHECK(p.data_channels[0].topic == "topic/a");
  CHECK(p.data_channels[0].message_encoding == "thyme");
  CHECK(p.data_channels[0].metadata.size() == 1);
  CHECK(p.data_channels[0].metadata[0].first == "origin");
  CHECK(p.data_channels[0].metadata[0].second == "unit-test");
  CHECK(FindChannel(p, "topic/b") != nullptr);
  CHECK(p.summary_channels.size() == p.data_channels.size());

  CHECK(p.messages.size() == 4);
  CHECK(p.messages[0].channel_id == 1);
  CHECK(p.messages[0].sequence == 0);
  CHECK(p.messages[0].log_time == 1000);
  CHECK(p.messages[0].publish_time == 1000);
  CHECK(p.messages[0].data.size() == 3);
  CHECK(p.messages[0].data[0] == 1 && p.messages[0].data[2] == 3);
  CHECK(p.messages[1].sequence == 1);
  CHECK(p.messages[1].data.empty());
  CHECK(p.messages[2].channel_id == 2);
  CHECK(p.messages[2].log_time == 900);
  CHECK(p.messages[2].data.size() == sizeof(big));
  CHECK(std::memcmp(p.messages[2].data.data(), big, sizeof(big)) == 0);
  CHECK(p.messages[3].sequence == 2);
  CHECK(p.messages[3].data.size() == sizeof(later));
  CHECK(std::memcmp(p.messages[3].data.data(), later, sizeof(later)) == 0);

  // Data End CRC covers the data section, Header record excluded.
  CHECK(p.data_end_crc != 0);
  CHECK(p.data_end_crc == p.RecomputeDataCrc());

  // Summary: repeated schemas/channels then Statistics then Summary Offsets.
  CHECK(p.has_statistics);
  CHECK(p.statistics.message_count == 4);
  CHECK(p.statistics.schema_count == 2);
  CHECK(p.statistics.channel_count == 2);
  CHECK(p.statistics.attachment_count == 0);
  CHECK(p.statistics.metadata_count == 0);
  CHECK(p.statistics.chunk_count == 0);
  CHECK(p.statistics.start_time == 900);
  CHECK(p.statistics.end_time == 2000);
  uint64_t channel1_count = 0;
  uint64_t channel2_count = 0;
  for (const auto& entry : p.statistics.channel_message_counts) {
    if (entry.first == 1) channel1_count = entry.second;
    if (entry.first == 2) channel2_count = entry.second;
  }
  CHECK(channel1_count == 3);
  CHECK(channel2_count == 1);

  CHECK(p.summary_offsets.size() == 3);
  CHECK(p.summary_offsets[0].group_opcode == mcap_test::kSchema);
  CHECK(p.summary_offsets[1].group_opcode == mcap_test::kChannel);
  CHECK(p.summary_offsets[2].group_opcode == mcap_test::kStatistics);
  for (const auto& offset : p.summary_offsets) {
    CHECK(offset.group_start >= p.summary_start);
    CHECK(offset.group_start + offset.group_length <= p.footer_offset);
    CHECK(offset.group_length >= 9);
    CHECK(p.file[static_cast<std::size_t>(offset.group_start)] ==
          offset.group_opcode);
    const mcap_test::Record record =
        mcap_test::ReadRecord(p.file, static_cast<std::size_t>(offset.group_start));
    CHECK(record.offset + offset.group_length <= p.footer_offset);
  }
  CHECK(p.summary_offsets[0].group_start == p.summary_start);

  std::remove(kPath);
}

void NoSummaryTest() {
  McapWriterOptions options{};
  options.include_summary = false;
  options.data_section_crc = false;

  McapWriter writer;
  CHECK(writer.Open(kNoSummaryPath, options) == McapStatus::kOk);
  CHECK(writer.AddSchema(1, "S", "jsonschema", "", 0) == McapStatus::kOk);
  CHECK(writer.AddChannel(1, 1, "t", "thyme") == McapStatus::kOk);
  const uint8_t payload[2] = {9, 8};
  CHECK(writer.WriteMessage(1, 42, payload, sizeof(payload)) ==
        McapStatus::kOk);
  CHECK(writer.Close() == McapStatus::kOk);

  const mcap_test::Parsed p = mcap_test::Parse(kNoSummaryPath);
  CHECK(p.messages.size() == 1);
  CHECK(p.messages[0].log_time == 42);
  CHECK(p.data_end_crc == 0);
  CHECK(p.footer_summary_start == 0);
  CHECK(p.footer_summary_offset_start == 0);
  CHECK(p.footer_summary_crc == 0);
  CHECK(!p.has_statistics);
  CHECK(p.summary_start == p.footer_offset);
  std::remove(kNoSummaryPath);
}

#if !defined(_WIN32)
void PipeTest() {
  int fds[2];
  CHECK(pipe(fds) == 0);
  McapWriter writer;
  McapWriterOptions options{};
  options.buffer_bytes = 256;
  CHECK(writer.OpenFd(fds[1], false, options) == McapStatus::kOk);
  CHECK(writer.AddSchema(1, "P", "jsonschema", "{}", 2) == McapStatus::kOk);
  CHECK(writer.AddChannel(1, 1, "pipe", "thyme") == McapStatus::kOk);
  const uint8_t payload[5] = {1, 2, 3, 4, 5};
  CHECK(writer.WriteMessage(1, 123, payload, sizeof(payload)) ==
        McapStatus::kOk);
  CHECK(writer.Close() == McapStatus::kOk);
  CHECK(close(fds[1]) == 0);

  std::vector<uint8_t> bytes;
  uint8_t chunk[512];
  for (;;) {
    const ssize_t n = read(fds[0], chunk, sizeof(chunk));
    CHECK(n >= 0);
    if (n == 0) break;
    bytes.insert(bytes.end(), chunk, chunk + n);
  }
  CHECK(close(fds[0]) == 0);
  CHECK(bytes.size() > 32);
  CHECK(std::memcmp(bytes.data(), mcap_test::kMagic, 8) == 0);

  std::FILE* fp = std::fopen(kPipePath, "wb");
  CHECK(fp != nullptr);
  CHECK(std::fwrite(bytes.data(), 1, bytes.size(), fp) == bytes.size());
  std::fclose(fp);
  const mcap_test::Parsed p = mcap_test::Parse(kPipePath);
  CHECK(p.messages.size() == 1);
  CHECK(p.messages[0].log_time == 123);
  CHECK(p.messages[0].data.size() == sizeof(payload));
  std::remove(kPipePath);
}
#endif

void AllocationFreeMessagePath() {
  McapWriter writer;
  McapWriterOptions options{};
  options.buffer_bytes = 4096;
  CHECK(writer.Open(kNoAllocPath, options) == McapStatus::kOk);
  CHECK(writer.AddSchema(1, "A", "jsonschema", "{}", 2) == McapStatus::kOk);
  CHECK(writer.AddChannel(1, 1, "alloc", "thyme") == McapStatus::kOk);

  uint8_t payload[128];
  for (std::size_t i = 0; i < sizeof(payload); ++i) {
    payload[i] = static_cast<uint8_t>(i);
  }
  uint8_t large[9000];
  std::memset(large, 3, sizeof(large));
  for (int i = 0; i < 8; ++i) {
    CHECK(writer.WriteMessage(1, 100, payload, sizeof(payload)) ==
          McapStatus::kOk);
  }
  CHECK(writer.WriteMessage(1, 101, large, sizeof(large)) == McapStatus::kOk);
  CHECK(writer.Flush() == McapStatus::kOk);

  g_allocs.store(0);
  g_armed.store(true);
  McapStatus status = McapStatus::kOk;
  for (int i = 0; i < 300; ++i) {
    status = writer.WriteMessage(1, static_cast<uint64_t>(1000 + i), payload,
                                 sizeof(payload));
    if (status != McapStatus::kOk) break;
  }
  if (status == McapStatus::kOk) {
    status = writer.WriteMessage(1, 5000, large, sizeof(large));
  }
  g_armed.store(false);
  const std::uint64_t allocs = g_allocs.load();
  CHECK(status == McapStatus::kOk);
  CHECK(allocs == 0);
  CHECK(writer.Close() == McapStatus::kOk);

  const mcap_test::Parsed p = mcap_test::Parse(kNoAllocPath);
  CHECK(p.messages.size() == 8 + 1 + 300 + 1);
  std::remove(kNoAllocPath);
}

}  // namespace

int main() {
  StructuralTest();
  NoSummaryTest();
#if !defined(_WIN32)
  PipeTest();
#endif
  AllocationFreeMessagePath();
  return 0;
}
