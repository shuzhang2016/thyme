#include "canonical.h"

#include <cstring>
#include <limits>
#include <vector>

namespace thyme {
namespace canonical {
namespace {

constexpr uint32_t kMaxBytes = 16u * 1024u * 1024u;
constexpr int kMaxDepth = 8;

void PutU8(std::string* o, uint8_t v) { o->push_back(static_cast<char>(v)); }

void PutVarint(std::string* o, uint64_t v) {
  while (v >= 0x80) {
    PutU8(o, static_cast<uint8_t>((v & 0x7Fu) | 0x80u));
    v >>= 7;
  }
  PutU8(o, static_cast<uint8_t>(v));
}

void PutU32Le(std::string* o, uint32_t v) {
  for (int i = 0; i < 4; ++i) PutU8(o, static_cast<uint8_t>(v >> (8 * i)));
}

void PutU64Le(std::string* o, uint64_t v) {
  for (int i = 0; i < 8; ++i) PutU8(o, static_cast<uint8_t>(v >> (8 * i)));
}

bool DepthOk(int depth, std::string* err) {
  if (depth > kMaxDepth) {
    *err = "canonical: nesting exceeds 8";
    return false;
  }
  return true;
}

bool InRange(uint32_t off, uint32_t n, uint32_t len) {
  return n <= len && off <= len - n;
}

uint64_t LoadLe(const uint8_t* p, uint32_t n) {
  uint64_t v = 0;
  for (uint32_t i = 0; i < n; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

int64_t LoadSigned(const uint8_t* p, uint32_t n) {
  uint64_t u = LoadLe(p, n);
  if (n < 8) {
    const uint64_t sign = 1ull << (n * 8 - 1);
    if ((u & sign) != 0) {
      const uint64_t mask = (1ull << (n * 8)) - 1;
      u |= ~mask;
    }
  }
  return static_cast<int64_t>(u);
}

const schema::RecordDesc* FindByName(const schema::PackageSchema& schema,
                                    const std::string& name) {
  for (const schema::RecordDesc& r : schema.records) {
    if (r.name == name) return &r;
  }
  return nullptr;
}

bool IsInt(const std::string& t) {
  return t == "int8" || t == "int16" || t == "int32" || t == "int64";
}
bool IsUint(const std::string& t) {
  return t == "uint8" || t == "uint16" || t == "uint32" || t == "uint64";
}
bool IsChar(const std::string& t) { return t == "char" || t == "string"; }

uint32_t Width(const std::string& t) {
  if (t == "bool" || t == "uint8" || t == "int8" || t == "char") return 1;
  if (t == "uint16" || t == "int16") return 2;
  if (t == "uint32" || t == "int32" || t == "float32") return 4;
  if (t == "uint64" || t == "int64" || t == "float64") return 8;
  return 0;
}

std::string Ident(std::string name) {
  if (name.empty() || (name[0] >= '0' && name[0] <= '9')) name = "T_" + name;
  for (char& c : name) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_';
    if (!ok) c = '_';
  }
  return name;
}

std::string ProtoScalar(const std::string& t) {
  if (t == "bool") return "bool";
  if (t == "float32") return "float";
  if (t == "float64") return "double";
  if (t == "int64") return "int64";
  if (IsInt(t)) return "int32";
  if (t == "uint64") return "uint64";
  if (IsUint(t)) return "uint32";
  return "";
}

// descriptor.proto Type enum.
int ProtoTypeNum(const std::string& t) {
  if (t == "float64") return 1;
  if (t == "float32") return 2;
  if (t == "int64") return 3;
  if (t == "uint64") return 4;
  if (IsInt(t)) return 5;
  if (t == "bool") return 8;
  if (IsChar(t)) return 9;
  if (IsUint(t)) return 13;
  return 11;
}

void Collect(const schema::PackageSchema& schema, const schema::RecordDesc& rec,
             std::vector<const schema::RecordDesc*>* out) {
  for (const schema::RecordDesc* have : *out) {
    if (have->name == rec.name) return;
  }
  for (const schema::FieldDesc& f : rec.fields) {
    if (f.type == "optional" || IsChar(f.type) || Width(f.type) != 0) continue;
    const schema::RecordDesc* nested = FindByName(schema, f.type);
    if (nested != nullptr) Collect(schema, *nested, out);
  }
  out->push_back(&rec);
}

std::string ProtoFieldLine(const schema::FieldDesc& f, uint32_t number,
                           const schema::PackageSchema& schema) {
  if (f.type == "optional") {
    return "  optional bool " + Ident(f.name) + " = " + std::to_string(number) +
           ";\n";
  }
  if (IsChar(f.type)) {
    return "  string " + Ident(f.name) + " = " + std::to_string(number) + ";\n";
  }
  const std::string scalar = ProtoScalar(f.type);
  if (!scalar.empty()) {
    if (f.count > 1) {
      return "  repeated " + scalar + " " + Ident(f.name) + " = " +
             std::to_string(number) + ";\n";
    }
    return "  " + scalar + " " + Ident(f.name) + " = " +
           std::to_string(number) + ";\n";
  }
  if (FindByName(schema, f.type) == nullptr) return "";
  const std::string prefix = f.count > 1 ? "  repeated " : "  ";
  return prefix + Ident(f.type) + " " + Ident(f.name) + " = " +
         std::to_string(number) + ";\n";
}

std::string MsgType(const schema::FieldDesc& f,
                    const schema::PackageSchema& schema) {
  if (f.type == "optional" || f.type == "bool") return "bool";
  if (IsChar(f.type)) return "string";
  if (f.type == "uint8" || f.type == "int8" || f.type == "uint16" ||
      f.type == "int16" || f.type == "uint32" || f.type == "int32" ||
      f.type == "uint64" || f.type == "int64" || f.type == "float32" ||
      f.type == "float64") {
    return f.type;
  }
  if (FindByName(schema, f.type) != nullptr) return Ident(f.type);
  return "";
}

void AppendProtoString(std::string* o, uint32_t field, const std::string& s) {
  PutVarint(o, (static_cast<uint64_t>(field) << 3) | 2u);
  PutVarint(o, s.size());
  o->append(s);
}

void AppendProtoVarint(std::string* o, uint32_t field, uint64_t v) {
  PutVarint(o, (static_cast<uint64_t>(field) << 3) | 0u);
  PutVarint(o, v);
}

void AppendProtoMsg(std::string* o, uint32_t field, const std::string& body) {
  PutVarint(o, (static_cast<uint64_t>(field) << 3) | 2u);
  PutVarint(o, body.size());
  o->append(body);
}

bool EncodeProto(const schema::PackageSchema& schema,
                 const std::vector<schema::FieldDesc>& fields, uint32_t origin,
                 const uint8_t* payload, uint32_t len, std::string* out,
                 std::string* err, int depth);

bool OneProto(const schema::PackageSchema& schema, const schema::FieldDesc& f,
              uint32_t number, uint32_t origin, const uint8_t* payload,
              uint32_t len, std::string* out, std::string* err, int depth) {
  if (!DepthOk(depth, err)) return false;
  if (f.count == 0) {
    *err = "canonical: field '" + f.name + "' has count 0";
    return false;
  }
  const uint64_t abs64 =
      static_cast<uint64_t>(origin) + static_cast<uint64_t>(f.offset);
  if (abs64 > len) {
    *err = "canonical: field '" + f.name + "' offset past payload";
    return false;
  }
  const uint32_t abs = static_cast<uint32_t>(abs64);
  if (f.type == "optional") {
    return true;
  }
  if (IsChar(f.type)) {
    if (!InRange(abs, f.count, len)) {
      *err = "canonical: field '" + f.name + "' string out of range";
      return false;
    }
    std::string s(reinterpret_cast<const char*>(payload + abs), f.count);
    const auto nul = s.find('\0');
    if (nul != std::string::npos) s.resize(nul);
    AppendProtoString(out, number, s);
    return true;
  }
  const uint32_t width = Width(f.type);
  if (width != 0) {
    if (f.stride < width) {
      *err = "canonical: field '" + f.name + "' stride < width";
      return false;
    }
    const uint64_t need =
        static_cast<uint64_t>(f.count - 1) * f.stride + width;
    if (!InRange(abs, static_cast<uint32_t>(need), len)) {
      *err = "canonical: field '" + f.name + "' out of range";
      return false;
    }
    auto scalar = [&](uint32_t at, std::string* dest) {
      const uint8_t* p = payload + at;
      if (f.type == "bool") {
        if (p[0] > 1) return false;
        AppendProtoVarint(dest, number, p[0]);
        return true;
      }
      if (f.type == "float32") {
        PutVarint(dest, (static_cast<uint64_t>(number) << 3) | 5u);
        PutU32Le(dest, static_cast<uint32_t>(LoadLe(p, 4)));
        return true;
      }
      if (f.type == "float64") {
        PutVarint(dest, (static_cast<uint64_t>(number) << 3) | 1u);
        PutU64Le(dest, LoadLe(p, 8));
        return true;
      }
      if (IsInt(f.type)) {
        AppendProtoVarint(dest, number,
                          static_cast<uint64_t>(LoadSigned(p, width)));
        return true;
      }
      AppendProtoVarint(dest, number, LoadLe(p, width));
      return true;
    };
    if (f.count == 1) {
      if (!scalar(abs, out)) {
        *err = "canonical: field '" + f.name + "' bool is not 0 or 1";
        return false;
      }
      return true;
    }
    std::string packed;
    for (uint32_t i = 0; i < f.count; ++i) {
      std::string one;
      if (!scalar(abs + i * f.stride, &one)) {
        *err = "canonical: field '" + f.name + "' bool is not 0 or 1";
        return false;
      }
      // scalar() wrote a key. Packed form keeps only the value.
      uint32_t used = 0;
      uint64_t key = 0;
      int shift = 0;
      while (used < one.size()) {
        const uint8_t b = static_cast<uint8_t>(one[used++]);
        key |= static_cast<uint64_t>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) break;
        shift += 7;
      }
      packed.append(one, used, std::string::npos);
    }
    AppendProtoMsg(out, number, packed);
    return true;
  }
  const schema::RecordDesc* nested = FindByName(schema, f.type);
  if (nested == nullptr || nested->layout != schema::LayoutState::kKnown) {
    *err = "canonical: unknown field type '" + f.type + "' on '" + f.name + "'";
    return false;
  }
  for (uint32_t i = 0; i < f.count; ++i) {
    const uint64_t at64 =
        static_cast<uint64_t>(abs) + static_cast<uint64_t>(i) * f.stride;
    if (at64 > len) {
      *err = "canonical: nested field '" + f.name + "' out of range";
      return false;
    }
    std::string body;
    if (!EncodeProto(schema, nested->fields, static_cast<uint32_t>(at64),
                     payload, len, &body, err, depth + 1)) {
      return false;
    }
    AppendProtoMsg(out, number, body);
  }
  return true;
}

bool EncodeProto(const schema::PackageSchema& schema,
                 const std::vector<schema::FieldDesc>& fields, uint32_t origin,
                 const uint8_t* payload, uint32_t len, std::string* out,
                 std::string* err, int depth) {
  uint32_t number = 1;
  for (const schema::FieldDesc& f : fields) {
    if (!OneProto(schema, f, number, origin, payload, len, out, err, depth)) {
      return false;
    }
    ++number;
  }
  if (out->size() > kMaxBytes) {
    *err = "canonical: value exceeds 16MiB";
    return false;
  }
  return true;
}

struct Cursor {
  const uint8_t* p = nullptr;
  std::size_t n = 0;
  std::size_t i = 0;
};

bool Need(const Cursor& c, std::size_t n, std::string* err) {
  if (c.i + n > c.n) {
    *err = "canonical: truncated value";
    return false;
  }
  return true;
}

bool ReadVarint(Cursor* c, uint64_t* v, std::string* err) {
  uint64_t out = 0;
  int shift = 0;
  for (int i = 0; i < 10; ++i) {
    if (!Need(*c, 1, err)) return false;
    const uint8_t b = c->p[c->i++];
    out |= static_cast<uint64_t>(b & 0x7Fu) << shift;
    if ((b & 0x80u) == 0) {
      *v = out;
      return true;
    }
    shift += 7;
  }
  *err = "canonical: bad protobuf varint";
  return false;
}

struct Wire {
  uint32_t number = 0;
  uint32_t wt = 0;
  uint64_t varint = 0;
  uint32_t fixed32 = 0;
  uint64_t fixed64 = 0;
  std::string bytes;
};

bool ReadWire(Cursor* c, Wire* w, std::string* err) {
  uint64_t key = 0;
  if (!ReadVarint(c, &key, err)) return false;
  w->number = static_cast<uint32_t>(key >> 3);
  w->wt = static_cast<uint32_t>(key & 7u);
  if (w->number == 0) {
    *err = "canonical: protobuf field number 0";
    return false;
  }
  if (w->wt == 0) return ReadVarint(c, &w->varint, err);
  if (w->wt == 1) {
    if (!Need(*c, 8, err)) return false;
    w->fixed64 = LoadLe(c->p + c->i, 8);
    c->i += 8;
    return true;
  }
  if (w->wt == 5) {
    if (!Need(*c, 4, err)) return false;
    w->fixed32 = static_cast<uint32_t>(LoadLe(c->p + c->i, 4));
    c->i += 4;
    return true;
  }
  if (w->wt == 2) {
    uint64_t n = 0;
    if (!ReadVarint(c, &n, err)) return false;
    if (n > kMaxBytes || !Need(*c, static_cast<std::size_t>(n), err)) {
      *err = "canonical: protobuf length out of range";
      return false;
    }
    w->bytes.assign(reinterpret_cast<const char*>(c->p + c->i),
                    static_cast<std::size_t>(n));
    c->i += static_cast<std::size_t>(n);
    return true;
  }
  *err = "canonical: unknown protobuf wire type";
  return false;
}

bool DecodeProto(const schema::PackageSchema& schema,
                 const std::vector<schema::FieldDesc>& fields, const uint8_t* p,
                 std::size_t n, selfdesc::Value* out, std::string* err,
                 int depth);

bool ProtoScalarValue(const schema::FieldDesc& f, const Wire& w,
                      selfdesc::Value* out, std::string* err) {
  if (f.type == "bool") {
    if (w.wt != 0 || w.varint > 1) {
      *err = "canonical: bad protobuf bool";
      return false;
    }
    *out = selfdesc::Bool(w.varint == 1);
    return true;
  }
  if (f.type == "float32") {
    if (w.wt != 5) {
      *err = "canonical: bad protobuf float";
      return false;
    }
    float fv = 0;
    std::memcpy(&fv, &w.fixed32, 4);
    *out = selfdesc::F64(static_cast<double>(fv));
    return true;
  }
  if (f.type == "float64") {
    if (w.wt != 1) {
      *err = "canonical: bad protobuf double";
      return false;
    }
    double dv = 0;
    std::memcpy(&dv, &w.fixed64, 8);
    *out = selfdesc::F64(dv);
    return true;
  }
  if (IsChar(f.type)) {
    if (w.wt != 2) {
      *err = "canonical: bad protobuf string";
      return false;
    }
    *out = selfdesc::Str(w.bytes);
    return true;
  }
  if (w.wt != 0) {
    *err = "canonical: bad protobuf integer";
    return false;
  }
  if (IsInt(f.type)) {
    if (f.type == "int64") *out = selfdesc::I64(static_cast<int64_t>(w.varint));
    else *out = selfdesc::I64(static_cast<int32_t>(w.varint));
    return true;
  }
  *out = selfdesc::U64(w.varint);
  return true;
}

bool UnpackPacked(const schema::FieldDesc& f, const std::string& bytes,
                  std::vector<selfdesc::Value>* items, std::string* err) {
  Cursor c{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), 0};
  while (c.i < c.n) {
    Wire w;
    w.number = 1;
    if (f.type == "float32") {
      if (!Need(c, 4, err)) return false;
      w.wt = 5;
      w.fixed32 = static_cast<uint32_t>(LoadLe(c.p + c.i, 4));
      c.i += 4;
    } else if (f.type == "float64") {
      if (!Need(c, 8, err)) return false;
      w.wt = 1;
      w.fixed64 = LoadLe(c.p + c.i, 8);
      c.i += 8;
    } else {
      w.wt = 0;
      if (!ReadVarint(&c, &w.varint, err)) return false;
    }
    selfdesc::Value one;
    if (!ProtoScalarValue(f, w, &one, err)) return false;
    items->push_back(std::move(one));
  }
  return true;
}

bool DecodeProto(const schema::PackageSchema& schema,
                 const std::vector<schema::FieldDesc>& fields, const uint8_t* p,
                 std::size_t n, selfdesc::Value* out, std::string* err,
                 int depth) {
  if (!DepthOk(depth, err)) return false;
  std::vector<Wire> wires;
  Cursor c{p, n, 0};
  while (c.i < c.n) {
    Wire w;
    if (!ReadWire(&c, &w, err)) return false;
    wires.push_back(std::move(w));
  }
  std::vector<std::pair<std::string, selfdesc::Value>> map;
  for (uint32_t i = 0; i < fields.size(); ++i) {
    const schema::FieldDesc& f = fields[i];
    const uint32_t number = i + 1;
    std::vector<const Wire*> hits;
    for (const Wire& w : wires) {
      if (w.number == number) hits.push_back(&w);
    }
    for (const Wire& w : wires) {
      if (w.number > fields.size()) {
        *err = "canonical: unknown protobuf field";
        return false;
      }
    }
    if (f.type == "optional") {
      map.emplace_back(f.name, hits.empty() ? selfdesc::Null()
                                            : selfdesc::Bool(false));
      if (!hits.empty() && hits[0]->varint != 0) {
        map.back().second = selfdesc::Bool(true);
      }
      continue;
    }
    if (IsChar(f.type)) {
      if (hits.empty()) {
        *err = "canonical: missing protobuf field '" + f.name + "'";
        return false;
      }
      selfdesc::Value v;
      if (!ProtoScalarValue(f, *hits[0], &v, err)) return false;
      map.emplace_back(f.name, std::move(v));
      continue;
    }
    if (hits.empty() && f.count > 1) {
      map.emplace_back(f.name, selfdesc::Array({}));
      continue;
    }
    if (hits.empty()) {
      *err = "canonical: missing protobuf field '" + f.name + "'";
      return false;
    }
    if (Width(f.type) != 0 && f.count > 1 && hits.size() == 1 &&
        hits[0]->wt == 2) {
      std::vector<selfdesc::Value> items;
      if (!UnpackPacked(f, hits[0]->bytes, &items, err)) return false;
      map.emplace_back(f.name, selfdesc::Array(std::move(items)));
      continue;
    }
    if (f.count == 1 && Width(f.type) != 0) {
      selfdesc::Value v;
      if (!ProtoScalarValue(f, *hits[0], &v, err)) return false;
      map.emplace_back(f.name, std::move(v));
      continue;
    }
    const schema::RecordDesc* nested = FindByName(schema, f.type);
    if (nested == nullptr || nested->layout != schema::LayoutState::kKnown) {
      *err = "canonical: unknown field type '" + f.type + "'";
      return false;
    }
    std::vector<selfdesc::Value> items;
    for (const Wire* w : hits) {
      if (w->wt != 2) {
        *err = "canonical: bad protobuf message";
        return false;
      }
      selfdesc::Value child;
      if (!DecodeProto(schema, nested->fields,
                       reinterpret_cast<const uint8_t*>(w->bytes.data()),
                       w->bytes.size(), &child, err, depth + 1)) {
        return false;
      }
      items.push_back(std::move(child));
    }
    if (f.count == 1) map.emplace_back(f.name, std::move(items[0]));
    else map.emplace_back(f.name, selfdesc::Array(std::move(items)));
  }
  *out = selfdesc::Map(std::move(map));
  return true;
}

uint32_t CdrAlignOf(const schema::PackageSchema& schema,
                    const std::string& type) {
  if (type == "string" || type == "char") return 4;
  const uint32_t w = Width(type);
  if (w != 0) return w;
  if (type == "optional") return 1;
  const schema::RecordDesc* nested = FindByName(schema, type);
  if (nested == nullptr) return 1;
  uint32_t align = 1;
  for (const schema::FieldDesc& f : nested->fields) {
    const uint32_t a = CdrAlignOf(schema, f.type);
    if (a > align) align = a;
  }
  return align;
}

void CdrPad(std::string* o, uint32_t align) {
  while ((o->size() % align) != 0) o->push_back('\0');
}

bool EncodeCdr(const schema::PackageSchema& schema,
               const std::vector<schema::FieldDesc>& fields, uint32_t origin,
               const uint8_t* payload, uint32_t len, std::string* out,
               std::string* err, int depth);

bool OneCdr(const schema::PackageSchema& schema, const schema::FieldDesc& f,
            uint32_t origin, const uint8_t* payload, uint32_t len,
            std::string* out, std::string* err, int depth) {
  if (!DepthOk(depth, err)) return false;
  if (f.count == 0) {
    *err = "canonical: field '" + f.name + "' has count 0";
    return false;
  }
  const uint64_t abs64 =
      static_cast<uint64_t>(origin) + static_cast<uint64_t>(f.offset);
  if (abs64 > len) {
    *err = "canonical: field '" + f.name + "' offset past payload";
    return false;
  }
  const uint32_t abs = static_cast<uint32_t>(abs64);
  if (f.type == "optional") {
    CdrPad(out, 1);
    PutU8(out, 0);
    return true;
  }
  if (IsChar(f.type)) {
    if (!InRange(abs, f.count, len)) {
      *err = "canonical: field '" + f.name + "' string out of range";
      return false;
    }
    std::string s(reinterpret_cast<const char*>(payload + abs), f.count);
    const auto nul = s.find('\0');
    if (nul != std::string::npos) s.resize(nul);
    s.push_back('\0');
    CdrPad(out, 4);
    PutU32Le(out, static_cast<uint32_t>(s.size()));
    out->append(s);
    return true;
  }
  const uint32_t width = Width(f.type);
  if (width != 0) {
    if (f.stride < width) {
      *err = "canonical: field '" + f.name + "' stride < width";
      return false;
    }
    const uint64_t need =
        static_cast<uint64_t>(f.count - 1) * f.stride + width;
    if (!InRange(abs, static_cast<uint32_t>(need), len)) {
      *err = "canonical: field '" + f.name + "' out of range";
      return false;
    }
    for (uint32_t i = 0; i < f.count; ++i) {
      const uint8_t* p = payload + abs + i * f.stride;
      if (f.type == "bool" && p[0] > 1) {
        *err = "canonical: field '" + f.name + "' bool is not 0 or 1";
        return false;
      }
      CdrPad(out, width);
      out->append(reinterpret_cast<const char*>(p), width);
    }
    return true;
  }
  const schema::RecordDesc* nested = FindByName(schema, f.type);
  if (nested == nullptr || nested->layout != schema::LayoutState::kKnown) {
    *err = "canonical: unknown field type '" + f.type + "' on '" + f.name + "'";
    return false;
  }
  for (uint32_t i = 0; i < f.count; ++i) {
    const uint64_t at64 =
        static_cast<uint64_t>(abs) + static_cast<uint64_t>(i) * f.stride;
    if (at64 > len) {
      *err = "canonical: nested field '" + f.name + "' out of range";
      return false;
    }
    if (!EncodeCdr(schema, nested->fields, static_cast<uint32_t>(at64), payload,
                   len, out, err, depth + 1)) {
      return false;
    }
  }
  return true;
}

bool EncodeCdr(const schema::PackageSchema& schema,
               const std::vector<schema::FieldDesc>& fields, uint32_t origin,
               const uint8_t* payload, uint32_t len, std::string* out,
               std::string* err, int depth) {
  uint32_t align = 1;
  for (const schema::FieldDesc& f : fields) {
    const uint32_t a = CdrAlignOf(schema, f.type);
    if (a > align) align = a;
  }
  CdrPad(out, align);
  const std::size_t start = out->size();
  for (const schema::FieldDesc& f : fields) {
    if (!OneCdr(schema, f, origin, payload, len, out, err, depth)) return false;
  }
  while (((out->size() - start) % align) != 0) out->push_back('\0');
  return out->size() <= kMaxBytes;
}

bool Take(Cursor* c, uint32_t n, const uint8_t** p, std::string* err) {
  if (!Need(*c, n, err)) return false;
  *p = c->p + c->i;
  c->i += n;
  return true;
}

bool CdrPadRead(Cursor* c, uint32_t align, std::string* err) {
  while ((c->i % align) != 0) {
    if (!Need(*c, 1, err)) return false;
    ++c->i;
  }
  return true;
}

bool DecodeCdr(const schema::PackageSchema& schema,
               const std::vector<schema::FieldDesc>& fields, Cursor* c,
               selfdesc::Value* out, std::string* err, int depth) {
  if (!DepthOk(depth, err)) return false;
  uint32_t align = 1;
  for (const schema::FieldDesc& f : fields) {
    const uint32_t a = CdrAlignOf(schema, f.type);
    if (a > align) align = a;
  }
  if (!CdrPadRead(c, align, err)) return false;
  const std::size_t start = c->i;
  std::vector<std::pair<std::string, selfdesc::Value>> map;
  for (const schema::FieldDesc& f : fields) {
    if (f.count == 0) {
      *err = "canonical: field '" + f.name + "' has count 0";
      return false;
    }
    if (f.type == "optional") {
      if (!CdrPadRead(c, 1, err)) return false;
      const uint8_t* p = nullptr;
      if (!Take(c, 1, &p, err)) return false;
      map.emplace_back(f.name, p[0] == 0 ? selfdesc::Null()
                                        : selfdesc::Bool(p[0] == 1));
      continue;
    }
    if (IsChar(f.type)) {
      if (!CdrPadRead(c, 4, err)) return false;
      const uint8_t* p = nullptr;
      if (!Take(c, 4, &p, err)) return false;
      const uint32_t n = static_cast<uint32_t>(LoadLe(p, 4));
      if (n > kMaxBytes || !Take(c, n, &p, err)) {
        *err = "canonical: bad CDR string";
        return false;
      }
      std::string s(reinterpret_cast<const char*>(p), n);
      if (!s.empty() && s.back() == '\0') s.pop_back();
      map.emplace_back(f.name, selfdesc::Str(std::move(s)));
      continue;
    }
    const uint32_t width = Width(f.type);
    if (width != 0) {
      std::vector<selfdesc::Value> items;
      for (uint32_t i = 0; i < f.count; ++i) {
        if (!CdrPadRead(c, width, err)) return false;
        const uint8_t* p = nullptr;
        if (!Take(c, width, &p, err)) return false;
        selfdesc::Value v;
        if (f.type == "bool") {
          if (p[0] > 1) {
            *err = "canonical: bad CDR bool";
            return false;
          }
          v = selfdesc::Bool(p[0] == 1);
        } else if (f.type == "float32") {
          float fv = 0;
          std::memcpy(&fv, p, 4);
          v = selfdesc::F64(static_cast<double>(fv));
        } else if (f.type == "float64") {
          double dv = 0;
          std::memcpy(&dv, p, 8);
          v = selfdesc::F64(dv);
        } else if (IsInt(f.type)) {
          v = selfdesc::I64(LoadSigned(p, width));
        } else {
          v = selfdesc::U64(LoadLe(p, width));
        }
        items.push_back(std::move(v));
      }
      if (f.count == 1) map.emplace_back(f.name, std::move(items[0]));
      else map.emplace_back(f.name, selfdesc::Array(std::move(items)));
      continue;
    }
    const schema::RecordDesc* nested = FindByName(schema, f.type);
    if (nested == nullptr || nested->layout != schema::LayoutState::kKnown) {
      *err = "canonical: unknown field type '" + f.type + "'";
      return false;
    }
    std::vector<selfdesc::Value> items;
    for (uint32_t i = 0; i < f.count; ++i) {
      selfdesc::Value child;
      if (!DecodeCdr(schema, nested->fields, c, &child, err, depth + 1)) {
        return false;
      }
      items.push_back(std::move(child));
    }
    if (f.count == 1) map.emplace_back(f.name, std::move(items[0]));
    else map.emplace_back(f.name, selfdesc::Array(std::move(items)));
  }
  const std::size_t body = c->i - start;
  const std::size_t padded = (body + align - 1) / align * align;
  if (!Need(*c, padded - body, err)) return false;
  c->i += padded - body;
  *out = selfdesc::Map(std::move(map));
  return true;
}

bool Prepare(const schema::PackageSchema& schema, const schema::RecordDesc& rec,
             const uint8_t* payload, std::string* err) {
  if (!SupportedSchemaVersion(schema.version)) {
    *err = "canonical: unknown schema version";
    return false;
  }
  if (rec.layout != schema::LayoutState::kKnown || rec.contract_version == 0) {
    *err = "canonical: layout unavailable for '" + rec.name +
           "'; use --encode=raw";
    return false;
  }
  if (payload == nullptr && rec.payload_size != 0) {
    *err = "canonical: null payload";
    return false;
  }
  return true;
}

}  // namespace

bool SupportedSchemaVersion(uint32_t version) {
  return version >= schema::kVersionMin && version <= schema::kVersion;
}

const schema::RecordDesc* FindRecord(const schema::PackageSchema& schema,
                                    uint32_t struct_id) {
  for (const schema::RecordDesc& r : schema.records) {
    if (r.struct_id == struct_id) return &r;
  }
  return nullptr;
}

bool Encode(Encoding enc, const schema::PackageSchema& schema,
            const schema::RecordDesc& rec, const uint8_t* payload, uint32_t len,
            std::string* out, std::string* err) {
  if (!Prepare(schema, rec, payload, err)) return false;
  out->clear();
  const uint32_t origin = rec.payload_offset;
  if (enc == Encoding::kProtobuf) {
    return EncodeProto(schema, rec.fields, origin, payload, len, out, err, 1);
  }
  PutU8(out, 0);
  PutU8(out, 1);
  PutU8(out, 0);
  PutU8(out, 0);
  return EncodeCdr(schema, rec.fields, origin, payload, len, out, err, 1);
}

bool Decode(Encoding enc, const schema::PackageSchema& schema,
            const schema::RecordDesc& rec, const uint8_t* data, std::size_t len,
            selfdesc::Value* fields, std::string* err) {
  if (!Prepare(schema, rec, data, err)) return false;
  if (enc == Encoding::kProtobuf) {
    return DecodeProto(schema, rec.fields, data, len, fields, err, 1);
  }
  if (len < 4 || data[0] != 0 || data[1] != 1 || data[2] != 0 || data[3] != 0) {
    *err = "canonical: unknown CDR encapsulation";
    return false;
  }
  Cursor c{data, len, 4};
  if (!DecodeCdr(schema, rec.fields, &c, fields, err, 1)) return false;
  if (c.i != c.n) {
    *err = "canonical: trailing CDR bytes";
    return false;
  }
  return true;
}

std::string RenderProtoSet(
    const schema::PackageSchema& schema,
    const std::vector<const schema::RecordDesc*>& recs) {
  std::vector<const schema::RecordDesc*> order;
  for (const schema::RecordDesc* r : recs) {
    if (r != nullptr) Collect(schema, *r, &order);
  }
  std::string out = "syntax = \"proto3\";\npackage thyme;\n// thyme-canonical " +
                    std::to_string(kVersion) + "\n";
  for (const schema::RecordDesc* r : order) {
    out += "// layout_hash " + schema::FormatManifestHash(r->layout_hash) + "\n";
    out += "message " + Ident(r->name) + " {\n";
    uint32_t number = 1;
    for (const schema::FieldDesc& f : r->fields) {
      out += ProtoFieldLine(f, number, schema);
      ++number;
    }
    out += "}\n";
  }
  return out;
}

std::string RenderProto(const schema::PackageSchema& schema,
                        const schema::RecordDesc& rec) {
  return RenderProtoSet(schema, {&rec});
}

std::string RenderMsg(const schema::PackageSchema& schema,
                      const schema::RecordDesc& rec) {
  std::vector<const schema::RecordDesc*> order;
  Collect(schema, rec, &order);
  std::string out;
  for (std::size_t n = 0; n < order.size(); ++n) {
    const schema::RecordDesc* r =
        order[order.size() - 1 - n];
    if (n == 0) {
      out += "# thyme-canonical " + std::to_string(kVersion) + "\n";
    } else {
      out += "\n================================================================================\nMSG: " +
             Ident(r->name) + "\n";
    }
    out += "# layout_hash " + schema::FormatManifestHash(r->layout_hash) + "\n";
    for (const schema::FieldDesc& f : r->fields) {
      std::string ty = MsgType(f, schema);
      if (ty.empty()) ty = "uint8";
      if (f.count > 1 && !IsChar(f.type) && f.type != "optional") {
        ty += "[" + std::to_string(f.count) + "]";
      }
      out += ty + " " + Ident(f.name) + "\n";
    }
  }
  return out;
}

bool RenderFileDescriptorSet(const schema::PackageSchema& schema,
                             const schema::RecordDesc& rec, std::string* out,
                             std::string* err) {
  if (!SupportedSchemaVersion(schema.version) ||
      rec.layout != schema::LayoutState::kKnown) {
    *err = "canonical: layout unavailable; use --encode=raw";
    return false;
  }
  std::vector<const schema::RecordDesc*> order;
  Collect(schema, rec, &order);
  std::string messages;
  for (const schema::RecordDesc* r : order) {
    std::string fields;
    uint32_t number = 1;
    for (const schema::FieldDesc& f : r->fields) {
      std::string field;
      AppendProtoString(&field, 1, Ident(f.name));
      AppendProtoVarint(&field, 3, number);
      const bool repeated = f.count > 1 && !IsChar(f.type) && f.type != "optional";
      const bool optional = f.type == "optional";
      AppendProtoVarint(&field, 4, repeated ? 3 : 1);
      if (optional || IsChar(f.type) || Width(f.type) != 0) {
        const int ty = optional ? 8 : ProtoTypeNum(f.type);
        AppendProtoVarint(&field, 5, static_cast<uint64_t>(ty));
      } else {
        AppendProtoVarint(&field, 5, 11);
        AppendProtoString(&field, 6, ".thyme." + Ident(f.type));
      }
      if (optional) AppendProtoVarint(&field, 17, 1);
      AppendProtoMsg(&fields, 2, field);
      ++number;
    }
    std::string message;
    AppendProtoString(&message, 1, Ident(r->name));
    message.append(fields);
    AppendProtoMsg(&messages, 4, message);
  }
  std::string file;
  AppendProtoString(&file, 1, "thyme.proto");
  AppendProtoString(&file, 2, "thyme");
  file.append(messages);
  AppendProtoString(&file, 12, "proto3");
  out->clear();
  AppendProtoMsg(out, 1, file);
  return true;
}

}  // namespace canonical
}  // namespace thyme
