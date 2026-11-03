#include "thyme/check.h"
#include "thyme/record/thyme_bin.h"

#include <cstdio>
#include <cstring>

using namespace thyme;


int main() {
  const char* path = "/tmp/thyme_bin_test.bin";
  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = 16;
  std::snprintf(desc.name, sizeof(desc.name), "Demo");

  {
    ThymeBinWriter w(path);
    CHECK(w.ok());
    CHECK(w.WriteHeader(42, 100, &desc, 1));
    uint8_t payload[16];
    for (int i = 0; i < 16; ++i) payload[i] = static_cast<uint8_t>(i + 1);
    CHECK(w.WriteData(1, 0, 200, 9, payload, 16));
  }

  {
    ThymeBinReader r(path);
    CHECK(r.ok());
    CHECK(r.header().session_id == 42);
    CHECK(r.header().start_mono_ns == 100);
    CHECK(r.registry().size() == 1);
    CHECK(std::strcmp(r.registry()[0].name, "Demo") == 0);

    ThymeBinDataEntry e{};
    CHECK(r.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk);
    CHECK(e.type == ThymeBinEntryType::kData);
    CHECK(e.record_sequence == 1);
    CHECK(e.struct_id == 0);
    CHECK(e.mono_ns == 200);
    CHECK(e.source_sequence == 9);
    CHECK(e.payload_size == 16);
    CHECK(e.payload.size() == 16);
    CHECK(e.payload[0] == 1);
    CHECK(e.payload[15] == 16);

    ThymeBinDataEntry eof{};
    CHECK(r.ReadNext(eof) == thyme::ThymeBinReader::ReadResult::kEof);
  }

  std::remove(path);

  // WriteData(nullptr, size>0) must be rejected, not passed through to the sink.
  {
    ThymeBinWriter w2(path);
    CHECK(w2.ok());
    CHECK(w2.WriteHeader(43, 101, &desc, 1));
    CHECK(!w2.WriteData(1, 0, 201, 10, nullptr, 16));
    CHECK(w2.ok());  // rejection must not corrupt the stream
    // nullptr with size 0 is a valid empty payload.
    CHECK(w2.WriteData(2, 0, 202, 11, nullptr, 0));
  }
  {
    ThymeBinReader r2(path);
    CHECK(r2.ok());
    ThymeBinDataEntry e2{};
    CHECK(r2.ReadNext(e2) == thyme::ThymeBinReader::ReadResult::kOk);
    CHECK(e2.record_sequence == 2);
    CHECK(e2.payload_size == 0);
    CHECK(e2.payload.empty());
  }
  std::remove(path);
  return 0;
}
