#pragma once

// Minimal independent MCAP parser used by the REC-06 tests. It walks the
// unchunked subset the writer emits and CHECKs structural invariants: magics,
// record bounds, data-end/footer placement and summary offsets. It is not a
// general MCAP reader (no chunks, indexes, attachments), on purpose.

#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace mcap_test {

inline constexpr uint8_t kMagic[8] = {0x89u, 0x4Du, 0x43u, 0x41u,
                                      0x50u, 0x30u, 0x0Du, 0x0Au};

enum Opcode : uint8_t {
  kHeader = 0x01,
  kFooter = 0x02,
  kSchema = 0x03,
  kChannel = 0x04,
  kMessage = 0x05,
  kStatistics = 0x0B,
  kSummaryOffset = 0x0E,
  kDataEnd = 0x0F,
};

inline std::vector<uint8_t> LoadFile(const char* path) {
  std::FILE* fp = std::fopen(path, "rb");
  CHECK(fp != nullptr);
  CHECK(std::fseek(fp, 0, SEEK_END) == 0);
  const long size = std::ftell(fp);
  CHECK(size >= 0);
  CHECK(std::fseek(fp, 0, SEEK_SET) == 0);
  std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    CHECK(std::fread(bytes.data(), 1, bytes.size(), fp) == bytes.size());
  }
  std::fclose(fp);
  return bytes;
}

inline uint8_t ReadU8(const std::vector<uint8_t>& b, std::size_t& p) {
  CHECK(p < b.size());
  return b[p++];
}

inline uint16_t ReadU16(const std::vector<uint8_t>& b, std::size_t& p) {
  CHECK(p + 2 <= b.size());
  const uint16_t v = static_cast<uint16_t>(
      static_cast<uint16_t>(b[p]) |
      static_cast<uint16_t>(static_cast<uint16_t>(b[p + 1]) << 8));
  p += 2;
  return v;
}

inline uint32_t ReadU32(const std::vector<uint8_t>& b, std::size_t& p) {
  CHECK(p + 4 <= b.size());
  uint32_t v = static_cast<uint32_t>(b[p]);
  v |= static_cast<uint32_t>(b[p + 1]) << 8;
  v |= static_cast<uint32_t>(b[p + 2]) << 16;
  v |= static_cast<uint32_t>(b[p + 3]) << 24;
  p += 4;
  return v;
}

inline uint64_t ReadU64(const std::vector<uint8_t>& b, std::size_t& p) {
  CHECK(p + 8 <= b.size());
  const uint64_t lo = static_cast<uint64_t>(ReadU32(b, p));
  const uint64_t hi = static_cast<uint64_t>(ReadU32(b, p));
  return lo | (hi << 32);
}

inline std::string ReadString(const std::vector<uint8_t>& b, std::size_t& p) {
  const uint32_t n = ReadU32(b, p);
  CHECK(static_cast<std::size_t>(n) <= b.size() - p);
  std::string s(reinterpret_cast<const char*>(b.data() + p), n);
  p += n;
  return s;
}

inline std::vector<uint8_t> ReadBytes(const std::vector<uint8_t>& b,
                                      std::size_t& p) {
  const uint32_t n = ReadU32(b, p);
  CHECK(static_cast<std::size_t>(n) <= b.size() - p);
  std::vector<uint8_t> out(b.begin() + static_cast<std::ptrdiff_t>(p),
                           b.begin() + static_cast<std::ptrdiff_t>(p + n));
  p += n;
  return out;
}

struct Record {
  std::size_t offset = 0;
  uint8_t opcode = 0;
  uint64_t length = 0;
  std::size_t body_offset = 0;
  std::size_t next_offset = 0;
};

inline Record ReadRecord(const std::vector<uint8_t>& b, std::size_t pos) {
  CHECK(pos + 9 <= b.size());
  Record r;
  r.offset = pos;
  r.opcode = b[pos];
  std::size_t p = pos + 1;
  r.length = ReadU64(b, p);
  r.body_offset = pos + 9;
  CHECK(r.length <= static_cast<uint64_t>(b.size() - r.body_offset));
  r.next_offset = r.body_offset + static_cast<std::size_t>(r.length);
  return r;
}

struct Schema {
  uint16_t id = 0;
  std::string name;
  std::string encoding;
  std::vector<uint8_t> data;
};

struct Channel {
  uint16_t id = 0;
  uint16_t schema_id = 0;
  std::string topic;
  std::string message_encoding;
  std::vector<std::pair<std::string, std::string>> metadata;
};

struct Message {
  uint16_t channel_id = 0;
  uint32_t sequence = 0;
  uint64_t log_time = 0;
  uint64_t publish_time = 0;
  std::vector<uint8_t> data;
};

struct Statistics {
  uint64_t message_count = 0;
  uint16_t schema_count = 0;
  uint32_t channel_count = 0;
  uint32_t attachment_count = 0;
  uint32_t metadata_count = 0;
  uint32_t chunk_count = 0;
  uint64_t start_time = 0;
  uint64_t end_time = 0;
  std::vector<std::pair<uint16_t, uint64_t>> channel_message_counts;
};

struct SummaryOffset {
  uint8_t group_opcode = 0;
  uint64_t group_start = 0;
  uint64_t group_length = 0;
};

struct Parsed {
  std::vector<uint8_t> file;
  std::string profile;
  std::string library;

  std::vector<Schema> data_schemas;
  std::vector<Channel> data_channels;
  std::vector<Message> messages;
  uint64_t header_end = 0;
  uint64_t data_end_offset = 0;
  uint32_t data_end_crc = 0;

  uint64_t summary_start = 0;          // actual first summary record
  uint64_t summary_offset_start = 0;   // actual first summary-offset record
  std::vector<Schema> summary_schemas;
  std::vector<Channel> summary_channels;
  Statistics statistics{};
  bool has_statistics = false;
  std::vector<SummaryOffset> summary_offsets;

  uint64_t footer_offset = 0;
  uint64_t footer_summary_start = 0;
  uint64_t footer_summary_offset_start = 0;
  uint32_t footer_summary_crc = 0;
  uint64_t closing_magic_offset = 0;

  uint32_t RecomputeDataCrc() const {
    CHECK(data_end_offset >= header_end);
    return thyme::Crc32Update(0xFFFFFFFFu, file.data() + header_end,
                              static_cast<std::size_t>(data_end_offset -
                                                       header_end)) ^
           0xFFFFFFFFu;
  }
  uint32_t RecomputeSummaryCrc() const {
    CHECK(footer_offset >= summary_start);
    if (footer_offset == summary_start) return 0;
    return thyme::Crc32Update(0xFFFFFFFFu, file.data() + summary_start,
                              static_cast<std::size_t>(footer_offset -
                                                       summary_start)) ^
           0xFFFFFFFFu;
  }
};

inline Schema ParseSchema(const std::vector<uint8_t>& b, const Record& r) {
  Schema s;
  std::size_t p = r.body_offset;
  s.id = ReadU16(b, p);
  s.name = ReadString(b, p);
  s.encoding = ReadString(b, p);
  s.data = ReadBytes(b, p);
  CHECK(p == r.next_offset);
  return s;
}

inline Channel ParseChannel(const std::vector<uint8_t>& b, const Record& r) {
  Channel c;
  std::size_t p = r.body_offset;
  c.id = ReadU16(b, p);
  c.schema_id = ReadU16(b, p);
  c.topic = ReadString(b, p);
  c.message_encoding = ReadString(b, p);
  const uint32_t map_len = ReadU32(b, p);
  CHECK(static_cast<std::size_t>(map_len) <= r.next_offset - p);
  const std::size_t map_end = p + map_len;
  while (p < map_end) {
    const std::string key = ReadString(b, p);
    const std::string value = ReadString(b, p);
    c.metadata.emplace_back(key, value);
  }
  CHECK(p == map_end);
  CHECK(p == r.next_offset);
  return c;
}

inline Message ParseMessage(const std::vector<uint8_t>& b, const Record& r) {
  Message m;
  std::size_t p = r.body_offset;
  m.channel_id = ReadU16(b, p);
  m.sequence = ReadU32(b, p);
  m.log_time = ReadU64(b, p);
  m.publish_time = ReadU64(b, p);
  m.data.assign(b.begin() + static_cast<std::ptrdiff_t>(p),
                b.begin() + static_cast<std::ptrdiff_t>(r.next_offset));
  return m;
}

inline Statistics ParseStatistics(const std::vector<uint8_t>& b,
                                  const Record& r) {
  Statistics s;
  std::size_t p = r.body_offset;
  s.message_count = ReadU64(b, p);
  s.schema_count = ReadU16(b, p);
  s.channel_count = ReadU32(b, p);
  s.attachment_count = ReadU32(b, p);
  s.metadata_count = ReadU32(b, p);
  s.chunk_count = ReadU32(b, p);
  s.start_time = ReadU64(b, p);
  s.end_time = ReadU64(b, p);
  const uint32_t map_len = ReadU32(b, p);
  CHECK(static_cast<std::size_t>(map_len) <= r.next_offset - p);
  const std::size_t map_end = p + map_len;
  while (p < map_end) {
    const uint16_t id = ReadU16(b, p);
    const uint64_t count = ReadU64(b, p);
    s.channel_message_counts.emplace_back(id, count);
  }
  CHECK(p == map_end);
  CHECK(p == r.next_offset);
  return s;
}

inline SummaryOffset ParseSummaryOffset(const std::vector<uint8_t>& b,
                                        const Record& r) {
  SummaryOffset o;
  std::size_t p = r.body_offset;
  o.group_opcode = ReadU8(b, p);
  o.group_start = ReadU64(b, p);
  o.group_length = ReadU64(b, p);
  CHECK(p == r.next_offset);
  return o;
}

inline Parsed Parse(const char* path) {
  Parsed out;
  out.file = LoadFile(path);
  const std::vector<uint8_t>& b = out.file;
  CHECK(b.size() >= 8 + 9 + 9 + 8);
  CHECK(std::memcmp(b.data(), kMagic, sizeof(kMagic)) == 0);
  CHECK(std::memcmp(b.data() + b.size() - sizeof(kMagic), kMagic,
                    sizeof(kMagic)) == 0);

  std::size_t pos = 8;
  const Record header = ReadRecord(b, pos);
  CHECK(header.opcode == kHeader);
  {
    std::size_t p = header.body_offset;
    out.profile = ReadString(b, p);
    out.library = ReadString(b, p);
    CHECK(p == header.next_offset);
  }
  out.header_end = header.next_offset;
  pos = header.next_offset;

  bool saw_data_end = false;
  for (;;) {
    const Record r = ReadRecord(b, pos);
    if (r.opcode == kDataEnd) {
      CHECK(r.length == 4);
      std::size_t p = r.body_offset;
      out.data_end_crc = ReadU32(b, p);
      out.data_end_offset = r.offset;
      pos = r.next_offset;
      saw_data_end = true;
      break;
    }
    CHECK(r.opcode == kSchema || r.opcode == kChannel || r.opcode == kMessage);
    if (r.opcode == kSchema) {
      out.data_schemas.push_back(ParseSchema(b, r));
    } else if (r.opcode == kChannel) {
      out.data_channels.push_back(ParseChannel(b, r));
    } else {
      out.messages.push_back(ParseMessage(b, r));
    }
    pos = r.next_offset;
  }
  CHECK(saw_data_end);
  out.summary_start = pos;

  bool saw_footer = false;
  bool saw_offset = false;
  for (;;) {
    const Record r = ReadRecord(b, pos);
    if (r.opcode == kFooter) {
      CHECK(r.length == 20);
      std::size_t p = r.body_offset;
      out.footer_summary_start = ReadU64(b, p);
      out.footer_summary_offset_start = ReadU64(b, p);
      out.footer_summary_crc = ReadU32(b, p);
      CHECK(p == r.next_offset);
      out.footer_offset = r.offset;
      pos = r.next_offset;
      saw_footer = true;
      break;
    }
    if (r.opcode == kSchema) {
      out.summary_schemas.push_back(ParseSchema(b, r));
    } else if (r.opcode == kChannel) {
      out.summary_channels.push_back(ParseChannel(b, r));
    } else if (r.opcode == kStatistics) {
      CHECK(!out.has_statistics);
      out.statistics = ParseStatistics(b, r);
      out.has_statistics = true;
    } else if (r.opcode == kSummaryOffset) {
      if (!saw_offset) {
        out.summary_offset_start = r.offset;
        saw_offset = true;
      }
      out.summary_offsets.push_back(ParseSummaryOffset(b, r));
    } else {
      CHECK(false);
    }
    pos = r.next_offset;
  }
  CHECK(saw_footer);
  CHECK(pos + sizeof(kMagic) == b.size());
  CHECK(std::memcmp(b.data() + pos, kMagic, sizeof(kMagic)) == 0);
  out.closing_magic_offset = pos;

  if (out.footer_summary_start == 0) {
    CHECK(out.summary_schemas.empty());
    CHECK(out.summary_channels.empty());
    CHECK(!out.has_statistics);
    CHECK(out.summary_offsets.empty());
    CHECK(out.summary_start == out.footer_offset);
  } else {
    CHECK(out.footer_summary_start == out.summary_start);
  }
  if (out.footer_summary_offset_start == 0) {
    CHECK(out.summary_offsets.empty());
  } else {
    CHECK(saw_offset);
    CHECK(out.footer_summary_offset_start == out.summary_offset_start);
  }
  if (out.footer_summary_crc != 0) {
    CHECK(out.RecomputeSummaryCrc() == out.footer_summary_crc);
  }
  return out;
}

}  // namespace mcap_test
