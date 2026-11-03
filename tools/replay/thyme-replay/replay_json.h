#pragma once

// replay_json.h — the tiny streaming JSON emitter shared by the CLI commands.
//
// Values are written as they are produced (no document is ever buffered), so
// the same emitter serves list/info/verify summaries, the live status file and
// the NDJSON export. It is deliberately not a parser: the CLI only ever reads
// the status file it wrote itself (RunStatus echoes it for --json).

#include <cmath>
#include <cstdio>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

namespace thyme {
namespace replaycli {

class Json {
 public:
  explicit Json(std::ostream& out) : out_(out) {}

  void BeginObject() { Open('{'); }
  void EndObject() { Close('}'); }
  void BeginArray() { Open('['); }
  void EndArray() { Close(']'); }

  void Key(const char* key) {
    Separator();
    out_ << '"' << Escape(key) << "\":";
    after_key_ = true;
  }
  void Str(const std::string& v) {
    ValuePrefix();
    out_ << '"' << Escape(v) << '"';
  }
  void U64(uint64_t v) {
    ValuePrefix();
    out_ << v;
  }
  void I64(int64_t v) {
    ValuePrefix();
    out_ << v;
  }
  void Double(double v) {
    ValuePrefix();
    if (!std::isfinite(v)) {
      out_ << "null";
      return;
    }
    out_ << std::fixed << std::setprecision(3) << v;
  }
  void Bool(bool v) {
    ValuePrefix();
    out_ << (v ? "true" : "false");
  }
  void Null() {
    ValuePrefix();
    out_ << "null";
  }
  void KeyStr(const char* k, const std::string& v) {
    Key(k);
    Str(v);
  }
  void KeyU64(const char* k, uint64_t v) {
    Key(k);
    U64(v);
  }
  void KeyBool(const char* k, bool v) {
    Key(k);
    Bool(v);
  }
  void KeyDouble(const char* k, double v) {
    Key(k);
    Double(v);
  }
  void KeyNull(const char* k) {
    Key(k);
    Null();
  }

 private:
  static std::string Escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (const char c : s) {
      switch (c) {
        case '"':
          o += "\\\"";
          break;
        case '\\':
          o += "\\\\";
          break;
        case '\n':
          o += "\\n";
          break;
        case '\r':
          o += "\\r";
          break;
        case '\t':
          o += "\\t";
          break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x",
                          static_cast<unsigned>(static_cast<unsigned char>(c)));
            o += buf;
          } else {
            o += c;
          }
      }
    }
    return o;
  }

  void Open(char c) {
    Separator();
    out_ << c;
    stack_.push_back(true);
    after_key_ = false;
  }
  void Close(char c) {
    if (!stack_.empty()) {
      stack_.pop_back();
    }
    out_ << '\n' << c;
    after_key_ = false;
    // A closed container counts as a completed value in its parent.
    if (!stack_.empty()) {
      stack_.back() = false;
    }
  }
  void ValuePrefix() {
    if (!after_key_) {
      Separator();
    }
    after_key_ = false;
  }
  void Separator() {
    if (after_key_) {
      return;
    }
    if (!stack_.empty()) {
      if (!stack_.back()) {
        out_ << ',';
      }
      out_ << '\n';
      stack_.back() = false;
    }
  }

  std::ostream& out_;
  std::vector<bool> stack_;  // true = first element pending
  bool after_key_ = false;
};

}  // namespace replaycli
}  // namespace thyme
