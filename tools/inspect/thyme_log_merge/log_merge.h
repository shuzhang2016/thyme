#pragma once
// thyme_log_merge — OBS-06: merge per-process observability logs into one
// interleaved timeline ordered by absolute CLOCK_MONOTONIC ns.
//
// Supported inputs (sniffed per file, mixed freely):
//   * .fsm / JSONL text logs (OBS-05 trigger-center event logs and any other
//     JSONL with a "ts":<mono_ns> field). Lines without a parseable "ts" are
//     skipped and counted.
//   * T9P1 perf dump binaries (perf_log.h: sample/work/percentile/sys blocks,
//     OBS-09 type-6 histogram blocks, ABI-10 type-7 record-manifest blocks,
//     OBS-07 type-8 series-name and type-9 node-budget metadata, ARCH-15
//     type-10 mono/realtime anchors, plus type-4 epoch blocks; u32 ts words
//     are expanded to a u64 axis using
//     the epoch base, bumping on low-word regression — same rule as
//     eval_chains.cpp). A truncated/unknown tail (post-crash partial block)
//     ends parsing of that file cleanly.
//
// ABI-10 manifest contract: every input may declare the GEN-12 record
// manifest it was produced under (perf_log.h type 7). RecordStructId is a row
// index into the sorted recorded-channel list, so inputs produced under
// different manifests cannot be joined: a cross-file hash conflict, an
// unsupported manifest version, or a mismatch against --expect-hash is
// reported and the merge exits 4 without emitting a (silently wrong)
// timeline. Legacy logs without the block are accepted unless --expect-hash
// asks for verification.
//
// Ordering contract: each source is read through a bounded reorder window
// (kReorderWindow events for text, one dump window for perf — delimited by
// epoch blocks) and sorted by ts before entering the k-way merge. Within-one
// -file disorder shorter than the window is corrected; longer disorder is
// emitted as-is (per-source streams are expected to be nearly ordered: ring
// dumps append chronologically). Perf windows larger than kMaxWindowEvents
// are split, which can misorder at the split boundary. All state is bounded
// — files are never loaded whole.
//
// Output (one line per event, ts ascending; ties break by input order):
//   text (default): <ts_ns>\t<source>\t<kind> <fields>
//                   (fsm events print the original JSON line as the payload)
//   --json:         {"ts":..,"src":"..","kind":"..",...}
// --from/--to filter on ts_ns (inclusive both ends). Perf events after a
// type-10 anchor also carry realtime_ns, converted with that source's latest
// anchor. Legacy/text inputs and conversion overflow have no realtime_ns.
//
//   --hist:         no timeline; merge OBS-09 histogram blocks per
//                   (node, series) — bucket-wise sums for the percentiles,
//                   max for the max — and print one summary line each as
//                   node=N series=S n=.. p50_ns=.. p90_ns=.. p99_ns=..
//                   p999_ns=.. max_ns=.. (or the same fields as JSON).
//   --sys:          timeline restricted to OBS-04 type-5 process/resource
//                   rows (kind "sys"); same renderers, so --json output can
//                   feed thyme_metrics_exporter --sys-json.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace thyme {
namespace logmerge {

enum class Kind : uint8_t {
  kFsm,
  kPerfLat,
  kPerfPct,
  kPerfWork,
  kPerfSys,
  kPerfHist
};

struct Event {
  uint64_t ts_ns = 0;
  bool has_realtime_ns = false;
  uint64_t realtime_ns = 0;  // valid only when has_realtime_ns is true
  uint32_t src = 0;  // index into the Merge() paths vector
  Kind kind = Kind::kFsm;
  std::string raw;   // kFsm: the original JSON line (no trailing newline)
  uint64_t f[8] = {};  // kind-specific fields (see log_merge.cpp renderers)
  std::string series_name;
  bool has_node_budget = false;
  uint64_t period_ns = 0;
  uint64_t deadline_ns = 0;
};

class Source {
 public:
  virtual ~Source() = default;
  // Next event in per-source sorted order; false at EOF / parse end.
  virtual bool Next(Event* ev) = 0;
  // Records skipped as unparseable (text) or dropped at a truncated tail
  // (perf). For diagnostics only.
  uint64_t skipped() const { return skipped_; }

 protected:
  uint64_t skipped_ = 0;
};

// Opens `path`, sniffs the format (T9P1 magic -> perf binary, else JSONL
// text). nullptr on open failure (err is set). reorder_window bounds the
// per-source sort buffer for text sources.
std::unique_ptr<Source> OpenSource(const char* path, std::string* err,
                                   uint32_t reorder_window = 65536);

struct Options {
  uint64_t from_ns = 0;
  uint64_t to_ns = ~0ull;
  bool json = false;
  // ABI-10: require every input's record manifest to match this hash.
  bool have_expect_hash = false;
  uint64_t expect_hash = 0;
  // OBS-09: instead of the interleaved timeline, aggregate every type-6
  // histogram by (node, series) across all inputs — bucket-wise sums for the
  // percentiles, max for the max — and print one summary line per series.
  bool hist = false;
  // OBS-04: emit only type-5 process/resource series rows (kind "sys"), in
  // timeline order. Same renderers as the full timeline, so --json output is
  // directly consumable by thyme_metrics_exporter --sys-json.
  bool sys_only = false;
};

// ABI-10 result of the manifest scan (all inputs).
struct ManifestReport {
  bool declared = false;      // at least one input carried a manifest block
  bool conflict = false;      // inputs disagreed on hash/version
  uint64_t hash = 0;          // first declared hash
  uint16_t version = 0;       // first declared version
  uint32_t files = 0;         // inputs that declared a manifest
};

// Merges all inputs to `out`. Returns 0 on success, 3 if any input failed to
// open (nothing is emitted in that case), 4 on a record-manifest mismatch.
// Per-source skip counts are reported to `diag` when non-null and non-zero.
// `report` (optional) receives the manifest scan result.
int Merge(const std::vector<std::string>& paths, const Options& opt,
          FILE* out, FILE* diag, ManifestReport* report = nullptr);

// Full line renderers (no trailing newline). Exposed for tests.
std::string RenderText(const Event& e, const char* src_name);
std::string RenderJson(const Event& e, const char* src_name);

}  // namespace logmerge
}  // namespace thyme
