#include "log_merge.h"

#include "thyme/record/record_schema.h"  // ABI-10 manifest version
#include "thyme/runtime/histogram.h"
#include "thyme/runtime/perf_log.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <utility>

namespace thyme {
namespace logmerge {

namespace {

// ---------------- text (JSONL) source ----------------

// Extracts the "ts":<u64> field. The fsm writer emits "ts":N with no
// spaces; a small amount of whitespace around the colon is tolerated.
bool ExtractTs(const std::string& line, uint64_t* out) {
  const std::size_t p = line.find("\"ts\"");
  if (p == std::string::npos) return false;
  std::size_t i = p + 4;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  if (i >= line.size() || line[i] != ':') return false;
  ++i;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  if (i >= line.size() || line[i] < '0' || line[i] > '9') return false;
  uint64_t v = 0;
  while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
    const uint64_t nv = v * 10 + static_cast<uint64_t>(line[i] - '0');
    if (nv < v) return false;  // overflow: not a plausible timestamp
    v = nv;
    ++i;
  }
  *out = v;
  return true;
}

class JsonlSource final : public Source {
 public:
  JsonlSource(FILE* fp, uint32_t window) : fp_(fp), window_(window) {}
  ~JsonlSource() override {
    if (fp_ != nullptr) std::fclose(fp_);
  }

  bool Next(Event* ev) override {
    while (head_ == buf_.size()) {
      if (!Fill()) return false;
      head_ = 0;
    }
    *ev = std::move(buf_[head_++]);
    return true;
  }

 private:
  bool Fill() {
    buf_.clear();
    if (eof_) return false;
    std::string line;
    line.reserve(256);
    while (buf_.size() < window_) {
      line.clear();
      int c;
      while ((c = std::fgetc(fp_)) != EOF && c != '\n') line.push_back(static_cast<char>(c));
      if (c == EOF) {
        eof_ = true;
        if (line.empty()) break;
      }
      Event e;
      if (ExtractTs(line, &e.ts_ns)) {
        e.kind = Kind::kFsm;
        e.raw = std::move(line);
        buf_.push_back(std::move(e));
        line.clear();
      } else {
        ++skipped_;
      }
      if (eof_) break;
    }
    if (buf_.empty()) return false;
    std::stable_sort(buf_.begin(), buf_.end(),
                     [](const Event& a, const Event& b) {
                       return a.ts_ns < b.ts_ns;
                     });
    return true;
  }

  FILE* fp_;
  uint32_t window_;
  std::vector<Event> buf_;
  std::size_t head_ = 0;
  bool eof_ = false;
};

// ---------------- T9P1 perf binary source ----------------

constexpr uint32_t kTsShift = 14;   // perf_log.h PerfSampler::kTsShift
constexpr uint32_t kLatShift = 13;  // 8.192us latency unit
constexpr uint32_t kDurShift = 10;  // 1.024us work-duration unit

uint32_t RdU24(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16);
}
uint32_t RdU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t RdU64(const uint8_t* p) {
  return RdU32(p) | (static_cast<uint64_t>(RdU32(p + 4)) << 32);
}

// ABI-10: record-manifest scan across every input. The block is build-wide
// (node byte 0), so all inputs of one build must agree; a disagreement means
// the logs were produced by different recorded-channel layouts and their
// RecordStructIds are not comparable.
class ManifestSink {
 public:
  void Add(uint16_t version, uint64_t hash) {
    if (declared_ && (version != version_ || hash != hash_)) conflict_ = true;
    if (!declared_) {
      declared_ = true;
      version_ = version;
      hash_ = hash;
    }
    ++files_;
  }

  bool declared() const { return declared_; }
  bool conflict() const { return conflict_; }
  uint64_t hash() const { return hash_; }
  uint16_t version() const { return version_; }
  uint32_t files() const { return files_; }

 private:
  bool declared_ = false;
  bool conflict_ = false;
  uint64_t hash_ = 0;
  uint16_t version_ = 0;
  uint32_t files_ = 0;
};

// OBS-09: cross-process histogram aggregation (--hist). One entry per
// (node, series) across every input file: bucket counts are summed (valid for
// percentiles — proportions add), max is merged with max. Percentiles are
// recomputed from the merged buckets with the runtime's own nearest-rank
// rule, so offline numbers are exactly what the sampler would report for the
// concatenated windows. Methods are defined below with the other renderers.
struct NodeBudget {
  uint64_t period_ns = 0;
  uint64_t deadline_ns = 0;
};

class HistSink {
 public:
  HistSink(uint64_t from_ns, uint64_t to_ns) : from_(from_ns), to_(to_ns) {}

  // counts_le points at LatencyHistogram::kBuckets little-endian u32 counts.
  void Add(uint8_t node, uint8_t series, uint64_t end_ts, uint64_t n,
           uint64_t max_ns, const uint8_t* counts_le);
  void AddBudget(uint8_t node, const NodeBudget& budget) {
    budgets_[node] = budget;
  }

  std::size_t size() const { return aggs_.size(); }
  void Print(FILE* out, bool json) const;

 private:
  using Key = std::pair<uint8_t, uint8_t>;
  struct Agg {
    uint64_t max_ns = 0;
    std::vector<uint64_t> buckets;
  };
  std::map<Key, Agg> aggs_;
  std::map<uint8_t, NodeBudget> budgets_;
  uint64_t from_;
  uint64_t to_;
};

class PerfSource final : public Source {
 public:
  // A single dump window can hold a full rate-sized ring (<= 1<<18 samples
  // per block type); split windows larger than this (documented limit).
  static constexpr std::size_t kMaxWindowEvents = 1 << 20;

  explicit PerfSource(FILE* fp, HistSink* sink = nullptr)
      : fp_(fp), sink_(sink) {}
  ~PerfSource() override {
    if (fp_ != nullptr) std::fclose(fp_);
  }

  bool Next(Event* ev) override {
    while (head_ == buf_.size()) {
      if (!Fill()) return false;
      head_ = 0;
    }
    *ev = std::move(buf_[head_++]);
    return true;
  }

 private:
  struct Epoch {
    uint64_t base = 0;
  };

  // u32 sample ts -> u64 ns. The per-window epoch block carries the high
  // bits at dump time and is authoritative (one dump window is always far
  // shorter than the ~19.5h wrap, so no intra-window regression heuristic is
  // needed — blocks are grouped by id, so raw lows DO regress legitimately
  // within a window). Files without epoch blocks (legacy single-Flush dumps)
  // expand with base 0.
  uint64_t Expand(uint8_t node, uint32_t low) {
    const Epoch& e = epochs_[node];
    return (e.base + low) << kTsShift;
  }

  struct ClockAnchor {
    uint64_t mono_ns = 0;
    uint64_t realtime_ns = 0;
  };

  bool SetClockAnchor(uint64_t mono_ns, uint64_t realtime_ns) {
    if (have_clock_anchor_ && mono_ns < clock_anchor_.mono_ns) return false;
    clock_anchor_ = ClockAnchor{mono_ns, realtime_ns};
    have_clock_anchor_ = true;
    return true;
  }

  void ApplyClockAnchor(Event* e) const {
    if (!have_clock_anchor_) return;
    uint64_t realtime_ns = 0;
    if (e->ts_ns >= clock_anchor_.mono_ns) {
      const uint64_t delta = e->ts_ns - clock_anchor_.mono_ns;
      if (clock_anchor_.realtime_ns >
          std::numeric_limits<uint64_t>::max() - delta) {
        return;
      }
      realtime_ns = clock_anchor_.realtime_ns + delta;
    } else {
      const uint64_t delta = clock_anchor_.mono_ns - e->ts_ns;
      if (clock_anchor_.realtime_ns < delta) return;
      realtime_ns = clock_anchor_.realtime_ns - delta;
    }
    e->has_realtime_ns = true;
    e->realtime_ns = realtime_ns;
  }

  void AppendEvent(Event* e) {
    ApplyClockAnchor(e);
    buf_.push_back(std::move(*e));
  }

  // Reads exactly n bytes; false on short read (EOF or crash-truncated
  // tail — both end this source cleanly).
  bool Read(void* dst, std::size_t n) {
    return std::fread(dst, 1, n, fp_) == n;
  }

  void Annotate(Event* e) const {
    const auto budget = budgets_.find(static_cast<uint8_t>(e->f[0]));
    if (budget != budgets_.end()) {
      e->has_node_budget = true;
      e->period_ns = budget->second.period_ns;
      e->deadline_ns = budget->second.deadline_ns;
    }
    if (e->kind == Kind::kPerfSys) {
      const auto name = series_names_.find(
          {static_cast<uint8_t>(e->f[0]), static_cast<uint8_t>(e->f[1])});
      if (name != series_names_.end()) e->series_name = name->second;
    }
  }

  // Parses one block, appending its events to buf_. Returns false at clean
  // EOF or on a truncated/unknown tail. An epoch block marks a window
  // boundary (sets window_break_ after applying the new base).
  bool ParseOneBlock() {
    uint8_t hdr[6];
    for (;;) {
      uint8_t peek[4];
      if (!Read(peek, 4)) return false;
      // A repeated magic means the file was re-opened for append (new
      // process run): skip it; epoch state carries on.
      if (std::memcmp(peek, "T9P1", 4) == 0) continue;
      hdr[0] = peek[0];
      hdr[1] = peek[1];
      hdr[2] = peek[2];
      hdr[3] = peek[3];
      if (!Read(hdr + 4, 2)) return false;
      break;
    }
    const uint8_t type = hdr[0];
    const uint8_t node = hdr[1];
    const uint32_t cnt = RdU32(&hdr[2]);
    std::size_t rec_size = 0;
    switch (type) {
      case 1: case 3: case 5: rec_size = 10; break;
      case 2: rec_size = 20; break;
      case 4: rec_size = 8; break;
      case kPerfHistogramBlockType: rec_size = kPerfHistRecordBytes; break;
      case kPerfManifestBlockType: rec_size = kPerfManifestRecordBytes; break;
      case kPerfSeriesNameBlockType: rec_size = kPerfSeriesNameRecordBytes; break;
      case kPerfNodeBudgetBlockType: rec_size = kPerfNodeBudgetRecordBytes; break;
      case kPerfClockAnchorBlockType: rec_size = kPerfClockAnchorRecordBytes; break;
      default: return false;  // unknown block: stop like any truncated tail
    }
    if (cnt > (1u << 22)) return false;  // corrupt count, not a real block
    std::vector<uint8_t> body(static_cast<std::size_t>(cnt) * rec_size);
    if (!body.empty() && !Read(body.data(), body.size())) return false;
    const uint8_t* p = body.data();
    if (type == 1) {  // latency samples
      for (uint32_t i = 0; i < cnt; ++i, p += 10) {
        Event e;
        e.kind = Kind::kPerfLat;
        e.ts_ns = Expand(node, RdU32(p));
        e.f[0] = node;
        e.f[1] = p[7];  // id
        e.f[2] = static_cast<uint64_t>(RdU24(p + 4)) << kLatShift;
        e.f[3] = p[8];  // io
        AppendEvent(&e);
      }
    } else if (type == 2) {  // percentile summaries (ts = window end)
      for (uint32_t i = 0; i < cnt; ++i, p += 20) {
        Event e;
        e.kind = Kind::kPerfPct;
        e.ts_ns = Expand(node, RdU32(p + 16));
        e.f[0] = node;
        e.f[1] = p[1];  // id
        e.f[2] = static_cast<uint64_t>(p[2]) | (static_cast<uint64_t>(p[3]) << 8);
        e.f[3] = static_cast<uint64_t>(RdU24(p + 4)) << kLatShift;   // p50
        e.f[4] = static_cast<uint64_t>(RdU24(p + 7)) << kLatShift;   // p95
        e.f[5] = static_cast<uint64_t>(RdU24(p + 10)) << kLatShift;  // p99
        e.f[6] = static_cast<uint64_t>(RdU24(p + 13)) << kLatShift;  // max
        AppendEvent(&e);
      }
    } else if (type == 3) {  // work records
      for (uint32_t i = 0; i < cnt; ++i, p += 10) {
        Event e;
        e.kind = Kind::kPerfWork;
        e.ts_ns = Expand(node, RdU32(p));
        e.f[0] = p[8];  // node (record carries it; header id is redundant)
        e.f[1] = static_cast<uint64_t>(RdU24(p + 4)) << kDurShift;
        e.f[2] = p[7];  // io
        e.f[3] = p[9];  // flags
        AppendEvent(&e);
      }
    } else if (type == 4) {  // epoch: u64 wrap base per record
      for (uint32_t i = 0; i < cnt; ++i, p += 8) {
        epochs_[node].base = RdU64(p);
      }
      window_break_ = true;
    } else if (type == 5) {  // proc/queue series
      for (uint32_t i = 0; i < cnt; ++i, p += 10) {
        Event e;
        e.kind = Kind::kPerfSys;
        e.ts_ns = Expand(node, RdU32(p));
        e.f[0] = node;
        e.f[1] = p[8];  // series
        e.f[2] = RdU32(p + 4);
        AppendEvent(&e);
      }
    } else if (type == kPerfSeriesNameBlockType) {
      for (uint32_t i = 0; i < cnt; ++i, p += kPerfSeriesNameRecordBytes) {
        std::size_t len = 0;
        while (len < kPerfSeriesNameBytes && p[1 + len] != 0) ++len;
        series_names_[{node, p[0]}] =
            std::string(reinterpret_cast<const char*>(p + 1), len);
      }
    } else if (type == kPerfNodeBudgetBlockType) {
      for (uint32_t i = 0; i < cnt; ++i, p += kPerfNodeBudgetRecordBytes) {
        const NodeBudget budget{RdU64(p), RdU64(p + 8)};
        budgets_[node] = budget;
        if (sink_ != nullptr) sink_->AddBudget(node, budget);
      }
    } else if (type == kPerfClockAnchorBlockType) {
      if (cnt != 1 || node != 0 ||
          !SetClockAnchor(RdU64(p), RdU64(p + 8))) {
        return false;
      }
    } else if (type == kPerfManifestBlockType) {
      // ABI-10: 12-byte records (version u16 | reserved u16 | hash u64).
      // No samples; the manifest is collected by the pre-scan so a mismatch
      // is refused BEFORE a timeline is emitted (see Merge). A corrupt record
      // still stops parsing: an unreadable manifest block is a truncated
      // file, not something to skip.
      for (uint32_t i = 0; i < cnt; ++i, p += kPerfManifestRecordBytes) {
        const uint16_t version =
            static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
        const uint16_t reserved =
            static_cast<uint16_t>(p[2] | (static_cast<uint16_t>(p[3]) << 8));
        if (version == 0 || reserved != 0) return false;
      }
    } else {  // OBS-09 type 6: fixed-size histogram record
      if (cnt != 1) return false;
      const uint8_t tag = p[0];
      const uint8_t series = p[1];
      const uint16_t buckets =
          static_cast<uint16_t>(p[2] | (static_cast<uint16_t>(p[3]) << 8));
      // A different bucket count means a future wire revision: stop like any
      // unknown block instead of misreading the counts.
      if (tag != kPerfHistogramRecordTag ||
          buckets != LatencyHistogram::kBuckets) {
        return false;
      }
      const uint64_t n = RdU64(p + 4);
      const uint64_t max_ns = RdU64(p + 12);
      const uint32_t end_low = RdU32(p + 20);
      const uint8_t* counts = p + 24;
      const uint64_t end_ns = Expand(node, end_low);
      if (sink_ != nullptr) {
        sink_->Add(node, series, end_ns, n, max_ns, counts);
      }
      // Timeline event: percentiles recomputed from the buckets (the wire
      // stores counts, not percentile values, so they merge losslessly).
      uint64_t counts64[LatencyHistogram::kBuckets];
      for (uint32_t i = 0; i < LatencyHistogram::kBuckets; ++i) {
        counts64[i] = RdU32(counts + i * 4);
      }
      Event e;
      e.kind = Kind::kPerfHist;
      e.ts_ns = end_ns;
      e.f[0] = node;
      e.f[1] = series;
      e.f[2] = n;
      e.f[3] = LatencyHistogram::Percentile(counts64, n, 50, 100);
      e.f[4] = LatencyHistogram::Percentile(counts64, n, 90, 100);
      e.f[5] = LatencyHistogram::Percentile(counts64, n, 99, 100);
      e.f[6] = LatencyHistogram::Percentile(counts64, n, 999, 1000);
      e.f[7] = max_ns;
      AppendEvent(&e);
    }
    return true;
  }

  bool Fill() {
    buf_.clear();
    if (eof_) return false;
    while (buf_.size() < kMaxWindowEvents) {
      if (!ParseOneBlock()) {
        eof_ = true;
        break;
      }
      if (window_break_) {
        window_break_ = false;
        if (!buf_.empty()) break;  // epoch starts the next window
      }
    }
    if (buf_.empty()) return false;
    for (Event& e : buf_) Annotate(&e);
    std::stable_sort(buf_.begin(), buf_.end(),
                     [](const Event& a, const Event& b) {
                       return a.ts_ns < b.ts_ns;
                     });
    return true;
  }

  FILE* fp_;
  HistSink* sink_ = nullptr;
  Epoch epochs_[256]{};
  ClockAnchor clock_anchor_{};
  bool have_clock_anchor_ = false;
  std::map<std::pair<uint8_t, uint8_t>, std::string> series_names_;
  std::map<uint8_t, NodeBudget> budgets_;
  std::vector<Event> buf_;
  std::size_t head_ = 0;
  bool window_break_ = false;
  bool eof_ = false;
};

void JsonEscape(std::string* out, const char* s) {
  for (const char* p = s; *p != '\0'; ++p) {
    if (*p == '"' || *p == '\\') out->push_back('\\');
    out->push_back(*p);
  }
}

void AppendU(std::string* out, uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
  *out += buf;
}

// OBS-09 aggregation sink (declared above with PerfSource).
void HistSink::Add(uint8_t node, uint8_t series, uint64_t end_ts, uint64_t n,
                   uint64_t max_ns, const uint8_t* counts_le) {
  if (end_ts < from_ || end_ts > to_) return;
  // The record's count field must equal the bucket sum; reject a record whose
  // counts were truncated or corrupted rather than skew the proportions.
  uint64_t sum = 0;
  for (uint32_t i = 0; i < LatencyHistogram::kBuckets; ++i) {
    sum += RdU32(counts_le + static_cast<std::size_t>(i) * 4);
  }
  if (sum != n) return;
  Agg& a = aggs_[Key(node, series)];
  if (a.buckets.empty()) a.buckets.assign(LatencyHistogram::kBuckets, 0);
  a.max_ns = std::max(a.max_ns, max_ns);
  for (uint32_t i = 0; i < LatencyHistogram::kBuckets; ++i) {
    a.buckets[i] += RdU32(counts_le + static_cast<std::size_t>(i) * 4);
  }
}

void HistSink::Print(FILE* out, bool json) const {
  for (const auto& kv : aggs_) {
    const uint64_t node = kv.first.first;
    const uint64_t series = kv.first.second;
    const Agg& a = kv.second;
    uint64_t total = 0;
    for (uint64_t c : a.buckets) total += c;
    const uint64_t p50 = LatencyHistogram::Percentile(
        a.buckets.data(), total, 50, 100);
    const uint64_t p90 = LatencyHistogram::Percentile(
        a.buckets.data(), total, 90, 100);
    const uint64_t p99 = LatencyHistogram::Percentile(
        a.buckets.data(), total, 99, 100);
    const uint64_t p999 = LatencyHistogram::Percentile(
        a.buckets.data(), total, 999, 1000);
    std::string line;
    if (json) {
      line += "{\"node\":";
      AppendU(&line, node);
      line += ",\"series\":";
      AppendU(&line, series);
      line += ",\"n\":";
      AppendU(&line, total);
    } else {
      line += "node=";
      AppendU(&line, node);
      line += " series=";
      AppendU(&line, series);
      line += " n=";
      AppendU(&line, total);
    }
    const char* keys[4] = {"p50_ns", "p90_ns", "p99_ns", "p999_ns"};
    const uint64_t vals[4] = {p50, p90, p99, p999};
    for (int i = 0; i < 4; ++i) {
      if (json) {
        line += ",\"";
        line += keys[i];
        line += "\":";
      } else {
        line += ' ';
        line += keys[i];
        line += '=';
      }
      AppendU(&line, vals[i]);
    }
    if (json) {
      line += ",\"max_ns\":";
    } else {
      line += " max_ns=";
    }
    AppendU(&line, a.max_ns);
    const auto budget = budgets_.find(kv.first.first);
    if (budget != budgets_.end()) {
      if (json) {
        line += ",\"period_ns\":";
        AppendU(&line, budget->second.period_ns);
        line += ",\"deadline_ns\":";
        AppendU(&line, budget->second.deadline_ns);
      } else {
        line += " period_ns=";
        AppendU(&line, budget->second.period_ns);
        line += " deadline_ns=";
        AppendU(&line, budget->second.deadline_ns);
      }
    }
    if (json) line.push_back('}');
    line.push_back('\n');
    std::fwrite(line.data(), 1, line.size(), out);
  }
}

std::unique_ptr<Source> OpenSourceWithSink(const char* path, std::string* err,
                                           uint32_t reorder_window,
                                           HistSink* sink) {
  FILE* fp = std::fopen(path, "rb");
  if (fp == nullptr) {
    *err = std::string("cannot open ") + path;
    return nullptr;
  }
  uint8_t magic[4];
  const std::size_t n = std::fread(magic, 1, 4, fp);
  std::rewind(fp);
  if (n == 4 && std::memcmp(magic, "T9P1", 4) == 0) {
    return std::unique_ptr<Source>(new PerfSource(fp, sink));
  }
  return std::unique_ptr<Source>(new JsonlSource(fp, reorder_window));
}

}  // namespace

std::unique_ptr<Source> OpenSource(const char* path, std::string* err,
                                   uint32_t reorder_window) {
  return OpenSourceWithSink(path, err, reorder_window, nullptr);
}

std::string RenderText(const Event& e, const char* src_name) {
  std::string out;
  AppendU(&out, e.ts_ns);
  out.push_back('\t');
  out += src_name;
  out.push_back('\t');
  switch (e.kind) {
    case Kind::kFsm:
      out += "fsm ";
      out += e.raw;
      break;
    case Kind::kPerfLat:
      out += "lat node=";
      AppendU(&out, e.f[0]);
      out += " id=";
      AppendU(&out, e.f[1]);
      out += " lat_ns=";
      AppendU(&out, e.f[2]);
      out += " io=";
      AppendU(&out, e.f[3]);
      break;
    case Kind::kPerfPct:
      out += "pct node=";
      AppendU(&out, e.f[0]);
      out += " id=";
      AppendU(&out, e.f[1]);
      out += " n=";
      AppendU(&out, e.f[2]);
      out += " p50_ns=";
      AppendU(&out, e.f[3]);
      out += " p95_ns=";
      AppendU(&out, e.f[4]);
      out += " p99_ns=";
      AppendU(&out, e.f[5]);
      out += " max_ns=";
      AppendU(&out, e.f[6]);
      break;
    case Kind::kPerfWork:
      out += "work node=";
      AppendU(&out, e.f[0]);
      out += " dur_ns=";
      AppendU(&out, e.f[1]);
      out += " io=";
      AppendU(&out, e.f[2]);
      out += " flags=";
      AppendU(&out, e.f[3]);
      break;
    case Kind::kPerfSys:
      out += "sys node=";
      AppendU(&out, e.f[0]);
      out += " series=";
      AppendU(&out, e.f[1]);
      out += " value=";
      AppendU(&out, e.f[2]);
      break;
    case Kind::kPerfHist:
      out += "hist node=";
      AppendU(&out, e.f[0]);
      out += " series=";
      AppendU(&out, e.f[1]);
      out += " n=";
      AppendU(&out, e.f[2]);
      out += " p50_ns=";
      AppendU(&out, e.f[3]);
      out += " p90_ns=";
      AppendU(&out, e.f[4]);
      out += " p99_ns=";
      AppendU(&out, e.f[5]);
      out += " p999_ns=";
      AppendU(&out, e.f[6]);
      out += " max_ns=";
      AppendU(&out, e.f[7]);
      break;
  }
  if (e.has_realtime_ns) {
    out += " realtime_ns=";
    AppendU(&out, e.realtime_ns);
  }
  if (!e.series_name.empty()) {
    out += " series_name=";
    out += e.series_name;
  }
  if (e.has_node_budget) {
    out += " period_ns=";
    AppendU(&out, e.period_ns);
    out += " deadline_ns=";
    AppendU(&out, e.deadline_ns);
  }
  return out;
}

std::string RenderJson(const Event& e, const char* src_name) {
  std::string out = "{\"ts\":";
  AppendU(&out, e.ts_ns);
  out += ",\"src\":\"";
  JsonEscape(&out, src_name);
  out += "\",\"kind\":\"";
  switch (e.kind) {
    case Kind::kFsm:
      out += "fsm\",\"event\":";
      out += e.raw;  // already a JSON object
      break;
    case Kind::kPerfLat:
      out += "lat\",\"node\":";
      AppendU(&out, e.f[0]);
      out += ",\"id\":";
      AppendU(&out, e.f[1]);
      out += ",\"lat_ns\":";
      AppendU(&out, e.f[2]);
      out += ",\"io\":";
      AppendU(&out, e.f[3]);
      break;
    case Kind::kPerfPct:
      out += "pct\",\"node\":";
      AppendU(&out, e.f[0]);
      out += ",\"id\":";
      AppendU(&out, e.f[1]);
      out += ",\"n\":";
      AppendU(&out, e.f[2]);
      out += ",\"p50_ns\":";
      AppendU(&out, e.f[3]);
      out += ",\"p95_ns\":";
      AppendU(&out, e.f[4]);
      out += ",\"p99_ns\":";
      AppendU(&out, e.f[5]);
      out += ",\"max_ns\":";
      AppendU(&out, e.f[6]);
      break;
    case Kind::kPerfWork:
      out += "work\",\"node\":";
      AppendU(&out, e.f[0]);
      out += ",\"dur_ns\":";
      AppendU(&out, e.f[1]);
      out += ",\"io\":";
      AppendU(&out, e.f[2]);
      out += ",\"flags\":";
      AppendU(&out, e.f[3]);
      break;
    case Kind::kPerfSys:
      out += "sys\",\"node\":";
      AppendU(&out, e.f[0]);
      out += ",\"series\":";
      AppendU(&out, e.f[1]);
      out += ",\"value\":";
      AppendU(&out, e.f[2]);
      break;
    case Kind::kPerfHist:
      out += "hist\",\"node\":";
      AppendU(&out, e.f[0]);
      out += ",\"series\":";
      AppendU(&out, e.f[1]);
      out += ",\"n\":";
      AppendU(&out, e.f[2]);
      out += ",\"p50_ns\":";
      AppendU(&out, e.f[3]);
      out += ",\"p90_ns\":";
      AppendU(&out, e.f[4]);
      out += ",\"p99_ns\":";
      AppendU(&out, e.f[5]);
      out += ",\"p999_ns\":";
      AppendU(&out, e.f[6]);
      out += ",\"max_ns\":";
      AppendU(&out, e.f[7]);
      break;
  }
  if (e.has_realtime_ns) {
    out += ",\"realtime_ns\":";
    AppendU(&out, e.realtime_ns);
  }
  if (!e.series_name.empty()) {
    out += ",\"series_name\":\"";
    JsonEscape(&out, e.series_name.c_str());
    out += "\"";
  }
  if (e.has_node_budget) {
    out += ",\"period_ns\":";
    AppendU(&out, e.period_ns);
    out += ",\"deadline_ns\":";
    AppendU(&out, e.deadline_ns);
  }
  out.push_back('}');
  return out;
}

namespace {

// ABI-10: refuse to merge logs produced under different record manifests.
// Returns 0 when the merge may proceed, 4 on a manifest conflict.
int CheckManifest(const ManifestSink& manifest, const Options& opt,
                  FILE* diag) {
  if (manifest.declared()) {
    if (diag != nullptr) {
      std::fprintf(diag,
                   "thyme_log_merge: record manifest hash=0x%016llx version=%u "
                   "(%u input(s))\n",
                   static_cast<unsigned long long>(manifest.hash()),
                   static_cast<unsigned>(manifest.version()),
                   static_cast<unsigned>(manifest.files()));
    }
    if (manifest.version() != schema::kManifestVersion) {
      if (diag != nullptr) {
        std::fprintf(diag,
                     "thyme_log_merge: unsupported record manifest version %u "
                     "(this build reads %u); refusing to join RecordStructIds\n",
                     static_cast<unsigned>(manifest.version()),
                     static_cast<unsigned>(schema::kManifestVersion));
      }
      return 4;
    }
    if (manifest.conflict()) {
      if (diag != nullptr) {
        std::fprintf(diag,
                     "thyme_log_merge: inputs were produced under different "
                     "record manifests; RecordStructId values are not "
                     "comparable (re-run with logs from one build)\n");
      }
      return 4;
    }
    if (opt.have_expect_hash && manifest.hash() != opt.expect_hash) {
      if (diag != nullptr) {
        std::fprintf(diag,
                     "thyme_log_merge: record manifest mismatch: logs=%s "
                     "expected=%s\n",
                     schema::FormatManifestHash(manifest.hash()).c_str(),
                     schema::FormatManifestHash(opt.expect_hash).c_str());
      }
      return 4;
    }
    return 0;
  }
  if (opt.have_expect_hash) {
    if (diag != nullptr) {
      std::fprintf(diag,
                   "thyme_log_merge: no record-manifest block in the inputs "
                   "(legacy logs); cannot verify --expect-hash\n");
    }
    return 4;
  }
  return 0;
}

// ABI-10 pre-scan: walk the T9P1 block headers of `path` and feed every
// record-manifest block (type 7) into `sink`, without materializing any
// samples. Returns false only on a read error; a non-T9P1 or truncated file
// simply contributes nothing here (the normal source open reports it).
bool ScanManifestBlocks(const std::string& path, ManifestSink* sink) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (fp == nullptr) return false;
  {
    uint8_t magic[4];
    if (std::fread(magic, 1, 4, fp) != 4 ||
        std::memcmp(magic, "T9P1", 4) != 0) {
      std::fclose(fp);
      return true;  // not a perf log: text inputs have no manifest block
    }
    std::fseek(fp, 4, SEEK_SET);
  }
  bool ok = true;
  for (;;) {
    uint8_t hdr[6];
    if (std::fread(hdr, 1, 4, fp) != 4) break;  // EOF / crash-truncated tail
    if (std::memcmp(hdr, "T9P1", 4) == 0) continue;  // re-opened append
    if (std::fread(hdr + 4, 1, 2, fp) != 2) break;
    const uint8_t type = hdr[0];
    const uint32_t cnt = RdU32(&hdr[2]);
    std::size_t rec = 0;
    switch (type) {
      case 1:
      case 3:
      case 5:
        rec = 10;
        break;
      case 2:
        rec = 20;
        break;
      case 4:
        rec = 8;
        break;
      case kPerfManifestBlockType:
        rec = kPerfManifestRecordBytes;
        break;
      case kPerfHistogramBlockType:
        rec = kPerfHistRecordBytes;
        break;
      case kPerfSeriesNameBlockType:
        rec = kPerfSeriesNameRecordBytes;
        break;
      case kPerfNodeBudgetBlockType:
        rec = kPerfNodeBudgetRecordBytes;
        break;
      case kPerfClockAnchorBlockType:
        rec = kPerfClockAnchorRecordBytes;
        break;
      default:
        std::fclose(fp);
        return ok;  // unknown block: same stop rule as the timeline parser
    }
    if (cnt > (1u << 22)) {
      std::fclose(fp);
      return ok;  // corrupt count: stop scanning, leave it to the parser
    }
    if (type == kPerfManifestBlockType) {
      for (uint32_t i = 0; i < cnt; ++i) {
        uint8_t rec_buf[kPerfManifestRecordBytes];
        if (std::fread(rec_buf, 1, sizeof(rec_buf), fp) != sizeof(rec_buf)) {
          ok = false;
          break;
        }
        const uint16_t version = static_cast<uint16_t>(
            rec_buf[0] | (static_cast<uint16_t>(rec_buf[1]) << 8));
        const uint16_t reserved = static_cast<uint16_t>(
            rec_buf[2] | (static_cast<uint16_t>(rec_buf[3]) << 8));
        if (reserved != 0 || version == 0) {
          ok = false;  // corrupt block: the timeline parser will reject it
          break;
        }
        sink->Add(version, RdU64(rec_buf + 4));
      }
      if (!ok) break;
    } else if (std::fseek(fp, static_cast<long>(cnt) * static_cast<long>(rec),
                          SEEK_CUR) != 0) {
      ok = false;
      break;
    }
  }
  std::fclose(fp);
  return ok;
}

}  // namespace

int Merge(const std::vector<std::string>& paths, const Options& opt,
          FILE* out, FILE* diag, ManifestReport* report) {
  HistSink sink(opt.from_ns, opt.to_ns);
  ManifestSink manifest;
  for (const std::string& p : paths) ScanManifestBlocks(p, &manifest);
  std::vector<std::unique_ptr<Source>> srcs;
  srcs.reserve(paths.size());
  for (const std::string& p : paths) {
    std::string err;
    std::unique_ptr<Source> s =
        OpenSourceWithSink(p.c_str(), &err, 65536, opt.hist ? &sink : nullptr);
    if (s == nullptr) {
      std::fprintf(stderr, "thyme_log_merge: %s\n", err.c_str());
      return 3;
    }
    srcs.push_back(std::move(s));
  }

  // OBS-09 --hist: drain every source (the histogram blocks are accumulated
  // into the sink during parsing) and print the merged per-series summary.
  if (opt.hist) {
    for (auto& src : srcs) {
      Event e;
      while (src->Next(&e)) {
      }
    }
    const int mrc = CheckManifest(manifest, opt, diag);
    if (report != nullptr) {
      report->declared = manifest.declared();
      report->conflict = manifest.conflict();
      report->hash = manifest.hash();
      report->version = manifest.version();
      report->files = manifest.files();
    }
    if (mrc != 0) return mrc;
    sink.Print(out, opt.json);
    if (diag != nullptr) {
      for (std::size_t i = 0; i < srcs.size(); ++i) {
        if (srcs[i]->skipped() != 0) {
          std::fprintf(
              diag, "thyme_log_merge: %s: %llu unparseable line(s) skipped\n",
              paths[i].c_str(),
              static_cast<unsigned long long>(srcs[i]->skipped()));
        }
      }
    }
    return 0;
  }

  // K-way merge over per-source heads (k is small; a linear scan beats a
  // heap here). Ties break by source index for a deterministic output.
  std::vector<Event> head(srcs.size());
  std::vector<bool> valid(srcs.size(), false);
  for (std::size_t i = 0; i < srcs.size(); ++i) {
    head[i].src = static_cast<uint32_t>(i);
    valid[i] = srcs[i]->Next(&head[i]);
    head[i].src = static_cast<uint32_t>(i);
  }
  for (;;) {
    std::size_t best = srcs.size();
    for (std::size_t i = 0; i < srcs.size(); ++i) {
      if (!valid[i]) continue;
      if (best == srcs.size() || head[i].ts_ns < head[best].ts_ns) best = i;
    }
    if (best == srcs.size()) break;
    const Event& e = head[best];
    if (e.ts_ns >= opt.from_ns && e.ts_ns <= opt.to_ns &&
        (!opt.sys_only || e.kind == Kind::kPerfSys)) {
      const std::string line = opt.json
                                   ? RenderJson(e, paths[e.src].c_str())
                                   : RenderText(e, paths[e.src].c_str());
      std::fwrite(line.data(), 1, line.size(), out);
      std::fputc('\n', out);
    }
    valid[best] = srcs[best]->Next(&head[best]);
    head[best].src = static_cast<uint32_t>(best);
  }

  if (diag != nullptr) {
    for (std::size_t i = 0; i < srcs.size(); ++i) {
      if (srcs[i]->skipped() != 0) {
        std::fprintf(diag, "thyme_log_merge: %s: %llu unparseable line(s) skipped\n",
                     paths[i].c_str(),
                     static_cast<unsigned long long>(srcs[i]->skipped()));
      }
    }
  }
  if (report != nullptr) {
    report->declared = manifest.declared();
    report->conflict = manifest.conflict();
    report->hash = manifest.hash();
    report->version = manifest.version();
    report->files = manifest.files();
  }
  return CheckManifest(manifest, opt, diag);
}

}  // namespace logmerge
}  // namespace thyme
