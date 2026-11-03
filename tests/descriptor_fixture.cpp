// descriptor_fixture.cpp — emit a protobuf FileDescriptorSet for the REP-15
// cross-check.
//
// The C++ side writes the descriptor set with a hand-rolled protobuf encoder.
// test_rep15_protobuf.py parses the same bytes with the official
// google.protobuf library: if the two disagree, the encoder is wrong. Keeping
// the fixture separate means the check needs no package fixture, no MCAP, and
// still exercises the exact function the exporter calls.
#include "canonical.h"

#include <cstdio>
#include <string>

using namespace thyme;

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <out.bin>\n", argv[0]);
    return 2;
  }
  schema::PackageSchema schema;
  schema.version = 1;

  schema::RecordDesc pt;
  pt.name = "Pt";
  pt.layout = schema::LayoutState::kKnown;
  pt.contract_version = 1;
  pt.payload_size = 8;
  pt.fields.push_back(schema::FieldDesc{"x", "uint32", 0, 4, 1, 4});
  pt.fields.push_back(schema::FieldDesc{"y", "uint32", 4, 4, 1, 4});

  schema::RecordDesc rec;
  rec.name = "Outer";
  rec.layout = schema::LayoutState::kKnown;
  rec.contract_version = 1;
  rec.payload_size = 20;
  rec.fields.push_back(schema::FieldDesc{"n", "uint32", 0, 4, 1, 4});
  rec.fields.push_back(schema::FieldDesc{"name", "char", 4, 4, 4, 1});
  rec.fields.push_back(schema::FieldDesc{"xs", "uint16", 8, 4, 2, 2});
  rec.fields.push_back(schema::FieldDesc{"ratio", "float32", 12, 4, 1, 4});
  rec.fields.push_back(schema::FieldDesc{"pt", "Pt", 16, 4, 1, 4});
  rec.fields.push_back(schema::FieldDesc{"opt", "optional", 20, 0, 1, 0});

  schema.records.push_back(pt);
  schema.records.push_back(rec);

  std::string out;
  std::string err;
  if (!canonical::RenderFileDescriptorSet(schema, rec, &out, &err)) {
    std::fprintf(stderr, "render failed: %s\n", err.c_str());
    return 1;
  }
  std::FILE* f = std::fopen(argv[1], "wb");
  if (f == nullptr) return 1;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok ? 0 : 1;
}
