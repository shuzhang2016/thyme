#include "thyme/check.h"
#include "thyme/record/thyme_bin.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace thyme;

#if defined(__linux__)
namespace {
int g_fseek_calls = 0;
int g_fail_fseek_call = -1;
}  // namespace

extern "C" int __real_fseek(std::FILE* stream, long offset, int whence);
extern "C" int __wrap_fseek(std::FILE* stream, long offset, int whence) {
  ++g_fseek_calls;
  if (g_fseek_calls == g_fail_fseek_call) {
    g_fail_fseek_call = -1;
    return -1;
  }
  return __real_fseek(stream, offset, whence);
}
#endif

namespace {

constexpr uint64_t kBaseMono = 1000000;
constexpr uint64_t kStep = 1000;

ThymeStructDesc MakeDesc() {
  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = 8;
  std::snprintf(desc.name, sizeof(desc.name), "Demo");
  return desc;
}

void WriteRecords(const char* path, int count) {
  ThymeStructDesc desc = MakeDesc();
  ThymeBinWriter w(path);
  CHECK(w.ok());
  CHECK(w.WriteHeader(77, kBaseMono, &desc, 1));
  const uint8_t payload[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  for (int i = 0; i < count; ++i) {
    CHECK(w.WriteData(static_cast<uint64_t>(i + 1), 0,
                      kBaseMono + static_cast<uint64_t>(i) * kStep,
                      static_cast<uint32_t>(i), payload, sizeof(payload)));
  }
}

struct Row {
  uint64_t sequence = 0;
  uint64_t mono_ns = 0;
};

std::vector<Row> ScanAll(const char* path) {
  ThymeBinReader reader(path);
  CHECK(reader.ok());
  std::vector<Row> rows;
  ThymeBinDataEntry e{};
  while (reader.ReadNext(e) == ThymeBinReader::ReadResult::kOk) {
    if (e.type == ThymeBinEntryType::kData) {
      rows.push_back(Row{e.record_sequence, e.mono_ns});
    }
  }
  return rows;
}

long FileSize(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  CHECK(f != nullptr);
  CHECK(std::fseek(f, 0, SEEK_END) == 0);
  const long size = std::ftell(f);
  std::fclose(f);
  CHECK(size >= 0);
  return size;
}

void Poke(const char* path, long off, const void* data, std::size_t n) {
  std::FILE* f = std::fopen(path, "r+b");
  CHECK(f != nullptr);
  CHECK(std::fseek(f, off, SEEK_SET) == 0);
  CHECK(std::fwrite(data, 1, n, f) == n);
  std::fclose(f);
}

void TruncateAt(const char* path, long len) {
  std::FILE* f = std::fopen(path, "r+b");
  CHECK(f != nullptr);
  CHECK(ftruncate(fileno(f), len) == 0);
  std::fclose(f);
}

ThymeBinIndexFooter ReadFooter(const char* path) {
  ThymeBinIndexFooter footer{};
  const long size = FileSize(path);
  CHECK(size >= static_cast<long>(sizeof(footer)));
  std::FILE* f = std::fopen(path, "rb");
  CHECK(f != nullptr);
  CHECK(std::fseek(f, size - static_cast<long>(sizeof(footer)), SEEK_SET) == 0);
  CHECK(std::fread(&footer, 1, sizeof(footer), f) == sizeof(footer));
  std::fclose(f);
  return footer;
}

uint32_t EntryCrc(const ThymeBinIndexEntry& e) {
  return Crc32Update(0xFFFFFFFFu,
                     reinterpret_cast<const uint8_t*>(&e),
                     offsetof(ThymeBinIndexEntry, crc32)) ^
         0xFFFFFFFFu;
}

// Round-trip: seek to a block, then the remaining sequence must match a full
// scan starting at that block.
void TestRoundTripAndSeek() {
  const char* path = "/tmp/thyme_bin_index_roundtrip.bin";
  const int kCount = 200;  // blocks at records 0, 64, 128, 192
  WriteRecords(path, kCount);

  ThymeBinReader reader(path);
  CHECK(reader.ok());
  CHECK(reader.header().file_abi_version == kThymeBinFileAbiVersionIndexed);
  CHECK(reader.index_status() == ThymeBinReader::IndexStatus::kOk);
  CHECK(reader.HasIndex());
  CHECK(reader.index_block_count() == 4);
  CHECK(reader.index_block_size() == kThymeBinIndexBlockSize);
  CHECK(reader.indexed_record_count() == static_cast<uint64_t>(kCount));
  CHECK(reader.first_mono_ns() == kBaseMono);
  CHECK(reader.last_mono_ns() == kBaseMono + static_cast<uint64_t>(kCount - 1) * kStep);

  const std::vector<Row> all = ScanAll(path);
  CHECK(all.size() == static_cast<std::size_t>(kCount));

  auto CheckTail = [&](uint64_t target, std::size_t first_index) {
    CHECK(reader.SeekToMonoNs(target));
    for (std::size_t i = first_index; i < all.size(); ++i) {
      ThymeBinDataEntry e{};
      CHECK(reader.ReadNext(e) == ThymeBinReader::ReadResult::kOk);
      CHECK(e.type == ThymeBinEntryType::kData);
      CHECK(e.record_sequence == all[i].sequence);
      CHECK(e.mono_ns == all[i].mono_ns);
    }
    ThymeBinDataEntry eof{};
    CHECK(reader.ReadNext(eof) == ThymeBinReader::ReadResult::kEof);
  };

  // Between blocks: record 150 lands on block 128 and scans 128..199.
  CheckTail(kBaseMono + 150 * kStep, 128);
  // Exact block boundary: mono of record 64 lands exactly on block 64.
  CheckTail(kBaseMono + 64 * kStep, 64);
  // Before the recording: the reader is positioned at the first record.
  CheckTail(0, 0);
  // Past the end: last block (192), then EOF.
  CheckTail(kBaseMono + 1000000 * kStep, 192);

  std::remove(path);
}

// Old/stream recordings simply have no footer; sequential reads still work.
void TestNoIndexFallbacks() {
  const char* path = "/tmp/thyme_bin_index_absent.bin";
  const int kCount = 10;

  // (a) Encoder without Finish(): no footer, version 2 header.
  {
    ThymeStructDesc desc = MakeDesc();
    ThymeBinFileSink file(path);
    CHECK(file.ok());
    ThymeBinEncoder enc(&file);
    CHECK(enc.WriteHeader(1, kBaseMono, &desc, 1));
    const uint8_t payload[8] = {0};
    for (int i = 0; i < kCount; ++i) {
      CHECK(enc.WriteData(static_cast<uint64_t>(i + 1), 0,
                          kBaseMono + static_cast<uint64_t>(i) * kStep, 0,
                          payload, sizeof(payload)));
    }
    CHECK(enc.records_written() == static_cast<uint64_t>(kCount));
  }
  {
    ThymeBinReader reader(path);
    CHECK(reader.ok());
    CHECK(!reader.HasIndex());
    CHECK(reader.index_status() == ThymeBinReader::IndexStatus::kAbsent);
    CHECK(reader.SeekToMonoNs(kBaseMono + 5 * kStep) == false);
    CHECK(ScanAll(path).size() == static_cast<std::size_t>(kCount));
  }

  // (b) Version 1 (pre-REC-04) header: still accepted.
  {
    ThymeBinFileHeader h{};
    std::FILE* f = std::fopen(path, "r+b");
    CHECK(f != nullptr);
    CHECK(std::fread(&h, 1, sizeof(h), f) == sizeof(h));
    h.file_abi_version = kThymeBinFileAbiVersionUnindexed;
    CHECK(std::fseek(f, 0, SEEK_SET) == 0);
    CHECK(std::fwrite(&h, 1, sizeof(h), f) == sizeof(h));
    std::fclose(f);
  }
  {
    ThymeBinReader reader(path);
    CHECK(reader.ok());
    CHECK(reader.header().file_abi_version == kThymeBinFileAbiVersionUnindexed);
    CHECK(!reader.HasIndex());
    CHECK(ScanAll(path).size() == static_cast<std::size_t>(kCount));
  }

  // (c) Crash-truncated recording: footer cut off, sequential read still works.
  WriteRecords(path, kCount);
  const ThymeBinIndexFooter footer = ReadFooter(path);
  CHECK(footer.block_count == 1);
  const long footer_start =
      static_cast<long>(footer.entries_offset);
  TruncateAt(path, footer_start);
  {
    ThymeBinReader reader(path);
    CHECK(reader.ok());
    CHECK(!reader.HasIndex());
    CHECK(reader.index_status() == ThymeBinReader::IndexStatus::kAbsent);
    CHECK(ScanAll(path).size() == static_cast<std::size_t>(kCount));
  }

  std::remove(path);
}

// Corrupt footers are rejected with a status, never followed blindly.
void TestCorruptFooter() {
  const char* path = "/tmp/thyme_bin_index_corrupt.bin";
  const int kCount = 70;  // blocks at 0 and 64

  // (a) Bad magic: treated as "no index" (an old/unknown trailer), and the
  // records before it remain readable.
  WriteRecords(path, kCount);
  {
    const long size = FileSize(path);
    const long magic_off = size - static_cast<long>(sizeof(ThymeBinIndexFooter));
    const char corrupt = 'X';
    Poke(path, magic_off, &corrupt, 1);
  }
  {
    ThymeBinReader reader(path);
    CHECK(reader.ok());
    CHECK(!reader.HasIndex());
    CHECK(reader.index_status() == ThymeBinReader::IndexStatus::kAbsent);
    int ok_rows = 0;
    ThymeBinDataEntry e{};
    while (reader.ReadNext(e) == ThymeBinReader::ReadResult::kOk) ++ok_rows;
    CHECK(ok_rows == kCount);
  }

  // (b) Out-of-range offset with a valid entry CRC: index rejected, records
  // before the footer still readable, sequential reads stop at the footer.
  WriteRecords(path, kCount);
  const ThymeBinIndexFooter footer = ReadFooter(path);
  CHECK(footer.block_count == 2);
  {
    std::FILE* f = std::fopen(path, "r+b");
    CHECK(f != nullptr);
    CHECK(std::fseek(f, static_cast<long>(footer.entries_offset), SEEK_SET) == 0);
    ThymeBinIndexEntry entry{};
    CHECK(std::fread(&entry, 1, sizeof(entry), f) == sizeof(entry));
    entry.offset = static_cast<uint64_t>(FileSize(path)) + 4096;
    entry.crc32 = EntryCrc(entry);
    CHECK(std::fseek(f, static_cast<long>(footer.entries_offset), SEEK_SET) == 0);
    CHECK(std::fwrite(&entry, 1, sizeof(entry), f) == sizeof(entry));
    std::fclose(f);
  }
  {
    ThymeBinReader reader(path);
    CHECK(reader.ok());
    CHECK(!reader.HasIndex());
    CHECK(reader.index_status() == ThymeBinReader::IndexStatus::kCorrupt);
    CHECK(reader.index_error() != nullptr);
    CHECK(reader.SeekToMonoNs(kBaseMono) == false);
    std::vector<Row> rows = ScanAll(path);
    CHECK(rows.size() == static_cast<std::size_t>(kCount));
  }

  // (c) Bad footer CRC: rejected before any entry is trusted.
  WriteRecords(path, kCount);
  {
    const long size = FileSize(path);
    const long crc_off = size - static_cast<long>(sizeof(ThymeBinIndexFooter)) +
                         static_cast<long>(offsetof(ThymeBinIndexFooter, crc32));
    const uint8_t flip = 0xFF;
    Poke(path, crc_off, &flip, 1);
  }
  {
    ThymeBinReader reader(path);
    CHECK(reader.ok());
    CHECK(reader.index_status() == ThymeBinReader::IndexStatus::kCorrupt);
    CHECK(reader.index_error() != nullptr);
    CHECK(reader.SeekToMonoNs(kBaseMono) == false);
  }

  std::remove(path);
}

#if !defined(_WIN32)
// A non-seekable fd (the TCP path uses the same sink) carries no footer.
void TestFdSinkHasNoIndex() {
  const char* path = "/tmp/thyme_bin_index_fdsink.bin";
  int sv[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

  ThymeStructDesc desc = MakeDesc();
  const uint8_t payload[8] = {9, 8, 7, 6, 5, 4, 3, 2};
  {
    ThymeBinFdSink sink(sv[0], /*owned=*/false);
    ThymeBinEncoder enc(&sink);
    CHECK(enc.WriteHeader(5, kBaseMono, &desc, 1));
    for (int i = 0; i < 3; ++i) {
      CHECK(enc.WriteData(static_cast<uint64_t>(i + 1), 0,
                          kBaseMono + static_cast<uint64_t>(i) * kStep, 0,
                          payload, sizeof(payload)));
    }
    CHECK(enc.Finish());
  }
  close(sv[0]);

  std::vector<uint8_t> bytes;
  char buf[4096];
  for (;;) {
    const ssize_t n = read(sv[1], buf, sizeof(buf));
    CHECK(n >= 0);
    if (n == 0) break;
    bytes.insert(bytes.end(), buf, buf + n);
  }
  close(sv[1]);
  CHECK(!bytes.empty());

  std::FILE* f = std::fopen(path, "wb");
  CHECK(f != nullptr);
  CHECK(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
  std::fclose(f);

  ThymeBinReader reader(path);
  CHECK(reader.ok());
  CHECK(reader.header().file_abi_version == kThymeBinFileAbiVersionUnindexed);
  CHECK(!reader.HasIndex());
  CHECK(reader.index_status() == ThymeBinReader::IndexStatus::kAbsent);
  CHECK(ScanAll(path).size() == 3);
  std::remove(path);
}
#endif

#if defined(__linux__)
void TestIndexIoErrors() {
  const char* path = "/tmp/thyme_bin_index_io_error.bin";
  WriteRecords(path, 70);

  // Calls in LoadIndex: end seek, footer seek, data restore, entries seek.
  // Each failure must be distinguishable from a missing or invalid footer.
  for (int fail_call : {1, 2, 4}) {
    g_fseek_calls = 0;
    g_fail_fseek_call = fail_call;
    {
      ThymeBinReader reader(path);
      g_fail_fseek_call = -1;
      CHECK(reader.ok());
      CHECK(reader.index_status() == ThymeBinReader::IndexStatus::kIoError);
      CHECK(reader.index_error() != nullptr);
      CHECK(!reader.HasIndex());
      CHECK(!reader.SeekToMonoNs(kBaseMono));
      CHECK(g_fseek_calls >= fail_call);
    }
    CHECK(ScanAll(path).size() == 70);
  }

  std::remove(path);
}
#endif

}  // namespace

int main() {
  TestRoundTripAndSeek();
  TestNoIndexFallbacks();
  TestCorruptFooter();
#if !defined(_WIN32)
  TestFdSinkHasNoIndex();
#endif
#if defined(__linux__)
  TestIndexIoErrors();
#endif
  std::puts("test_thyme_bin_index OK");
  return 0;
}
