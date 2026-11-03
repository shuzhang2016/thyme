// test_thyme_log_merge.cpp — OBS-06: multi-process log merge.
//
// Covered:
//   * .fsm/JSONL merge across files with interleaved timestamps
//   * out-of-order lines within one file (corrected inside the reorder
//     window), unparseable lines skipped and counted
//   * --from/--to filtering, --json output shape
//   * T9P1 perf binaries merged with JSONL on one absolute axis (sample /
//     work / percentile / sys blocks, multi-window epoch handling)
//   * truncated / unknown-block tails end a perf source cleanly
//   * epoch wrap (u32 ts crossing 2^32 units) expands monotonically
//   * ARCH-15 clock anchors map each source independently to realtime_ns;
//     legacy/corrupt/overflow cases never invent wall-clock timestamps
//   * missing input -> exit code 3
#include "thyme/check.h"
#include "thyme/runtime/histogram.h"
#include "thyme/runtime/perf_dump.h"
#include "thyme/runtime/perf_log.h"
#include "log_merge.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

char g_dir[128];

std::string Path(const char* name) {
  return std::string(g_dir) + "/" + name;
}

void WriteText(const std::string& path, const char* content) {
  FILE* f = std::fopen(path.c_str(), "wb");
  CHECK(f != nullptr);
  CHECK(std::fwrite(content, 1, std::strlen(content), f) ==
        std::strlen(content));
  std::fclose(f);
}

void WriteBytes(const std::string& path, const PerfLogWriter& w) {
  FILE* f = std::fopen(path.c_str(), "wb");
  CHECK(f != nullptr);
  CHECK(std::fwrite(w.data(), 1, w.size(), f) == w.size());
  std::fclose(f);
}

void WriteSample(PerfLogWriter* w, uint32_t ts) {
  w->U8(1);
  w->U8(1);
  w->U32(1);
  w->U32(ts);
  w->U24(1);
  w->U8(1);
  w->U8(0);
  w->U8(0);
}

std::vector<std::string> RunMerge(const std::vector<std::string>& paths,
                                  logmerge::Options opt) {
  const std::string out_path = Path("out.txt");
  FILE* out = std::fopen(out_path.c_str(), "w+");
  CHECK(out != nullptr);
  CHECK(logmerge::Merge(paths, opt, out, stderr) == 0);
  std::fflush(out);
  std::rewind(out);
  std::vector<std::string> lines;
  char buf[8192];
  while (std::fgets(buf, sizeof(buf), out) != nullptr) {
    std::string l(buf);
    while (!l.empty() && l.back() == '\n') l.pop_back();
    lines.push_back(l);
  }
  std::fclose(out);
  return lines;
}

struct Row {
  uint64_t ts;
  std::string src;
  std::string payload;
};
std::vector<Row> ParseRows(const std::vector<std::string>& lines) {
  std::vector<Row> rows;
  for (const std::string& l : lines) {
    const std::size_t t1 = l.find('\t');
    const std::size_t t2 = l.find('\t', t1 + 1);
    CHECK(t1 != std::string::npos && t2 != std::string::npos);
    Row r;
    r.ts = std::strtoull(l.substr(0, t1).c_str(), nullptr, 10);
    r.src = l.substr(t1 + 1, t2 - t1 - 1);
    r.payload = l.substr(t2 + 1);
    rows.push_back(r);
  }
  return rows;
}

uint64_t Ns(uint64_t units) { return units << 14; }  // perf ts units -> ns

// True when `v` falls inside the bucket `expected` was recorded into (the
// honest check for bucket-resolution percentiles).
bool InBucket(uint64_t v, uint64_t expected) {
  const uint32_t b = LatencyHistogram::BucketOf(expected);
  return v >= LatencyHistogram::BucketLower(b) &&
         v <= LatencyHistogram::BucketUpper(b);
}

// Extracts a "<key>=" decimal field from a rendered line; ~0ull if absent.
uint64_t Field(const std::string& line, const char* key) {
  const std::size_t p = line.find(key);
  if (p == std::string::npos) return ~0ull;
  return std::strtoull(line.c_str() + p + std::strlen(key), nullptr, 10);
}

}  // namespace

int main() {
  std::snprintf(g_dir, sizeof(g_dir), "/tmp/t9merge_%d",
                static_cast<int>(getpid()));
  {
    std::string cmd = "mkdir -p ";
    cmd += g_dir;
    CHECK(system(cmd.c_str()) == 0);
  }

  // --- JSONL interleave + per-file disorder + skipped lines ------------------
  const std::string a = Path("a.fsm");
  const std::string b = Path("b.fsm");
  {
    WriteText(a,
              "{\"ts\":300,\"id\":1,\"from\":0,\"to\":1,\"why\":0}\n"
              "{\"ts\":100,\"id\":2,\"from\":1,\"to\":2,\"why\":0}\n"
              "not a json line at all\n"
              "{\"ts\":500,\"id\":3,\"from\":2,\"to\":1,\"why\":0}\n");
    WriteText(b,
              "{\"ts\":200,\"id\":4,\"from\":0,\"to\":1,\"why\":0}\n"
              "{\"ts\":400,\"id\":5,\"from\":1,\"to\":0,\"why\":0}\n"
              "{\"ts\":600,\"id\":6,\"from\":0,\"to\":1,\"why\":0}\n");
    const std::vector<Row> rows = ParseRows(RunMerge({a, b}, {}));
    CHECK(rows.size() == 6);
    const uint64_t want_ts[6] = {100, 200, 300, 400, 500, 600};
    const char* want_src[6] = {"a", "b", "a", "b", "a", "b"};
    for (std::size_t i = 0; i < 6; ++i) {
      CHECK(rows[i].ts == want_ts[i]);  // per-file disorder corrected
      CHECK(rows[i].src.find(want_src[i]) != std::string::npos);
      CHECK(rows[i].payload.rfind("fsm ", 0) == 0);
    }
  }

  // --- --from / --to filtering (inclusive) -----------------------------------
  {
    logmerge::Options opt;
    opt.from_ns = 250;
    opt.to_ns = 500;
    const std::vector<Row> rows = ParseRows(RunMerge({a, b}, opt));
    CHECK(rows.size() == 3);
    CHECK(rows[0].ts == 300 && rows[1].ts == 400 && rows[2].ts == 500);
  }

  // --- --json output shape ----------------------------------------------------
  {
    logmerge::Options opt;
    opt.json = true;
    const std::vector<std::string> lines = RunMerge({a, b}, opt);
    CHECK(lines.size() == 6);
    for (const std::string& l : lines) {
      CHECK(l.front() == '{' && l.back() == '}');
      CHECK(l.find("\"ts\":") != std::string::npos);
      CHECK(l.find("\"src\":\"") != std::string::npos);
      CHECK(l.find("\"kind\":\"fsm\"") != std::string::npos);
      CHECK(l.find("\"event\":{\"ts\":") != std::string::npos);
    }
  }

  // --- T9P1 perf binary merged with JSONL on one axis -------------------------
  const std::string perf = Path("node1.bin");
  const std::string c = Path("c.fsm");
  {
    PerfSampler s;
    PerfWorkLog wl;
    PerfLogDumper d;
    CHECK(d.Open(perf.c_str(), 0, s.capacity(), wl.capacity()));
    s.Note(Ns(150), 100ull << 13, 1, 0);  // 100 lat units = 819200 ns
    wl.Note(Ns(250), 50, 0, 0, 0x6);
    s.Note(Ns(350), 100ull << 13, 1, 0);
    CHECK(d.DumpIfDue(0, s, &wl, 1));  // window 1
    s.Note(Ns(550), 100ull << 13, 1, 0);
    CHECK(d.DumpIfDue(1, s, &wl, 1));  // window 2
    d.Close();
    WriteText(c,
              "{\"ts\":1638400,\"id\":1,\"from\":0,\"to\":1,\"why\":0}\n"  // 100<<14
              "{\"ts\":6553600,\"id\":2,\"from\":1,\"to\":0,\"why\":0}\n");  // 400<<14

    const std::vector<Row> rows = ParseRows(RunMerge({perf, c}, {}));
    // Window sort order: lat150, work250, lat350, pct350, hist350 (OBS-09
    // histogram blocks carry the window's last ts and are emitted after the
    // percentile block), then window 2's lat/pct/hist at 550.
    const uint64_t want_ts[10] = {Ns(100), Ns(150), Ns(250), Ns(350),
                                  Ns(350), Ns(350), Ns(400), Ns(550),
                                  Ns(550), Ns(550)};
    const char* want_kind[10] = {"fsm",  "lat",  "work", "lat", "pct",
                                 "hist", "fsm", "lat",  "pct", "hist"};
    CHECK(rows.size() == 10);
    for (std::size_t i = 0; i < 10; ++i) {
      CHECK(rows[i].ts == want_ts[i]);
      CHECK(rows[i].payload.rfind(want_kind[i], 0) == 0);
      const bool is_perf = rows[i].src.find("node1.bin") != std::string::npos;
      CHECK(is_perf == (std::strcmp(want_kind[i], "fsm") != 0));
      CHECK(is_perf == (rows[i].payload.find("realtime_ns=") != std::string::npos));
    }
    // Spot-check decoded fields of the work record and a sample.
    CHECK(rows[2].payload.find("node=1") != std::string::npos);
    CHECK(rows[2].payload.find("flags=6") != std::string::npos);
    CHECK(rows[3].payload.find("lat_ns=819200") != std::string::npos);
    // The histogram line reports the bucket-resolution percentiles: two
    // samples at 819200 ns mean p999 lands in that bucket, and max is exact.
    CHECK(rows[4].payload.find("n=2") != std::string::npos);
    CHECK(rows[4].payload.find("max_ns=819200") != std::string::npos);
  }

  // --- ARCH-15: full-width clock anchors, cross-day/process wall mapping -----
  {
    const std::string anchored = Path("anchored.bin");
    const std::string legacy = Path("legacy.bin");
    const uint64_t realtime_a = 1700000000000000000ull;
    const uint64_t realtime_b = realtime_a + 86400000000000ull;
    PerfLogWriter anchored_writer;
    WritePerfClockAnchor(anchored_writer, Ns(100), realtime_a);
    WriteSample(&anchored_writer, 50);  // before the first anchor: use its negative delta
    WritePerfClockAnchor(anchored_writer, Ns(200), realtime_b);
    WriteSample(&anchored_writer, 250);
    WriteBytes(anchored, anchored_writer);
    PerfLogWriter old;
    WriteSample(&old, 150);
    WriteBytes(legacy, old);

    const std::vector<Row> rows = ParseRows(RunMerge({anchored, legacy}, {}));
    CHECK(rows.size() == 3);
    CHECK(rows[0].ts == Ns(50) && rows[0].src.find("anchored.bin") != std::string::npos);
    CHECK(Field(rows[0].payload, "realtime_ns=") == realtime_a - Ns(50));
    CHECK(rows[1].ts == Ns(150) && rows[1].src.find("legacy.bin") != std::string::npos);
    CHECK(rows[1].payload.find("lat_ns=8192") != std::string::npos);
    CHECK(rows[1].payload.find("realtime_ns=") == std::string::npos);
    CHECK(rows[2].ts == Ns(250) && rows[2].src.find("anchored.bin") != std::string::npos);
    CHECK(Field(rows[2].payload, "realtime_ns=") == realtime_b + Ns(50));

    logmerge::Options json_opt;
    json_opt.json = true;
    const std::vector<std::string> json = RunMerge({anchored, legacy}, json_opt);
    CHECK(json.size() == 3);
    CHECK(json[0].find("\"realtime_ns\":" +
                       std::to_string(realtime_a - Ns(50))) != std::string::npos);
    CHECK(json[1].find("\"realtime_ns\":") == std::string::npos);
    CHECK(json[2].find("\"realtime_ns\":" +
                       std::to_string(realtime_b + Ns(50))) != std::string::npos);
  }

  // A malformed/regressing anchor ends its source; no later record is decoded
  // and no wall timestamp is invented. Arithmetic overflow also stays mono-only.
  {
    const std::string bad_count = Path("anchor_bad_count.bin");
    PerfLogWriter bad;
    bad.U8(kPerfClockAnchorBlockType);
    bad.U8(0);
    bad.U32(2);
    bad.U64(Ns(100));
    bad.U64(1700000000000000000ull);
    bad.U64(Ns(200));
    bad.U64(1700000000000000000ull);
    WriteSample(&bad, 250);
    WriteBytes(bad_count, bad);
    CHECK(RunMerge({bad_count}, {}).empty());

    const std::string regress = Path("anchor_regress.bin");
    PerfLogWriter r;
    WritePerfClockAnchor(r, Ns(200), 1700000000000000000ull);
    WriteSample(&r, 250);
    WritePerfClockAnchor(r, Ns(100), 1700000000000000000ull);
    WriteSample(&r, 300);
    WriteBytes(regress, r);
    const std::vector<Row> rows = ParseRows(RunMerge({regress}, {}));
    CHECK(rows.size() == 1 && rows[0].ts == Ns(250));

    const std::string overflow = Path("anchor_overflow.bin");
    PerfLogWriter o;
    WritePerfClockAnchor(o, 0, ~0ull);
    WriteSample(&o, 1);
    WriteBytes(overflow, o);
    const std::vector<Row> overflow_rows = ParseRows(RunMerge({overflow}, {}));
    CHECK(overflow_rows.size() == 1 && overflow_rows[0].ts == Ns(1));
    CHECK(overflow_rows[0].payload.find("realtime_ns=") == std::string::npos);
  }

  // --- truncated tail / unknown block: source ends cleanly --------------------
  {
    const std::vector<uint8_t> good = [&] {
      FILE* f = std::fopen(perf.c_str(), "rb");
      CHECK(f != nullptr);
      std::vector<uint8_t> d;
      uint8_t buf[4096];
      std::size_t n;
      while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        d.insert(d.end(), buf, buf + n);
      }
      std::fclose(f);
      return d;
    }();
    // (i) partial block header tail
    const std::string t1 = Path("trunc1.bin");
    WriteText(t1, "");  // create
    FILE* f = std::fopen(t1.c_str(), "wb");
    CHECK(f != nullptr);
    CHECK(std::fwrite(good.data(), 1, good.size(), f) == good.size());
    CHECK(std::fwrite("T9", 1, 2, f) == 2);  // partial magic/header
    std::fclose(f);
    // (ii) unknown block type tail
    const std::string t2 = Path("trunc2.bin");
    f = std::fopen(t2.c_str(), "wb");
    CHECK(f != nullptr);
    CHECK(std::fwrite(good.data(), 1, good.size(), f) == good.size());
    const uint8_t bogus[6] = {99, 0, 1, 0, 0, 0};
    CHECK(std::fwrite(bogus, 1, sizeof(bogus), f) == sizeof(bogus));
    std::fclose(f);

    for (const std::string& t : {t1, t2}) {
      const std::vector<Row> rows = ParseRows(RunMerge({t}, {}));
      // 3 lat + 2 pct + 1 work + 2 hist (one per dump window), tail dropped
      CHECK(rows.size() == 8);
    }
  }

  // --- epoch wrap: u32 ts crossing 2^32 units expands monotonically ----------
  {
    const std::string w = Path("wrap.bin");
    PerfSampler s;
    PerfLogDumper d;
    CHECK(d.Open(w.c_str(), 0, s.capacity(), 0));
    const uint64_t wrap = 1ull << 32;
    const uint64_t before = (wrap - 2) << 14;
    const uint64_t after = (wrap + 3) << 14;
    s.Note(before, 10, 1, 0);
    CHECK(d.DumpIfDue(before, s, nullptr, 1));
    s.Note(after, 20, 1, 0);
    CHECK(d.DumpIfDue(after, s, nullptr, 1));
    d.Close();
    const std::vector<Row> rows = ParseRows(RunMerge({w}, {}));
    // lat + pct + hist per window, ts ascending across the wrap.
    CHECK(rows.size() == 6);
    CHECK(rows[0].ts == before);
    CHECK(rows[3].ts == after);
    CHECK(rows[3].ts > rows[0].ts);
    for (const Row& r : rows) {
      CHECK(r.payload.rfind("lat", 0) == 0 || r.payload.rfind("pct", 0) == 0 ||
            r.payload.rfind("hist", 0) == 0);
    }
  }

  // --- OBS-09: histogram timeline events + cross-process --hist merge ------
  const std::string ha = Path("hist_a.bin");
  const std::string hb = Path("hist_b.bin");
  const std::string hw = Path("hist_work.bin");
  {
    PerfSampler native_a;  // node 1, series 0 (node latency)
    PerfLogDumper da;
    CHECK(da.Open(ha.c_str(), 0, native_a.capacity(), 0));
    for (int i = 0; i < 100; ++i) {
      native_a.Note(Ns(100 + static_cast<uint64_t>(i)), 1000, 1, 0);
    }
    CHECK(da.DumpIfDue(Ns(200), native_a, nullptr, 1));
    da.Close();

    PerfSampler native_b;  // a second "process" for the same series
    PerfLogDumper db;
    CHECK(db.Open(hb.c_str(), 0, native_b.capacity(), 0));
    for (int i = 0; i < 100; ++i) {
      native_b.Note(Ns(300 + static_cast<uint64_t>(i)), 1000, 1, 0);
    }
    native_b.Note(Ns(450), 1000000, 1, 0);  // one 1 ms tail sample
    CHECK(db.DumpIfDue(Ns(500), native_b, nullptr, 1));
    db.Close();

    // A different series (SCH-08 callback work) on the same node.
    PerfSampler cw(PerfSampler::kCap, PerfSampler::kHistSeriesCallbackWork);
    PerfLogDumper dc;
    CHECK(dc.Open(hw.c_str(), 0, cw.capacity(), 0));
    for (int i = 0; i < 10; ++i) {
      cw.Note(Ns(600 + static_cast<uint64_t>(i)), 20000, 7, 0);
    }
    CHECK(dc.DumpIfDue(Ns(700), cw, nullptr, 1));
    dc.Close();
  }
  // Timeline: one hist line per dump window, carrying the full percentile set.
  {
    const std::vector<Row> rows = ParseRows(RunMerge({ha, hb, hw}, {}));
    int hists = 0;
    for (const Row& r : rows) {
      if (r.payload.rfind("hist ", 0) != 0) continue;
      ++hists;
      CHECK(r.payload.find("p999_ns=") != std::string::npos);
      CHECK(r.payload.find("max_ns=") != std::string::npos);
    }
    CHECK(hists == 3);
  }
  // --hist: bucket-wise merge across the two processes; max is max.
  {
    logmerge::Options opt;
    opt.hist = true;
    const std::vector<std::string> lines = RunMerge({ha, hb, hw}, opt);
    CHECK(lines.size() == 2);  // (node 1, series 0) and (node 1, series 2)
    CHECK(lines[0].rfind("node=1 series=0 ", 0) == 0);
    CHECK(lines[0].find(" n=201 ") != std::string::npos);
    CHECK(InBucket(Field(lines[0], "p50_ns="), 1000));
    CHECK(InBucket(Field(lines[0], "p999_ns="), 1000000));  // the tail survives
    CHECK(Field(lines[0], "max_ns=") == 1000000);
    CHECK(lines[1].rfind("node=1 series=2 ", 0) == 0);
    CHECK(lines[1].find(" n=10 ") != std::string::npos);
    CHECK(Field(lines[1], "max_ns=") == 20000);

    // --from/--to filters on the histogram window end: file A ends at
    // Ns(199), so only file B (Ns(450)) and the work series survive.
    opt.from_ns = Ns(300);
    const std::vector<std::string> windowed = RunMerge({ha, hb, hw}, opt);
    CHECK(windowed.size() == 2);
    CHECK(windowed[0].find(" n=101 ") != std::string::npos);

    opt.json = true;
    const std::vector<std::string> json = RunMerge({ha, hb, hw}, opt);
    CHECK(json.size() == 2);
    for (const std::string& l : json) {
      CHECK(l.front() == '{' && l.back() == '}');
      CHECK(l.find("\"p999_ns\":") != std::string::npos);
      CHECK(l.find("\"max_ns\":") != std::string::npos);
    }
  }
  // A type-6 record with an unknown bucket count ends the source cleanly
  // (wire versioning: stop rather than misread the counts).
  {
    const std::string bad = Path("hist_bad.bin");
    FILE* f = std::fopen(ha.c_str(), "rb");
    CHECK(f != nullptr);
    uint8_t buf[4096];
    std::size_t n = 0;
    FILE* g = std::fopen(bad.c_str(), "wb");
    CHECK(g != nullptr);
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
      CHECK(std::fwrite(buf, 1, n, g) == n);
    }
    std::fclose(f);
    const uint8_t hdr[6] = {6, 1, 1, 0, 0, 0};
    CHECK(std::fwrite(hdr, 1, sizeof(hdr), g) == sizeof(hdr));
    std::vector<uint8_t> rec(kPerfHistRecordBytes, 0);
    rec[0] = kPerfHistogramRecordTag;
    rec[2] = 7;  // buckets = 7 (not the compiled kBuckets)
    CHECK(std::fwrite(rec.data(), 1, rec.size(), g) == rec.size());
    std::fclose(g);
    const std::vector<Row> rows = ParseRows(RunMerge({bad}, {}));
    // The good window's 100 latency samples + 1 pct + 1 hist, then the
    // unknown-bucket-count block ends the source.
    CHECK(rows.size() == 102);
  }

  // --- OBS-07-C: type-5 names and node latency budgets -----------------------
  {
    const std::string metadata = Path("metadata.bin");
    PerfSampler sampler;
    ProcSampler proc;
    CHECK(proc.Start(ProcSampler::Options{}));
    std::atomic<uint64_t> overruns{41};
    CHECK(proc.RegisterCounter("overrun_long_callback", &overruns) ==
          ProcSampler::kProbeBase);
    PerfLogDumper dumper;
    CHECK(dumper.Open(metadata.c_str(), 0));
    dumper.SetNodeBudget(10000000, 8000000);
    sampler.Note(Ns(800), 9000000, PerfSampler::kNoId, 0);
    CHECK(proc.SampleNow(Ns(800)) == 17);
    CHECK(dumper.DumpIfDue(Ns(800), sampler, nullptr, 3, &proc));
    dumper.Close();

    logmerge::Options opt;
    opt.json = true;
    const std::vector<std::string> rows = RunMerge({metadata}, opt);
    bool named_sys = false;
    bool named_resources = false;
    bool latency_budget = false;
    for (const std::string& row : rows) {
      if (row.find("\"kind\":\"sys\"") != std::string::npos &&
          row.find("\"series_name\":\"overrun_long_callback\"") !=
              std::string::npos) {
        named_sys = row.find("\"value\":41") != std::string::npos;
      }
      if (row.find("\"kind\":\"sys\"") != std::string::npos &&
          row.find("\"series_name\":\"rss_kb\"") != std::string::npos) {
        named_resources = true;
      }
      if (row.find("\"kind\":\"lat\"") != std::string::npos) {
        latency_budget =
            row.find("\"period_ns\":10000000") != std::string::npos &&
            row.find("\"deadline_ns\":8000000") != std::string::npos;
      }
    }
    CHECK(named_sys);
    CHECK(named_resources);
    CHECK(latency_budget);

    opt.hist = true;
    const std::vector<std::string> hist = RunMerge({metadata}, opt);
    CHECK(hist.size() == 1);
    CHECK(hist[0].find("\"period_ns\":10000000") != std::string::npos);
    CHECK(hist[0].find("\"deadline_ns\":8000000") != std::string::npos);
  }

  // --- OBS-04 --sys: only process/resource rows, no timeline noise ----------
  {
    const std::string path = Path("sys_only.bin");
    PerfSampler sampler;
    ProcSampler proc;
    CHECK(proc.Start(ProcSampler::Options{}));
    std::atomic<uint64_t> overruns{7};
    CHECK(proc.RegisterCounter("overrun_long_callback", &overruns) ==
          ProcSampler::kProbeBase);
    PerfLogWriter w;
    proc.FlushMetadata(w, 4);
    proc.SampleNow(Ns(1000));
    proc.FlushNew(w, 4);
    sampler.Note(Ns(1000), 500, PerfSampler::kNoId, 0);
    sampler.FlushNew(w, 4);
    WriteBytes(path, w);

    logmerge::Options opt;
    opt.json = true;
    opt.sys_only = true;
    const std::vector<std::string> rows = RunMerge({path}, opt);
    CHECK(rows.size() == 17);  // 16 proc series + 1 probe, no lat row
    for (const std::string& row : rows) {
      CHECK(row.find("\"kind\":\"sys\"") != std::string::npos);
      CHECK(row.find("\"series_name\":\"") != std::string::npos);
    }

    opt.sys_only = false;
    const std::vector<std::string> all = RunMerge({path}, opt);
    // The same 17 sys rows plus the latency sample, its percentile block and
    // the OBS-09 histogram block.
    CHECK(all.size() == 20);
    bool saw_lat = false;
    for (const std::string& row : all) {
      if (row.find("\"kind\":\"lat\"") != std::string::npos) saw_lat = true;
    }
    CHECK(saw_lat);
  }

  // --- missing input -> 3 ------------------------------------------------------
  {
    logmerge::Options opt;
    const std::string out_path = Path("out_none.txt");
    FILE* out = std::fopen(out_path.c_str(), "w");
    CHECK(out != nullptr);
    const std::vector<std::string> bad = {Path("does_not_exist.fsm")};
    CHECK(logmerge::Merge(bad, opt, out, nullptr) == 3);
    std::fclose(out);
  }

  {
    std::string cmd = "rm -rf ";
    cmd += g_dir;
    CHECK(system(cmd.c_str()) == 0);
  }
  return 0;
}
