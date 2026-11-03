// test_self_desc.cpp — ECO-13 offline self-describing export.
//
// The encoder lives in thyme-replay, not the record/replay hot path.
// Default export stays raw. A decoder of the self-describing file does not
// need schema.json; layout_hash is an identity, not a decode key.

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "replay_export.h"
#include "replay_synthetic_package.h"
#include "self_desc.h"
#include "thyme/check.h"
#include "thyme/record/record_schema.h"

using namespace thyme;

namespace {

const uint8_t kGoldenTlv[] = {
    'T', 'H', 'Y', 'S',  'D', '0', '1', 0, 1,   3,    0, 0, 1, 0, 0, 0, 0x09, 23, 0, 0, 0, 1,
    0,   0,   0,   0x07, 1,   0,   0,   0, 'n', 0x03, 8, 0, 0, 0, 1, 0, 0,    0,  0, 0, 0, 0,
};

schema::FieldDesc Field(const char* name, const char* type, uint32_t off, uint32_t size,
                        uint32_t count, uint32_t stride) {
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

selfdesc::Value Sample() {
  std::vector<selfdesc::Value> tags;
  tags.push_back(selfdesc::Str("a"));
  tags.push_back(selfdesc::Str("b"));
  std::vector<std::pair<std::string, selfdesc::Value>> nested;
  nested.emplace_back("inner", selfdesc::Str("yes"));
  nested.emplace_back("missing", selfdesc::Null());
  std::vector<std::pair<std::string, selfdesc::Value>> fields;
  fields.emplace_back("name", selfdesc::Str("wheel"));
  fields.emplace_back("count", selfdesc::U64(2));
  fields.emplace_back("ok", selfdesc::Bool(true));
  fields.emplace_back("delta", selfdesc::I64(-7));
  fields.emplace_back("ratio", selfdesc::F64(1.5));
  fields.emplace_back("blob", selfdesc::Bytes(std::string("\0\xff", 2)));
  fields.emplace_back("tags", selfdesc::Array(std::move(tags)));
  fields.emplace_back("nested", selfdesc::Map(std::move(nested)));
  return selfdesc::Map(std::move(fields));
}

void RoundTrip(selfdesc::Encoding enc) {
  const selfdesc::Value rec = Sample();
  std::string bytes;
  selfdesc::Stats stats;
  std::string err;
  std::vector<selfdesc::Value> records(1, rec);
  CHECK(selfdesc::EncodeDocument(enc, records, &bytes, &stats, &err));
  CHECK(stats.encoded_bytes == bytes.size());
  CHECK(stats.encoded_bytes > stats.raw_bytes);
  CHECK(stats.raw_bytes > 0);
  std::string again;
  CHECK(selfdesc::EncodeDocument(enc, records, &again, nullptr, &err));
  CHECK(bytes == again);
  selfdesc::Encoding got = selfdesc::Encoding::kCbor;
  std::vector<selfdesc::Value> back;
  CHECK(selfdesc::DecodeDocument(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &got,
                                 &back, &err));
  CHECK(got == enc);
  CHECK(back.size() == 1);
  CHECK(selfdesc::ValuesEqual(back[0], rec));
}

void Reject(const uint8_t* p, std::size_t n, const char* needle) {
  selfdesc::Encoding enc = selfdesc::Encoding::kCbor;
  std::vector<selfdesc::Value> recs;
  std::string err;
  CHECK(!selfdesc::DecodeDocument(p, n, &enc, &recs, &err));
  CHECK(err.find(needle) != std::string::npos);
}

void TestCodec() {
  CHECK(std::string(selfdesc::kMagic, 8) == std::string("THYSD01\0", 8));
  RoundTrip(selfdesc::Encoding::kCbor);
  RoundTrip(selfdesc::Encoding::kMsgpack);
  RoundTrip(selfdesc::Encoding::kTlv);

  selfdesc::Encoding enc = selfdesc::Encoding::kTlv;
  std::vector<selfdesc::Value> recs;
  std::string err;
  CHECK(selfdesc::DecodeDocument(kGoldenTlv, sizeof(kGoldenTlv), &enc, &recs, &err));
  CHECK(enc == selfdesc::Encoding::kTlv);
  CHECK(recs.size() == 1);
  const selfdesc::Value* n = Get(recs[0], "n");
  CHECK(n != nullptr && n->type == selfdesc::Type::kU64 && n->u == 1);

  uint8_t bad_ver[sizeof(kGoldenTlv)];
  std::memcpy(bad_ver, kGoldenTlv, sizeof(bad_ver));
  bad_ver[8] = 2;
  Reject(bad_ver, sizeof(bad_ver), "unknown version");

  const uint8_t bad_tag[] = {'T', 'H', 'Y', 'S', 'D', '0',  '1', 0, 1, 3, 0,
                             0,   1,   0,   0,   0,   0xFE, 0,   0, 0, 0};
  Reject(bad_tag, sizeof(bad_tag), "unknown TLV tag");

  const uint8_t bad_cbor[] = {'T', 'H', 'Y', 'S', 'D', '0', '1', 0, 1, 1, 0, 0, 1, 0, 0, 0, 0xC0};
  Reject(bad_cbor, sizeof(bad_cbor), "unknown CBOR tag");

  const uint8_t bad_mp[] = {'T', 'H', 'Y', 'S', 'D', '0', '1', 0, 1, 2, 0, 0, 1, 0, 0, 0, 0xCA};
  Reject(bad_mp, sizeof(bad_mp), "unknown MessagePack float32");

  // Truncation at every boundary: header, count, and inside the value. The
  // decoder must fail closed instead of reading past the buffer, and the
  // error must say "truncated" (not a generic parse failure).
  for (std::size_t cut = 1; cut <= 8; ++cut) {
    Reject(kGoldenTlv, sizeof(kGoldenTlv) - cut, "truncated");
  }
}

void TestFields() {
  schema::PackageSchema schema;
  schema::RecordDesc pt;
  pt.name = "Pt";
  pt.layout = schema::LayoutState::kKnown;
  pt.fields.push_back(Field("x", "uint32", 0, 4, 1, 4));
  schema.records.push_back(pt);

  schema::RecordDesc rec;
  rec.name = "Outer";
  rec.layout = schema::LayoutState::kKnown;
  rec.fields.push_back(Field("n", "uint32", 0, 4, 1, 4));
  rec.fields.push_back(Field("name", "char", 4, 4, 4, 1));
  rec.fields.push_back(Field("xs", "uint16", 8, 4, 2, 2));
  rec.fields.push_back(Field("pt", "Pt", 12, 4, 1, 4));
  rec.fields.push_back(Field("opt", "optional", 16, 0, 1, 0));
  uint8_t buf[16] = {};
  buf[0] = 42;
  buf[4] = 'a';
  buf[5] = 'b';
  buf[8] = 1;
  buf[10] = 2;
  buf[12] = 7;
  selfdesc::Value fields;
  std::string err;
  CHECK(selfdesc::FieldsFromRecord(schema, rec, buf, sizeof(buf), &fields, &err));
  const selfdesc::Value* n = Get(fields, "n");
  const selfdesc::Value* name = Get(fields, "name");
  const selfdesc::Value* xs = Get(fields, "xs");
  const selfdesc::Value* nested = Get(fields, "pt");
  const selfdesc::Value* opt = Get(fields, "opt");
  CHECK(n != nullptr && n->type == selfdesc::Type::kU64 && n->u == 42);
  CHECK(name != nullptr && name->type == selfdesc::Type::kString && name->text == "ab");
  CHECK(xs != nullptr && xs->type == selfdesc::Type::kArray && xs->items.size() == 2 &&
        xs->items[0].u == 1 && xs->items[1].u == 2);
  CHECK(nested != nullptr);
  const selfdesc::Value* x = Get(*nested, "x");
  CHECK(x != nullptr && x->u == 7);
  CHECK(opt != nullptr && opt->type == selfdesc::Type::kNull);

  schema::RecordDesc bad = rec;
  bad.fields.push_back(Field("lidar", "lidar_point", 0, 4, 1, 4));
  CHECK(!selfdesc::FieldsFromRecord(schema, bad, buf, sizeof(buf), &fields, &err));
  CHECK(err.find("unknown field type") != std::string::npos);

  std::string bytes;
  selfdesc::Stats stats;
  CHECK(selfdesc::FieldsFromRecord(schema, rec, buf, sizeof(buf), &fields, &err));
  std::vector<selfdesc::Value> one(1, fields);
  uint64_t cpu = 0;
  for (int i = 0; i < 64 && cpu == 0; ++i) {
    CHECK(selfdesc::EncodeDocument(selfdesc::Encoding::kTlv, one, &bytes, &stats, &err));
    cpu += stats.encode_ns;
  }
  CHECK(cpu > 0);
  CHECK(stats.encoded_bytes > stats.raw_bytes);
}

void RmTree(const std::string& path) {
  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) {
    unlink(path.c_str());
    return;
  }
  while (dirent* ent = readdir(dir)) {
    if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) {
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

void RewriteKnownLayout(const std::string& pkg) {
  const ThymeStructDesc descs[4] = {
      replay_synth::Desc(63, "TrigMeta", 8),
      replay_synth::Desc(replay_synth::kSmallStructId, "Pod16", sizeof(replay_synth::Pod16)),
      replay_synth::Desc(triggered::kFrameIndexStructId, "FrameIdx",
                         sizeof(triggered::FrameIndexRecord)),
      replay_synth::Desc(triggered::kFrameGapStructId, "FrameGap", 0),
  };
  schema::RecordManifest manifest;
  manifest.version = 0;
  manifest.contract = SynthContract;
  std::string err;
  CHECK(schema::WriteSchemaFile(pkg + "/schema.json", descs, 4, &manifest, false, &err));
}

void TestExport() {
  char root_buf[160];
  std::snprintf(root_buf, sizeof(root_buf), "/tmp/thyme_eco13_%ld", static_cast<long>(getpid()));
  const std::string root = root_buf;
  RmTree(root);
  CHECK(mkdir(root.c_str(), 0700) == 0);

  replay_synth::Spec spec;
  spec.small_rows = 2;
  spec.frame_rows = 1;
  spec.schema = true;
  const std::string bare = root + "/bare";
  std::string err;
  CHECK(replay_synth::WritePackage(bare, spec, &err));

  replaycli::ExportOptions opt;
  opt.package_dir = bare;
  opt.out_path = root + "/bare.thysd";
  opt.encode = replaycli::ExportEncode::kCbor;
  replaycli::ExportSummary summary;
  std::ostringstream sink;
  CHECK(replaycli::RunExport(opt, sink, &summary) != 0);
  CHECK(summary.error.find("use --encode=raw") != std::string::npos);
  CHECK(access(opt.out_path.c_str(), F_OK) != 0);

  opt.format = replaycli::ExportFormat::kNdjson;
  opt.format_explicit = true;
  CHECK(replaycli::RunExport(opt, sink, &summary) == replaycli::kExitUsage);
  CHECK(summary.error.find("mutually exclusive") != std::string::npos);

  const std::string pkg = root + "/pkg";
  CHECK(replay_synth::WritePackage(pkg, spec, &err));
  RewriteKnownLayout(pkg);
  opt.package_dir = pkg;
  opt.format_explicit = false;
  opt.format = replaycli::ExportFormat::kMcap;
  opt.encode = replaycli::ExportEncode::kRaw;
  opt.out_path = root + "/pkg.mcap";
  CHECK(replaycli::RunExport(opt, sink, &summary) == 0);
  CHECK(summary.encode == "raw");
  std::ifstream raw(opt.out_path, std::ios::binary);
  char magic = 0;
  raw.read(&magic, 1);
  CHECK(magic != 'T');

  opt.encode = replaycli::ExportEncode::kTlv;
  opt.out_path = root + "/pkg.thysd";
  CHECK(replaycli::RunExport(opt, sink, &summary) == 0);
  CHECK(summary.encode == "tlv");
  CHECK(summary.encoded_bytes > summary.payload_bytes);
  CHECK(summary.encode_ns > 0);
  CHECK(summary.events > 0);

  std::ifstream in(opt.out_path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(bytes.size() == summary.encoded_bytes);
  selfdesc::Encoding enc = selfdesc::Encoding::kCbor;
  std::vector<selfdesc::Value> recs;
  CHECK(selfdesc::DecodeDocument(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &enc,
                                 &recs, &err));
  CHECK(enc == selfdesc::Encoding::kTlv);
  CHECK(recs.size() == summary.events);
  bool saw_pod = false;
  bool saw_frame = false;
  for (const selfdesc::Value& rec : recs) {
    const selfdesc::Value* kind = Get(rec, "kind");
    const selfdesc::Value* channel = Get(rec, "channel");
    CHECK(kind != nullptr && channel != nullptr);
    if (kind->text == "data" && channel->text == "small") {
      const selfdesc::Value* body = Get(rec, "fields");
      CHECK(body != nullptr);
      const selfdesc::Value* a = Get(*body, "a");
      CHECK(a != nullptr && a->type == selfdesc::Type::kU64);
      const selfdesc::Value* name = Get(rec, "struct");
      CHECK(name != nullptr && name->text == "Pod16");
      saw_pod = true;
    }
    if (kind->text == "data" && channel->text == "cam_a") {
      const selfdesc::Value* payload = Get(rec, "payload");
      const selfdesc::Value* profile = Get(rec, "profile");
      CHECK(payload != nullptr && payload->type == selfdesc::Type::kBytes);
      CHECK(payload->text.size() == 512);
      CHECK(profile != nullptr && profile->type == selfdesc::Type::kNull);
      saw_frame = true;
    }
  }
  CHECK(saw_pod);
  CHECK(saw_frame);
  RmTree(root);
}

}  // namespace

int main() {
  TestCodec();
  TestFields();
  TestExport();
  return 0;
}
