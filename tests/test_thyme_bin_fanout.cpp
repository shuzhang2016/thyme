#include "thyme/check.h"
#include "thyme/record/thyme_bin.h"

#include <cstdio>
#include <cstring>

using namespace thyme;


class FailSink final : public ThymeBinSink {
 public:
  bool ok() const override { return true; }
  bool Write(const void*, std::size_t) override { return false; }
};

int CountDataEntries(const char* path) {
  ThymeBinReader r(path);
  if (!r.ok()) return -1;
  int n = 0;
  ThymeBinDataEntry e{};
  while (r.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
    if (e.type == ThymeBinEntryType::kData) ++n;
  }
  return n;
}

int main() {
  const char* path = "/tmp/thyme_bin_fanout.bin";
  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = 8;
  std::snprintf(desc.name, sizeof(desc.name), "Demo");

  ThymeBinFileSink file(path);
  CHECK(file.ok());
  FailSink fail;
  ThymeBinFanoutSink fanout;
  fanout.Add(&file);
  fanout.Add(&fail);

  ThymeBinEncoder encoder(&fanout);
  CHECK(encoder.WriteHeader(1, 0, &desc, 1));

  uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(encoder.WriteData(1, 0, 100, 1, payload, 8));
  CHECK(file.Sync());
  CHECK(CountDataEntries(path) == 1);

  // Primary fail + secondary OK → Write fails (backpressure follows primary).
  ThymeBinFileSink file_only("/tmp/thyme_bin_fanout_secondary.bin");
  FailSink fail_primary;
  ThymeBinFanoutSink fanout_primary_fail;
  fanout_primary_fail.Add(&fail_primary);
  fanout_primary_fail.Add(&file_only);
  ThymeBinEncoder enc_fail(&fanout_primary_fail);
  CHECK(!enc_fail.WriteData(2, 0, 101, 2, payload, 8));
  // Primary dead: subsequent writes must still fail (not TCP-only success).
  CHECK(!enc_fail.ok());
  CHECK(!enc_fail.WriteData(3, 0, 102, 3, payload, 8));
  std::remove("/tmp/thyme_bin_fanout_secondary.bin");

  // A secondary failure does not fail the write; the primary file keeps
  // accepting data (entries_written only counts primary successes).
  CHECK(encoder.WriteData(4, 0, 103, 4, payload, 8));
  CHECK(file.Sync());
  CHECK(CountDataEntries(path) == 2);

  std::remove(path);
  return 0;
}
