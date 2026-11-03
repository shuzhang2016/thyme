// thyme_py_fixture.cpp — writes the packages used by the ECO-03 Python tests.
//
// The Python reader must be checked against files produced by the real C++
// writer, not hand-assembled bytes. This fixture emits:
//
//   <root>/pkg-v4/small.tbin   version 4 (ABI-11 keyed registry), ABI-10
//                              manifest block, REC-04 index
//   <root>/pkg-v4/schema.json  ABI-11/REC-27 sidecar (schema_version 3) with a known
//                              Pod16 layout and the channel key table
//   <root>/legacy-v3.tbin      version 3, legacy 40-byte registry + manifest
//                              (derived from the v4 file: registry resized,
//                              footer index dropped; see WriteV3FromV4)
//   <root>/legacy-v2.tbin      version 2, footer index, no manifest
//   <root>/stream-v1.tbin      version 1, non-seekable sink, no footer
//
// Values are deterministic and mirrored by tests/test_thyme_py.py.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/record_schema.h"
#include "thyme/record/thyme_bin.h"

namespace {

using namespace thyme;

// A plain FILE sink that deliberately reports itself as non-seekable, which is
// how the encoder produces a version-1 (no footer) stream. ThymeBinFdSink is
// socket-oriented (send(2)), so it cannot write a regular file.
class StreamSink : public ThymeBinSink {
 public:
  explicit StreamSink(const char* path) : fp_(std::fopen(path, "wb")) {}
  ~StreamSink() override {
    if (fp_ != nullptr) std::fclose(fp_);
  }
  bool ok() const override { return fp_ != nullptr; }
  bool Write(const void* data, std::size_t n) override {
    return fp_ != nullptr && std::fwrite(data, 1, n, fp_) == n;
  }
  bool sync() { return fp_ != nullptr && std::fflush(fp_) == 0; }

 private:
  std::FILE* fp_;
};

constexpr uint64_t kBase = 1000000000ull;
constexpr uint64_t kManifestHash = 0x9914c81f74037d59ull;

struct Pod16 {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};
static_assert(sizeof(Pod16) == 16, "fixture contract");

const schema::FieldLayout kPodFields[] = {
    {"a", "uint64", 0, 8, 1, 8},
    {"b", "uint32", 8, 4, 1, 4},
    {"c", "uint32", 12, 4, 1, 4},
};
const schema::ContractLayout kPodLayout = {"Pod16", sizeof(Pod16), 3,
                                           alignof(Pod16), kPodFields};

const schema::ContractLayout* FindContract(const char* name) {
  return std::strcmp(name, "Pod16") == 0 ? &kPodLayout : nullptr;
}

ThymeStructDesc Desc(uint16_t id, const char* name, uint32_t size) {
  ThymeStructDesc d{};
  d.struct_id = id;
  d.contract_version = 1;
  d.payload_size = size;
  std::snprintf(d.name, sizeof(d.name), "%s", name);
  return d;
}

// ABI-11 descriptor: channel/key present (the generated record table mirrors
// one into the other) and a layout hash when a contract layout exists.
ThymeStructDesc KeyedDesc(uint16_t id, const char* name, uint32_t size,
                          const char* channel, bool with_key,
                          uint64_t layout_hash) {
  ThymeStructDesc d = Desc(id, name, size);
  SetThymeStructChannel(&d, channel);
  if (with_key) SetThymeStructRecordKey(&d, channel);
  d.layout_hash = layout_hash;
  return d;
}

uint32_t SessionCrc(uint64_t session_id, uint16_t reason) {
  uint32_t c = 0xFFFFFFFFu;
  const uint8_t type = static_cast<uint8_t>(ThymeBinEntryType::kSession);
  c = Crc32Update(c, &type, 1);
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&session_id),
                  sizeof(session_id));
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&reason), sizeof(reason));
  return c ^ 0xFFFFFFFFu;
}

void WriteSession(ThymeBinSink& sink, uint64_t session_id, uint16_t reason) {
  const uint8_t type = static_cast<uint8_t>(ThymeBinEntryType::kSession);
  const uint8_t pad[3] = {};
  const uint16_t reserved = 0;
  const uint32_t crc = SessionCrc(session_id, reason);
  CHECK(sink.Write(&type, 1));
  CHECK(sink.Write(pad, sizeof(pad)));
  CHECK(sink.Write(&session_id, sizeof(session_id)));
  CHECK(sink.Write(&reason, sizeof(reason)));
  CHECK(sink.Write(&reserved, sizeof(reserved)));
  CHECK(sink.Write(&crc, sizeof(crc)));
}

void WriteDataRow(ThymeBinEncoder& enc, uint64_t sequence, uint16_t struct_id,
                  uint64_t mono_ns) {
  Pod16 pod{};
  pod.a = sequence;
  pod.b = static_cast<uint32_t>(sequence * 2);
  pod.c = static_cast<uint32_t>(sequence * 3);
  CHECK(enc.WriteData(sequence, struct_id, mono_ns,
                      static_cast<uint32_t>(sequence), &pod, sizeof(pod)));
}

void WriteV4(const std::string& dir) {
  std::filesystem::create_directories(dir);
  const uint64_t pod_hash = schema::ComputeLayoutHash(kPodLayout);
  const ThymeStructDesc descs[2] = {
      KeyedDesc(0, "Pod16", sizeof(Pod16), "cam.pod16", true, pod_hash),
      // No explicit record_key: the writer falls back to the channel name.
      KeyedDesc(9, "CamIdx", 8, "cam.idx", false, 0)};
  ThymeBinFileSink sink((dir + "/small.tbin").c_str());
  CHECK(sink.ok());
  ThymeBinEncoder enc(&sink);
  const ThymeBinManifestBlock manifest =
      MakeThymeBinManifestBlock(kManifestHash, 1);
  CHECK(enc.WriteHeader(0x1234, kBase, descs, 2, &manifest));

  for (uint64_t i = 0; i < 5; ++i) {
    WriteDataRow(enc, i, 0, kBase + 100 * (i + 1));
  }
  CHECK(enc.WriteGap(0, kBase + 550, 3, 5));
  CHECK(enc.WriteTrigger(42, kBase + 600, 1000, 2000));
  WriteSession(sink, 0x5678, 2);
  CHECK(enc.Finish());
  CHECK(sink.Sync());

  schema::RecordManifest record_manifest;
  record_manifest.hash = kManifestHash;
  record_manifest.version = schema::kManifestVersion;
  record_manifest.contract = &FindContract;
  std::string err;
  CHECK(schema::WriteSchemaFile(dir + "/schema.json", descs, 2,
                                &record_manifest, false, &err));
  CHECK(err.empty());
}

void WriteV2(const std::string& path) {
  const ThymeStructDesc desc = Desc(0, "Pod16", sizeof(Pod16));
  ThymeBinFileSink sink(path.c_str());
  CHECK(sink.ok());
  ThymeBinEncoder enc(&sink);
  CHECK(enc.WriteHeader(0x2222, kBase, &desc, 1));
  WriteDataRow(enc, 0, 0, kBase + 10);
  WriteDataRow(enc, 1, 0, kBase + 20);
  CHECK(enc.Finish());
  CHECK(sink.Sync());
}

void WriteV1(const std::string& path) {
  StreamSink sink(path.c_str());
  CHECK(sink.ok());
  ThymeBinEncoder enc(&sink);
  const ThymeStructDesc desc = Desc(0, "Pod16", sizeof(Pod16));
  CHECK(enc.WriteHeader(0x1111, kBase, &desc, 1));
  WriteDataRow(enc, 0, 0, kBase + 30);
  WriteDataRow(enc, 1, 0, kBase + 40);
  CHECK(enc.Finish());
  CHECK(sink.sync());
}

// Rewrites the ABI-11 (file version 4) package as the version-3 byte stream a
// pre-ABI-11 writer emitted: same manifest block and records, but the version
// word is 3 and each registry entry is the legacy 40-byte form. The footer
// index is dropped (a v3 stream capture is legal) because it pins the record
// data offset, which moves with the registry size. Only the registry layout
// differs, so this keeps a real v3 compatibility sample without
// hand-assembling record bytes or their CRCs.
void WriteV3FromV4(const std::string& src, const std::string& dst) {
  std::ifstream in(src, std::ios::binary);
  CHECK(in.good());
  const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
  CHECK(bytes.size() > sizeof(ThymeBinFileHeader) + sizeof(ThymeBinIndexFooter));
  ThymeBinFileHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  CHECK(header.file_abi_version == kThymeBinFileAbiVersion);
  CHECK(header.struct_count <= kThymeBinMaxStructCount);
  ThymeBinIndexFooter footer{};
  std::memcpy(&footer, bytes.data() + bytes.size() - sizeof(footer),
              sizeof(footer));
  CHECK(std::memcmp(footer.magic, kThymeBinIndexMagic,
                    sizeof(footer.magic)) == 0);

  std::string out;
  header.file_abi_version = kThymeBinFileAbiVersionManifest;
  out.append(reinterpret_cast<const char*>(&header), sizeof(header));
  const std::size_t registry_start = sizeof(header);
  const std::size_t trailer_start =
      registry_start + header.struct_count * sizeof(ThymeStructDesc);
  CHECK(trailer_start <= footer.entries_offset &&
        footer.entries_offset <= bytes.size());
  for (uint32_t i = 0; i < header.struct_count; ++i) {
    ThymeStructDesc keyed{};
    std::memcpy(&keyed,
                bytes.data() + registry_start + i * sizeof(ThymeStructDesc),
                sizeof(keyed));
    const ThymeBinLegacyStructDesc legacy = ToThymeBinLegacyDesc(keyed);
    out.append(reinterpret_cast<const char*>(&legacy), sizeof(legacy));
  }
  out.append(bytes.data() + trailer_start,
             static_cast<std::size_t>(footer.entries_offset) - trailer_start);

  std::ofstream file(dst, std::ios::binary);
  CHECK(file.good());
  file.write(out.data(), static_cast<std::streamsize>(out.size()));
  CHECK(file.good());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string root = argv[1];
  std::filesystem::create_directories(root);
  WriteV4(root + "/pkg-v4");
  WriteV3FromV4(root + "/pkg-v4/small.tbin", root + "/legacy-v3.tbin");
  WriteV2(root + "/legacy-v2.tbin");
  WriteV1(root + "/stream-v1.tbin");
  return 0;
}
