#pragma once

// ECO-13: offline self-describing export values.
//
// The encoder runs in thyme-replay only. Each record carries its own field
// names, types and lengths, so a decoder does not need schema.json or a
// .tbin registry. Default package export stays raw (--encode=raw).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "thyme/record/record_schema.h"

namespace thyme {
namespace selfdesc {

enum class Encoding : uint8_t { kCbor = 1, kMsgpack = 2, kTlv = 3 };

inline constexpr uint8_t kVersion = 1;
inline constexpr char kMagic[8] = {'T', 'H', 'Y', 'S', 'D', '0', '1', '\0'};

enum class Type : uint8_t {
  kNull = 1,
  kBool = 2,
  kU64 = 3,
  kI64 = 4,
  kF64 = 5,
  kBytes = 6,
  kString = 7,
  kArray = 8,
  kMap = 9,
};

struct Value {
  Type type = Type::kNull;
  bool b = false;
  uint64_t u = 0;
  int64_t i = 0;
  double f = 0;
  std::string text;
  std::vector<Value> items;
  std::vector<std::pair<std::string, Value>> fields;
};

Value Null();
Value Bool(bool v);
Value U64(uint64_t v);
Value I64(int64_t v);
Value F64(double v);
Value Str(std::string v);
Value Bytes(std::string v);
Value Array(std::vector<Value> v);
Value Map(std::vector<std::pair<std::string, Value>> v);

struct Stats {
  uint64_t raw_bytes = 0;      // string/bytes payload, without names
  uint64_t encoded_bytes = 0;  // whole document, including the header
  uint64_t encode_ns = 0;
};

// Header is 16 bytes: magic, version, encoding, reserved, record count.
// Unknown version, encoding or TLV/CBOR/MessagePack tag fails closed.
bool EncodeDocument(Encoding enc, const std::vector<Value>& records, std::string* out, Stats* stats,
                    std::string* err);
bool EncodeValue(Encoding enc, const Value& value, std::string* out, std::string* err);
bool DecodeDocument(const uint8_t* data, std::size_t len, Encoding* enc,
                    std::vector<Value>* records, std::string* err);

bool ValuesEqual(const Value& a, const Value& b);

// Walk a known contract layout. Unknown field types are an error; this
// function does not invent a layout. "optional" encodes as null.
bool FieldsFromRecord(const schema::PackageSchema& schema, const schema::RecordDesc& rec,
                      const uint8_t* payload, uint32_t len, Value* fields, std::string* err);

const char* EncodingName(Encoding enc);
bool ParseEncoding(const std::string& text, Encoding* out);

}  // namespace selfdesc
}  // namespace thyme
