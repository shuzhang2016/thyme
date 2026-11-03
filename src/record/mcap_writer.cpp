#include "thyme/record/mcap_writer.h"

#include "thyme/contracts/frame_header.h"

#include <cstring>
#include <utility>

#if !defined(_WIN32)
#include <unistd.h>
#else
#include <io.h>
#endif

namespace thyme {

namespace {

// MCAP magic: 0x89 'M' 'C' 'A' 'P' '0' 0x0D 0x0A.
constexpr uint8_t kMcapMagic[8] = {0x89u, 0x4Du, 0x43u, 0x41u,
                                   0x50u, 0x30u, 0x0Du, 0x0Au};

constexpr uint8_t kOpHeader = 0x01;
constexpr uint8_t kOpFooter = 0x02;
constexpr uint8_t kOpSchema = 0x03;
constexpr uint8_t kOpChannel = 0x04;
constexpr uint8_t kOpMessage = 0x05;
constexpr uint8_t kOpStatistics = 0x0B;
constexpr uint8_t kOpSummaryOffset = 0x0E;
constexpr uint8_t kOpDataEnd = 0x0F;

void PutU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFFu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void PutU32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFFu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

void PutU64(uint8_t* p, uint64_t v) {
  PutU32(p, static_cast<uint32_t>(v & 0xFFFFFFFFu));
  PutU32(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

void AppendU8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }

void AppendU16(std::vector<uint8_t>& out, uint16_t v) {
  const std::size_t at = out.size();
  out.resize(at + 2);
  PutU16(out.data() + at, v);
}

void AppendU32(std::vector<uint8_t>& out, uint32_t v) {
  const std::size_t at = out.size();
  out.resize(at + 4);
  PutU32(out.data() + at, v);
}

void AppendU64(std::vector<uint8_t>& out, uint64_t v) {
  const std::size_t at = out.size();
  out.resize(at + 8);
  PutU64(out.data() + at, v);
}

void AppendBytes(std::vector<uint8_t>& out, const void* data, std::size_t n) {
  if (data == nullptr || n == 0) return;
  const auto* p = static_cast<const uint8_t*>(data);
  out.insert(out.end(), p, p + n);
}

void AppendString(std::vector<uint8_t>& out, const char* s) {
  const std::size_t n = (s == nullptr) ? 0 : std::strlen(s);
  AppendU32(out, static_cast<uint32_t>(n));
  AppendBytes(out, s, n);
}

std::vector<uint8_t> BuildRecord(uint8_t opcode,
                                 const std::vector<uint8_t>& body) {
  std::vector<uint8_t> record;
  record.reserve(body.size() + 9);
  AppendU8(record, opcode);
  AppendU64(record, static_cast<uint64_t>(body.size()));
  AppendBytes(record, body.data(), body.size());
  return record;
}

}  // namespace

McapWriter::McapWriter() = default;

McapWriter::~McapWriter() {
  if (fp_ != nullptr && !closed_) {
    (void)Close();
  }
}

void McapWriter::Reset() {
  closed_ = false;
  status_ = McapStatus::kOk;
  error_ = nullptr;
  options_ = McapWriterOptions{};
  buffer_.clear();
  buffer_used_ = 0;
  offset_ = 0;
  message_count_ = 0;
  first_log_time_ = 0;
  last_log_time_ = 0;
  schema_count_ = 0;
  channel_count_ = 0;
  data_crc_ = 0xFFFFFFFFu;
  summary_crc_ = 0xFFFFFFFFu;
  in_data_section_ = false;
  summary_start_ = 0;
  summary_offset_start_ = 0;
  schema_group_start_ = 0;
  schema_group_length_ = 0;
  channel_group_start_ = 0;
  channel_group_length_ = 0;
  stats_group_start_ = 0;
  stats_group_length_ = 0;
  for (auto& slot : schemas_) {
    slot.used = false;
    slot.id = 0;
    slot.record.clear();
  }
  for (auto& slot : channels_) {
    slot.used = false;
    slot.id = 0;
    slot.schema_id = 0;
    slot.message_count = 0;
    slot.record.clear();
  }
}

McapStatus McapWriter::SetError(McapStatus status, const char* message) {
  status_ = status;
  error_ = message;
  return status;
}

McapStatus McapWriter::Reject(McapStatus status, const char* message) {
  error_ = message;
  return status;
}

McapStatus McapWriter::Open(const char* path,
                            const McapWriterOptions& options) {
  if (path == nullptr) return SetError(McapStatus::kInvalidArgument,
                                       "mcap: null path");
  if (fp_ != nullptr) return SetError(McapStatus::kBadState,
                                      "mcap: writer already open");
  if (options.buffer_bytes < 256 ||
      options.buffer_bytes > std::size_t{16} * 1024 * 1024) {
    return SetError(McapStatus::kInvalidArgument,
                    "mcap: buffer_bytes out of range");
  }
  Reset();
  options_ = options;
  buffer_.resize(options_.buffer_bytes);
  fp_ = FilePtr(std::fopen(path, "wb"));
  if (fp_ == nullptr) return SetError(McapStatus::kIoError,
                                      "mcap: cannot open output");
  return StartFile();
}

McapStatus McapWriter::OpenFd(int fd, bool owned,
                              const McapWriterOptions& options) {
  if (fd < 0) return SetError(McapStatus::kInvalidArgument,
                              "mcap: invalid fd");
  if (fp_ != nullptr) return SetError(McapStatus::kBadState,
                                      "mcap: writer already open");
  if (options.buffer_bytes < 256 ||
      options.buffer_bytes > std::size_t{16} * 1024 * 1024) {
    return SetError(McapStatus::kInvalidArgument,
                    "mcap: buffer_bytes out of range");
  }
  Reset();
  options_ = options;
  buffer_.resize(options_.buffer_bytes);
  // The FILE owns its fd; duplicate when the caller keeps ownership so that
  // fclose() never closes the caller's descriptor.
#if !defined(_WIN32)
  const int file_fd = owned ? fd : dup(fd);
  if (file_fd < 0) return SetError(McapStatus::kIoError,
                                   "mcap: cannot duplicate fd");
  fp_ = FilePtr(fdopen(file_fd, "wb"));
  if (fp_ == nullptr) {
    (void)close(file_fd);
    return SetError(McapStatus::kIoError, "mcap: fdopen failed");
  }
#else
  const int file_fd = owned ? fd : _dup(fd);
  if (file_fd < 0) return SetError(McapStatus::kIoError,
                                   "mcap: cannot duplicate fd");
  fp_ = FilePtr(_fdopen(file_fd, "wb"));
  if (fp_ == nullptr) {
    (void)_close(file_fd);
    return SetError(McapStatus::kIoError, "mcap: fdopen failed");
  }
#endif
  return StartFile();
}

McapStatus McapWriter::StartFile() {
  std::vector<uint8_t> header;
  AppendString(header, options_.profile == nullptr ? "" : options_.profile);
  AppendString(header, options_.library == nullptr ? "" : options_.library);
  const std::vector<uint8_t> record = BuildRecord(kOpHeader, header);
  McapStatus status = WriteRaw(kMcapMagic, sizeof(kMcapMagic));
  if (status != McapStatus::kOk) return status;
  status = WriteRaw(record.data(), record.size());
  if (status != McapStatus::kOk) return status;
  in_data_section_ = true;
  return McapStatus::kOk;
}

McapStatus McapWriter::AddSchema(uint16_t id, const char* name,
                                 const char* encoding, const void* data,
                                 std::size_t data_len) {
  if (status_ != McapStatus::kOk) return status_;
  if (fp_ == nullptr || closed_) return SetError(McapStatus::kBadState,
                                                 "mcap: writer not open");
  if (id == 0 || name == nullptr || encoding == nullptr ||
      (data == nullptr && data_len != 0)) {
    return Reject(McapStatus::kInvalidArgument, "mcap: invalid schema");
  }
  if (data_len > 0xFFFFFFFFu) {
    return Reject(McapStatus::kInvalidArgument, "mcap: schema too large");
  }
  if (FindSchema(id)) {
    return Reject(McapStatus::kDuplicateId, "mcap: duplicate schema id");
  }
  std::size_t slot = kMcapMaxSchemas;
  for (std::size_t i = 0; i < kMcapMaxSchemas; ++i) {
    if (!schemas_[i].used) {
      slot = i;
      break;
    }
  }
  if (slot == kMcapMaxSchemas) {
    return Reject(McapStatus::kOutOfCapacity, "mcap: schema capacity");
  }

  std::vector<uint8_t> body;
  AppendU16(body, id);
  AppendString(body, name);
  AppendString(body, encoding);
  AppendU32(body, static_cast<uint32_t>(data_len));
  AppendBytes(body, data, data_len);
  std::vector<uint8_t> record = BuildRecord(kOpSchema, body);

  const McapStatus status = WriteRaw(record.data(), record.size());
  if (status != McapStatus::kOk) return status;
  schemas_[slot].used = true;
  schemas_[slot].id = id;
  schemas_[slot].record = std::move(record);
  ++schema_count_;
  return McapStatus::kOk;
}

McapStatus McapWriter::AddChannel(uint16_t id, uint16_t schema_id,
                                  const char* topic,
                                  const char* message_encoding,
                                  const McapMetadataEntry* metadata,
                                  std::size_t metadata_count) {
  if (status_ != McapStatus::kOk) return status_;
  if (fp_ == nullptr || closed_) return SetError(McapStatus::kBadState,
                                                 "mcap: writer not open");
  if (id == 0 || topic == nullptr || message_encoding == nullptr) {
    return Reject(McapStatus::kInvalidArgument, "mcap: invalid channel");
  }
  if (schema_id != 0 && !FindSchema(schema_id)) {
    return Reject(McapStatus::kUnknownSchema, "mcap: unknown schema id");
  }
  if (metadata == nullptr && metadata_count != 0) {
    return Reject(McapStatus::kInvalidArgument, "mcap: invalid metadata");
  }
  for (std::size_t i = 0; i < metadata_count; ++i) {
    if (metadata[i].key == nullptr || metadata[i].value == nullptr) {
      return Reject(McapStatus::kInvalidArgument, "mcap: invalid metadata");
    }
  }
  for (std::size_t i = 0; i < kMcapMaxChannels; ++i) {
    if (channels_[i].used && channels_[i].id == id) {
      return Reject(McapStatus::kDuplicateId, "mcap: duplicate channel id");
    }
  }
  std::size_t slot = kMcapMaxChannels;
  for (std::size_t i = 0; i < kMcapMaxChannels; ++i) {
    if (!channels_[i].used) {
      slot = i;
      break;
    }
  }
  if (slot == kMcapMaxChannels) {
    return Reject(McapStatus::kOutOfCapacity, "mcap: channel capacity");
  }

  std::vector<uint8_t> map;
  for (std::size_t i = 0; i < metadata_count; ++i) {
    AppendString(map, metadata[i].key);
    AppendString(map, metadata[i].value);
  }
  std::vector<uint8_t> body;
  AppendU16(body, id);
  AppendU16(body, schema_id);
  AppendString(body, topic);
  AppendString(body, message_encoding);
  AppendU32(body, static_cast<uint32_t>(map.size()));
  AppendBytes(body, map.data(), map.size());
  std::vector<uint8_t> record = BuildRecord(kOpChannel, body);

  const McapStatus status = WriteRaw(record.data(), record.size());
  if (status != McapStatus::kOk) return status;
  channels_[slot].used = true;
  channels_[slot].id = id;
  channels_[slot].schema_id = schema_id;
  channels_[slot].message_count = 0;
  channels_[slot].record = std::move(record);
  ++channel_count_;
  return McapStatus::kOk;
}

McapStatus McapWriter::WriteMessage(uint16_t channel_id, uint64_t log_time_ns,
                                    const void* data, std::size_t len) {
  std::size_t slot = 0;
  if (status_ != McapStatus::kOk) return status_;
  if (!FindChannel(channel_id, &slot)) {
    return Reject(McapStatus::kUnknownChannel, "mcap: unknown channel id");
  }
  const uint32_t sequence =
      static_cast<uint32_t>(channels_[slot].message_count);
  return WriteMessage(channel_id, sequence, log_time_ns, log_time_ns, data,
                      len);
}

McapStatus McapWriter::WriteMessage(uint16_t channel_id, uint32_t sequence,
                                    uint64_t log_time_ns,
                                    uint64_t publish_time_ns, const void* data,
                                    std::size_t len) {
  if (status_ != McapStatus::kOk) return status_;
  if (fp_ == nullptr || closed_) return SetError(McapStatus::kBadState,
                                                 "mcap: writer not open");
  if (data == nullptr && len != 0) {
    return Reject(McapStatus::kInvalidArgument, "mcap: null message data");
  }
  std::size_t slot = 0;
  if (!FindChannel(channel_id, &slot)) {
    return Reject(McapStatus::kUnknownChannel, "mcap: unknown channel id");
  }
  // Record body: channel_id u16, sequence u32, log_time u64, publish_time u64,
  // then the payload as the remaining bytes.
  uint8_t header[1 + 8 + 2 + 4 + 8 + 8];
  header[0] = kOpMessage;
  PutU64(header + 1, static_cast<uint64_t>(2 + 4 + 8 + 8) + len);
  PutU16(header + 9, channel_id);
  PutU32(header + 11, sequence);
  PutU64(header + 15, log_time_ns);
  PutU64(header + 23, publish_time_ns);

  McapStatus status = WriteRaw(header, sizeof(header));
  if (status != McapStatus::kOk) return status;
  if (len != 0) {
    status = WriteRaw(data, len);
    if (status != McapStatus::kOk) return status;
  }

  if (message_count_ == 0) {
    first_log_time_ = log_time_ns;
    last_log_time_ = log_time_ns;
  } else {
    if (log_time_ns < first_log_time_) first_log_time_ = log_time_ns;
    if (log_time_ns > last_log_time_) last_log_time_ = log_time_ns;
  }
  ++message_count_;
  ++channels_[slot].message_count;
  return McapStatus::kOk;
}

McapStatus McapWriter::WriteSummaryRaw(const void* data, std::size_t n) {
  if (n != 0) {
    summary_crc_ = Crc32Update(
        summary_crc_, static_cast<const uint8_t*>(data), n);
  }
  return WriteRaw(data, n);
}

McapStatus McapWriter::WriteSummary() {
  summary_start_ = offset_;

  schema_group_start_ = offset_;
  for (const auto& slot : schemas_) {
    if (!slot.used) continue;
    const McapStatus status =
        WriteSummaryRaw(slot.record.data(), slot.record.size());
    if (status != McapStatus::kOk) return status;
  }
  schema_group_length_ = offset_ - schema_group_start_;

  channel_group_start_ = offset_;
  for (const auto& slot : channels_) {
    if (!slot.used) continue;
    const McapStatus status =
        WriteSummaryRaw(slot.record.data(), slot.record.size());
    if (status != McapStatus::kOk) return status;
  }
  channel_group_length_ = offset_ - channel_group_start_;

  std::vector<uint8_t> counts;
  for (const auto& slot : channels_) {
    if (!slot.used || slot.message_count == 0) continue;
    AppendU16(counts, slot.id);
    AppendU64(counts, slot.message_count);
  }
  std::vector<uint8_t> body;
  AppendU64(body, message_count_);
  AppendU16(body, static_cast<uint16_t>(schema_count_));
  AppendU32(body, channel_count_);
  AppendU32(body, 0);  // attachment_count
  AppendU32(body, 0);  // metadata_count
  AppendU32(body, 0);  // chunk_count
  AppendU64(body, message_count_ == 0 ? 0 : first_log_time_);
  AppendU64(body, message_count_ == 0 ? 0 : last_log_time_);
  AppendU32(body, static_cast<uint32_t>(counts.size()));
  AppendBytes(body, counts.data(), counts.size());
  const std::vector<uint8_t> stats = BuildRecord(kOpStatistics, body);
  stats_group_start_ = offset_;
  McapStatus status = WriteSummaryRaw(stats.data(), stats.size());
  if (status != McapStatus::kOk) return status;
  stats_group_length_ = offset_ - stats_group_start_;

  summary_offset_start_ = offset_;
  auto write_offset = [this](uint8_t opcode, uint64_t start,
                             uint64_t length) -> McapStatus {
    std::vector<uint8_t> offset_body;
    AppendU8(offset_body, opcode);
    AppendU64(offset_body, start);
    AppendU64(offset_body, length);
    const std::vector<uint8_t> record =
        BuildRecord(kOpSummaryOffset, offset_body);
    return WriteSummaryRaw(record.data(), record.size());
  };
  if (schema_group_length_ != 0) {
    status = write_offset(kOpSchema, schema_group_start_, schema_group_length_);
    if (status != McapStatus::kOk) return status;
  }
  if (channel_group_length_ != 0) {
    status =
        write_offset(kOpChannel, channel_group_start_, channel_group_length_);
    if (status != McapStatus::kOk) return status;
  }
  status = write_offset(kOpStatistics, stats_group_start_, stats_group_length_);
  return status;
}

McapStatus McapWriter::Flush() {
  if (status_ != McapStatus::kOk) return status_;
  if (fp_ == nullptr) return SetError(McapStatus::kBadState,
                                      "mcap: writer not open");
  McapStatus status = FlushBuffer();
  if (status != McapStatus::kOk) return status;
  if (std::fflush(fp_.get()) != 0) {
    return SetError(McapStatus::kIoError, "mcap: flush failed");
  }
  return McapStatus::kOk;
}

McapStatus McapWriter::Close() {
  if (closed_) return status_;
  if (fp_ == nullptr) return SetError(McapStatus::kBadState,
                                      "mcap: writer not open");
  if (status_ != McapStatus::kOk) {
    CloseFile();
    closed_ = true;
    return status_;
  }

  in_data_section_ = false;
  const uint32_t data_crc =
      options_.data_section_crc ? (data_crc_ ^ 0xFFFFFFFFu) : 0u;
  uint8_t data_end[1 + 8 + 4];
  data_end[0] = kOpDataEnd;
  PutU64(data_end + 1, 4);
  PutU32(data_end + 9, data_crc);
  if (WriteRaw(data_end, sizeof(data_end)) != McapStatus::kOk) {
    CloseFile();
    closed_ = true;
    return status_;
  }

  if (options_.include_summary &&
      (schema_count_ != 0 || channel_count_ != 0)) {
    if (WriteSummary() != McapStatus::kOk) {
      CloseFile();
      closed_ = true;
      return status_;
    }
  }

  const uint64_t summary_start = summary_start_;
  const uint64_t summary_offset_start = summary_offset_start_;
  const uint32_t summary_crc =
      summary_start == 0 ? 0u : (summary_crc_ ^ 0xFFFFFFFFu);
  uint8_t footer[1 + 8 + 20];
  footer[0] = kOpFooter;
  PutU64(footer + 1, 20);
  PutU64(footer + 9, summary_start);
  PutU64(footer + 17, summary_offset_start);
  PutU32(footer + 25, summary_crc);
  if (WriteRaw(footer, sizeof(footer)) != McapStatus::kOk ||
      WriteRaw(kMcapMagic, sizeof(kMcapMagic)) != McapStatus::kOk) {
    CloseFile();
    closed_ = true;
    return status_;
  }
  (void)Flush();
  if (fp_ != nullptr) {
    if (FileCloser::Close(fp_.release()) != 0 && status_ == McapStatus::kOk) {
      (void)SetError(McapStatus::kIoError, "mcap: close failed");
    }
  }
  closed_ = true;
  return status_;
}

McapStatus McapWriter::WriteRaw(const void* data, std::size_t n) {
  if (status_ != McapStatus::kOk) return status_;
  if (n == 0) return McapStatus::kOk;
  const auto* bytes = static_cast<const uint8_t*>(data);
  if (in_data_section_ && options_.data_section_crc) {
    data_crc_ = Crc32Update(data_crc_, bytes, n);
  }
  offset_ += static_cast<uint64_t>(n);
  if (buffer_used_ + n <= buffer_.size()) {
    std::memcpy(buffer_.data() + buffer_used_, bytes, n);
    buffer_used_ += n;
    return McapStatus::kOk;
  }
  const McapStatus status = FlushBuffer();
  if (status != McapStatus::kOk) return status;
  if (n >= buffer_.size()) return WriteDirect(data, n);
  std::memcpy(buffer_.data(), bytes, n);
  buffer_used_ = n;
  return McapStatus::kOk;
}

McapStatus McapWriter::WriteDirect(const void* data, std::size_t n) {
  if (fp_ == nullptr) return SetError(McapStatus::kBadState,
                                      "mcap: writer not open");
  const auto* bytes = static_cast<const uint8_t*>(data);
  std::size_t written = 0;
  while (written < n) {
    const std::size_t chunk =
        std::fwrite(bytes + written, 1, n - written, fp_.get());
    if (chunk == 0) return SetError(McapStatus::kIoError, "mcap: write failed");
    written += chunk;
  }
  return McapStatus::kOk;
}

McapStatus McapWriter::FlushBuffer() {
  if (buffer_used_ == 0) return McapStatus::kOk;
  const McapStatus status = WriteDirect(buffer_.data(), buffer_used_);
  if (status != McapStatus::kOk) return status;
  buffer_used_ = 0;
  return McapStatus::kOk;
}

void McapWriter::CloseFile() { fp_.reset(); }

bool McapWriter::FindChannel(uint16_t id, std::size_t* slot) const {
  for (std::size_t i = 0; i < kMcapMaxChannels; ++i) {
    if (channels_[i].used && channels_[i].id == id) {
      if (slot != nullptr) *slot = i;
      return true;
    }
  }
  return false;
}

bool McapWriter::FindSchema(uint16_t id) const {
  for (const auto& slot : schemas_) {
    if (slot.used && slot.id == id) return true;
  }
  return false;
}

}  // namespace thyme
