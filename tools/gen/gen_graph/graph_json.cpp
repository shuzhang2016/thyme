// ECO-08: canonical JSON <-> graph.yaml round-trip.
//
// The editor-facing contract is deliberately smaller than the YAML surface:
// it is the *effective* graph after LoadGraphYaml derives delivery/defaults
// (source paths and yaml line numbers are diagnostics and never emitted).
// JSON is a strict subset: unknown keys, duplicate keys, non-integer numbers
// and wrong types are rejected. Reconstructed yaml is fed back through
// LoadGraphYamlFromString so the yaml rules stay the single semantic authority.

#include "graph_json.h"

#include "yaml.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct JVal {
  enum class Type { kNull, kBool, kNum, kStr, kArr, kObj };
  Type type = Type::kNull;
  bool b = false;
  std::string text;  // string value, or the number lexeme
  std::vector<JVal> arr;
  std::vector<std::pair<std::string, JVal>> obj;
};

JVal JNum(uint64_t v) {
  JVal j;
  j.type = JVal::Type::kNum;
  j.text = std::to_string(v);
  return j;
}

JVal JInt(int64_t v) {
  JVal j;
  j.type = JVal::Type::kNum;
  j.text = std::to_string(v);
  return j;
}

JVal JStr(const std::string& v) {
  JVal j;
  j.type = JVal::Type::kStr;
  j.text = v;
  return j;
}

JVal JBool(bool v) {
  JVal j;
  j.type = JVal::Type::kBool;
  j.b = v;
  return j;
}

JVal JArr() {
  JVal j;
  j.type = JVal::Type::kArr;
  return j;
}

JVal JObj() {
  JVal j;
  j.type = JVal::Type::kObj;
  return j;
}

void Put(JVal* o, const char* key, JVal v) {
  o->obj.emplace_back(key, std::move(v));
}

void JsonEscape(std::string* out, const std::string& s) {
  out->push_back('"');
  for (const unsigned char c : s) {
    switch (c) {
      case '"':
        *out += "\\\"";
        break;
      case '\\':
        *out += "\\\\";
        break;
      case '\b':
        *out += "\\b";
        break;
      case '\f':
        *out += "\\f";
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
        if (c < 0x20) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          *out += buf;
        } else {
          out->push_back(static_cast<char>(c));
        }
    }
  }
  out->push_back('"');
}

void Serialize(const JVal& v, int indent, std::string* out) {
  const std::string pad(static_cast<std::size_t>(indent), ' ');
  switch (v.type) {
    case JVal::Type::kNull:
      *out += "null";
      break;
    case JVal::Type::kBool:
      *out += v.b ? "true" : "false";
      break;
    case JVal::Type::kNum:
      *out += v.text;
      break;
    case JVal::Type::kStr:
      JsonEscape(out, v.text);
      break;
    case JVal::Type::kArr:
      if (v.arr.empty()) {
        *out += "[]";
        break;
      }
      *out += "[\n";
      for (std::size_t i = 0; i < v.arr.size(); ++i) {
        out->append(pad.size() + 2, ' ');
        Serialize(v.arr[i], indent + 2, out);
        if (i + 1 < v.arr.size()) *out += ',';
        *out += '\n';
      }
      out->append(pad);
      *out += ']';
      break;
    case JVal::Type::kObj:
      if (v.obj.empty()) {
        *out += "{}";
        break;
      }
      *out += "{\n";
      for (std::size_t i = 0; i < v.obj.size(); ++i) {
        out->append(pad.size() + 2, ' ');
        JsonEscape(out, v.obj[i].first);
        *out += ": ";
        Serialize(v.obj[i].second, indent + 2, out);
        if (i + 1 < v.obj.size()) *out += ',';
        *out += '\n';
      }
      out->append(pad);
      *out += '}';
      break;
  }
}

// ---- strict parser -------------------------------------------------------

class JsonParser {
 public:
  explicit JsonParser(const std::string& text) : text_(text) {}

  bool Parse(JVal* out, std::string* err) {
    SkipWs();
    if (!ParseValue(out, 0)) {
      *err = err_;
      return false;
    }
    SkipWs();
    if (pos_ != text_.size()) {
      SetError("trailing characters after the JSON value");
      *err = err_;
      return false;
    }
    return true;
  }

 private:
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

  void SetError(const std::string& message) {
    if (!err_.empty()) return;
    int line = 1;
    int col = 1;
    for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
      if (text_[i] == '\n') {
        ++line;
        col = 1;
      } else {
        ++col;
      }
    }
    err_ = "line " + std::to_string(line) + ", column " +
           std::to_string(col) + ": " + message;
  }

  bool ParseValue(JVal* out, int depth) {
    if (depth > 64) {
      SetError("JSON nesting is too deep");
      return false;
    }
    if (pos_ >= text_.size()) {
      SetError("unexpected end of input");
      return false;
    }
    switch (text_[pos_]) {
      case '{':
        return ParseObject(out, depth);
      case '[':
        return ParseArray(out, depth);
      case '"': {
        out->type = JVal::Type::kStr;
        return ParseString(&out->text);
      }
      case 't':
        out->type = JVal::Type::kBool;
        out->b = true;
        return ParseLiteral("true");
      case 'f':
        out->type = JVal::Type::kBool;
        out->b = false;
        return ParseLiteral("false");
      case 'n':
        out->type = JVal::Type::kNull;
        return ParseLiteral("null");
      default:
        if (text_[pos_] == '-' || std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
          out->type = JVal::Type::kNum;
          return ParseNumber(&out->text);
        }
        SetError("expected a JSON value");
        return false;
    }
  }

  bool ParseLiteral(const char* lit) {
    const std::size_t n = std::char_traits<char>::length(lit);
    if (text_.compare(pos_, n, lit) != 0) {
      SetError("bad literal");
      return false;
    }
    pos_ += n;
    return true;
  }

  static int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  bool ParseString(std::string* out) {
    if (pos_ >= text_.size() || text_[pos_] != '"') {
      SetError("expected a string");
      return false;
    }
    ++pos_;
    out->clear();
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) {
        SetError("control character in string");
        return false;
      }
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) {
        SetError("unterminated escape");
        return false;
      }
      const char esc = text_[pos_++];
      switch (esc) {
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
          uint32_t cp = 0;
          if (!ParseHex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // Surrogate pair: require the low half immediately.
            if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' ||
                text_[pos_ + 1] != 'u') {
              SetError("unpaired high surrogate");
              return false;
            }
            pos_ += 2;
            uint32_t low = 0;
            if (!ParseHex4(&low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) {
              SetError("bad low surrogate");
              return false;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            SetError("unpaired low surrogate");
            return false;
          }
          AppendUtf8(cp, out);
          break;
        }
        default:
          SetError("unknown escape");
          return false;
      }
    }
    SetError("unterminated string");
    return false;
  }

  bool ParseHex4(uint32_t* out) {
    if (pos_ + 4 > text_.size()) {
      SetError("short \\u escape");
      return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const int d = HexDigit(text_[pos_++]);
      if (d < 0) {
        SetError("bad \\u escape");
        return false;
      }
      v = (v << 4) | static_cast<uint32_t>(d);
    }
    *out = v;
    return true;
  }

  static void AppendUtf8(uint32_t cp, std::string* out) {
    if (cp <= 0x7F) {
      out->push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool ParseNumber(std::string* out) {
    const std::size_t start = pos_;
    if (text_[pos_] == '-') ++pos_;
    if (pos_ >= text_.size()) {
      SetError("bad number");
      return false;
    }
    if (text_[pos_] == '0') {
      ++pos_;
      if (pos_ < text_.size() &&
          std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        SetError("leading zero in number");
        return false;
      }
    } else if (std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
      while (pos_ < text_.size() &&
             std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        ++pos_;
      }
    } else {
      SetError("bad number");
      return false;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      if (pos_ >= text_.size() ||
          !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        SetError("bad fraction");
        return false;
      }
      while (pos_ < text_.size() &&
             std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        ++pos_;
      }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= text_.size() ||
          !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        SetError("bad exponent");
        return false;
      }
      while (pos_ < text_.size() &&
             std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        ++pos_;
      }
    }
    *out = text_.substr(start, pos_ - start);
    return true;
  }

  bool ParseArray(JVal* out, int depth) {
    ++pos_;  // '['
    out->type = JVal::Type::kArr;
    SkipWs();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipWs();
      JVal item;
      if (!ParseValue(&item, depth + 1)) return false;
      out->arr.push_back(std::move(item));
      SkipWs();
      if (pos_ >= text_.size()) {
        SetError("unterminated array");
        return false;
      }
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      SetError("expected ',' or ']'");
      return false;
    }
  }

  bool ParseObject(JVal* out, int depth) {
    ++pos_;  // '{'
    out->type = JVal::Type::kObj;
    SkipWs();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipWs();
      std::string key;
      if (!ParseString(&key)) return false;
      for (const auto& kv : out->obj) {
        if (kv.first == key) {
          SetError("duplicate key '" + key + "'");
          return false;
        }
      }
      SkipWs();
      if (pos_ >= text_.size() || text_[pos_] != ':') {
        SetError("expected ':' after object key");
        return false;
      }
      ++pos_;
      SkipWs();
      JVal value;
      if (!ParseValue(&value, depth + 1)) return false;
      out->obj.emplace_back(std::move(key), std::move(value));
      SkipWs();
      if (pos_ >= text_.size()) {
        SetError("unterminated object");
        return false;
      }
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      SetError("expected ',' or '}'");
      return false;
    }
  }

  const std::string& text_;
  std::size_t pos_ = 0;
  std::string err_;
};

// ---- DOM -> Graph --------------------------------------------------------

const JVal* Find(const JVal& obj, const char* key) {
  for (const auto& kv : obj.obj) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

bool CheckKeys(const JVal& obj, std::initializer_list<const char*> allowed,
               const std::string& context, std::string* err) {
  for (const auto& kv : obj.obj) {
    bool ok = false;
    for (const char* key : allowed) {
      if (kv.first == key) {
        ok = true;
        break;
      }
    }
    if (!ok) {
      *err = "unknown key '" + kv.first + "' in " + context;
      return false;
    }
  }
  return true;
}

bool ExpectUInt(const JVal& v, const std::string& where, uint64_t* out,
                std::string* err) {
  if (v.type != JVal::Type::kNum) {
    *err = where + " must be an integer";
    return false;
  }
  if (v.text.find_first_of(".eE") != std::string::npos) {
    *err = where + " must be an integer";
    return false;
  }
  if (!v.text.empty() && v.text[0] == '-') {
    *err = where + " must be >= 0";
    return false;
  }
  char* end = nullptr;
  const unsigned long long n = std::strtoull(v.text.c_str(), &end, 10);
  if (end == v.text.c_str() || *end != '\0') {
    *err = where + " is not a valid integer";
    return false;
  }
  *out = static_cast<uint64_t>(n);
  return true;
}

bool ExpectInt32(const JVal& v, const std::string& where, int* out,
                 std::string* err) {
  if (v.type != JVal::Type::kNum ||
      v.text.find_first_of(".eE") != std::string::npos) {
    *err = where + " must be an integer";
    return false;
  }
  char* end = nullptr;
  const long long n = std::strtoll(v.text.c_str(), &end, 10);
  if (end == v.text.c_str() || *end != '\0' || n < INT_MIN || n > INT_MAX) {
    *err = where + " is out of range";
    return false;
  }
  *out = static_cast<int>(n);
  return true;
}

bool HasControlChars(const std::string& s) {
  for (const unsigned char c : s) {
    if (c < 0x20) return true;
  }
  return false;
}

bool RequireString(const JVal& obj, const char* key,
                   const std::string& context, std::string* out,
                   std::string* err) {
  const JVal* v = Find(obj, key);
  if (v == nullptr) {
    *err = "missing required key '" + std::string(key) + "' in " + context;
    return false;
  }
  if (v->type != JVal::Type::kStr) {
    *err = std::string(key) + " in " + context + " must be a string";
    return false;
  }
  if (HasControlChars(v->text)) {
    *err = std::string(key) + " in " + context +
           " must not contain control characters (the yaml dialect is "
           "line-based)";
    return false;
  }
  *out = v->text;
  return true;
}

bool OptionalString(const JVal& obj, const char* key,
                    const std::string& context, std::string* out,
                    std::string* err) {
  const JVal* v = Find(obj, key);
  if (v == nullptr) return true;
  if (v->type != JVal::Type::kStr) {
    *err = std::string(key) + " in " + context + " must be a string";
    return false;
  }
  if (HasControlChars(v->text)) {
    *err = std::string(key) + " in " + context +
           " must not contain control characters (the yaml dialect is "
           "line-based)";
    return false;
  }
  *out = v->text;
  return true;
}

bool OptionalBool(const JVal& obj, const char* key,
                  const std::string& context, bool* out, std::string* err) {
  const JVal* v = Find(obj, key);
  if (v == nullptr) return true;
  if (v->type != JVal::Type::kBool) {
    *err = std::string(key) + " in " + context + " must be a boolean";
    return false;
  }
  *out = v->b;
  return true;
}

bool OptionalUInt(const JVal& obj, const char* key,
                  const std::string& context, uint64_t* out,
                  std::string* err) {
  const JVal* v = Find(obj, key);
  if (v == nullptr) return true;
  return ExpectUInt(*v, std::string(key) + " in " + context, out, err);
}

bool OptionalPositiveInt(const JVal& obj, const char* key,
                         const std::string& context, int* out,
                         std::string* err) {
  const JVal* v = Find(obj, key);
  if (v == nullptr) return true;
  uint64_t n = 0;
  if (!ExpectUInt(*v, std::string(key) + " in " + context, &n, err)) {
    return false;
  }
  if (n == 0 || n > static_cast<uint64_t>(INT_MAX)) {
    *err = std::string(key) + " in " + context + " must be > 0";
    return false;
  }
  *out = static_cast<int>(n);
  return true;
}

bool ParseNode(const JVal& v, Node* n, std::string* err) {
  if (v.type != JVal::Type::kObj) {
    *err = "each node must be an object";
    return false;
  }
  std::string id;
  if (!RequireString(v, "id", "node", &id, err)) return false;
  const std::string context = "node '" + id + "'";
  if (!CheckKeys(v,
                 {"id", "type", "hz", "process", "cpu_set", "sched_policy",
                  "priority", "placement_fail", "wait_inputs", "exec",
                  "publish", "deadline_ns", "wcet_ns", "input_budget", "produce",
                  "overrun", "threads"},
                 context, err)) {
    return false;
  }
  n->id = id;
  if (!RequireString(v, "type", context, &n->type, err)) return false;
  if (n->type.empty()) {
    *err = "type in " + context + " must not be empty";
    return false;
  }
  if (Find(v, "hz") == nullptr) {
    *err = "missing required key 'hz' in " + context;
    return false;
  }
  if (!OptionalPositiveInt(v, "hz", context, &n->hz, err)) return false;
  if (n->hz > 10000) {
    *err = "hz in " + context + " must be <= 10000";
    return false;
  }
  if (!OptionalString(v, "process", context, &n->process, err)) return false;
  if (!OptionalString(v, "sched_policy", context, &n->sched_policy, err)) {
    return false;
  }
  if (!OptionalString(v, "placement_fail", context, &n->placement_fail, err)) {
    return false;
  }
  if (!OptionalString(v, "wait_inputs", context, &n->wait_inputs, err)) {
    return false;
  }
  if (!OptionalString(v, "exec", context, &n->exec, err)) return false;
  if (!OptionalString(v, "publish", context, &n->publish, err)) return false;
  if (!OptionalString(v, "overrun", context, &n->overrun, err)) return false;
  if (const JVal* prio = Find(v, "priority")) {
    if (!ExpectInt32(*prio, "priority in " + context, &n->priority, err)) {
      return false;
    }
  }
  if (const JVal* cpus = Find(v, "cpu_set")) {
    if (cpus->type != JVal::Type::kArr) {
      *err = "cpu_set in " + context + " must be an array";
      return false;
    }
    for (const JVal& item : cpus->arr) {
      int cpu = 0;
      if (!ExpectInt32(item, "cpu_set entry in " + context, &cpu, err)) {
        return false;
      }
      n->cpu_set.push_back(cpu);
    }
  }
  uint64_t u64 = 0;
  if (!OptionalUInt(v, "deadline_ns", context, &u64, err)) return false;
  n->deadline_ns = u64;
  u64 = 0;
  if (!OptionalUInt(v, "wcet_ns", context, &u64, err)) return false;
  n->wcet_ns = u64;
  u64 = 0;
  if (const JVal* pv = Find(v, "produce")) {
    if (pv->type != JVal::Type::kStr) {
      *err = "produce in " + context + " must be a string";
      return false;
    }
    if (!pv->text.empty() && pv->text != "one" && pv->text != "many") {
      *err = "produce in " + context + " must be one|many";
      return false;
    }
    n->produce = pv->text;
  }
  if (!OptionalUInt(v, "input_budget", context, &u64, err)) return false;
  if (u64 > UINT32_MAX) {
    *err = "input_budget in " + context + " is out of range";
    return false;
  }
  n->input_budget = static_cast<uint32_t>(u64);
  u64 = 0;
  if (!OptionalUInt(v, "threads", context, &u64, err)) return false;
  if (u64 > 64) {
    *err = "threads in " + context + " must be in 1..64";
    return false;
  }
  n->threads = static_cast<uint32_t>(u64);
  if (u64 == 0 && Find(v, "threads") != nullptr) {
    *err = "threads in " + context + " must be in 1..64";
    return false;
  }
  if (u64 == 0) {
    n->threads = 1;
  }
  return true;
}

bool ParseEdge(const JVal& v, Edge* e, std::string* err) {
  if (v.type != JVal::Type::kObj) {
    *err = "each edge must be an object";
    return false;
  }
  if (!RequireString(v, "from", "edge", &e->from, err)) return false;
  if (!RequireString(v, "to", "edge", &e->to, err)) return false;
  if (!RequireString(v, "channel", "edge", &e->channel, err)) return false;
  const std::string context =
      "edge '" + e->from + "' -> '" + e->to + "' (" + e->channel + ")";
  if (!CheckKeys(v,
                 {"from", "to", "channel", "cpp_type", "pool", "transport",
                  "shm", "delivery", "read", "record_priority", "depth",
                  "record", "record_gate", "record_kind", "bitrate_kbps", "avg_frame_bytes",
                  "max_frame_bytes", "record_shm", "dynamic", "mp_gate",
                  "feedback", "delay", "lossless", "max_lag_ms", "before_ms",
                  "after_ms", "per_frame_write_ms", "pin_budget"},
                 context, err)) {
    return false;
  }
  if (!OptionalString(v, "cpp_type", context, &e->cpp_type, err)) return false;
  if (!OptionalString(v, "pool", context, &e->pool, err)) return false;
  if (!OptionalString(v, "transport", context, &e->transport, err)) {
    return false;
  }
  if (!OptionalString(v, "shm", context, &e->shm, err)) return false;
  if (!OptionalString(v, "delivery", context, &e->delivery, err)) return false;
  if (!OptionalString(v, "read", context, &e->read, err)) return false;
  if (!OptionalString(v, "record_priority", context, &e->record_priority,
                      err)) {
    return false;
  }
  if (!OptionalPositiveInt(v, "depth", context, &e->depth, err)) return false;
  if (!OptionalBool(v, "record", context, &e->record, err)) return false;
  if (!OptionalBool(v, "record_gate", context, &e->record_gate, err)) return false;
  if (!OptionalString(v, "record_kind", context, &e->record_kind, err)) {
    return false;
  }
  if (e->record_kind == "mirror") e->record_kind.clear();
  uint64_t bitrate = 0;
  if (!OptionalUInt(v, "bitrate_kbps", context, &bitrate, err)) return false;
  if (bitrate > 0xffffffffull) {
    *err = "bitrate_kbps out of range in " + context;
    return false;
  }
  e->bitrate_kbps = static_cast<uint32_t>(bitrate);
  if (!OptionalUInt(v, "avg_frame_bytes", context, &e->avg_frame_bytes, err)) {
    return false;
  }
  if (!OptionalUInt(v, "max_frame_bytes", context, &e->max_frame_bytes, err)) {
    return false;
  }
  if (!OptionalString(v, "record_shm", context, &e->record_shm, err)) {
    return false;
  }
  if (!OptionalBool(v, "dynamic", context, &e->dynamic, err)) return false;
  if (!OptionalBool(v, "mp_gate", context, &e->mp_gate, err)) return false;
  if (!OptionalBool(v, "feedback", context, &e->feedback, err)) return false;
  if (!OptionalBool(v, "lossless", context, &e->lossless, err)) return false;
  uint64_t delay = 0;
  if (!OptionalUInt(v, "delay", context, &delay, err)) return false;
  e->delay_ns = delay;
  if (!OptionalUInt(v, "max_lag_ms", context, &e->max_lag_ms, err)) return false;
  if (!OptionalUInt(v, "before_ms", context, &e->before_ms, err)) return false;
  if (!OptionalUInt(v, "after_ms", context, &e->after_ms, err)) return false;
  if (!OptionalUInt(v, "per_frame_write_ms", context, &e->per_frame_write_ms,
                    err)) {
    return false;
  }
  if (!OptionalUInt(v, "pin_budget", context, &e->pin_budget, err)) return false;
  return true;
}

bool ParseProcess(const JVal& v, Process* p, std::string* err) {
  if (v.type != JVal::Type::kObj) {
    *err = "each process must be an object";
    return false;
  }
  if (!RequireString(v, "id", "process", &p->id, err)) return false;
  const std::string context = "process '" + p->id + "'";
  if (!CheckKeys(v, {"id", "role"}, context, err)) return false;
  if (!OptionalString(v, "role", context, &p->role, err)) return false;
  return true;
}

bool ParseGraphObject(const JVal& root, Graph* g, std::string* err) {
  if (root.type != JVal::Type::kObj) {
    *err = "the graph must be a JSON object";
    return false;
  }
  if (!CheckKeys(root,
                 {"version", "pool_n", "ring_cap", "phase_mode", "nodes",
                  "edges", "processes", "supervisor"},
                 "graph", err)) {
    return false;
  }
  const JVal* version = Find(root, "version");
  if (version == nullptr) {
    *err = "missing required key 'version' in graph";
    return false;
  }
  uint64_t ver = 0;
  if (!ExpectUInt(*version, "version in graph", &ver, err)) return false;
  if (ver != 1) {
    *err = "version must be 1";
    return false;
  }
  g->version = 1;
  uint64_t u64 = 0;
  if (const JVal* v = Find(root, "pool_n")) {
    if (!ExpectUInt(*v, "pool_n in graph", &u64, err)) return false;
    if (u64 == 0 || u64 > static_cast<uint64_t>(INT_MAX)) {
      *err = "pool_n in graph must be > 0";
      return false;
    }
    g->pool_n = static_cast<int>(u64);
  }
  if (const JVal* v = Find(root, "ring_cap")) {
    if (!ExpectUInt(*v, "ring_cap in graph", &u64, err)) return false;
    if (u64 == 0 || u64 > static_cast<uint64_t>(INT_MAX)) {
      *err = "ring_cap in graph must be > 0";
      return false;
    }
    g->ring_cap = static_cast<int>(u64);
  }
  if (!OptionalString(root, "phase_mode", "graph", &g->phase_mode, err)) {
    return false;
  }
  if (!g->phase_mode.empty() && !IsValidPhaseMode(g->phase_mode)) {
    *err = "phase_mode must be off|best_effort|require";
    return false;
  }

  const JVal* nodes = Find(root, "nodes");
  if (nodes == nullptr) {
    *err = "missing required key 'nodes' in graph";
    return false;
  }
  if (nodes->type != JVal::Type::kArr) {
    *err = "nodes in graph must be an array";
    return false;
  }
  for (const JVal& item : nodes->arr) {
    Node n;
    if (!ParseNode(item, &n, err)) return false;
    g->nodes.push_back(std::move(n));
  }

  if (const JVal* edges = Find(root, "edges")) {
    if (edges->type != JVal::Type::kArr) {
      *err = "edges in graph must be an array";
      return false;
    }
    for (const JVal& item : edges->arr) {
      Edge e;
      if (!ParseEdge(item, &e, err)) return false;
      g->edges.push_back(std::move(e));
    }
  }

  if (const JVal* procs = Find(root, "processes")) {
    if (procs->type != JVal::Type::kArr) {
      *err = "processes in graph must be an array";
      return false;
    }
    for (const JVal& item : procs->arr) {
      Process p;
      if (!ParseProcess(item, &p, err)) return false;
      g->processes.push_back(std::move(p));
    }
  }

  if (const JVal* sup = Find(root, "supervisor")) {
    if (sup->type != JVal::Type::kObj) {
      *err = "supervisor in graph must be an object";
      return false;
    }
    if (!CheckKeys(*sup, {"shm_prefix", "health_n"}, "supervisor", err)) {
      return false;
    }
    if (!OptionalString(*sup, "shm_prefix", "supervisor", &g->shm_prefix,
                        err)) {
      return false;
    }
    if (!OptionalPositiveInt(*sup, "health_n", "supervisor", &g->health_n,
                             err)) {
      return false;
    }
  }
  return true;
}

// ---- Graph -> canonical yaml --------------------------------------------

std::string QuoteYaml(const std::string& value) { return "\"" + value + "\""; }

void AppendYamlNode(const Node& n, std::ostringstream* os) {
  *os << "  - id: " << QuoteYaml(n.id) << '\n';
  *os << "    type: " << QuoteYaml(n.type) << '\n';
  *os << "    hz: " << n.hz << '\n';
  if (!n.process.empty()) *os << "    process: " << QuoteYaml(n.process) << '\n';
  if (!n.cpu_set.empty()) {
    *os << "    cpu_set: [";
    for (std::size_t i = 0; i < n.cpu_set.size(); ++i) {
      if (i != 0) *os << ", ";
      *os << n.cpu_set[i];
    }
    *os << "]\n";
  }
  if (!n.sched_policy.empty()) {
    *os << "    sched_policy: " << QuoteYaml(n.sched_policy) << '\n';
  }
  if (n.priority != -1) *os << "    priority: " << n.priority << '\n';
  if (!n.placement_fail.empty()) {
    *os << "    placement_fail: " << QuoteYaml(n.placement_fail) << '\n';
  }
  if (!n.wait_inputs.empty()) {
    *os << "    wait_inputs: " << QuoteYaml(n.wait_inputs) << '\n';
  }
  if (!n.exec.empty()) *os << "    exec: " << QuoteYaml(n.exec) << '\n';
  if (!n.publish.empty()) *os << "    publish: " << QuoteYaml(n.publish) << '\n';
  if (n.deadline_ns != 0) *os << "    deadline_ns: " << n.deadline_ns << '\n';
  if (n.wcet_ns != 0) *os << "    wcet_ns: " << n.wcet_ns << '\n';
  if (!n.produce.empty()) *os << "    produce: " << n.produce << '\n';
  if (n.input_budget != 0) *os << "    input_budget: " << n.input_budget << '\n';
  if (!n.overrun.empty()) *os << "    overrun: " << QuoteYaml(n.overrun) << '\n';
  if (n.threads != 1) *os << "    threads: " << n.threads << '\n';
}

void AppendYamlEdge(const Edge& e, std::ostringstream* os) {
  *os << "  - from: " << QuoteYaml(e.from) << '\n';
  *os << "    to: " << QuoteYaml(e.to) << '\n';
  *os << "    channel: " << QuoteYaml(e.channel) << '\n';
  if (!e.cpp_type.empty()) *os << "    cpp_type: " << QuoteYaml(e.cpp_type) << '\n';
  if (!e.pool.empty()) *os << "    pool: " << QuoteYaml(e.pool) << '\n';
  if (!e.transport.empty()) {
    *os << "    transport: " << QuoteYaml(e.transport) << '\n';
  }
  if (!e.shm.empty()) *os << "    shm: " << QuoteYaml(e.shm) << '\n';
  if (!e.delivery.empty()) {
    *os << "    delivery: " << QuoteYaml(e.delivery) << '\n';
  }
  if (!e.read.empty()) *os << "    read: " << QuoteYaml(e.read) << '\n';
  if (!e.record_priority.empty()) {
    *os << "    record_priority: " << QuoteYaml(e.record_priority) << '\n';
  }
  if (e.depth != 0) *os << "    depth: " << e.depth << '\n';
  if (!e.record) *os << "    record: false\n";
  if (!e.record_gate) *os << "    record_gate: false\n";
  if (!e.record_kind.empty()) {
    *os << "    record_kind: " << QuoteYaml(e.record_kind) << '\n';
  }
  if (e.bitrate_kbps != 0) *os << "    bitrate_kbps: " << e.bitrate_kbps << '\n';
  if (e.avg_frame_bytes != 0) {
    *os << "    avg_frame_bytes: " << e.avg_frame_bytes << '\n';
  }
  if (e.max_frame_bytes != 0) {
    *os << "    max_frame_bytes: " << e.max_frame_bytes << '\n';
  }
  if (!e.record_shm.empty()) {
    *os << "    record_shm: " << QuoteYaml(e.record_shm) << '\n';
  }
  if (e.dynamic) *os << "    dynamic: true\n";
  if (e.mp_gate) *os << "    mp_gate: true\n";
  if (e.feedback) *os << "    feedback: true\n";
  if (e.delay_ns != 0) *os << "    delay: " << e.delay_ns << '\n';
  if (e.lossless) *os << "    lossless: true\n";
  if (e.max_lag_ms != 0) *os << "    max_lag_ms: " << e.max_lag_ms << '\n';
  if (e.before_ms != 0) *os << "    before_ms: " << e.before_ms << '\n';
  if (e.after_ms != 0) *os << "    after_ms: " << e.after_ms << '\n';
  if (e.per_frame_write_ms != 0) {
    *os << "    per_frame_write_ms: " << e.per_frame_write_ms << '\n';
  }
  if (e.pin_budget != 0) *os << "    pin_budget: " << e.pin_budget << '\n';
}

}  // namespace

std::string GraphToCanonicalJson(const Graph& g) {
  JVal root = JObj();
  Put(&root, "version", JNum(static_cast<uint64_t>(g.version)));
  if (g.pool_n > 0) Put(&root, "pool_n", JNum(static_cast<uint64_t>(g.pool_n)));
  if (g.ring_cap > 0) {
    Put(&root, "ring_cap", JNum(static_cast<uint64_t>(g.ring_cap)));
  }
  if (!g.phase_mode.empty()) Put(&root, "phase_mode", JStr(g.phase_mode));

  JVal nodes = JArr();
  for (const Node& n : g.nodes) {
    JVal j = JObj();
    Put(&j, "id", JStr(n.id));
    Put(&j, "type", JStr(n.type));
    Put(&j, "hz", JNum(static_cast<uint64_t>(n.hz)));
    if (!n.process.empty()) Put(&j, "process", JStr(n.process));
    if (!n.cpu_set.empty()) {
      JVal cpus = JArr();
      for (int cpu : n.cpu_set) cpus.arr.push_back(JInt(cpu));
      Put(&j, "cpu_set", std::move(cpus));
    }
    if (!n.sched_policy.empty()) Put(&j, "sched_policy", JStr(n.sched_policy));
    if (n.priority != -1) Put(&j, "priority", JInt(n.priority));
    if (!n.placement_fail.empty()) {
      Put(&j, "placement_fail", JStr(n.placement_fail));
    }
    if (!n.wait_inputs.empty()) Put(&j, "wait_inputs", JStr(n.wait_inputs));
    if (!n.exec.empty()) Put(&j, "exec", JStr(n.exec));
    if (!n.publish.empty()) Put(&j, "publish", JStr(n.publish));
    if (n.deadline_ns != 0) Put(&j, "deadline_ns", JNum(n.deadline_ns));
    if (n.wcet_ns != 0) Put(&j, "wcet_ns", JNum(n.wcet_ns));
    if (n.input_budget != 0) Put(&j, "input_budget", JNum(n.input_budget));
    if (!n.overrun.empty()) Put(&j, "overrun", JStr(n.overrun));
    if (n.threads != 1) Put(&j, "threads", JNum(n.threads));
    nodes.arr.push_back(std::move(j));
  }
  Put(&root, "nodes", std::move(nodes));

  JVal edges = JArr();
  for (const Edge& e : g.edges) {
    JVal j = JObj();
    Put(&j, "from", JStr(e.from));
    Put(&j, "to", JStr(e.to));
    Put(&j, "channel", JStr(e.channel));
    if (!e.cpp_type.empty()) Put(&j, "cpp_type", JStr(e.cpp_type));
    if (!e.pool.empty()) Put(&j, "pool", JStr(e.pool));
    if (!e.transport.empty()) Put(&j, "transport", JStr(e.transport));
    if (!e.shm.empty()) Put(&j, "shm", JStr(e.shm));
    if (!e.delivery.empty()) Put(&j, "delivery", JStr(e.delivery));
    if (!e.read.empty()) Put(&j, "read", JStr(e.read));
    if (!e.record_priority.empty()) {
      Put(&j, "record_priority", JStr(e.record_priority));
    }
    if (e.depth != 0) Put(&j, "depth", JNum(static_cast<uint64_t>(e.depth)));
    if (!e.record) Put(&j, "record", JBool(false));
    if (!e.record_gate) Put(&j, "record_gate", JBool(false));
    if (!e.record_kind.empty()) Put(&j, "record_kind", JStr(e.record_kind));
    if (e.bitrate_kbps != 0) {
      Put(&j, "bitrate_kbps", JNum(e.bitrate_kbps));
    }
    if (e.avg_frame_bytes != 0) {
      Put(&j, "avg_frame_bytes", JNum(e.avg_frame_bytes));
    }
    if (e.max_frame_bytes != 0) {
      Put(&j, "max_frame_bytes", JNum(e.max_frame_bytes));
    }
    if (!e.record_shm.empty()) Put(&j, "record_shm", JStr(e.record_shm));
    if (e.dynamic) Put(&j, "dynamic", JBool(true));
    if (e.mp_gate) Put(&j, "mp_gate", JBool(true));
    if (e.feedback) Put(&j, "feedback", JBool(true));
    if (e.delay_ns != 0) Put(&j, "delay", JNum(e.delay_ns));
    if (e.lossless) Put(&j, "lossless", JBool(true));
    if (e.max_lag_ms != 0) Put(&j, "max_lag_ms", JNum(e.max_lag_ms));
    if (e.before_ms != 0) Put(&j, "before_ms", JNum(e.before_ms));
    if (e.after_ms != 0) Put(&j, "after_ms", JNum(e.after_ms));
    if (e.per_frame_write_ms != 0) {
      Put(&j, "per_frame_write_ms", JNum(e.per_frame_write_ms));
    }
    if (e.pin_budget != 0) Put(&j, "pin_budget", JNum(e.pin_budget));
    edges.arr.push_back(std::move(j));
  }
  Put(&root, "edges", std::move(edges));

  if (!g.processes.empty()) {
    JVal procs = JArr();
    for (const Process& p : g.processes) {
      JVal j = JObj();
      Put(&j, "id", JStr(p.id));
      if (!p.role.empty()) Put(&j, "role", JStr(p.role));
      procs.arr.push_back(std::move(j));
    }
    Put(&root, "processes", std::move(procs));
  }

  if (!g.shm_prefix.empty() || g.health_n > 0) {
    JVal sup = JObj();
    if (!g.shm_prefix.empty()) Put(&sup, "shm_prefix", JStr(g.shm_prefix));
    if (g.health_n > 0) {
      Put(&sup, "health_n", JNum(static_cast<uint64_t>(g.health_n)));
    }
    Put(&root, "supervisor", std::move(sup));
  }

  std::string out;
  Serialize(root, 0, &out);
  out += '\n';
  return out;
}

std::string GraphToCanonicalYaml(const Graph& g) {
  std::ostringstream os;
  os << "version: 1\n";
  if (g.pool_n > 0) os << "pool_n: " << g.pool_n << '\n';
  if (g.ring_cap > 0) os << "ring_cap: " << g.ring_cap << '\n';
  if (!g.phase_mode.empty()) os << "phase_mode: " << g.phase_mode << '\n';
  os << "nodes:\n";
  for (const Node& n : g.nodes) AppendYamlNode(n, &os);
  os << "edges:\n";
  for (const Edge& e : g.edges) AppendYamlEdge(e, &os);
  if (!g.processes.empty()) {
    os << "processes:\n";
    for (const Process& p : g.processes) {
      os << "  - id: " << QuoteYaml(p.id) << '\n';
      if (!p.role.empty()) os << "    role: " << QuoteYaml(p.role) << '\n';
    }
  }
  if (!g.shm_prefix.empty() || g.health_n > 0) {
    os << "supervisor:\n";
    if (!g.shm_prefix.empty()) {
      os << "  shm_prefix: " << QuoteYaml(g.shm_prefix) << '\n';
    }
    if (g.health_n > 0) os << "  health_n: " << g.health_n << '\n';
  }
  return os.str();
}

bool LoadGraphJsonFromString(const std::string& text, Graph* out,
                             const std::string& source) {
  if (out == nullptr) return false;
  JVal root;
  std::string err;
  JsonParser parser(text);
  if (!parser.Parse(&root, &err)) {
    std::fprintf(stderr, "%s: %s\n", source.c_str(), err.c_str());
    return false;
  }
  Graph parsed;
  if (!ParseGraphObject(root, &parsed, &err)) {
    std::fprintf(stderr, "%s: %s\n", source.c_str(), err.c_str());
    return false;
  }
  const std::string yaml = GraphToCanonicalYaml(parsed);
  Graph loaded;
  if (!LoadGraphYamlFromString(yaml, &loaded, source)) return false;
  *out = std::move(loaded);
  return true;
}

bool LoadGraphJson(const std::string& path, Graph* out) {
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "LoadGraphJson: cannot open %s\n", path.c_str());
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return LoadGraphJsonFromString(ss.str(), out, path);
}
