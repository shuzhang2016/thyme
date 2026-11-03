#include "self_desc.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <limits>

namespace thyme {
namespace selfdesc {
namespace {

constexpr uint32_t kMaxBytes = 16u * 1024u * 1024u;
constexpr int kMaxDepth = 8;

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

void PutU8(std::string* o, uint8_t v) { o->push_back(static_cast<char>(v)); }

void PutU32Le(std::string* o, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    PutU8(o, static_cast<uint8_t>(v >> (8 * i)));
  }
}

void PutU64Be(std::string* o, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    PutU8(o, static_cast<uint8_t>(v >> (8 * i)));
  }
}

void PutU64Le(std::string* o, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    PutU8(o, static_cast<uint8_t>(v >> (8 * i)));
  }
}

struct Cursor {
  const uint8_t* p;
  std::size_t n;
  std::size_t i;
};

bool Need(const Cursor& c, std::size_t n, std::string* err) {
  if (c.i + n > c.n) {
    *err = "selfdesc: truncated value";
    return false;
  }
  return true;
}

bool ReadU8(Cursor* c, uint8_t* v, std::string* err) {
  if (!Need(*c, 1, err)) return false;
  *v = c->p[c->i++];
  return true;
}

bool ReadU32Le(Cursor* c, uint32_t* v, std::string* err) {
  if (!Need(*c, 4, err)) return false;
  uint32_t x = 0;
  for (int i = 0; i < 4; ++i) {
    x |= static_cast<uint32_t>(c->p[c->i++]) << (8 * i);
  }
  *v = x;
  return true;
}

bool ReadU64Be(Cursor* c, uint64_t* v, std::string* err) {
  if (!Need(*c, 8, err)) return false;
  uint64_t x = 0;
  for (int i = 0; i < 8; ++i) {
    x = (x << 8) | c->p[c->i++];
  }
  *v = x;
  return true;
}

bool ReadU64Le(Cursor* c, uint64_t* v, std::string* err) {
  if (!Need(*c, 8, err)) return false;
  uint64_t x = 0;
  for (int i = 0; i < 8; ++i) {
    x |= static_cast<uint64_t>(c->p[c->i++]) << (8 * i);
  }
  *v = x;
  return true;
}

double BitsToDouble(uint64_t bits) {
  double d = 0;
  static_assert(sizeof(d) == 8, "float64");
  std::memcpy(&d, &bits, 8);
  return d;
}

uint64_t DoubleToBits(double d) {
  uint64_t bits = 0;
  std::memcpy(&bits, &d, 8);
  return bits;
}

uint64_t RawLeafBytes(const Value& v) {
  uint64_t n = 0;
  if (v.type == Type::kString || v.type == Type::kBytes) n += v.text.size();
  for (const Value& c : v.items) n += RawLeafBytes(c);
  for (const auto& kv : v.fields) n += RawLeafBytes(kv.second);
  return n;
}

bool EncodeTlv(const Value& v, std::string* o, std::string* err, int depth);
bool EncodeCbor(const Value& v, std::string* o, std::string* err, int depth);
bool EncodeMsgpack(const Value& v, std::string* o, std::string* err, int depth);
bool DecodeTlv(Cursor* c, Value* v, std::string* err, int depth);
bool DecodeCbor(Cursor* c, Value* v, std::string* err, int depth);
bool DecodeMsgpack(Cursor* c, Value* v, std::string* err, int depth);

bool DepthOk(int depth, std::string* err) {
  if (depth > kMaxDepth) {
    *err = "selfdesc: nesting exceeds 8";
    return false;
  }
  return true;
}

std::vector<std::pair<std::string, Value>> Sorted(
    const std::vector<std::pair<std::string, Value>>& fields) {
  auto out = fields;
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  return out;
}

bool EncodeTlv(const Value& v, std::string* o, std::string* err, int depth) {
  if (!DepthOk(depth, err)) return false;
  const std::size_t mark = o->size();
  PutU8(o, static_cast<uint8_t>(v.type));
  PutU32Le(o, 0);
  const std::size_t body = o->size();
  switch (v.type) {
    case Type::kNull:
      break;
    case Type::kBool:
      PutU8(o, v.b ? 1 : 0);
      break;
    case Type::kU64:
      PutU64Le(o, v.u);
      break;
    case Type::kI64:
      PutU64Le(o, static_cast<uint64_t>(v.i));
      break;
    case Type::kF64:
      PutU64Le(o, DoubleToBits(v.f));
      break;
    case Type::kBytes:
    case Type::kString:
      if (v.text.size() > kMaxBytes) {
        *err = "selfdesc: value exceeds 16MiB";
        return false;
      }
      o->append(v.text);
      break;
    case Type::kArray: {
      if (v.items.size() > kMaxBytes) {
        *err = "selfdesc: array too large";
        return false;
      }
      PutU32Le(o, static_cast<uint32_t>(v.items.size()));
      for (const Value& c : v.items) {
        if (!EncodeTlv(c, o, err, depth + 1)) return false;
      }
      break;
    }
    case Type::kMap: {
      const auto fields = Sorted(v.fields);
      PutU32Le(o, static_cast<uint32_t>(fields.size()));
      for (const auto& kv : fields) {
        Value key = Str(kv.first);
        if (!EncodeTlv(key, o, err, depth + 1)) return false;
        if (!EncodeTlv(kv.second, o, err, depth + 1)) return false;
      }
      break;
    }
    default:
      *err = "selfdesc: unknown type tag";
      return false;
  }
  const std::size_t n = o->size() - body;
  if (n > kMaxBytes) {
    *err = "selfdesc: value exceeds 16MiB";
    return false;
  }
  const uint32_t len = static_cast<uint32_t>(n);
  for (int i = 0; i < 4; ++i) {
    (*o)[mark + 1 + static_cast<std::size_t>(i)] =
        static_cast<char>(static_cast<uint8_t>(len >> (8 * i)));
  }
  return true;
}

bool CborHead(std::string* o, uint8_t major, uint64_t n) {
  const uint8_t maj = static_cast<uint8_t>(major << 5);
  if (n < 24) {
    PutU8(o, static_cast<uint8_t>(maj | n));
  } else if (n <= 0xFF) {
    PutU8(o, static_cast<uint8_t>(maj | 24));
    PutU8(o, static_cast<uint8_t>(n));
  } else if (n <= 0xFFFF) {
    PutU8(o, static_cast<uint8_t>(maj | 25));
    PutU8(o, static_cast<uint8_t>(n >> 8));
    PutU8(o, static_cast<uint8_t>(n));
  } else if (n <= 0xFFFFFFFFu) {
    PutU8(o, static_cast<uint8_t>(maj | 26));
    PutU8(o, static_cast<uint8_t>(n >> 24));
    PutU8(o, static_cast<uint8_t>(n >> 16));
    PutU8(o, static_cast<uint8_t>(n >> 8));
    PutU8(o, static_cast<uint8_t>(n));
  } else {
    PutU8(o, static_cast<uint8_t>(maj | 27));
    PutU64Be(o, n);
  }
  return true;
}

bool EncodeCbor(const Value& v, std::string* o, std::string* err, int depth) {
  if (!DepthOk(depth, err)) return false;
  switch (v.type) {
    case Type::kNull:
      PutU8(o, 0xF6);
      return true;
    case Type::kBool:
      PutU8(o, v.b ? 0xF5 : 0xF4);
      return true;
    case Type::kU64:
      return CborHead(o, 0, v.u);
    case Type::kI64:
      if (v.i >= 0) return CborHead(o, 0, static_cast<uint64_t>(v.i));
      // n = -1 - i, computed in unsigned so INT64_MIN does not overflow.
      return CborHead(o, 1, ~static_cast<uint64_t>(v.i));
    case Type::kF64:
      PutU8(o, 0xFB);
      PutU64Be(o, DoubleToBits(v.f));
      return true;
    case Type::kBytes:
      if (v.text.size() > kMaxBytes) {
        *err = "selfdesc: value exceeds 16MiB";
        return false;
      }
      CborHead(o, 2, v.text.size());
      o->append(v.text);
      return true;
    case Type::kString:
      if (v.text.size() > kMaxBytes) {
        *err = "selfdesc: value exceeds 16MiB";
        return false;
      }
      CborHead(o, 3, v.text.size());
      o->append(v.text);
      return true;
    case Type::kArray:
      CborHead(o, 4, v.items.size());
      for (const Value& c : v.items) {
        if (!EncodeCbor(c, o, err, depth + 1)) return false;
      }
      return true;
    case Type::kMap: {
      const auto fields = Sorted(v.fields);
      CborHead(o, 5, fields.size());
      for (const auto& kv : fields) {
        Value key = Str(kv.first);
        if (!EncodeCbor(key, o, err, depth + 1)) return false;
        if (!EncodeCbor(kv.second, o, err, depth + 1)) return false;
      }
      return true;
    }
    default:
      *err = "selfdesc: unknown type tag";
      return false;
  }
}

bool MpHead(std::string* o, uint8_t marker, uint64_t n, int width) {
  PutU8(o, marker);
  for (int i = width - 1; i >= 0; --i) {
    PutU8(o, static_cast<uint8_t>(n >> (8 * i)));
  }
  return true;
}

bool EncodeMsgpack(const Value& v, std::string* o, std::string* err, int depth) {
  if (!DepthOk(depth, err)) return false;
  switch (v.type) {
    case Type::kNull:
      PutU8(o, 0xC0);
      return true;
    case Type::kBool:
      PutU8(o, v.b ? 0xC3 : 0xC2);
      return true;
    case Type::kU64:
      if (v.u <= 0x7F) {
        PutU8(o, static_cast<uint8_t>(v.u));
      } else if (v.u <= 0xFF) {
        MpHead(o, 0xCC, v.u, 1);
      } else if (v.u <= 0xFFFF) {
        MpHead(o, 0xCD, v.u, 2);
      } else if (v.u <= 0xFFFFFFFFu) {
        MpHead(o, 0xCE, v.u, 4);
      } else {
        MpHead(o, 0xCF, v.u, 8);
      }
      return true;
    case Type::kI64:
      if (v.i >= 0) {
        Value u = U64(static_cast<uint64_t>(v.i));
        return EncodeMsgpack(u, o, err, depth);
      }
      if (v.i >= -32) {
        PutU8(o, static_cast<uint8_t>(v.i));
      } else if (v.i >= -128) {
        MpHead(o, 0xD0, static_cast<uint64_t>(static_cast<int64_t>(v.i) & 0xFF), 1);
      } else if (v.i >= -32768) {
        MpHead(o, 0xD1, static_cast<uint64_t>(v.i) & 0xFFFF, 2);
      } else if (v.i >= static_cast<int64_t>(-2147483647 - 1)) {
        MpHead(o, 0xD2, static_cast<uint64_t>(v.i) & 0xFFFFFFFFu, 4);
      } else {
        MpHead(o, 0xD3, static_cast<uint64_t>(v.i), 8);
      }
      return true;
    case Type::kF64:
      PutU8(o, 0xCB);
      PutU64Be(o, DoubleToBits(v.f));
      return true;
    case Type::kBytes:
      if (v.text.size() > kMaxBytes) {
        *err = "selfdesc: value exceeds 16MiB";
        return false;
      }
      if (v.text.size() <= 0xFF) {
        MpHead(o, 0xC4, v.text.size(), 1);
      } else if (v.text.size() <= 0xFFFF) {
        MpHead(o, 0xC5, v.text.size(), 2);
      } else {
        MpHead(o, 0xC6, v.text.size(), 4);
      }
      o->append(v.text);
      return true;
    case Type::kString:
      if (v.text.size() > kMaxBytes) {
        *err = "selfdesc: value exceeds 16MiB";
        return false;
      }
      if (v.text.size() < 32) {
        PutU8(o, static_cast<uint8_t>(0xA0 | v.text.size()));
      } else if (v.text.size() <= 0xFF) {
        MpHead(o, 0xD9, v.text.size(), 1);
      } else if (v.text.size() <= 0xFFFF) {
        MpHead(o, 0xDA, v.text.size(), 2);
      } else {
        MpHead(o, 0xDB, v.text.size(), 4);
      }
      o->append(v.text);
      return true;
    case Type::kArray:
      if (v.items.size() < 16) {
        PutU8(o, static_cast<uint8_t>(0x90 | v.items.size()));
      } else if (v.items.size() <= 0xFFFF) {
        MpHead(o, 0xDC, v.items.size(), 2);
      } else {
        MpHead(o, 0xDD, v.items.size(), 4);
      }
      for (const Value& c : v.items) {
        if (!EncodeMsgpack(c, o, err, depth + 1)) return false;
      }
      return true;
    case Type::kMap: {
      const auto fields = Sorted(v.fields);
      if (fields.size() < 16) {
        PutU8(o, static_cast<uint8_t>(0x80 | fields.size()));
      } else if (fields.size() <= 0xFFFF) {
        MpHead(o, 0xDE, fields.size(), 2);
      } else {
        MpHead(o, 0xDF, fields.size(), 4);
      }
      for (const auto& kv : fields) {
        Value key = Str(kv.first);
        if (!EncodeMsgpack(key, o, err, depth + 1)) return false;
        if (!EncodeMsgpack(kv.second, o, err, depth + 1)) return false;
      }
      return true;
    }
    default:
      *err = "selfdesc: unknown type tag";
      return false;
  }
}

bool DecodeTlv(Cursor* c, Value* v, std::string* err, int depth) {
  if (!DepthOk(depth, err)) return false;
  uint8_t tag = 0;
  uint32_t len = 0;
  if (!ReadU8(c, &tag, err) || !ReadU32Le(c, &len, err)) return false;
  if (tag < 1 || tag > 9) {
    *err = "selfdesc: unknown TLV tag";
    return false;
  }
  if (len > kMaxBytes || !Need(*c, len, err)) {
    if (err->empty()) *err = "selfdesc: truncated value";
    return false;
  }
  const std::size_t end = c->i + len;
  v->type = static_cast<Type>(tag);
  switch (v->type) {
    case Type::kNull:
      if (len != 0) {
        *err = "selfdesc: null length";
        return false;
      }
      break;
    case Type::kBool:
      if (len != 1 || (c->p[c->i] > 1)) {
        *err = "selfdesc: bad bool";
        return false;
      }
      v->b = c->p[c->i] == 1;
      c->i += 1;
      break;
    case Type::kU64:
    case Type::kI64:
    case Type::kF64: {
      if (len != 8) {
        *err = "selfdesc: bad scalar length";
        return false;
      }
      uint64_t bits = 0;
      if (!ReadU64Le(c, &bits, err)) return false;
      if (v->type == Type::kU64)
        v->u = bits;
      else if (v->type == Type::kI64)
        v->i = static_cast<int64_t>(bits);
      else
        v->f = BitsToDouble(bits);
      break;
    }
    case Type::kBytes:
    case Type::kString:
      v->text.assign(reinterpret_cast<const char*>(c->p + c->i), len);
      c->i += len;
      break;
    case Type::kArray: {
      uint32_t n = 0;
      if (!ReadU32Le(c, &n, err)) return false;
      v->items.reserve(n);
      for (uint32_t i = 0; i < n; ++i) {
        Value child;
        if (!DecodeTlv(c, &child, err, depth + 1)) return false;
        v->items.push_back(std::move(child));
      }
      break;
    }
    case Type::kMap: {
      uint32_t n = 0;
      if (!ReadU32Le(c, &n, err)) return false;
      for (uint32_t i = 0; i < n; ++i) {
        Value key;
        Value child;
        if (!DecodeTlv(c, &key, err, depth + 1)) return false;
        if (key.type != Type::kString) {
          *err = "selfdesc: map key is not a string";
          return false;
        }
        if (!DecodeTlv(c, &child, err, depth + 1)) return false;
        v->fields.emplace_back(std::move(key.text), std::move(child));
      }
      break;
    }
  }
  if (c->i != end) {
    *err = "selfdesc: TLV length does not match value";
    return false;
  }
  return true;
}

bool CborLen(Cursor* c, uint8_t ai, uint64_t* n, std::string* err) {
  if (ai < 24) {
    *n = ai;
    return true;
  }
  if (ai == 24) {
    uint8_t b = 0;
    if (!ReadU8(c, &b, err)) return false;
    *n = b;
    return true;
  }
  if (ai == 25) {
    if (!Need(*c, 2, err)) return false;
    *n = (static_cast<uint64_t>(c->p[c->i]) << 8) | c->p[c->i + 1];
    c->i += 2;
    return true;
  }
  if (ai == 26) {
    if (!Need(*c, 4, err)) return false;
    *n = 0;
    for (int i = 0; i < 4; ++i) *n = (*n << 8) | c->p[c->i++];
    return true;
  }
  if (ai == 27) return ReadU64Be(c, n, err);
  *err = "selfdesc: unknown CBOR additional info";
  return false;
}

bool DecodeCbor(Cursor* c, Value* v, std::string* err, int depth) {
  if (!DepthOk(depth, err)) return false;
  uint8_t ib = 0;
  if (!ReadU8(c, &ib, err)) return false;
  const uint8_t major = static_cast<uint8_t>(ib >> 5);
  const uint8_t ai = static_cast<uint8_t>(ib & 0x1F);
  if (major == 7) {
    if (ib == 0xF4 || ib == 0xF5) {
      v->type = Type::kBool;
      v->b = ib == 0xF5;
      return true;
    }
    if (ib == 0xF6) {
      v->type = Type::kNull;
      return true;
    }
    if (ib == 0xFB) {
      uint64_t bits = 0;
      if (!ReadU64Be(c, &bits, err)) return false;
      v->type = Type::kF64;
      v->f = BitsToDouble(bits);
      return true;
    }
    *err = "selfdesc: unknown CBOR simple/float tag";
    return false;
  }
  if (major == 6) {
    *err = "selfdesc: unknown CBOR tag";
    return false;
  }
  uint64_t n = 0;
  if (!CborLen(c, ai, &n, err)) return false;
  if (n > kMaxBytes) {
    *err = "selfdesc: value exceeds 16MiB";
    return false;
  }
  if (major == 0) {
    v->type = Type::kU64;
    v->u = n;
    return true;
  }
  if (major == 1) {
    if (n > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      *err = "selfdesc: CBOR negative integer out of range";
      return false;
    }
    v->type = Type::kI64;
    v->i = static_cast<int64_t>(~n);
    return true;
  }
  if (major == 2 || major == 3) {
    if (!Need(*c, static_cast<std::size_t>(n), err)) return false;
    v->type = major == 2 ? Type::kBytes : Type::kString;
    v->text.assign(reinterpret_cast<const char*>(c->p + c->i), static_cast<std::size_t>(n));
    c->i += static_cast<std::size_t>(n);
    return true;
  }
  if (major == 4) {
    v->type = Type::kArray;
    v->items.reserve(static_cast<std::size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
      Value child;
      if (!DecodeCbor(c, &child, err, depth + 1)) return false;
      v->items.push_back(std::move(child));
    }
    return true;
  }
  if (major == 5) {
    v->type = Type::kMap;
    for (uint64_t i = 0; i < n; ++i) {
      Value key;
      Value child;
      if (!DecodeCbor(c, &key, err, depth + 1)) return false;
      if (key.type != Type::kString) {
        *err = "selfdesc: map key is not a string";
        return false;
      }
      if (!DecodeCbor(c, &child, err, depth + 1)) return false;
      v->fields.emplace_back(std::move(key.text), std::move(child));
    }
    return true;
  }
  *err = "selfdesc: unknown CBOR major type";
  return false;
}

bool MpRead(Cursor* c, uint64_t* n, int width, std::string* err) {
  if (!Need(*c, static_cast<std::size_t>(width), err)) return false;
  uint64_t x = 0;
  for (int i = 0; i < width; ++i) x = (x << 8) | c->p[c->i++];
  *n = x;
  return true;
}

bool DecodeMsgpack(Cursor* c, Value* v, std::string* err, int depth) {
  if (!DepthOk(depth, err)) return false;
  uint8_t b = 0;
  if (!ReadU8(c, &b, err)) return false;
  if (b <= 0x7F) {
    v->type = Type::kU64;
    v->u = b;
    return true;
  }
  if (b >= 0xE0) {
    v->type = Type::kI64;
    v->i = static_cast<int8_t>(b);
    return true;
  }
  if ((b & 0xF0) == 0x80) {
    v->type = Type::kMap;
    const uint32_t n = b & 0x0F;
    for (uint32_t i = 0; i < n; ++i) {
      Value key;
      Value child;
      if (!DecodeMsgpack(c, &key, err, depth + 1)) return false;
      if (key.type != Type::kString) {
        *err = "selfdesc: map key is not a string";
        return false;
      }
      if (!DecodeMsgpack(c, &child, err, depth + 1)) return false;
      v->fields.emplace_back(std::move(key.text), std::move(child));
    }
    return true;
  }
  if ((b & 0xF0) == 0x90) {
    v->type = Type::kArray;
    const uint32_t n = b & 0x0F;
    for (uint32_t i = 0; i < n; ++i) {
      Value child;
      if (!DecodeMsgpack(c, &child, err, depth + 1)) return false;
      v->items.push_back(std::move(child));
    }
    return true;
  }
  if ((b & 0xE0) == 0xA0) {
    const uint32_t n = b & 0x1F;
    if (!Need(*c, n, err)) return false;
    v->type = Type::kString;
    v->text.assign(reinterpret_cast<const char*>(c->p + c->i), n);
    c->i += n;
    return true;
  }
  auto payload = [&](Type ty, int width) -> bool {
    uint64_t n = 0;
    if (!MpRead(c, &n, width, err)) return false;
    if (n > kMaxBytes &&
        (ty == Type::kBytes || ty == Type::kString || ty == Type::kArray || ty == Type::kMap)) {
      *err = "selfdesc: value exceeds 16MiB";
      return false;
    }
    if (ty == Type::kU64) {
      v->type = Type::kU64;
      v->u = n;
      return true;
    }
    if (ty == Type::kI64) {
      v->type = Type::kI64;
      if (width == 1)
        v->i = static_cast<int8_t>(n);
      else if (width == 2)
        v->i = static_cast<int16_t>(n);
      else if (width == 4)
        v->i = static_cast<int32_t>(n);
      else
        v->i = static_cast<int64_t>(n);
      return true;
    }
    if (ty == Type::kBytes || ty == Type::kString) {
      if (!Need(*c, static_cast<std::size_t>(n), err)) return false;
      v->type = ty;
      v->text.assign(reinterpret_cast<const char*>(c->p + c->i), static_cast<std::size_t>(n));
      c->i += static_cast<std::size_t>(n);
      return true;
    }
    if (ty == Type::kArray) {
      v->type = Type::kArray;
      for (uint64_t i = 0; i < n; ++i) {
        Value child;
        if (!DecodeMsgpack(c, &child, err, depth + 1)) return false;
        v->items.push_back(std::move(child));
      }
      return true;
    }
    v->type = Type::kMap;
    for (uint64_t i = 0; i < n; ++i) {
      Value key;
      Value child;
      if (!DecodeMsgpack(c, &key, err, depth + 1)) return false;
      if (key.type != Type::kString) {
        *err = "selfdesc: map key is not a string";
        return false;
      }
      if (!DecodeMsgpack(c, &child, err, depth + 1)) return false;
      v->fields.emplace_back(std::move(key.text), std::move(child));
    }
    return true;
  };
  switch (b) {
    case 0xC0:
      v->type = Type::kNull;
      return true;
    case 0xC2:
    case 0xC3:
      v->type = Type::kBool;
      v->b = b == 0xC3;
      return true;
    case 0xC4:
      return payload(Type::kBytes, 1);
    case 0xC5:
      return payload(Type::kBytes, 2);
    case 0xC6:
      return payload(Type::kBytes, 4);
    case 0xCA:
      *err = "selfdesc: unknown MessagePack float32 tag";
      return false;
    case 0xCB: {
      uint64_t bits = 0;
      if (!ReadU64Be(c, &bits, err)) return false;
      v->type = Type::kF64;
      v->f = BitsToDouble(bits);
      return true;
    }
    case 0xCC:
      return payload(Type::kU64, 1);
    case 0xCD:
      return payload(Type::kU64, 2);
    case 0xCE:
      return payload(Type::kU64, 4);
    case 0xCF:
      return payload(Type::kU64, 8);
    case 0xD0:
      return payload(Type::kI64, 1);
    case 0xD1:
      return payload(Type::kI64, 2);
    case 0xD2:
      return payload(Type::kI64, 4);
    case 0xD3:
      return payload(Type::kI64, 8);
    case 0xD9:
      return payload(Type::kString, 1);
    case 0xDA:
      return payload(Type::kString, 2);
    case 0xDB:
      return payload(Type::kString, 4);
    case 0xDC:
      return payload(Type::kArray, 2);
    case 0xDD:
      return payload(Type::kArray, 4);
    case 0xDE:
      return payload(Type::kMap, 2);
    case 0xDF:
      return payload(Type::kMap, 4);
    default:
      *err = "selfdesc: unknown MessagePack tag";
      return false;
  }
}

const schema::RecordDesc* FindByName(const schema::PackageSchema& schema, const std::string& name) {
  for (const schema::RecordDesc& r : schema.records) {
    if (r.name == name) return &r;
  }
  return nullptr;
}

uint64_t LoadLe(const uint8_t* p, uint32_t n) {
  uint64_t v = 0;
  for (uint32_t i = 0; i < n; ++i) {
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return v;
}

int64_t LoadSigned(const uint8_t* p, uint32_t n) {
  uint64_t u = LoadLe(p, n);
  if (n < 8) {
    const uint64_t sign = 1ull << (n * 8 - 1);
    if ((u & sign) != 0) {
      const uint64_t mask = (n == 8) ? ~0ull : ((1ull << (n * 8)) - 1);
      u |= ~mask;
    }
  }
  return static_cast<int64_t>(u);
}

bool InRange(uint32_t off, uint32_t n, uint32_t len) { return n <= len && off <= len - n; }

bool FillFields(const schema::PackageSchema& schema, const std::vector<schema::FieldDesc>& fields,
                uint32_t origin, const uint8_t* payload, uint32_t len, Value* out, std::string* err,
                int depth);

bool OneField(const schema::PackageSchema& schema, const schema::FieldDesc& f, uint32_t origin,
              const uint8_t* payload, uint32_t len, Value* out, std::string* err, int depth) {
  if (!DepthOk(depth, err)) return false;
  if (f.count == 0) {
    *err = "selfdesc: field '" + f.name + "' has count 0";
    return false;
  }
  const uint64_t abs64 = static_cast<uint64_t>(origin) + static_cast<uint64_t>(f.offset);
  if (abs64 > len) {
    *err = "selfdesc: field '" + f.name + "' offset past payload";
    return false;
  }
  const uint32_t abs = static_cast<uint32_t>(abs64);
  if (f.type == "optional") {
    if (f.size != 0 && !InRange(abs, f.size, len)) {
      *err = "selfdesc: optional field '" + f.name + "' out of range";
      return false;
    }
    *out = Null();
    return true;
  }
  auto prim = [&](uint32_t width, Type ty) -> bool {
    if (f.stride < width) {
      *err = "selfdesc: field '" + f.name + "' stride < width";
      return false;
    }
    const uint64_t need = static_cast<uint64_t>(f.count - 1) * f.stride + width;
    if (!InRange(abs, static_cast<uint32_t>(need), len)) {
      *err = "selfdesc: field '" + f.name + "' out of range";
      return false;
    }
    auto one = [&](uint32_t at) {
      Value v;
      const uint8_t* p = payload + at;
      if (ty == Type::kU64) {
        v = U64(LoadLe(p, width));
      } else if (ty == Type::kI64) {
        v = I64(LoadSigned(p, width));
      } else if (ty == Type::kBool) {
        if (p[0] > 1) return Value{};
        v = Bool(p[0] == 1);
        v.u = p[0] > 1 ? 2 : 0;
      } else if (width == 4) {
        uint32_t bits = static_cast<uint32_t>(LoadLe(p, 4));
        float fv = 0;
        std::memcpy(&fv, &bits, 4);
        v = F64(static_cast<double>(fv));
      } else {
        v = F64(BitsToDouble(LoadLe(p, 8)));
      }
      return v;
    };
    if (f.count == 1) {
      Value v = one(abs);
      if (ty == Type::kBool && payload[abs] > 1) {
        *err = "selfdesc: field '" + f.name + "' bool is not 0 or 1";
        return false;
      }
      *out = std::move(v);
      return true;
    }
    std::vector<Value> items;
    items.reserve(f.count);
    for (uint32_t i = 0; i < f.count; ++i) {
      const uint32_t at = abs + i * f.stride;
      if (ty == Type::kBool && payload[at] > 1) {
        *err = "selfdesc: field '" + f.name + "' bool is not 0 or 1";
        return false;
      }
      items.push_back(one(at));
    }
    *out = Array(std::move(items));
    return true;
  };
  if (f.type == "uint8") return prim(1, Type::kU64);
  if (f.type == "uint16") return prim(2, Type::kU64);
  if (f.type == "uint32") return prim(4, Type::kU64);
  if (f.type == "uint64") return prim(8, Type::kU64);
  if (f.type == "int8") return prim(1, Type::kI64);
  if (f.type == "int16") return prim(2, Type::kI64);
  if (f.type == "int32") return prim(4, Type::kI64);
  if (f.type == "int64") return prim(8, Type::kI64);
  if (f.type == "bool") return prim(1, Type::kBool);
  if (f.type == "float32") return prim(4, Type::kF64);
  if (f.type == "float64") return prim(8, Type::kF64);
  if (f.type == "char" || f.type == "string") {
    if (!InRange(abs, f.count, len)) {
      *err = "selfdesc: field '" + f.name + "' string out of range";
      return false;
    }
    std::string s(reinterpret_cast<const char*>(payload + abs), f.count);
    const auto nul = s.find('\0');
    if (nul != std::string::npos) s.resize(nul);
    *out = Str(std::move(s));
    return true;
  }
  const schema::RecordDesc* nested = FindByName(schema, f.type);
  if (nested == nullptr || nested->layout != schema::LayoutState::kKnown) {
    *err = "selfdesc: unknown field type '" + f.type + "' on '" + f.name + "'";
    return false;
  }
  if (f.count == 1) {
    return FillFields(schema, nested->fields, abs, payload, len, out, err, depth + 1);
  }
  std::vector<Value> items;
  items.reserve(f.count);
  for (uint32_t i = 0; i < f.count; ++i) {
    const uint64_t at64 = static_cast<uint64_t>(abs) + static_cast<uint64_t>(i) * f.stride;
    if (at64 > len) {
      *err = "selfdesc: nested field '" + f.name + "' out of range";
      return false;
    }
    Value child;
    if (!FillFields(schema, nested->fields, static_cast<uint32_t>(at64), payload, len, &child, err,
                    depth + 1)) {
      return false;
    }
    items.push_back(std::move(child));
  }
  *out = Array(std::move(items));
  return true;
}

bool FillFields(const schema::PackageSchema& schema, const std::vector<schema::FieldDesc>& fields,
                uint32_t origin, const uint8_t* payload, uint32_t len, Value* out, std::string* err,
                int depth) {
  std::vector<std::pair<std::string, Value>> map;
  map.reserve(fields.size());
  for (const schema::FieldDesc& f : fields) {
    Value child;
    if (!OneField(schema, f, origin, payload, len, &child, err, depth)) {
      return false;
    }
    map.emplace_back(f.name, std::move(child));
  }
  *out = Map(std::move(map));
  return true;
}

}  // namespace

Value Null() { return Value{}; }
Value Bool(bool v) {
  Value o;
  o.type = Type::kBool;
  o.b = v;
  return o;
}
Value U64(uint64_t v) {
  Value o;
  o.type = Type::kU64;
  o.u = v;
  return o;
}
Value I64(int64_t v) {
  Value o;
  o.type = Type::kI64;
  o.i = v;
  return o;
}
Value F64(double v) {
  Value o;
  o.type = Type::kF64;
  o.f = v;
  return o;
}
Value Str(std::string v) {
  Value o;
  o.type = Type::kString;
  o.text = std::move(v);
  return o;
}
Value Bytes(std::string v) {
  Value o;
  o.type = Type::kBytes;
  o.text = std::move(v);
  return o;
}
Value Array(std::vector<Value> v) {
  Value o;
  o.type = Type::kArray;
  o.items = std::move(v);
  return o;
}
Value Map(std::vector<std::pair<std::string, Value>> v) {
  Value o;
  o.type = Type::kMap;
  o.fields = std::move(v);
  return o;
}

bool EncodeValue(Encoding enc, const Value& value, std::string* out, std::string* err) {
  if (enc == Encoding::kCbor) return EncodeCbor(value, out, err, 0);
  if (enc == Encoding::kMsgpack) return EncodeMsgpack(value, out, err, 0);
  if (enc == Encoding::kTlv) return EncodeTlv(value, out, err, 0);
  *err = "selfdesc: unknown encoding";
  return false;
}

bool EncodeDocument(Encoding enc, const std::vector<Value>& records, std::string* out, Stats* stats,
                    std::string* err) {
  if (out == nullptr || err == nullptr) return false;
  const uint64_t t0 = NowNs();
  out->clear();
  out->append(kMagic, 8);
  PutU8(out, kVersion);
  PutU8(out, static_cast<uint8_t>(enc));
  PutU8(out, 0);
  PutU8(out, 0);
  PutU32Le(out, static_cast<uint32_t>(records.size()));
  uint64_t raw = 0;
  for (const Value& rec : records) {
    raw += RawLeafBytes(rec);
    if (!EncodeValue(enc, rec, out, err)) return false;
  }
  if (stats != nullptr) {
    stats->raw_bytes = raw;
    stats->encoded_bytes = out->size();
    stats->encode_ns = NowNs() - t0;
  }
  return true;
}

bool DecodeDocument(const uint8_t* data, std::size_t len, Encoding* enc,
                    std::vector<Value>* records, std::string* err) {
  if (data == nullptr || enc == nullptr || records == nullptr || err == nullptr) {
    return false;
  }
  records->clear();
  if (len < 16 || std::memcmp(data, kMagic, 8) != 0) {
    *err = "selfdesc: bad magic";
    return false;
  }
  if (data[8] != kVersion) {
    *err = "selfdesc: unknown version";
    return false;
  }
  const uint8_t code = data[9];
  if (code < 1 || code > 3) {
    *err = "selfdesc: unknown encoding";
    return false;
  }
  *enc = static_cast<Encoding>(code);
  const uint32_t count = static_cast<uint32_t>(data[12]) | (static_cast<uint32_t>(data[13]) << 8) |
                         (static_cast<uint32_t>(data[14]) << 16) |
                         (static_cast<uint32_t>(data[15]) << 24);
  Cursor c{data, len, 16};
  records->reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Value v;
    bool ok = false;
    if (*enc == Encoding::kCbor)
      ok = DecodeCbor(&c, &v, err, 0);
    else if (*enc == Encoding::kMsgpack)
      ok = DecodeMsgpack(&c, &v, err, 0);
    else
      ok = DecodeTlv(&c, &v, err, 0);
    if (!ok) return false;
    records->push_back(std::move(v));
  }
  if (c.i != c.n) {
    *err = "selfdesc: trailing bytes";
    return false;
  }
  return true;
}

bool ValuesEqual(const Value& a, const Value& b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case Type::kNull:
      return true;
    case Type::kBool:
      return a.b == b.b;
    case Type::kU64:
      return a.u == b.u;
    case Type::kI64:
      return a.i == b.i;
    case Type::kF64:
      return DoubleToBits(a.f) == DoubleToBits(b.f);
    case Type::kBytes:
    case Type::kString:
      return a.text == b.text;
    case Type::kArray:
      if (a.items.size() != b.items.size()) return false;
      for (std::size_t i = 0; i < a.items.size(); ++i) {
        if (!ValuesEqual(a.items[i], b.items[i])) return false;
      }
      return true;
    case Type::kMap: {
      if (a.fields.size() != b.fields.size()) return false;
      const auto as = Sorted(a.fields);
      const auto bs = Sorted(b.fields);
      for (std::size_t i = 0; i < as.size(); ++i) {
        if (as[i].first != bs[i].first) return false;
        if (!ValuesEqual(as[i].second, bs[i].second)) return false;
      }
      return true;
    }
  }
  return false;
}

bool FieldsFromRecord(const schema::PackageSchema& schema, const schema::RecordDesc& rec,
                      const uint8_t* payload, uint32_t len, Value* fields, std::string* err) {
  if (fields == nullptr || err == nullptr) return false;
  if (rec.layout != schema::LayoutState::kKnown) {
    *err = "selfdesc: layout unavailable for '" + rec.name + "'";
    return false;
  }
  if (payload == nullptr && len != 0) {
    *err = "selfdesc: null payload";
    return false;
  }
  return FillFields(schema, rec.fields, rec.payload_offset, payload, len, fields, err, 0);
}

const char* EncodingName(Encoding enc) {
  switch (enc) {
    case Encoding::kCbor:
      return "cbor";
    case Encoding::kMsgpack:
      return "msgpack";
    case Encoding::kTlv:
      return "tlv";
  }
  return "unknown";
}

bool ParseEncoding(const std::string& text, Encoding* out) {
  if (out == nullptr) return false;
  if (text == "cbor") {
    *out = Encoding::kCbor;
    return true;
  }
  if (text == "msgpack") {
    *out = Encoding::kMsgpack;
    return true;
  }
  if (text == "tlv") {
    *out = Encoding::kTlv;
    return true;
  }
  return false;
}

}  // namespace selfdesc
}  // namespace thyme
