#include "thyme/check.h"
#include "thyme/record/thyme_bin.h"

#include <cstdio>
#include <cstring>

using namespace thyme;


namespace {

uint32_t SessionCrc(uint64_t new_session_id, uint16_t reason) {
  uint32_t c = 0xFFFFFFFFu;
  const uint8_t type_byte = static_cast<uint8_t>(ThymeBinEntryType::kSession);
  c = Crc32Update(c, &type_byte, 1);
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&new_session_id),
                  sizeof(new_session_id));
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&reason), sizeof(reason));
  return c ^ 0xFFFFFFFFu;
}

bool WriteSessionEntry(const char* path, uint64_t new_session_id, uint16_t reason) {
  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = 4;
  std::snprintf(desc.name, sizeof(desc.name), "Demo");

  ThymeBinFileSink file(path);
  if (!file.ok()) return false;
  ThymeBinEncoder enc(&file);
  if (!enc.WriteHeader(1, 0, &desc, 1)) return false;

  const uint8_t type_byte = static_cast<uint8_t>(ThymeBinEntryType::kSession);
  uint8_t pad[3] = {};
  uint16_t reserved = 0;
  const uint32_t crc = SessionCrc(new_session_id, reason);
  if (!file.Write(&type_byte, 1)) return false;
  if (!file.Write(pad, 3)) return false;
  if (!file.Write(&new_session_id, sizeof(new_session_id))) return false;
  if (!file.Write(&reason, sizeof(reason))) return false;
  if (!file.Write(&reserved, sizeof(reserved))) return false;
  return file.Write(&crc, sizeof(crc));
}

}  // namespace

int main() {
  const char* path = "/tmp/thyme_bin_session.bin";

  CHECK(WriteSessionEntry(path, 99, 2));

  ThymeBinReader reader(path);
  CHECK(reader.ok());

  ThymeBinDataEntry session{};
  CHECK(reader.ReadNext(session) == thyme::ThymeBinReader::ReadResult::kOk);
  CHECK(session.type == ThymeBinEntryType::kSession);
  CHECK(session.record_sequence == 99);
  CHECK(session.reserved == 2);

  ThymeBinDataEntry eof{};
  CHECK(reader.ReadNext(eof) == thyme::ThymeBinReader::ReadResult::kEof);

  std::remove(path);
  return 0;
}
