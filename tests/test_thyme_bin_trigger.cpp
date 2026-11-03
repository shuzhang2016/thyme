// test_thyme_bin_trigger.cpp — TRIGGER entries expose pre_roll_ns /
// post_roll_ns through ThymeBinReader (API-gap fix: the values were on disk
// and crc-verified but discarded before being returned).
#include "thyme/check.h"
#include "thyme/record/thyme_bin.h"

#include <cstdio>
#include <unistd.h>

using namespace thyme;

int main() {
  char path[64];
  std::snprintf(path, sizeof(path), "/tmp/c9tbintrig_%d.tbin",
                static_cast<int>(getpid()));
  std::remove(path);

  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = 8;
  std::snprintf(desc.name, sizeof(desc.name), "Demo");

  {
    ThymeBinFileSink sink(path);
    CHECK(sink.ok());
    ThymeBinEncoder enc(&sink);
    CHECK(enc.WriteHeader(77, 1000, &desc, 1));
    CHECK(enc.WriteTrigger(5, 123456789, 10000000000ULL, 5000000000ULL));
    const uint64_t v = 42;
    CHECK(enc.WriteData(0, 0, 2000, 7, &v, sizeof(v)));
    CHECK(enc.WriteGap(0, 3000, 1, 2));
    CHECK(sink.Sync());
  }

  ThymeBinReader rd(path);
  CHECK(rd.ok());
  CHECK(rd.header().session_id == 77);

  ThymeBinDataEntry e;
  CHECK(rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk);
  CHECK(e.type == ThymeBinEntryType::kTrigger);
  CHECK(e.record_sequence == 5);               // trigger_id
  CHECK(e.mono_ns == 123456789);
  CHECK(e.pre_roll_ns == 10000000000ULL);      // the previously dropped fields
  CHECK(e.post_roll_ns == 5000000000ULL);

  // Non-trigger entries carry zero roll fields.
  CHECK(rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk);
  CHECK(e.type == ThymeBinEntryType::kData);
  CHECK(e.pre_roll_ns == 0 && e.post_roll_ns == 0);

  CHECK(rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk);
  CHECK(e.type == ThymeBinEntryType::kGap);
  CHECK(e.pre_roll_ns == 0 && e.post_roll_ns == 0);

  CHECK(rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kEof);  // clean EOF
  std::remove(path);
  return 0;
}
