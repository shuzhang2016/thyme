#include "thyme/record/record_schema.h"

#include "thyme/contracts/frame_header.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace thyme {
namespace schema {

uint64_t ComputeLayoutHash(const ContractLayout& layout) {
  return ComputeLayoutHashConstexpr(layout);
}

uint64_t HashProfileParams(const std::string& params) {
  if (params.empty()) return 0;
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : params) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ULL;
  }
  return h == 0 ? 1 : h;
}

namespace {

constexpr std::size_t kFieldDescBytes = 96;

void AppendString(std::string* out, const std::string& s) {
  out->push_back('"');
  for (const char c : s) {
    switch (c) {
      case '"':
        *out += "\\\"";
        break;
      case '\\':
        *out += "\\\\";
        break;
      case '\n':
        *out += "\\n";
        break;
      case '\r':
        *out += "\\r";
        break;
      case '\t':
        *out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x",
                        static_cast<unsigned int>(
                            static_cast<unsigned char>(c)));
          *out += buf;
        } else {
          out->push_back(c);
        }
        break;
    }
  }
  out->push_back('"');
}

void AppendField(std::string* out, const FieldDesc& f, const char* indent) {
  *out += indent;
  *out += "{\"name\":";
  AppendString(out, f.name);
  *out += ",\"type\":";
  AppendString(out, f.type);
  *out += ",\"offset\":" + std::to_string(f.offset);
  *out += ",\"size\":" + std::to_string(f.size);
  *out += ",\"count\":" + std::to_string(f.count);
  *out += ",\"stride\":" + std::to_string(f.stride);
  *out += "}";
}

std::string TextField(const char* data, std::size_t cap) {
  std::size_t n = 0;
  while (n < cap && data[n] != '\0') ++n;
  return std::string(data, n);
}

std::string BuildJson(const ThymeStructDesc* descs, std::size_t count,
                      const RecordManifest* manifest, bool partial,
                      const PayloadProfileDesc* profiles,
                      std::size_t profile_count) {
  const bool declared = manifest != nullptr && manifest->version != 0;
  std::string out;
  out.reserve(2048 + count * (256 + kFieldDescBytes * 4));
  out += "{\n  \"format\": ";
  AppendString(&out, kFormat);
  out += ",\n  \"schema_version\": " + std::to_string(kVersion);
  out += ",\n  \"partial\": ";
  out += partial ? "true" : "false";
  out += ",\n  \"record_manifest\": {\n    \"hash\": ";
  if (declared) {
    AppendString(&out, FormatManifestHash(manifest->hash));
    out += ",\n    \"version\": " + std::to_string(manifest->version);
  } else {
    out += "null,\n    \"version\": null";
  }
  // ABI-11: the sorted per-channel identity table. It is empty on a legacy
  // descriptor set (no generated channel keys); it is never invented.
  out += ",\n    \"channels\": [";
  bool first_channel = true;
  for (std::size_t i = 0; i < count; ++i) {
    const ThymeStructDesc& d = descs[i];
    const std::string channel = TextField(d.channel, sizeof(d.channel));
    if (channel.empty()) continue;
    out += first_channel ? "\n" : ",\n";
    first_channel = false;
    out += "      {\"struct_id\": " + std::to_string(d.struct_id);
    out += ", \"channel\": ";
    AppendString(&out, channel);
    out += ", \"record_key\": ";
    AppendString(&out, TextField(ThymeStructDescKey(d), kThymeStructKeyBytes));
    out += ", \"contract_version\": " +
           std::to_string(d.contract_version);
    out += ", \"payload_size\": " + std::to_string(d.payload_size);
    out += ", \"layout_hash\": ";
    if (d.layout_hash == kLayoutHashNone) {
      out += "null";
    } else {
      AppendString(&out, FormatManifestHash(d.layout_hash));
    }
    out += "}";
  }
  out += first_channel ? "]" : "\n    ]";
  out += "\n  },\n  \"frame_header\": {\n    \"bytes\": " +
         std::to_string(sizeof(FrameHeader));
  out += ",\n    \"fields\": [";
  std::size_t fh_count = 0;
  const FieldDesc* fh = FrameHeaderFields(&fh_count);
  for (std::size_t i = 0; i < fh_count; ++i) {
    out += (i == 0 ? "\n" : ",\n");
    AppendField(&out, fh[i], "      ");
  }
  out += (fh_count == 0 ? "]\n" : "\n    ]\n");
  out += "  },\n  \"records\": [";
  for (std::size_t i = 0; i < count; ++i) {
    const ThymeStructDesc& d = descs[i];
    const std::string name = TextField(d.name, sizeof(d.name));
    const ContractLayout* c = nullptr;
    if (manifest != nullptr && manifest->contract != nullptr) {
      c = manifest->contract(name.c_str());
    }
    // The layout table is trusted only when it describes exactly this
    // descriptor. The registry's payload_size is authoritative and says which
    // convention the producer used:
    //   * framed (generated apps record sizeof(Frame<T>)): Frame is not
    //     packed, so the payload member starts at align_up(sizeof(FrameHeader),
    //     align) and the framed size is that offset + the bare payload,
    //     rounded up to the same alignment;
    //   * bare (a mirror ring configured with the raw contract, as older
    //     fixtures do): the payload IS the contract, offset 0.
    // A mismatch (stale header, opaque envelope) is reported as unavailable,
    // never emitted as a wrong layout.
    uint32_t payload_offset = 0;
    bool known = false;
    if (c != nullptr && c->fields != nullptr && c->field_count != 0 &&
        c->field_count <= kMaxFields && c->align != 0) {
      const uint64_t align = c->align;
      const uint64_t framed_offset =
          (sizeof(FrameHeader) + align - 1) / align * align;
      const uint64_t framed_size =
          (framed_offset + c->payload_bytes + align - 1) / align * align;
      if (framed_size == d.payload_size) {
        known = true;
        payload_offset = static_cast<uint32_t>(framed_offset);
      } else if (c->payload_bytes == d.payload_size) {
        known = true;
        payload_offset = 0;
      }
    }
    out += (i == 0 ? "\n" : ",\n");
    out += "    {\"struct_id\": " + std::to_string(d.struct_id);
    out += ", \"name\": ";
    AppendString(&out, name);
    out += ", \"contract_version\": " + std::to_string(d.contract_version);
    out += ", \"payload_size\": " + std::to_string(d.payload_size);
    out += ", \"channel\": ";
    const std::string channel = TextField(d.channel, sizeof(d.channel));
    if (channel.empty()) {
      out += "null";
    } else {
      AppendString(&out, channel);
    }
    out += ", \"record_key\": ";
    const std::string key =
        TextField(ThymeStructDescKey(d), kThymeStructKeyBytes);
    if (key.empty()) {
      out += "null";
    } else {
      AppendString(&out, key);
    }
    out += ", \"layout_hash\": ";
    if (d.layout_hash == kLayoutHashNone) {
      out += "null";
    } else {
      AppendString(&out, FormatManifestHash(d.layout_hash));
    }
    out += ", \"frame_header_bytes\": " + std::to_string(sizeof(FrameHeader));
    out += ", \"layout\": ";
    out += known ? "\"known\"" : "\"unavailable\"";
    out += ", \"payload_offset\": ";
    out += known ? std::to_string(payload_offset) : "null";
    out += ", \"contract_bytes\": ";
    out += known ? std::to_string(c->payload_bytes) : "null";
    out += ", \"fields\": ";
    if (!known) {
      out += "null";
    } else {
      out += "[";
      for (uint16_t f = 0; f < c->field_count; ++f) {
        const FieldLayout& l = c->fields[f];
        FieldDesc desc;
        desc.name = l.name != nullptr ? l.name : "";
        desc.type = l.type != nullptr ? l.type : "";
        desc.offset = l.offset;
        desc.size = l.size;
        desc.count = l.count;
        desc.stride = l.stride;
        out += (f == 0 ? "\n" : ",\n");
        AppendField(&out, desc, "        ");
      }
      out += "\n      ]";
    }
    out += "}";
  }
  out += (count == 0 ? "]" : "\n  ]");
  out += ",\n  \"payload_profiles\": [";
  for (std::size_t i = 0; i < profile_count; ++i) {
    const PayloadProfileDesc& p = profiles[i];
    out += (i == 0 ? "\n" : ",\n");
    out += "    {\"channel\": ";
    AppendString(&out, p.channel);
    out += ", \"profile\": ";
    if (p.profile.empty()) {
      out += "null";
    } else {
      AppendString(&out, p.profile);
    }
    out += ", \"profile_version\": " + std::to_string(p.version);
    out += ", \"params_digest\": ";
    if (p.params_digest == 0) {
      out += "null";
    } else {
      AppendString(&out, FormatManifestHash(p.params_digest));
    }
    out += "}";
  }
  out += (profile_count == 0 ? "]\n" : "\n  ]\n");
  out += "}\n";
  return out;
}

void SyncDirOf(const std::string& file_path) {
  const std::size_t slash = file_path.find_last_of('/');
  const std::string dir =
      slash == std::string::npos ? std::string(".") : file_path.substr(0, slash);
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return;
  ::fsync(fd);
  ::close(fd);
}

bool WriteFileAtomic(const std::string& path, const std::string& body,
                     std::string* err) {
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                        0644);
  if (fd < 0) {
    *err = "cannot create " + tmp + ": " + std::strerror(errno);
    return false;
  }
  std::size_t done = 0;
  bool ok = true;
  while (done < body.size()) {
    const ssize_t n = ::write(fd, body.data() + done, body.size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      ok = false;
      break;
    }
    done += static_cast<std::size_t>(n);
  }
  if (ok && ::fsync(fd) != 0) ok = false;
  if (::close(fd) != 0) ok = false;
  if (!ok) {
    ::unlink(tmp.c_str());
    *err = "write failed: " + tmp;
    return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    *err = "rename failed: " + path + ": " + std::strerror(errno);
    return false;
  }
  SyncDirOf(path);
  return true;
}

bool ReadWholeFile(const std::string& path, std::string* out, std::string* err,
                   bool* absent) {
  *absent = false;
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) {
      *absent = true;
      return false;
    }
    *err = "cannot open " + path + ": " + std::strerror(errno);
    return false;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    *err = "not a regular file: " + path;
    return false;
  }
  if (static_cast<uint64_t>(st.st_size) > kMaxFileBytes) {
    ::close(fd);
    *err = "file too large: " + path;
    return false;
  }
  out->resize(static_cast<std::size_t>(st.st_size));
  std::size_t done = 0;
  while (done < out->size()) {
    const ssize_t n = ::read(fd, &(*out)[done], out->size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      *err = "read failed: " + path;
      return false;
    }
    if (n == 0) break;
    done += static_cast<std::size_t>(n);
  }
  out->resize(done);
  ::close(fd);
  return true;
}

// ---- strict JSON reader ---------------------------------------------------
//
// The sidecar is the only JSON this layer reads, and it is machine-written
// with a fixed shape. Rather than embedding a general JSON library in the
// -fno-exceptions core, this reader accepts exactly the defined shape and
// rejects everything else (unknown keys, duplicate keys, floats, trailing
// bytes). A file this build does not fully understand is invalid — never
// partially trusted.

class JsonReader {
 public:
  JsonReader(const std::string& text, std::string* err)
      : text_(text), err_(err) {}

  bool Fail(const std::string& why) {
    if (err_ != nullptr && err_->empty()) {
      *err_ = why + " at offset " + std::to_string(pos_);
    }
    return false;
  }

  void SkipWs() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool Peek(char c) {
    SkipWs();
    return pos_ < text_.size() && text_[pos_] == c;
  }

  bool Expect(char c) {
    SkipWs();
    if (pos_ >= text_.size() || text_[pos_] != c) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "expected '%c'", c);
      return Fail(buf);
    }
    ++pos_;
    return true;
  }

  bool AtEnd() {
    SkipWs();
    return pos_ == text_.size();
  }

  bool String(std::string* out) {
    if (!Expect('"')) return false;
    out->clear();
    while (pos_ < text_.size()) {
      const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
      if (c == '"') return true;
      if (c < 0x20) return Fail("control character in string");
      if (c != '\\') {
        out->push_back(static_cast<char>(c));
        if (out->size() > kMaxStringBytes) return Fail("string too long");
        continue;
      }
      if (pos_ >= text_.size()) return Fail("truncated escape");
      const char e = text_[pos_++];
      switch (e) {
        case '"':
          out->push_back('"');
          break;
        case '\\':
          out->push_back('\\');
          break;
        case '/':
          out->push_back('/');
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u': {
          if (pos_ + 4 > text_.size()) return Fail("truncated \\u escape");
          unsigned v = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_++];
            v <<= 4;
            if (h >= '0' && h <= '9') {
              v |= static_cast<unsigned>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              v |= static_cast<unsigned>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              v |= static_cast<unsigned>(h - 'A' + 10);
            } else {
              return Fail("bad \\u escape");
            }
          }
          // The writer only escapes control characters (< 0x80); anything
          // beyond Latin-1 would need a UTF-8 decoder, so reject it.
          if (v > 0x7FF) return Fail("unsupported \\u escape");
          if (v < 0x80) {
            out->push_back(static_cast<char>(v));
          } else {
            out->push_back(static_cast<char>(0xC0 | (v >> 6)));
            out->push_back(static_cast<char>(0x80 | (v & 0x3F)));
          }
          break;
        }
        default:
          return Fail("unknown escape");
      }
      if (out->size() > kMaxStringBytes) return Fail("string too long");
    }
    return Fail("unterminated string");
  }

  bool UInt(uint64_t max, uint64_t* out) {
    SkipWs();
    if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
      return Fail("expected an unsigned integer");
    }
    uint64_t v = 0;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
      const uint64_t digit = static_cast<uint64_t>(text_[pos_] - '0');
      if (v > (max - digit) / 10) return Fail("integer out of range");
      v = v * 10 + digit;
      ++pos_;
    }
    *out = v;
    return true;
  }

  bool Bool(bool* out) {
    if (Match("true")) {
      *out = true;
      return true;
    }
    if (Match("false")) {
      *out = false;
      return true;
    }
    return Fail("expected true/false");
  }

  bool Null() { return Match("null"); }

  // Silent lookahead+consume: used by null/boolean checks whose failure is not
  // itself an error (the caller then parses the actual value and reports).
  bool Match(const char* lit) {
    SkipWs();
    const std::size_t n = std::strlen(lit);
    if (text_.compare(pos_, n, lit) == 0) {
      pos_ += n;
      return true;
    }
    return false;
  }

  std::size_t pos() const { return pos_; }

 private:
  const std::string& text_;
  std::string* err_;
  std::size_t pos_ = 0;
};

bool ParseFieldObject(JsonReader& j, FieldDesc* f, const char* ctx) {
  if (!j.Expect('{')) return false;
  unsigned seen = 0;
  for (;;) {
    if (j.Peek('}')) {
      if (seen != 0x3F) {
        return j.Fail(std::string(ctx) + ": field object is missing keys");
      }
      return j.Expect('}');
    }
    std::string key;
    if (!j.String(&key)) return false;
    if (!j.Expect(':')) return false;
    unsigned bit = 0;
    if (key == "name") {
      bit = 1u << 0;
      if (!j.String(&f->name)) return false;
    } else if (key == "type") {
      bit = 1u << 1;
      if (!j.String(&f->type)) return false;
    } else if (key == "offset") {
      bit = 1u << 2;
      uint64_t v = 0;
      if (!j.UInt(0xFFFFFFFFull, &v)) return false;
      f->offset = static_cast<uint32_t>(v);
    } else if (key == "size") {
      bit = 1u << 3;
      uint64_t v = 0;
      if (!j.UInt(0xFFFFFFFFull, &v)) return false;
      f->size = static_cast<uint32_t>(v);
    } else if (key == "count") {
      bit = 1u << 4;
      uint64_t v = 0;
      if (!j.UInt(0xFFFFFFFFull, &v)) return false;
      f->count = static_cast<uint32_t>(v);
    } else if (key == "stride") {
      bit = 1u << 5;
      uint64_t v = 0;
      if (!j.UInt(0xFFFFFFFFull, &v)) return false;
      f->stride = static_cast<uint32_t>(v);
    } else {
      return j.Fail(std::string(ctx) + ": unknown field key '" + key + "'");
    }
    if ((seen & bit) != 0) {
      return j.Fail(std::string(ctx) + ": duplicate key '" + key + "'");
    }
    seen |= bit;
    if (j.Peek(',')) {
      if (!j.Expect(',')) return false;
      continue;
    }
    if (j.Peek('}')) continue;
    return j.Fail(std::string(ctx) + ": expected ',' or '}'");
  }
}

bool ParseFieldArray(JsonReader& j, std::vector<FieldDesc>* out,
                     const char* ctx) {
  if (!j.Expect('[')) return false;
  for (;;) {
    if (j.Peek(']')) return j.Expect(']');
    if (out->size() >= kMaxFields) {
      return j.Fail(std::string(ctx) + ": too many fields");
    }
    FieldDesc f;
    if (!ParseFieldObject(j, &f, ctx)) return false;
    out->push_back(std::move(f));
    if (j.Peek(',')) {
      if (!j.Expect(',')) return false;
      continue;
    }
    if (j.Peek(']')) continue;
    return j.Fail(std::string(ctx) + ": expected ',' or ']'");
  }
}

bool ValidateFields(const std::vector<FieldDesc>& fields,
                    const char* ctx, std::string* err) {
  for (std::size_t i = 0; i < fields.size(); ++i) {
    const FieldDesc& f = fields[i];
    if (f.name.empty()) {
      *err = std::string(ctx) + ": field without a name";
      return false;
    }
    if (f.type.empty()) {
      *err = std::string(ctx) + ": field '" + f.name + "' has no type";
      return false;
    }
    if (f.count == 0 || f.stride == 0) {
      *err = std::string(ctx) + ": field '" + f.name + "' has count/stride 0";
      return false;
    }
    if (static_cast<uint64_t>(f.count) * f.stride != f.size) {
      *err = std::string(ctx) + ": field '" + f.name +
             "' size != count * stride";
      return false;
    }
    for (std::size_t k = 0; k < i; ++k) {
      if (fields[k].name == f.name) {
        *err = std::string(ctx) + ": duplicate field '" + f.name + "'";
        return false;
      }
    }
  }
  return true;
}

bool ParseChannelArray(JsonReader& j, std::vector<ChannelDesc>* out,
                       std::string* err) {
  if (!j.Expect('[')) return false;
  for (;;) {
    if (j.Peek(']')) return j.Expect(']');
    if (out->size() >= kMaxStructs) {
      *err = "record_manifest.channels: too many channels";
      return false;
    }
    if (!j.Expect('{')) return false;
    ChannelDesc c;
    unsigned seen = 0;
    for (;;) {
      if (j.Peek('}')) {
        if (seen != 0x3F) {
          return j.Fail(
              "record_manifest.channels entry is missing keys");
        }
        break;
      }
      std::string key;
      if (!j.String(&key)) return false;
      if (!j.Expect(':')) return false;
      unsigned bit = 0;
      if (key == "struct_id") {
        bit = 1u << 0;
        uint64_t v = 0;
        if (!j.UInt(0xFFFFull, &v)) return false;
        c.struct_id = static_cast<uint16_t>(v);
      } else if (key == "channel") {
        bit = 1u << 1;
        if (!j.String(&c.channel)) return false;
      } else if (key == "record_key") {
        bit = 1u << 2;
        if (!j.String(&c.record_key)) return false;
      } else if (key == "contract_version") {
        bit = 1u << 3;
        uint64_t v = 0;
        if (!j.UInt(0xFFFFull, &v)) return false;
        c.contract_version = static_cast<uint16_t>(v);
      } else if (key == "payload_size") {
        bit = 1u << 4;
        uint64_t v = 0;
        if (!j.UInt(kThymeBinMaxPayloadSize, &v)) return false;
        c.payload_size = static_cast<uint32_t>(v);
      } else if (key == "layout_hash") {
        bit = 1u << 5;
        if (j.Null()) {
          c.layout_hash = kLayoutHashNone;
        } else {
          std::string hex;
          if (!j.String(&hex) || !ParseManifestHash(hex, &c.layout_hash)) {
            return j.Fail("record_manifest.channels.layout_hash is not 0x<16 hex>");
          }
        }
      } else {
        return j.Fail("unknown record_manifest.channels key '" + key + "'");
      }
      if ((seen & bit) != 0) {
        return j.Fail("duplicate record_manifest.channels key '" + key + "'");
      }
      seen |= bit;
      if (j.Peek(',')) {
        if (!j.Expect(',')) return false;
        continue;
      }
      if (j.Peek('}')) continue;
      return j.Fail("expected ',' or '}' in record_manifest.channels");
    }
    (void)j.Expect('}');
    if (c.channel.empty()) {
      *err = "record_manifest.channels entry has no channel";
      return false;
    }
    if (c.channel.size() >= kThymeStructChannelBytes) {
      *err = "record_manifest.channels entry channel is too long";
      return false;
    }
    if (c.record_key.size() >= kThymeStructKeyBytes) {
      *err = "record_manifest.channels entry record_key is too long";
      return false;
    }
    if (c.contract_version == 0) {
      *err = "record_manifest.channels entry has contract_version 0";
      return false;
    }
    out->push_back(std::move(c));
    if (j.Peek(',')) {
      if (!j.Expect(',')) return false;
      continue;
    }
    if (j.Peek(']')) continue;
    return j.Fail("expected ',' or ']' in record_manifest.channels");
  }
}

bool ParsePayloadProfileArray(JsonReader& j, std::vector<PayloadProfileDesc>* out,
                              std::string* err) {
  if (!j.Expect('[')) return false;
  for (;;) {
    if (j.Peek(']')) return j.Expect(']');
    if (out->size() >= kMaxStructs) {
      *err = "payload_profiles: too many entries";
      return false;
    }
    if (!j.Expect('{')) return false;
    PayloadProfileDesc p;
    unsigned seen = 0;
    for (;;) {
      if (j.Peek('}')) {
        if (seen != 0xF) {
          return j.Fail("payload_profiles entry is missing keys");
        }
        break;
      }
      std::string key;
      if (!j.String(&key)) return false;
      if (!j.Expect(':')) return false;
      unsigned bit = 0;
      if (key == "channel") {
        bit = 1u << 0;
        if (!j.String(&p.channel)) return false;
      } else if (key == "profile") {
        bit = 1u << 1;
        if (j.Null()) {
          p.profile.clear();
        } else if (!j.String(&p.profile)) {
          return false;
        }
      } else if (key == "profile_version") {
        bit = 1u << 2;
        uint64_t v = 0;
        if (!j.UInt(0xFFFFFFFFull, &v)) return false;
        p.version = static_cast<uint32_t>(v);
      } else if (key == "params_digest") {
        bit = 1u << 3;
        if (j.Null()) {
          p.params_digest = 0;
        } else {
          std::string hex;
          if (!j.String(&hex) || !ParseManifestHash(hex, &p.params_digest)) {
            return j.Fail("payload_profiles.params_digest is not 0x<16 hex>");
          }
        }
      } else {
        return j.Fail("unknown payload_profiles key '" + key + "'");
      }
      if ((seen & bit) != 0) {
        return j.Fail("duplicate payload_profiles key '" + key + "'");
      }
      seen |= bit;
      if (j.Peek(',')) {
        if (!j.Expect(',')) return false;
        continue;
      }
      if (j.Peek('}')) continue;
      return j.Fail("expected ',' or '}' in payload_profiles");
    }
    (void)j.Expect('}');
    if (p.channel.empty()) {
      *err = "payload_profiles entry has no channel";
      return false;
    }
    if (p.channel.size() >= kThymeStructChannelBytes) {
      *err = "payload_profiles channel is too long";
      return false;
    }
    if (p.profile.size() >= kMaxProfileBytes) {
      *err = "payload_profiles profile is too long";
      return false;
    }
    if (p.profile.empty() && (p.version != 0 || p.params_digest != 0)) {
      *err = "payload_profiles empty profile cannot carry version or digest";
      return false;
    }
    for (const PayloadProfileDesc& prev : *out) {
      if (prev.channel == p.channel) {
        *err = "duplicate payload_profiles channel '" + p.channel + "'";
        return false;
      }
    }
    out->push_back(std::move(p));
    if (j.Peek(',')) {
      if (!j.Expect(',')) return false;
      continue;
    }
    if (j.Peek(']')) continue;
    return j.Fail("expected ',' or ']' in payload_profiles");
  }
}

bool ParseRecordObject(JsonReader& j, RecordDesc* r, uint32_t schema_version,
                       std::string* err) {
  if (!j.Expect('{')) return false;
  unsigned seen = 0;
  bool layout_seen = false;
  for (;;) {
    if (j.Peek('}')) {
      // v1 has the original nine keys; v2 adds the ABI-11 identity triple.
      const unsigned required =
          schema_version >= 2 ? 0xFFFu : ((1u << 9) - 1u);
      if ((seen & required) != required) {
        return j.Fail("record entry is missing keys");
      }
      break;
    }
    std::string key;
    if (!j.String(&key)) return false;
    if (!j.Expect(':')) return false;
    unsigned bit = 0;
    if (key == "struct_id") {
      bit = 1u << 0;
      uint64_t v = 0;
      if (!j.UInt(0xFFFFull, &v)) return false;
      r->struct_id = static_cast<uint16_t>(v);
    } else if (key == "name") {
      bit = 1u << 1;
      if (!j.String(&r->name)) return false;
    } else if (key == "contract_version") {
      bit = 1u << 2;
      uint64_t v = 0;
      if (!j.UInt(0xFFFFull, &v)) return false;
      r->contract_version = static_cast<uint16_t>(v);
    } else if (key == "payload_size") {
      bit = 1u << 3;
      uint64_t v = 0;
      if (!j.UInt(kThymeBinMaxPayloadSize, &v)) return false;
      r->payload_size = static_cast<uint32_t>(v);
    } else if (key == "frame_header_bytes") {
      bit = 1u << 4;
      uint64_t v = 0;
      if (!j.UInt(0xFFFFull, &v)) return false;
      if (v != sizeof(FrameHeader)) {
        return j.Fail("record frame_header_bytes does not match this build");
      }
    } else if (key == "layout") {
      bit = 1u << 5;
      layout_seen = true;
      std::string state;
      if (!j.String(&state)) return false;
      if (state == "known") {
        r->layout = LayoutState::kKnown;
      } else if (state == "unavailable") {
        r->layout = LayoutState::kUnavailable;
      } else {
        return j.Fail("unknown layout state '" + state + "'");
      }
    } else if (key == "payload_offset") {
      bit = 1u << 6;
      if (j.Null()) {
        r->payload_offset = 0;
      } else {
        uint64_t v = 0;
        if (!j.UInt(kThymeBinMaxPayloadSize, &v)) return false;
        r->payload_offset = static_cast<uint32_t>(v);
      }
    } else if (key == "contract_bytes") {
      bit = 1u << 7;
      if (j.Null()) {
        r->contract_bytes = 0;
      } else {
        uint64_t v = 0;
        if (!j.UInt(kThymeBinMaxPayloadSize, &v)) return false;
        r->contract_bytes = static_cast<uint32_t>(v);
      }
    } else if (key == "fields") {
      bit = 1u << 8;
      if (j.Null()) {
        r->fields.clear();
      } else if (!ParseFieldArray(j, &r->fields, "record fields")) {
        return false;
      }
    } else if (key == "channel") {
      bit = 1u << 9;
      if (j.Null()) {
        r->channel.clear();
      } else if (!j.String(&r->channel)) {
        return false;
      }
    } else if (key == "record_key") {
      bit = 1u << 10;
      if (j.Null()) {
        r->record_key.clear();
      } else if (!j.String(&r->record_key)) {
        return false;
      }
    } else if (key == "layout_hash") {
      bit = 1u << 11;
      if (j.Null()) {
        r->layout_hash = kLayoutHashNone;
      } else {
        std::string hex;
        if (!j.String(&hex) || !ParseManifestHash(hex, &r->layout_hash)) {
          return j.Fail("record layout_hash is not 0x<16 hex>");
        }
      }
    } else {
      return j.Fail("unknown record key '" + key + "'");
    }
    if ((seen & bit) != 0) {
      return j.Fail("duplicate record key '" + key + "'");
    }
    seen |= bit;
    if (j.Peek(',')) {
      if (!j.Expect(',')) return false;
      continue;
    }
    if (j.Peek('}')) continue;
    return j.Fail("expected ',' or '}'");
  }
  (void)j.Expect('}');
  if (!layout_seen) {
    return j.Fail("record entry has no layout state");
  }
  if (r->name.empty()) {
    *err = "record without a name";
    return false;
  }
  if (r->contract_version == 0) {
    *err = "record '" + r->name + "' has contract_version 0 (unset)";
    return false;
  }
  if (r->layout == LayoutState::kKnown) {
    if (r->fields.empty()) {
      *err = "record '" + r->name +
             "' declares a known layout without fields";
      return false;
    }
    if (static_cast<uint64_t>(r->payload_offset) + r->contract_bytes >
        r->payload_size) {
      *err = "record '" + r->name +
             "' payload_offset/contract_bytes outside payload_size";
      return false;
    }
    if (!ValidateFields(r->fields, "record fields", err)) return false;
    for (const FieldDesc& f : r->fields) {
      if (static_cast<uint64_t>(f.offset) + f.size > r->contract_bytes) {
        *err = "record '" + r->name + "': field '" + f.name +
               "' extends past the contract";
        return false;
      }
    }
  } else {
    if (!r->fields.empty() || r->contract_bytes != 0 ||
        r->payload_offset != 0) {
      *err = "record '" + r->name +
             "' declares unavailable layout but carries a layout";
      return false;
    }
  }
  if (r->channel.size() >= kThymeStructChannelBytes) {
    *err = "record '" + r->name + "' channel is too long";
    return false;
  }
  if (r->record_key.size() >= kThymeStructKeyBytes) {
    *err = "record '" + r->name + "' record_key is too long";
    return false;
  }
  if (!r->channel.empty() && r->record_key.empty()) {
    r->record_key = r->channel;  // canonical key defaults to the channel
  }
  return true;
}

bool ParseManifestObject(JsonReader& j, PackageSchema* out,
                         uint32_t schema_version, std::string* err) {
  if (!j.Expect('{')) return false;
  unsigned seen = 0;
  bool hash_null = true;
  bool version_null = true;
  for (;;) {
    if (j.Peek('}')) {
      if (seen != 0x7) return j.Fail("record_manifest is missing keys");
      break;
    }
    std::string key;
    if (!j.String(&key)) return false;
    if (!j.Expect(':')) return false;
    unsigned bit = 0;
    if (key == "hash") {
      bit = 1u << 0;
      if (j.Null()) {
        hash_null = true;
      } else {
        hash_null = false;
        std::string hex;
        if (!j.String(&hex)) return false;
        if (!ParseManifestHash(hex, &out->manifest_hash)) {
          return j.Fail("record_manifest.hash is not 0x<16 hex>");
        }
      }
    } else if (key == "version") {
      bit = 1u << 1;
      if (j.Null()) {
        version_null = true;
      } else {
        version_null = false;
        uint64_t v = 0;
        if (!j.UInt(0xFFFFull, &v)) return false;
        out->manifest_version = static_cast<uint16_t>(v);
      }
    } else if (key == "channels") {
      bit = 1u << 2;
      if (j.Null()) {
        if (schema_version >= 2) {
          return j.Fail(
              "record_manifest.channels: schema_version 2 requires the "
              "channel key array");
        }
      } else if (schema_version < 2) {
        return j.Fail(
            "record_manifest.channels: only null is defined in "
            "schema_version 1 (channel keys land with ABI-11)");
      } else if (!ParseChannelArray(j, &out->channels, err)) {
        return false;
      }
    } else {
      return j.Fail("unknown record_manifest key '" + key + "'");
    }
    if ((seen & bit) != 0) {
      return j.Fail("duplicate record_manifest key '" + key + "'");
    }
    seen |= bit;
    if (j.Peek(',')) {
      if (!j.Expect(',')) return false;
      continue;
    }
    if (j.Peek('}')) continue;
    return j.Fail("expected ',' or '}'");
  }
  (void)j.Expect('}');
  if (hash_null != version_null) {
    *err = "record_manifest: hash and version must both be set or both null";
    return false;
  }
  if (!hash_null) {
    out->manifest_declared = true;
    if (out->manifest_version != kManifestVersion) {
      *err = "record_manifest: unsupported manifest version " +
             std::to_string(out->manifest_version) + " (this build reads " +
             std::to_string(kManifestVersion) + ")";
      return false;
    }
  }
  return true;
}

bool ParseSchemaText(const std::string& text, PackageSchema* out,
                     std::string* err) {
  JsonReader j(text, err);
  if (!j.Expect('{')) return false;
  unsigned seen = 0;
  bool format_ok = false;
  bool frame_seen = false;
  for (;;) {
    if (j.Peek('}')) {
      constexpr unsigned kBase = (1u << 0) | (1u << 1) | (1u << 2) |
                                 (1u << 3) | (1u << 4) | (1u << 5);
      constexpr unsigned kProfiles = 1u << 6;
      const unsigned required =
          out->version >= 3 ? (kBase | kProfiles) : kBase;
      if ((seen & required) != required) {
        *err = "schema is missing required keys";
        return false;
      }
      break;
    }
    std::string key;
    if (!j.String(&key)) return false;
    if (!j.Expect(':')) return false;
    unsigned bit = 0;
    if (key == "format") {
      bit = 1u << 0;
      std::string format;
      if (!j.String(&format)) return false;
      if (format != kFormat) {
        *err = "not a thyme record schema: format '" + format + "'";
        return false;
      }
      format_ok = true;
    } else if (key == "schema_version") {
      bit = 1u << 1;
      uint64_t v = 0;
      if (!j.UInt(0xFFFFFFFFull, &v)) return false;
      if (v < kVersionMin || v > kVersion) {
        *err = "unsupported schema_version " + std::to_string(v) +
               " (this build reads " + std::to_string(kVersionMin) + ".." +
               std::to_string(kVersion) + ")";
        return false;
      }
      out->version = static_cast<uint32_t>(v);
    } else if (key == "partial") {
      bit = 1u << 2;
      if (!j.Bool(&out->partial)) return false;
    } else if (key == "record_manifest") {
      bit = 1u << 3;
      if (!ParseManifestObject(j, out, out->version, err)) return false;
    } else if (key == "frame_header") {
      bit = 1u << 4;
      frame_seen = true;
      if (!j.Expect('{')) return false;
      unsigned fseen = 0;
      for (;;) {
        if (j.Peek('}')) {
          if (fseen != 0x3) {
            *err = "frame_header is missing keys";
            return false;
          }
          break;
        }
        std::string fkey;
        if (!j.String(&fkey)) return false;
        if (!j.Expect(':')) return false;
        if (fkey == "bytes") {
          if ((fseen & 1u) != 0) {
            *err = "duplicate frame_header key";
            return false;
          }
          fseen |= 1u;
          uint64_t v = 0;
          if (!j.UInt(0xFFFFull, &v)) return false;
          if (v != sizeof(FrameHeader)) {
            *err = "frame_header.bytes does not match this build";
            return false;
          }
          out->frame_header_bytes = static_cast<uint32_t>(v);
        } else if (fkey == "fields") {
          if ((fseen & 2u) != 0) {
            *err = "duplicate frame_header key";
            return false;
          }
          fseen |= 2u;
          if (!ParseFieldArray(j, &out->frame_header_fields,
                               "frame_header fields")) {
            return false;
          }
        } else {
          *err = "unknown frame_header key '" + fkey + "'";
          return false;
        }
        if (j.Peek(',')) {
          if (!j.Expect(',')) return false;
          continue;
        }
        if (j.Peek('}')) continue;
        *err = "expected ',' or '}' in frame_header";
        return false;
      }
      (void)j.Expect('}');
      if (!ValidateFields(out->frame_header_fields, "frame_header fields",
                          err)) {
        return false;
      }
    } else if (key == "records") {
      bit = 1u << 5;
      if (!j.Expect('[')) return false;
      for (;;) {
        if (j.Peek(']')) break;
        if (out->records.size() >= kMaxStructs) {
          *err = "too many records";
          return false;
        }
        RecordDesc r;
        if (!ParseRecordObject(j, &r, out->version, err)) return false;
        for (const RecordDesc& prev : out->records) {
          if (prev.struct_id == r.struct_id) {
            *err = "duplicate struct_id " + std::to_string(r.struct_id);
            return false;
          }
        }
        out->records.push_back(std::move(r));
        if (j.Peek(',')) {
          if (!j.Expect(',')) return false;
          continue;
        }
        if (j.Peek(']')) continue;
        *err = "expected ',' or ']' in records";
        return false;
      }
      (void)j.Expect(']');
    } else if (key == "payload_profiles") {
      bit = 1u << 6;
      if (!ParsePayloadProfileArray(j, &out->payload_profiles, err)) {
        return false;
      }
    } else {
      *err = "unknown schema key '" + key + "'";
      return false;
    }
    if ((seen & bit) != 0) {
      *err = "duplicate schema key '" + key + "'";
      return false;
    }
    seen |= bit;
    if (j.Peek(',')) {
      if (!j.Expect(',')) return false;
      continue;
    }
    if (j.Peek('}')) continue;
    *err = "expected ',' or '}'";
    return false;
  }
  (void)j.Expect('}');
  if (!j.AtEnd()) {
    *err = "trailing bytes after the schema object";
    return false;
  }
  if (!format_ok || !frame_seen) {
    *err = "schema is missing required keys";
    return false;
  }
  if (out->version < 3 && !out->payload_profiles.empty()) {
    *err = "schema_version " + std::to_string(out->version) +
           " cannot carry payload_profiles";
    return false;
  }
  if (out->version < 2) {
    // schema_version 1 has no ABI-11 identity fields. Accepting them would
    // make a v1 sidecar mean two different things depending on key order.
    if (!out->channels.empty()) {
      *err = "schema_version 1 cannot carry record_manifest.channels";
      return false;
    }
    for (const RecordDesc& r : out->records) {
      if (!r.channel.empty() || !r.record_key.empty() ||
          r.layout_hash != kLayoutHashNone) {
        *err = "schema_version 1 record '" + r.name +
               "' carries ABI-11 identity fields";
        return false;
      }
    }
  }
  if (out->version >= 2) {
    // ABI-11: record_manifest.channels and records[] are two views of the
    // same identity. Reject a sidecar that disagrees with itself instead of
    // letting a tool pick the wrong one.
    for (std::size_t i = 0; i < out->channels.size(); ++i) {
      const ChannelDesc& c = out->channels[i];
      if (i != 0 && out->channels[i - 1].struct_id >= c.struct_id) {
        *err = "record_manifest.channels is not strictly ordered by struct_id";
        return false;
      }
      const RecordDesc* rec = nullptr;
      for (const RecordDesc& r : out->records) {
        if (r.struct_id == c.struct_id) {
          rec = &r;
          break;
        }
      }
      if (rec == nullptr) {
        *err = "record_manifest.channels references unknown struct_id " +
               std::to_string(c.struct_id);
        return false;
      }
      for (std::size_t k = 0; k < i; ++k) {
        if (out->channels[k].channel == c.channel) {
          *err = "duplicate channel '" + c.channel + "'";
          return false;
        }
        if (out->channels[k].record_key == c.record_key) {
          *err = "duplicate record_key '" + c.record_key + "'";
          return false;
        }
      }
      if (rec->channel != c.channel || rec->record_key != c.record_key ||
          rec->contract_version != c.contract_version ||
          rec->payload_size != c.payload_size ||
          rec->layout_hash != c.layout_hash) {
        *err = "record_manifest.channels disagrees with record " +
               std::to_string(c.struct_id);
        return false;
      }
    }
    for (const RecordDesc& r : out->records) {
      if (r.channel.empty()) continue;
      bool found = false;
      for (const ChannelDesc& c : out->channels) {
        if (c.struct_id == r.struct_id) {
          found = true;
          break;
        }
      }
      if (!found) {
        *err = "record '" + r.name +
               "' has a channel but no record_manifest.channels entry";
        return false;
      }
    }
  }
  return true;
}

}  // namespace

const char* ReadStateName(ReadState state) {
  switch (state) {
    case ReadState::kOk:
      return "ok";
    case ReadState::kAbsent:
      return "absent";
    case ReadState::kInvalid:
      return "invalid";
  }
  return "invalid";
}

namespace {

bool WriteSchema(const std::string& path, const ThymeStructDesc* descs,
                 std::size_t count, const RecordManifest* manifest, bool partial,
                 std::string* err, integrity::PackageHashState* hashes,
                 const PayloadProfileDesc* profiles, std::size_t profile_count) {
  if (descs == nullptr && count != 0) {
    if (err != nullptr) *err = "schema: null registry";
    return false;
  }
  if (count > kMaxStructs) {
    if (err != nullptr) *err = "schema: too many records";
    return false;
  }
  if (profiles == nullptr && profile_count != 0) {
    if (err != nullptr) *err = "schema: null payload_profiles";
    return false;
  }
  if (profile_count > kMaxStructs) {
    if (err != nullptr) *err = "schema: too many payload_profiles";
    return false;
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (descs[i].contract_version == 0) {
      if (err != nullptr) {
        *err = "schema: descriptor without a contract version";
      }
      return false;
    }
  }
  for (std::size_t i = 0; i < profile_count; ++i) {
    if (profiles[i].channel.empty()) {
      if (err != nullptr) *err = "schema: payload_profile missing channel";
      return false;
    }
    if (profiles[i].profile.size() >= kMaxProfileBytes) {
      if (err != nullptr) *err = "schema: payload_profile name is too long";
      return false;
    }
    if (profiles[i].profile.empty() &&
        (profiles[i].version != 0 || profiles[i].params_digest != 0)) {
      if (err != nullptr) {
        *err = "schema: empty profile cannot carry version or params_digest";
      }
      return false;
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (profiles[j].channel == profiles[i].channel) {
        if (err != nullptr) {
          *err = "schema: duplicate payload_profile channel '" +
                 profiles[i].channel + "'";
        }
        return false;
      }
    }
  }
  const std::string body =
      BuildJson(descs, count, manifest, partial, profiles, profile_count);
  if (!WriteFileAtomic(path, body, err)) return false;
  if (hashes != nullptr) hashes->Replace(kFileName, body.data(), body.size());
  return true;
}

}  // namespace

bool WritePackageSchema(const std::string& package_dir,
                        const ThymeStructDesc* descs, std::size_t count,
                        const RecordManifest* manifest, bool partial,
                        std::string* err, integrity::PackageHashState* hashes,
                        const PayloadProfileDesc* profiles,
                        std::size_t profile_count) {
  return WriteSchema(package_dir + "/" + kFileName, descs, count, manifest,
                     partial, err, hashes, profiles, profile_count);
}

bool WriteSchemaFile(const std::string& path, const ThymeStructDesc* descs,
                     std::size_t count, const RecordManifest* manifest,
                     bool partial, std::string* err,
                     const PayloadProfileDesc* profiles,
                     std::size_t profile_count) {
  return WriteSchema(path, descs, count, manifest, partial, err, nullptr,
                     profiles, profile_count);
}

ReadState ReadPackageSchema(const std::string& package_dir, PackageSchema* out,
                            std::string* err) {
  return ReadSchemaFile(package_dir + "/" + kFileName, out, err);
}

ReadState ReadSchemaFile(const std::string& path, PackageSchema* out,
                         std::string* err) {
  std::string error;
  std::string body;
  bool absent = false;
  if (!ReadWholeFile(path, &body, &error, &absent)) {
    if (err != nullptr) *err = error;
    return absent ? ReadState::kAbsent : ReadState::kInvalid;
  }
  PackageSchema parsed;
  if (!ParseSchemaText(body, &parsed, &error)) {
    if (err != nullptr) *err = path + ": " + error;
    return ReadState::kInvalid;
  }
  if (out != nullptr) *out = std::move(parsed);
  return ReadState::kOk;
}

const FieldDesc* FrameHeaderFields(std::size_t* count) {
  static const FieldDesc kFields[] = {
      {"magic", "uint32", offsetof(FrameHeader, magic), 4, 1, 4},
      {"version", "uint16", offsetof(FrameHeader, version), 2, 1, 2},
      {"size", "uint16", offsetof(FrameHeader, size), 2, 1, 2},
      {"session_id", "uint64", offsetof(FrameHeader, session_id), 8, 1, 8},
      {"mono_ns", "uint64", offsetof(FrameHeader, mono_ns), 8, 1, 8},
      {"sequence", "uint32", offsetof(FrameHeader, sequence), 4, 1, 4},
      {"valid", "uint8", offsetof(FrameHeader, valid), 1, 1, 1},
      {"reserved", "uint8", offsetof(FrameHeader, reserved), 3, 3, 1},
      {"crc32", "uint32", offsetof(FrameHeader, crc32), 4, 1, 4},
      {"reserved2", "uint32", offsetof(FrameHeader, reserved2), 4, 1, 4},
  };
  static_assert(sizeof(FrameHeader) == 40, "frame header schema is 40 bytes");
  if (count != nullptr) *count = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

bool ParseManifestHash(const std::string& text, uint64_t* out) {
  std::size_t i = 0;
  if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    i = 2;
  }
  if (text.size() - i != 16) return false;
  uint64_t v = 0;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    v <<= 4;
    if (c >= '0' && c <= '9') {
      v |= static_cast<uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      v |= static_cast<uint64_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      v |= static_cast<uint64_t>(c - 'A' + 10);
    } else {
      return false;
    }
  }
  *out = v;
  return true;
}

std::string FormatManifestHash(uint64_t hash) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "0x%016llx",
                static_cast<unsigned long long>(hash));
  return std::string(buf);
}

std::string RenderSchemaText(const PackageSchema& schema) {
  std::string out;
  out += "schema   : present (v" + std::to_string(schema.version) +
         (schema.partial ? ", partial" : ", sealed") + ")\n";
  out += "manifest : ";
  if (schema.manifest_declared) {
    out += "hash=" + FormatManifestHash(schema.manifest_hash) +
           " version=" + std::to_string(schema.manifest_version) + "\n";
  } else {
    out += "unavailable (host built without a record manifest)\n";
  }
  out += "frame    : " + std::to_string(schema.frame_header_bytes) + " B, " +
         std::to_string(schema.frame_header_fields.size()) + " fields\n";
  out += "channels : " + std::to_string(schema.channels.size()) + "\n";
  out += "profiles : " + std::to_string(schema.payload_profiles.size()) + "\n";
  for (const PayloadProfileDesc& p : schema.payload_profiles) {
    out += "profile  : channel=" + p.channel + " profile=";
    if (p.profile.empty()) {
      out += "null";
    } else {
      out += p.profile;
    }
    out += " version=" + std::to_string(p.version) + " params_digest=";
    if (p.params_digest == 0) {
      out += "none\n";
    } else {
      out += FormatManifestHash(p.params_digest) + "\n";
    }
  }
  for (const ChannelDesc& c : schema.channels) {
    out += "channel  : struct_id=" + std::to_string(c.struct_id) +
           " channel=" + c.channel + " record_key=" + c.record_key +
           " contract_version=" + std::to_string(c.contract_version) +
           " payload_size=" + std::to_string(c.payload_size) +
           " layout_hash=" +
           (c.layout_hash == kLayoutHashNone ? std::string("unknown")
                                             : FormatManifestHash(c.layout_hash)) +
           "\n";
  }
  for (std::size_t i = 0; i < schema.records.size(); ++i) {
    const RecordDesc& r = schema.records[i];
    out += "record   : struct_id=" + std::to_string(r.struct_id) + " name=" +
           r.name + " contract_version=" + std::to_string(r.contract_version) +
           " payload_size=" + std::to_string(r.payload_size);
    if (!r.channel.empty()) {
      out += " channel=" + r.channel + " record_key=" + r.record_key +
             " layout_hash=" +
             (r.layout_hash == kLayoutHashNone
                  ? std::string("unknown")
                  : FormatManifestHash(r.layout_hash));
    }
    out += " layout=";
    if (r.layout == LayoutState::kKnown) {
      out += "known (" + std::to_string(r.contract_bytes) + " B, " +
             std::to_string(r.fields.size()) + " fields)\n";
      for (const FieldDesc& f : r.fields) {
        out += "  field  : " + f.name + " " + f.type + " @" +
               std::to_string(r.payload_offset + f.offset) + " (rel " +
               std::to_string(f.offset) + ") size=" +
               std::to_string(f.size) + " count=" + std::to_string(f.count) +
               " stride=" + std::to_string(f.stride) + "\n";
      }
    } else {
      out += "unavailable (decode raw bytes)\n";
    }
  }
  return out;
}

}  // namespace schema
}  // namespace thyme
