// test_canonical.cpp — REP-15 protobuf/CDR export.
//
// Default raw MCAP/NDJSON stays on the existing path. protobuf and cdr are
// schema-based encodings of a known layout. This test does not launch
// Foxglove or rosbag2; it checks the wire bytes and the MCAP schema encoding
// those tools document.

#include "canonical.h"
#include "mcap_test_parser.h"
#include "replay_export.h"
#include "replay_synthetic_package.h"

#include "thyme/check.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

schema::FieldDesc Field(const char* name, const char* type, uint32_t off,
                        uint32_t size, uint32_t count, uint32_t stride) {
  schema::FieldDesc f;
  f.name = name;
  f.type = type;
  f.offset = off;
  f.size = size;
  f.count = count;
  f.stride = stride;
  return f;
}

const selfdesc::Value* Get(const selfdesc::Value& map, const std::string& key) {
  for (const auto& kv : map.fields) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

schema::PackageSchema SchemaWith(schema::RecordDesc rec) {
  schema::PackageSchema schema;
  schema.version = 1;
  schema.records.push_back(std::move(rec));
  return schema;
}

void TestScalars() {
  schema::RecordDesc rec;
  rec.name = "N";
  rec.layout = schema::LayoutState::kKnown;
  rec.contract_version = 1;
  rec.payload_size = 4;
  rec.fields.push_back(Field("n", "uint32", 0, 4, 1, 4));
  const schema::PackageSchema schema = SchemaWith(rec);
  const uint8_t buf[4] = {42, 0, 0, 0};
  std::string bytes;
  std::string err;
  CHECK(canonical::Encode(canonical::Encoding::kProtobuf, schema, rec, buf, 4,
                          &bytes, &err));
  CHECK(bytes == std::string("\x08\x2a", 2));
  selfdesc::Value fields;
  CHECK(canonical::Decode(canonical::Encoding::kProtobuf, schema, rec,
                          reinterpret_cast<const uint8_t*>(bytes.data()),
                          bytes.size(), &fields, &err));
  const selfdesc::Value* n = Get(fields, "n");
  CHECK(n != nullptr && n->u == 42);

  CHECK(canonical::Encode(canonical::Encoding::kCdr, schema, rec, buf, 4,
                          &bytes, &err));
  CHECK(bytes == std::string("\x00\x01\x00\x00\x2a\x00\x00\x00", 8));
  CHECK(canonical::Decode(canonical::Encoding::kCdr, schema, rec,
                          reinterpret_cast<const uint8_t*>(bytes.data()),
                          bytes.size(), &fields, &err));
  n = Get(fields, "n");
  CHECK(n != nullptr && n->u == 42);

  schema::RecordDesc neg = rec;
  neg.fields[0] = Field("n", "int32", 0, 4, 1, 4);
  const uint8_t minus[4] = {0xff, 0xff, 0xff, 0xff};
  schema::PackageSchema neg_schema = SchemaWith(neg);
  CHECK(canonical::Encode(canonical::Encoding::kProtobuf, neg_schema, neg, minus,
                          4, &bytes, &err));
  const std::string want =
      std::string("\x08\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01", 11);
  CHECK(bytes == want);
}

void TestRoundTrip(canonical::Encoding enc) {
  schema::RecordDesc pt;
  pt.name = "Pt";
  pt.layout = schema::LayoutState::kKnown;
  pt.contract_version = 1;
  pt.fields.push_back(Field("x", "uint32", 0, 4, 1, 4));
  schema::RecordDesc rec;
  rec.name = "Outer";
  rec.layout = schema::LayoutState::kKnown;
  rec.contract_version = 1;
  rec.payload_size = 20;
  rec.fields.push_back(Field("n", "uint32", 0, 4, 1, 4));
  rec.fields.push_back(Field("name", "char", 4, 4, 4, 1));
  rec.fields.push_back(Field("xs", "uint16", 8, 4, 2, 2));
  rec.fields.push_back(Field("ratio", "float32", 12, 4, 1, 4));
  rec.fields.push_back(Field("pt", "Pt", 16, 4, 1, 4));
  rec.fields.push_back(Field("opt", "optional", 20, 0, 1, 0));
  schema::PackageSchema schema;
  schema.version = 1;
  schema.records.push_back(pt);
  schema.records.push_back(rec);
  uint8_t buf[20] = {};
  buf[0] = 42;
  buf[4] = 'a';
  buf[5] = 'b';
  buf[8] = 1;
  buf[10] = 2;
  float ratio = 1.5f;
  std::memcpy(buf + 12, &ratio, 4);
  buf[16] = 7;
  std::string bytes;
  std::string err;
  CHECK(canonical::Encode(enc, schema, rec, buf, sizeof(buf), &bytes, &err));
  selfdesc::Value fields;
  CHECK(canonical::Decode(enc, schema, rec,
                          reinterpret_cast<const uint8_t*>(bytes.data()),
                          bytes.size(), &fields, &err));
  CHECK(Get(fields, "n")->u == 42);
  CHECK(Get(fields, "name")->text == "ab");
  CHECK(Get(fields, "xs")->items.size() == 2);
  CHECK(Get(fields, "xs")->items[0].u == 1);
  CHECK(Get(fields, "xs")->items[1].u == 2);
  CHECK(Get(fields, "ratio")->f > 1.4 && Get(fields, "ratio")->f < 1.6);
  CHECK(Get(*Get(fields, "pt"), "x")->u == 7);
  CHECK(Get(fields, "opt")->type == selfdesc::Type::kNull);

  // A truncated wire image must fail closed. The exact error varies by
  // encoding: protobuf reports "length out of range" when its length prefix
  // overruns, "truncated value" when a varint is cut, and "missing field" when
  // a whole field disappears (protobuf has no total-length header, so a
  // truncated message and a shorter message are the same bytes); CDR reports
  // "truncated value" throughout. All are rejections, which is the contract.
  for (std::size_t cut = 1; cut <= 4 && cut < bytes.size(); ++cut) {
    CHECK(!canonical::Decode(enc, schema, rec,
                             reinterpret_cast<const uint8_t*>(bytes.data()),
                             bytes.size() - cut, &fields, &err));
    CHECK(!err.empty());
  }

  schema.version = 99;
  CHECK(!canonical::Encode(enc, schema, rec, buf, sizeof(buf), &bytes, &err));
  CHECK(err.find("unknown schema version") != std::string::npos);
  schema.version = 1;
  schema::RecordDesc bad = rec;
  bad.fields.push_back(Field("lidar", "lidar_point", 0, 4, 1, 4));
  CHECK(!canonical::Encode(enc, schema, bad, buf, sizeof(buf), &bytes, &err));
  CHECK(err.find("unknown field type") != std::string::npos);
  const std::string proto = canonical::RenderProto(schema, rec);
  CHECK(proto.find("message Outer") != std::string::npos);
  CHECK(proto.find("message Pt") != std::string::npos);
  const std::string msg = canonical::RenderMsg(schema, rec);
  CHECK(msg.find("uint32 n") != std::string::npos);
  CHECK(msg.find("MSG: Pt") != std::string::npos);
}

void RmTree(const std::string& path) {
  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) {
    unlink(path.c_str());
    return;
  }
  while (dirent* ent = readdir(dir)) {
    if (std::strcmp(ent->d_name, ".") == 0 ||
        std::strcmp(ent->d_name, "..") == 0) {
      continue;
    }
    RmTree(path + "/" + ent->d_name);
  }
  closedir(dir);
  rmdir(path.c_str());
}

const schema::ContractLayout* SynthContract(const char* name) {
  static const schema::FieldLayout meta_fields[] = {
      {"marker", "uint64", 0, 8, 1, 8},
  };
  static const schema::ContractLayout meta{"TrigMeta", 8, 1, 8, meta_fields};
  static const schema::FieldLayout pod_fields[] = {
      {"a", "uint64", 0, 8, 1, 8},
      {"b", "uint32", 8, 4, 1, 4},
      {"c", "uint32", 12, 4, 1, 4},
  };
  static const schema::ContractLayout pod{"Pod16", 16, 3, 8, pod_fields};
  if (std::strcmp(name, "TrigMeta") == 0) return &meta;
  if (std::strcmp(name, "Pod16") == 0) return &pod;
  return nullptr;
}

void TestExport() {
  char root_buf[160];
  std::snprintf(root_buf, sizeof(root_buf), "/tmp/thyme_rep15_%ld",
                static_cast<long>(getpid()));
  const std::string root = root_buf;
  RmTree(root);
  CHECK(mkdir(root.c_str(), 0700) == 0);
  replay_synth::Spec spec;
  spec.small_rows = 2;
  spec.frame_rows = 0;
  spec.schema = true;
  const std::string bare = root + "/bare";
  std::string err;
  CHECK(replay_synth::WritePackage(bare, spec, &err));
  replaycli::ExportOptions opt;
  opt.package_dir = bare;
  opt.out_path = root + "/bare.mcap";
  opt.encode = replaycli::ExportEncode::kProtobuf;
  replaycli::ExportSummary summary;
  std::ostringstream sink;
  CHECK(replaycli::RunExport(opt, sink, &summary) != 0);
  CHECK(summary.error.find("use --encode=raw") != std::string::npos);

  const std::string pkg = root + "/pkg";
  CHECK(replay_synth::WritePackage(pkg, spec, &err));
  const ThymeStructDesc descs[4] = {
      replay_synth::Desc(63, "TrigMeta", 8),
      replay_synth::Desc(replay_synth::kSmallStructId, "Pod16",
                         sizeof(replay_synth::Pod16)),
      replay_synth::Desc(triggered::kFrameIndexStructId, "FrameIdx",
                         sizeof(triggered::FrameIndexRecord)),
      replay_synth::Desc(triggered::kFrameGapStructId, "FrameGap", 0),
  };
  schema::RecordManifest manifest;
  manifest.version = 0;
  manifest.contract = SynthContract;
  CHECK(schema::WriteSchemaFile(pkg + "/schema.json", descs, 4, &manifest,
                                false, &err));
  opt.package_dir = pkg;
  opt.out_path = root + "/pkg.mcap";
  CHECK(replaycli::RunExport(opt, sink, &summary) == 0);
  const mcap_test::Parsed parsed =
      mcap_test::Parse(opt.out_path.c_str());
  bool saw_pod = false;
  for (const mcap_test::Schema& sc : parsed.data_schemas) {
    if (sc.name.find("Pod16") == std::string::npos) continue;
    CHECK(sc.encoding == "protobuf");
    const std::string text(sc.data.begin(), sc.data.end());
    CHECK(text.find("Pod16") != std::string::npos);
    saw_pod = true;
  }
  CHECK(saw_pod);
  std::ifstream proto(opt.out_path + ".proto");
  std::string proto_text((std::istreambuf_iterator<char>(proto)),
                         std::istreambuf_iterator<char>());
  CHECK(proto_text.find("message Pod16") != std::string::npos);
  CHECK(proto_text.find("uint64 a") != std::string::npos);
  RmTree(root);
}

}  // namespace

int main() {
  TestScalars();
  TestRoundTrip(canonical::Encoding::kProtobuf);
  TestRoundTrip(canonical::Encoding::kCdr);
  TestExport();
  return 0;
}
