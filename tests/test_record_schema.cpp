// test_record_schema.cpp — ABI-09 package schema sidecar.
//
// The recorder writes <pkg>/schema.json at seal (and over the survivors of an
// invalidated package). It describes the record contracts of the build that
// produced the package so an offline tool can decode the payloads without
// compiling thyme or the generated contract headers.
//
// Covered here (library level):
//   * writer -> reader round trip: version, partial, GEN-12 manifest identity,
//     the 40 B FrameHeader layout, and one record with a field-level layout;
//   * the honesty rule: without a contract table (or when the table disagrees
//     with the descriptor) the record says "layout unavailable" and emits no
//     fields — a wrong layout is never guessed;
//   * absence is a state (kAbsent), not an error;
//   * strict validation: unknown/duplicate keys, unsupported schema_version,
//     truncated files, trailing bytes, field size != count * stride, duplicate
//     struct ids, and a half-declared manifest are all rejected;
//   * FrameHeaderFields() mirrors the real struct (offsetof/sizeof).
#include "thyme/check.h"
#include "thyme/contracts/frame.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/probe/schema_view.h"
#include "thyme/record/record_schema.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace thyme;

namespace {

struct Pod16 {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};
static_assert(sizeof(Pod16) == 16, "fixture arithmetic");

const schema::FieldLayout kPodFields[] = {
    {"a", "uint64", 0, 8, 1, 8},
    {"b", "uint32", 8, 4, 1, 4},
    {"c", "uint32", 12, 4, 1, 4},
};
const schema::ContractLayout kPod = {"Pod16", sizeof(Pod16), 3,
                                     alignof(Pod16), kPodFields};
// A stale table for the same type name: payload_bytes does not describe the
// descriptor's framed payload, so the writer must refuse to use it.
const schema::ContractLayout kStalePod = {"Pod16", 64, 3,
                                          alignof(Pod16), kPodFields};

const schema::ContractLayout* FindPod(const char* cpp_type) {
  return (cpp_type != nullptr && std::strcmp(cpp_type, "Pod16") == 0)
             ? &kPod
             : nullptr;
}
const schema::ContractLayout* FindStalePod(const char* cpp_type) {
  return (cpp_type != nullptr && std::strcmp(cpp_type, "Pod16") == 0)
             ? &kStalePod
             : nullptr;
}

ThymeStructDesc MakeDesc(uint16_t id, uint32_t payload, const char* name) {
  ThymeStructDesc d{};
  d.struct_id = id;
  d.contract_version = 1;
  d.payload_size = payload;
  std::snprintf(d.name, sizeof(d.name), "%s", name);
  return d;
}

std::string Root() {
  const std::filesystem::path p =
      std::filesystem::temp_directory_path() / "thyme_record_schema_test";
  std::filesystem::remove_all(p);
  std::filesystem::create_directories(p);
  return p.string();
}

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::string body((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  return body;
}

void WriteAll(const std::string& path, const std::string& body) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
}

std::string ReplaceOnce(const std::string& body, const std::string& from,
                        const std::string& to) {
  const std::size_t pos = body.find(from);
  CHECK(pos != std::string::npos);
  std::string out = body;
  out.replace(pos, from.size(), to);
  return out;
}

// Writes `body` as the sidecar of a fresh directory and reads it back.
schema::ReadState ReadTampered(const std::string& root,
                               const std::string& body, std::string* err,
                               schema::PackageSchema* out = nullptr) {
  const std::string dir = root + "/tampered";
  std::filesystem::create_directories(dir);
  WriteAll(dir + "/" + schema::kFileName, body);
  schema::PackageSchema local;
  return schema::ReadPackageSchema(dir, out != nullptr ? out : &local, err);
}

const ThymeStructDesc kDescs[] = {
    MakeDesc(0, sizeof(Frame<Pod16>), "Pod16"),
    MakeDesc(60, sizeof(uint64_t) * 3, "FrameIdx"),
};

void TestRoundTrip() {
  const std::string root = Root();
  schema::RecordManifest manifest;
  manifest.hash = 0x9914c81f74037d59ull;
  manifest.version = schema::kManifestVersion;
  manifest.contract = &FindPod;

  std::string err;
  CHECK(schema::WritePackageSchema(root, kDescs, 2, &manifest,
                                   /*partial=*/false, &err));
  CHECK(err.empty());
  CHECK(std::filesystem::exists(root + "/" + schema::kFileName));

  schema::PackageSchema out;
  const auto state = schema::ReadPackageSchema(root, &out, &err);
  CHECK(state == schema::ReadState::kOk);
  CHECK(err.empty());
  CHECK(out.version == schema::kVersion);
  CHECK(!out.partial);
  CHECK(out.manifest_declared);
  CHECK(out.manifest_hash == manifest.hash);
  CHECK(out.manifest_version == schema::kManifestVersion);

  // FrameHeader layout is real: every field's offset is the compiler's.
  CHECK(out.frame_header_bytes == sizeof(FrameHeader));
  std::size_t fh_count = 0;
  const schema::FieldDesc* fh = schema::FrameHeaderFields(&fh_count);
  CHECK(fh_count == 10);
  CHECK(out.frame_header_fields.size() == fh_count);
  CHECK(out.frame_header_fields[0].name == "magic");
  CHECK(out.frame_header_fields[0].offset == offsetof(FrameHeader, magic));
  CHECK(out.frame_header_fields[4].offset == offsetof(FrameHeader, mono_ns));
  CHECK(out.frame_header_fields[8].offset == offsetof(FrameHeader, crc32));
  CHECK(fh[9].offset + fh[9].size == sizeof(FrameHeader));

  // Record 0: framed Pod16 -> field-level layout from the contract table.
  CHECK(out.records.size() == 2);
  const schema::RecordDesc& pod = out.records[0];
  CHECK(pod.struct_id == 0);
  CHECK(pod.name == "Pod16");
  CHECK(pod.contract_version == 1);
  CHECK(pod.payload_size == sizeof(Frame<Pod16>));
  CHECK(pod.layout == schema::LayoutState::kKnown);
  // Frame is not packed: the payload member starts after alignment padding.
  CHECK(pod.payload_offset == 40);
  CHECK(static_cast<uint32_t>(pod.payload_offset + pod.contract_bytes) ==
        sizeof(Frame<Pod16>));
  CHECK(pod.contract_bytes == sizeof(Pod16));
  CHECK(pod.fields.size() == 3);
  CHECK(pod.fields[0].name == "a");
  CHECK(pod.fields[0].offset == offsetof(Pod16, a));
  CHECK(pod.fields[1].offset == offsetof(Pod16, b));
  CHECK(pod.fields[2].offset == offsetof(Pod16, c));
  CHECK(pod.fields[2].count == 1);
  CHECK(pod.fields[2].stride == 4);

  // Record 1: an internal control record (bare, not framed) -> no layout.
  const schema::RecordDesc& ctrl = out.records[1];
  CHECK(ctrl.name == "FrameIdx");
  CHECK(ctrl.layout == schema::LayoutState::kUnavailable);
  CHECK(ctrl.payload_offset == 0);
  CHECK(ctrl.contract_bytes == 0);
  CHECK(ctrl.fields.empty());
  CHECK(out.payload_profiles.empty());

  const std::string text = schema::RenderSchemaText(out);
  CHECK(text.find("manifest : hash=0x9914c81f74037d59 version=1") !=
        std::string::npos);
  CHECK(text.find("name=Pod16") != std::string::npos);
  CHECK(text.find("layout=known") != std::string::npos);
  CHECK(text.find("unavailable (decode raw bytes)") != std::string::npos);
  CHECK(text.find("profiles : 0") != std::string::npos);

  // A producer that records the bare contract (payload_size == sizeof(T))
  // gets payload_offset 0: the schema says where the contract starts instead
  // of assuming a frame header.
  {
    const ThymeStructDesc bare[] = {MakeDesc(0, sizeof(Pod16), "Pod16")};
    CHECK(schema::WritePackageSchema(root, bare, 1, &manifest, false, &err));
    schema::PackageSchema bare_out;
    CHECK(schema::ReadPackageSchema(root, &bare_out, &err) ==
          schema::ReadState::kOk);
    CHECK(bare_out.records[0].layout == schema::LayoutState::kKnown);
    CHECK(bare_out.records[0].payload_offset == 0);
    CHECK(bare_out.records[0].contract_bytes == sizeof(Pod16));
  }

  // Partial marker is a distinct state (REC-18 .incomplete packages).
  CHECK(schema::WritePackageSchema(root, kDescs, 2, &manifest,
                                   /*partial=*/true, &err));
  schema::PackageSchema partial;
  CHECK(schema::ReadPackageSchema(root, &partial, &err) ==
        schema::ReadState::kOk);
  CHECK(partial.partial);
}

void TestHonestDegradation() {
  const std::string root = Root();

  // No manifest at all: identity and layout are declared unavailable.
  std::string err;
  CHECK(schema::WritePackageSchema(root, kDescs, 2, nullptr, false, &err));
  schema::PackageSchema out;
  CHECK(schema::ReadPackageSchema(root, &out, &err) == schema::ReadState::kOk);
  CHECK(!out.manifest_declared);
  CHECK(out.manifest_version == 0);
  CHECK(out.records[0].layout == schema::LayoutState::kUnavailable);
  CHECK(out.records[0].fields.empty());
  CHECK(schema::RenderSchemaText(out).find("manifest : unavailable") !=
        std::string::npos);

  // A table that does not describe the framed payload is not used.
  schema::RecordManifest stale;
  stale.hash = 1;
  stale.contract = &FindStalePod;
  CHECK(schema::WritePackageSchema(root, kDescs, 1, &stale, false, &err));
  schema::PackageSchema stale_out;
  CHECK(schema::ReadPackageSchema(root, &stale_out, &err) ==
        schema::ReadState::kOk);
  CHECK(stale_out.records[0].layout == schema::LayoutState::kUnavailable);

  // A descriptor without a contract version cannot be described: refuse.
  ThymeStructDesc bad = MakeDesc(0, 8, "NoVersion");
  bad.contract_version = 0;
  CHECK(!schema::WritePackageSchema(root, &bad, 1, nullptr, false, &err));
  CHECK(err.find("contract version") != std::string::npos);
}

void TestAbsenceAndTamper() {
  const std::string root = Root();
  std::string err;
  schema::PackageSchema out;

  // Absence is a documented state, not an error.
  CHECK(schema::ReadPackageSchema(root, &out, &err) ==
        schema::ReadState::kAbsent);
  CHECK(err.empty());

  schema::RecordManifest manifest;
  manifest.hash = 0x0123456789abcdefull;
  manifest.contract = &FindPod;
  CHECK(schema::WritePackageSchema(root, kDescs, 2, &manifest, false, &err));
  const std::string valid = ReadAll(root + "/" + schema::kFileName);
  CHECK(valid.find("\"format\": \"thyme-record-schema\"") != std::string::npos);

  struct Case {
    const char* name;
    std::string body;
    const char* needle;
  };
  std::string version_bump = ReplaceOnce(valid, "\"schema_version\": 3",
                                         "\"schema_version\": 4");
  std::string layout_typo = ReplaceOnce(valid, "\"layout\": \"known\"",
                                        "\"layout\": \"guessed\"");
  std::string bad_size = ReplaceOnce(
      valid, "{\"name\":\"a\",\"type\":\"uint64\",\"offset\":0,\"size\":8,"
             "\"count\":1,\"stride\":8}",
      "{\"name\":\"a\",\"type\":\"uint64\",\"offset\":0,\"size\":8,"
      "\"count\":2,\"stride\":8}");
  std::string dup_id = ReplaceOnce(valid, "\"struct_id\": 60",
                                   "\"struct_id\": 0");
  std::string half_manifest =
      ReplaceOnce(valid, "    \"version\": 1,",
                  "    \"version\": null,");
  std::string duplicate_key =
      ReplaceOnce(valid, "\"schema_version\": 3",
                  "\"schema_version\": 3, \"schema_version\": 3");
  std::string unknown_key = ReplaceOnce(valid, "\"partial\": false",
                                        "\"partial\": false, \"extra\": 1");
  std::string trailing = valid + "garbage";
  std::string truncated = valid.substr(0, valid.size() / 2);

  const Case cases[] = {
      {"schema_version", version_bump, "unsupported schema_version"},
      {"layout", layout_typo, "unknown layout state"},
      {"field size", bad_size, "size != count * stride"},
      {"duplicate struct_id", dup_id, "duplicate struct_id"},
      {"half manifest", half_manifest, "both be set or both null"},
      {"duplicate key", duplicate_key, "duplicate schema key"},
      {"unknown key", unknown_key, "unknown schema key"},
      {"trailing bytes", trailing, "trailing bytes"},
      {"truncated", truncated, ""},
  };
  for (const Case& c : cases) {
    std::string case_err;
    schema::PackageSchema parsed;
    CHECK(ReadTampered(root, c.body, &case_err, &parsed) ==
          schema::ReadState::kInvalid);
    CHECK(!case_err.empty());
    if (c.needle[0] != '\0' &&
        case_err.find(c.needle) == std::string::npos) {
      std::fprintf(stderr, "case '%s': expected '%s' in: %s\n", c.name,
                   c.needle, case_err.c_str());
    }
    if (c.needle[0] != '\0') {
      CHECK(case_err.find(c.needle) != std::string::npos);
    }
  }

  // A field that extends past the declared contract is rejected too.
  std::string past_end = ReplaceOnce(valid, "\"offset\":12", "\"offset\":4096");
  std::string case_err;
  CHECK(ReadTampered(root, past_end, &case_err) == schema::ReadState::kInvalid);
  CHECK(case_err.find("extends past the contract") != std::string::npos);

  // A contract with an unknown field type is accepted as a *named* type but a
  // NaN/float in a numeric slot is not: the parser only accepts integers.
  std::string float_offset = ReplaceOnce(valid, "\"offset\":8", "\"offset\":8.5");
  CHECK(ReadTampered(root, float_offset, &case_err) ==
        schema::ReadState::kInvalid);
}

void TestPayloadProfiles() {
  const std::string root = Root();
  std::string err;
  schema::PayloadProfileDesc profiles[2];
  profiles[0].channel = "cloud_1";
  profiles[0].profile = "inno-lidar/pointcloud@v2";
  profiles[0].version = 1;
  profiles[0].params_digest = schema::HashProfileParams("time_field=offset:0");
  profiles[1].channel = "h264_front";
  CHECK(schema::WritePackageSchema(root, kDescs, 2, nullptr, false, &err,
                                   nullptr, profiles, 2));
  schema::PackageSchema out;
  CHECK(schema::ReadPackageSchema(root, &out, &err) == schema::ReadState::kOk);
  CHECK(out.version == 3);
  CHECK(out.payload_profiles.size() == 2);
  CHECK(out.payload_profiles[0].channel == "cloud_1");
  CHECK(out.payload_profiles[0].profile == "inno-lidar/pointcloud@v2");
  CHECK(out.payload_profiles[0].version == 1);
  CHECK(out.payload_profiles[0].params_digest == profiles[0].params_digest);
  CHECK(out.payload_profiles[1].profile.empty());
  CHECK(out.payload_profiles[1].params_digest == 0);
  const std::string valid = ReadAll(root + "/" + schema::kFileName);
  CHECK(valid.find("\"payload_profiles\"") != std::string::npos);
  CHECK(schema::RenderSchemaText(out).find("profile=inno-lidar/pointcloud@v2") !=
        std::string::npos);

  CHECK(schema::HashProfileParams("") == 0);
  CHECK(schema::HashProfileParams("a") != 0);

  std::string v2 = ReplaceOnce(valid, "\"schema_version\": 3",
                               "\"schema_version\": 2");
  CHECK(ReadTampered(root, v2, &err) == schema::ReadState::kInvalid);
  CHECK(err.find("cannot carry payload_profiles") != std::string::npos);

  const std::size_t arr = v2.find("\"payload_profiles\": [");
  CHECK(arr != std::string::npos);
  const std::size_t end = v2.find(']', arr);
  CHECK(end != std::string::npos);
  std::string v2_empty = v2;
  v2_empty.replace(arr, end - arr + 1, "\"payload_profiles\": []");
  schema::PackageSchema v2_out;
  CHECK(ReadTampered(root, v2_empty, &err, &v2_out) == schema::ReadState::kOk);
  CHECK(v2_out.version == 2);
  CHECK(v2_out.payload_profiles.empty());
}

void TestManifestHashHelpers() {
  uint64_t h = 0;
  CHECK(schema::ParseManifestHash("0x9914c81f74037d59", &h));
  CHECK(h == 0x9914c81f74037d59ull);
  CHECK(schema::ParseManifestHash("9914C81F74037D59", &h));
  CHECK(h == 0x9914c81f74037d59ull);
  CHECK(!schema::ParseManifestHash("0x1234", &h));
  CHECK(!schema::ParseManifestHash("0x9914c81f74037d5g", &h));
  CHECK(!schema::ParseManifestHash("", &h));
  CHECK(schema::FormatManifestHash(0x9914c81f74037d59ull) ==
        "0x9914c81f74037d59");
  CHECK(schema::FormatManifestHash(0) == "0x0000000000000000");
  CHECK(schema::ReadStateName(schema::ReadState::kInvalid) ==
        std::string("invalid"));
}

void TestProbeSchemaView() {
  // ABI-09 tool surface: thyme_probe schema reads the sidecar without any
  // generated thyme headers. Absent/invalid are explicit states.
  const std::string root = Root();
  {
    schema::RecordManifest manifest;
    manifest.hash = 0x9914c81f74037d59ull;
    manifest.contract = &FindPod;
    std::string err;
    CHECK(schema::WritePackageSchema(root, kDescs, 2, &manifest, false, &err));
  }
  {
    const probe::SchemaView view = probe::LoadSchemaView(root);
    CHECK(view.state == schema::ReadState::kOk);
    CHECK(view.path == root + "/" + schema::kFileName);
    CHECK(probe::RenderSchemaViewText(view).find("name=Pod16") !=
          std::string::npos);
    const std::string json = probe::RenderSchemaViewJson(view);
    CHECK(json.find("\"command\":\"schema\"") != std::string::npos);
    CHECK(json.find("\"struct_id\":0") != std::string::npos);
    CHECK(json.find("\"payload_offset\":0") != std::string::npos);
  }
  {
    const std::string file = root + "/" + schema::kFileName;
    const probe::SchemaView view = probe::LoadSchemaView(file);
    CHECK(view.state == schema::ReadState::kOk);
  }
  {
    const std::string dir = root + "/absent";
    std::filesystem::create_directories(dir);
    const probe::SchemaView view = probe::LoadSchemaView(dir);
    CHECK(view.state == schema::ReadState::kAbsent);
    CHECK(probe::RenderSchemaViewText(view).find("absent") !=
          std::string::npos);
  }
  {
    const std::string dir = root + "/bad";
    std::filesystem::create_directories(dir);
    WriteAll(dir + "/" + schema::kFileName, "{\"format\":");
    const probe::SchemaView view = probe::LoadSchemaView(dir);
    CHECK(view.state == schema::ReadState::kInvalid);
    CHECK(!view.error.empty());
    CHECK(probe::RenderSchemaViewJson(view).find("\"ok\":false") !=
          std::string::npos);
  }
}

}  // namespace

int main() {
  TestRoundTrip();
  TestHonestDegradation();
  TestAbsenceAndTamper();
  TestPayloadProfiles();
  TestManifestHashHelpers();
  TestProbeSchemaView();
  std::printf("test_record_schema: OK\n");
  return 0;
}
