#include "thyme/check.h"
#include "thyme/record/thyme_bin.h"

#include <cstdio>

using namespace thyme;


int main() {
  const char* path = "/tmp/thyme_bin_gap.bin";
  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = 4;
  std::snprintf(desc.name, sizeof(desc.name), "Demo");

  {
    ThymeBinFileSink file(path);
    CHECK(file.ok());
    ThymeBinEncoder enc(&file);
    CHECK(enc.WriteHeader(1, 100, &desc, 1));
    CHECK(enc.WriteGap(0, 200, 1, 3));
  }

  ThymeBinReader reader(path);
  CHECK(reader.ok());
  ThymeBinDataEntry e{};
  CHECK(reader.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk);
  CHECK(e.type == ThymeBinEntryType::kGap);
  CHECK(e.struct_id == 0);
  CHECK(e.mono_ns == 200);
  CHECK(e.reserved == 1);
  CHECK(e.source_sequence == 3);

  std::remove(path);
  return 0;
}
