#include "thyme/check.h"
#include "thyme/record/thyme_bin.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <unistd.h>
#include <cstring>

using namespace thyme;


// Regression coverage for review findings: the reader must reject corrupt or
// hostile .thyme.bin files instead of over-allocating, misparsing, or silently
// accepting them.

static void WriteGoodFile(const char* path) {
  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = 8;
  std::snprintf(desc.name, sizeof(desc.name), "Demo");
  ThymeBinWriter w(path);
  CHECK(w.ok());
  CHECK(w.WriteHeader(7, 1000, &desc, 1));
  uint8_t payload[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  CHECK(w.WriteData(1, 0, 2000, 9, payload, 8));
}

static void Poke(const char* path, long off, const void* data, std::size_t n) {
  FILE* f = std::fopen(path, "r+b");
  CHECK(f != nullptr);
  CHECK(std::fseek(f, off, SEEK_SET) == 0);
  CHECK(std::fwrite(data, 1, n, f) == n);
  std::fclose(f);
}

static bool ReadsFirstEntry(const char* path) {
  ThymeBinReader r(path);
  if (!r.ok()) return false;
  ThymeBinDataEntry e{};
  return r.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk;
}

int main() {
  const char* path = "/tmp/thyme_bin_corrupt.bin";
  // WriteGoodFile() has no manifest -> legacy 40 B registry (file v2).
  const long entry_off = static_cast<long>(sizeof(ThymeBinFileHeader) +
                                           kThymeStructDescLegacySize);
  // kData layout: type(1) + pad(3) + seq(8) + struct_id(2) + reserved(2) +
  // mono_ns(8) + source_sequence(4) + payload_size(4) + payload(8) + crc(4).
  const long payload_size_off = entry_off + 1 + 3 + 8 + 2 + 2 + 8 + 4;
  const long payload_off = payload_size_off + 4;

  // Sanity: a well-formed file parses.
  WriteGoodFile(path);
  CHECK(ReadsFirstEntry(path));

  // a. struct_count bound, observable discrimination: a header advertising
  // MORE descriptors than kThymeBinMaxStructCount (with that many descriptors
  // actually present) must be rejected, while exactly the maximum is accepted.
  // Writing the full descriptor table is required: otherwise ReadExact would
  // also fail by running out of file and the bound check would not be the
  // distinguishing factor.
  {
    FILE* f = std::fopen(path, "wb");
    CHECK(f != nullptr);
    ThymeBinFileHeader h{};
    std::memcpy(h.magic, kThymeBinMagic, 4);
    h.file_abi_version = kThymeBinFileAbiVersionIndexed;
    h.struct_count = kThymeBinMaxStructCount;  // exactly at the bound: accepted
    std::fwrite(&h, sizeof(h), 1, f);
    ThymeBinLegacyStructDesc desc{};
    desc.contract_version = 1;
    desc.payload_size = 8;
    for (uint32_t i = 0; i < h.struct_count; ++i) {
      desc.struct_id = static_cast<uint16_t>(i);
      std::snprintf(desc.name, sizeof(desc.name), "S%u", i);
      std::fwrite(&desc, sizeof(desc), 1, f);
    }
    std::fclose(f);
    ThymeBinReader r(path);
    CHECK(r.ok());
  }
  {
    FILE* f = std::fopen(path, "wb");
    CHECK(f != nullptr);
    ThymeBinFileHeader h{};
    std::memcpy(h.magic, kThymeBinMagic, 4);
    h.file_abi_version = kThymeBinFileAbiVersionIndexed;
    h.struct_count = kThymeBinMaxStructCount + 1;  // over the bound: rejected
    std::fwrite(&h, sizeof(h), 1, f);
    ThymeBinLegacyStructDesc desc{};
    desc.contract_version = 1;
    desc.payload_size = 8;
    for (uint32_t i = 0; i < h.struct_count; ++i) {
      desc.struct_id = static_cast<uint16_t>(i);
      std::snprintf(desc.name, sizeof(desc.name), "S%u", i);
      std::fwrite(&desc, sizeof(desc), 1, f);
    }
    std::fclose(f);
    ThymeBinReader r(path);
    CHECK(!r.ok());
  }

  // b. payload_size beyond kThymeBinMaxPayloadSize: ReadNext must fail. Note:
  // a black-box file test cannot observe the bound check alone — with the
  // bound removed, ReadExact would also fail because the file is shorter than
  // the (hostile) declared size, so this case guards "hostile size is rejected
  // without crash/hang" rather than the bound itself. The bound additionally
  // prevents the ~payload_size allocation from ever happening.
  WriteGoodFile(path);
  {
    const uint32_t bad = kThymeBinMaxPayloadSize + 1;
    Poke(path, payload_size_off, &bad, sizeof(bad));
  }
  CHECK(!ReadsFirstEntry(path));

  // c. Unknown entry type byte: must be rejected, not fall through into the
  // kData path and skip CRC validation.
  WriteGoodFile(path);
  {
    const uint8_t bad_type = 0x77;
    Poke(path, entry_off, &bad_type, sizeof(bad_type));
  }
  CHECK(!ReadsFirstEntry(path));

  // d. Flipped payload byte: the kData CRC must catch it.
  WriteGoodFile(path);
  {
    const uint8_t flip = 0xFF;
    Poke(path, payload_off, &flip, sizeof(flip));
  }
  CHECK(!ReadsFirstEntry(path));

  // R5: the corrupt read is three-state with offset and expected/actual CRC.
  {
    ThymeBinReader r(path);
    CHECK(r.ok());
    ThymeBinDataEntry e{};
    CHECK(r.ReadNext(e) == ThymeBinReader::ReadResult::kCorrupt);
    CHECK(r.corrupt_offset() == static_cast<uint64_t>(entry_off));
    CHECK(r.corrupt_crc_actual() != 0);
    CHECK(r.corrupt_crc_expected() != r.corrupt_crc_actual());
    // After a CRC-corrupt record the reader is past it; next read is clean EOF.
    CHECK(r.ReadNext(e) == ThymeBinReader::ReadResult::kEof);
  }

  // e. Truncated tail (cut mid-record): corrupt, not EOF-folded.
  WriteGoodFile(path);
  {
    FILE* f = std::fopen(path, "r+b");
    CHECK(f != nullptr);
    CHECK(std::fseek(f, payload_off + 2, SEEK_SET) == 0);  // cut inside payload
    CHECK(ftruncate(fileno(f), payload_off + 2) == 0);
    std::fclose(f);
  }
  {
    ThymeBinReader r(path);
    CHECK(r.ok());
    ThymeBinDataEntry e{};
    CHECK(r.ReadNext(e) == ThymeBinReader::ReadResult::kCorrupt);
    CHECK(r.corrupt_offset() == static_cast<uint64_t>(entry_off));
  }

  std::puts("test_thyme_bin_corrupt OK");
  return 0;
}
