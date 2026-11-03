#include "thyme/check.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/runtime/record_mirror.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <unistd.h>
#include <vector>

using namespace thyme;


namespace {

// Above the old 17024-byte ceiling, below the new 512 KB ceiling.
constexpr uint32_t kLargePayload = 240000;

}  // namespace

int main() {
  CHECK(kLargePayload > 17024u);                 // was above the old ceiling
  CHECK(kLargePayload <= kThymeBinMaxPayloadSize);  // fits the new 512 KB ceiling
  CHECK(kLargePayload <= kRecordMirrorMaxPayload);

  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = kLargePayload;
  std::snprintf(desc.name, sizeof(desc.name), "Large");

  auto page = std::make_unique<RecordMirrorPage>();
  CHECK(page->Init(1, &desc, 1));

  std::vector<uint8_t> payload(kLargePayload);
  for (uint32_t i = 0; i < kLargePayload; ++i) {
    payload[i] = static_cast<uint8_t>(i * 7u + 1u);
  }

  // Mirror write via the tap path (as RecordTapMirror does).
  CHECK(page->TryWriteSlot(0, payload.data(), kLargePayload, 1000, 42));

  char path[64];
  std::snprintf(path, sizeof(path), "/tmp/thyme_large_payload_%d.bin",
                static_cast<int>(getpid()));

  {
    ThymeBinFileSink file(path);
    CHECK(file.ok());
    ThymeBinEncoder encoder(&file);
    CHECK(encoder.WriteHeader(1, 0, &desc, 1));

    // Drain the mirror slot the way a recorder does: seqlock-stable ReadSlot,
    // then one DATA row.
    std::vector<uint8_t> buf(kLargePayload);
    uint32_t rsz = 0;
    uint64_t mono = 0;
    uint32_t src = 0;
    CHECK(page->ReadSlot(0, buf.data(), kLargePayload, rsz, mono, src));
    CHECK(rsz == kLargePayload);
    CHECK(mono == 1000);
    CHECK(src == 42);
    CHECK(encoder.WriteData(1, 0, mono, src, buf.data(), rsz));
    CHECK(file.Sync());
  }

  ThymeBinReader reader(path);
  CHECK(reader.ok());
  ThymeBinDataEntry entry{};
  CHECK(reader.ReadNext(entry) == thyme::ThymeBinReader::ReadResult::kOk);
  CHECK(entry.type == ThymeBinEntryType::kData);
  CHECK(entry.struct_id == 0);
  CHECK(entry.payload_size == kLargePayload);
  CHECK(entry.source_sequence == 42);
  CHECK(std::memcmp(entry.payload.data(), payload.data(), kLargePayload) == 0);

  std::remove(path);
  std::puts("test_record_large_payload OK");
  return 0;
}
