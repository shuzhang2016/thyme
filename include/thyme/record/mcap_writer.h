#pragma once

#include "thyme/record/file_ptr.h"

// Streaming MCAP writer (https://mcap.dev). REC-06.
//
// Implemented subset: an unchunked data section with Header, Schema, Channel
// and Message records, an optional summary section (repeated Schema/Channel
// records, Statistics, Summary Offset records) and the Footer + closing magic.
// Not implemented: chunks, chunk/message/attachment/metadata indexes,
// attachments, metadata records and compression. Records are written
// sequentially to an fd/FILE; nothing is seeked and no whole-file buffer is
// kept, so the writer also works on pipes and sockets.
//
// `data_section_crc` (Data End record) and `summary_crc` (Footer record) are
// CRC-32 (IEEE, as Crc32Update in thyme/contracts/frame_header.h). The data
// section CRC is opt-in and written as 0 when disabled; 0 means "absent" per
// the MCAP spec. The summary CRC is always computed when a summary is written.
//
// Per-message path: WriteMessage appends to a fixed buffer allocated by Open()
// and performs no heap allocation. Schema/channel records are materialized
// once at setup (they are needed twice: data section and summary section).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace thyme {

enum class McapStatus : uint8_t {
  kOk = 0,
  kInvalidArgument,
  kBadState,
  kIoError,
  kDuplicateId,
  kOutOfCapacity,
  kUnknownChannel,
  kUnknownSchema,
};

// Fixed capacity keeps the writer object allocation-bounded and the channel
// lookup (linear, small) allocation-free. 128 is comfortably above the 64
// struct ceiling of .thyme.bin plus the event channels.
inline constexpr std::size_t kMcapMaxSchemas = 128;
inline constexpr std::size_t kMcapMaxChannels = 128;
inline constexpr std::size_t kMcapDefaultBufferBytes = std::size_t{64} * 1024;

struct McapWriterOptions {
  const char* profile = "";
  const char* library = "thyme";
  bool include_summary = true;
  // Opt-in: Data End data_section_crc. 0 = absent (default).
  bool data_section_crc = false;
  std::size_t buffer_bytes = kMcapDefaultBufferBytes;
};

struct McapMetadataEntry {
  const char* key = nullptr;
  const char* value = nullptr;
};

class McapWriter {
 public:
  McapWriter();
  ~McapWriter();
  McapWriter(const McapWriter&) = delete;
  McapWriter& operator=(const McapWriter&) = delete;

  // Opens `path` for writing and emits the file magic + Header record.
  McapStatus Open(const char* path,
                  const McapWriterOptions& options = McapWriterOptions{});
  // Wraps `fd` with fdopen(). `owned` controls whether close() closes the fd.
  // The fd is written sequentially; it must not be buffered elsewhere.
  McapStatus OpenFd(int fd, bool owned,
                    const McapWriterOptions& options = McapWriterOptions{});

  bool is_open() const { return fp_ != nullptr && !closed_; }
  bool is_closed() const { return closed_; }
  McapStatus status() const { return status_; }
  // Static literal; never allocated.
  const char* error() const { return error_; }

  // id must be non-zero and unique; encoding e.g. "jsonschema".
  McapStatus AddSchema(uint16_t id, const char* name, const char* encoding,
                       const void* data, std::size_t data_len);
  // id must be non-zero and unique; schema_id 0 means "no schema" and is
  // allowed. Metadata is an optional string map.
  McapStatus AddChannel(uint16_t id, uint16_t schema_id, const char* topic,
                        const char* message_encoding,
                        const McapMetadataEntry* metadata = nullptr,
                        std::size_t metadata_count = 0);
  // publish_time == log_time. `sequence` auto-increments per channel.
  McapStatus WriteMessage(uint16_t channel_id, uint64_t log_time_ns,
                          const void* data, std::size_t len);
  // Explicit sequence/publish_time variant.
  McapStatus WriteMessage(uint16_t channel_id, uint32_t sequence,
                          uint64_t log_time_ns, uint64_t publish_time_ns,
                          const void* data, std::size_t len);

  McapStatus Flush();
  // Writes Data End, the summary (when enabled) and the Footer + magic, then
  // closes the file. Idempotent.
  McapStatus Close();

  uint64_t message_count() const { return message_count_; }
  uint32_t schema_count() const { return schema_count_; }
  uint32_t channel_count() const { return channel_count_; }
  uint64_t bytes_written() const { return offset_; }

 private:
  struct SchemaSlot {
    bool used = false;
    uint16_t id = 0;
    std::vector<uint8_t> record;  // complete record: opcode + length + body
  };
  struct ChannelSlot {
    bool used = false;
    uint16_t id = 0;
    uint16_t schema_id = 0;
    uint64_t message_count = 0;
    std::vector<uint8_t> record;
  };

  void Reset();
  McapStatus StartFile();
  McapStatus SetError(McapStatus status, const char* message);
  McapStatus Reject(McapStatus status, const char* message);
  McapStatus WriteRaw(const void* data, std::size_t n);
  McapStatus WriteDirect(const void* data, std::size_t n);
  McapStatus WriteSummaryRaw(const void* data, std::size_t n);
  McapStatus FlushBuffer();
  McapStatus WriteSummary();
  void CloseFile();
  bool FindChannel(uint16_t id, std::size_t* slot) const;
  bool FindSchema(uint16_t id) const;

  FilePtr fp_;
  bool closed_ = false;
  McapStatus status_ = McapStatus::kOk;
  const char* error_ = nullptr;
  McapWriterOptions options_{};
  std::vector<uint8_t> buffer_;
  std::size_t buffer_used_ = 0;
  uint64_t offset_ = 0;

  uint64_t message_count_ = 0;
  uint64_t first_log_time_ = 0;
  uint64_t last_log_time_ = 0;
  uint32_t schema_count_ = 0;
  uint32_t channel_count_ = 0;

  uint32_t data_crc_ = 0xFFFFFFFFu;
  uint32_t summary_crc_ = 0xFFFFFFFFu;
  bool in_data_section_ = false;

  uint64_t summary_start_ = 0;
  uint64_t summary_offset_start_ = 0;
  uint64_t schema_group_start_ = 0;
  uint64_t schema_group_length_ = 0;
  uint64_t channel_group_start_ = 0;
  uint64_t channel_group_length_ = 0;
  uint64_t stats_group_start_ = 0;
  uint64_t stats_group_length_ = 0;

  SchemaSlot schemas_[kMcapMaxSchemas];
  ChannelSlot channels_[kMcapMaxChannels];
};

}  // namespace thyme
