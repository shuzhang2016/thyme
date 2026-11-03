#pragma once
// thyme_probe core API (ECO-09/ECO-11): read-only views over thyme SHM
// containers.
//
// READ-ONLY CONTRACT
//   Every operation in this header maps the target region with PROT_READ and
//   only ever loads from it. The probe never subscribes, never acquires a
//   PinnedSlot lease, never advances a shared consumer cursor, never calls
//   Push/Write/Commit and never unlinks a name. In particular it does not use
//   ShmSpscRing::TryPop (which writes tail) or PinnedSlotReader::TryRead (whose
//   per-reader cursor only moves forward), and LatestSlot<T>/SpmcReader are
//   typed APIs that a runtime-erased probe cannot instantiate. Instead every
//   sample is "the newest published slot" read through the container's own
//   seqlock/seq/footer validation, so a sampling run can miss frames but can
//   never move a producer or consumer cursor.
//
// GEOMETRY SOURCE
//   PinnedSlot, ShmSpscRing, ShmSpmcRing, SharedHealthPage, MpControl,
//   RecordMirrorPage and RecordMirrorRingPage are self-describing: their
//   magic/version at offset 0 identifies the kind. SharedHealthPage and
//   MpControl additionally expose the ARCH-11 super-header, so the probe
//   validates magic/abi/layout_hash/page_bytes/feature_bits/reserved before
//   reading any field. LatestSlot has no in-page magic or version (see
//   runtime/latest_slot.h), so a naked LatestSlot page is only identifiable
//   through a manifest entry that supplies the payload size. The manifest is
//   also the only source of the slot stride for the two ring kinds.

#include "thyme/contracts/frame_header.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace thyme {
namespace probe {

// Sentinel for "counter not present on this page kind".
inline constexpr uint64_t kNone = ~static_cast<uint64_t>(0);

enum class Kind : uint8_t {
  kUnknown = 0,
  kLatestSlot,
  kSpscRing,
  kSpmcRing,
  kPinnedSlot,
  kSharedHealth,
  kMpControl,
  kRecordMirrorRing,
  kRecordMirror,
};

// snake_case label used by --json and the manifest file.
const char* KindName(Kind kind);
// nullptr when the label is not recognized.
Kind KindFromName(const char* label);

enum class Status : uint8_t {
  kOk = 0,
  kUsage = 1,        // bad arguments / unparsable manifest
  kNotFound = 2,     // no such shm object, or not a thyme page
  kBadGeometry = 3,  // magic/version mismatch or geometry does not fit
  kNotReadable = 4,  // shm_open EACCES/EPERM
  kNoData = 5,       // window elapsed with no new sample
};

const char* StatusName(Status status);
// 0 for kOk, otherwise the roadmap §3.22 exit code (2/3/4/5), 1 for usage.
int ExitCode(Status status);

struct StructEntry {
  uint16_t id = 0;
  uint32_t bytes = 0;
  std::string name;
};

struct RegionInfo {
  std::string name;  // "/thyme_..."
  Kind kind = Kind::kUnknown;
  uint32_t abi_version = 0;  // 0 when the page has no version field
  bool compatible = true;    // false: magic/version/geometry mismatch
  uint32_t slots = 0;        // capacity (2 for LatestSlot)
  uint32_t slot_bytes = 0;   // payload capacity per slot (0 = unknown)
  uint32_t stride = 0;       // bytes per slot incl. any per-slot header
  uint64_t region_bytes = 0;
  // Publish counter: ring/PinnedSlot head, or LatestSlot seq/2 (write count).
  uint64_t head = kNone;
  // Shared consumer cursor, only ShmSpscRing publishes one.
  uint64_t tail = kNone;
  uint64_t epoch = kNone;
  // Container-specific failure counters where the page carries them.
  uint64_t drop_count = kNone;   // push_fail / dropped_oversize / mirror drop
  std::vector<uint64_t> reader_drop_slots;  // ShmSpmcRing only; empty otherwise
  uint64_t skip_count = kNone;   // PinnedSlot pinned_skip (lease table)
  uint64_t steal_count = kNone;  // PinnedSlot lease_steal (lease table)
  uint32_t session_id = 0;       // creator pid when the page records one
  bool has_session = false;
  uint32_t workers = 0;  // SharedHealthPage worker count
  std::vector<StructEntry> structs;  // record mirror registry summary
  bool frame_shaped = false;  // payload begins with a FrameHeader (declared)
  std::string geometry_source;  // "page" or "manifest"
  std::string note;             // human-readable caveat, may be empty
};

struct Sample {
  uint64_t sequence = 0;     // container publish counter (see RegionInfo::head)
  uint64_t producer_ns = 0;  // producer CLOCK_MONOTONIC stamp, 0 when absent
  uint64_t observed_ns = 0;  // probe read time, CLOCK_MONOTONIC
  uint32_t size = 0;         // published payload bytes
  double rate_hz = 0.0;      // rate since the previous emitted sample
  bool torn = false;
};

struct RateStats {
  uint64_t window_ns = 0;
  uint64_t published = 0;  // delta of the publish counter over the window
  double window_s = 0.0;
  double hz_avg = 0.0;
  double hz_min = 0.0;
  double hz_max = 0.0;
  double bytes_per_s = 0.0;
  double bytes_per_s_min = 0.0;
  double bytes_per_s_max = 0.0;
  uint64_t jitter_p50_ns = 0;
  uint64_t jitter_p99_ns = 0;
  uint64_t jitter_max_ns = 0;
  bool exact_bytes = false;  // per-frame lengths used (PinnedSlot --exact)
  uint64_t samples = 0;      // sampling intervals observed
};

struct Histogram {
  static constexpr std::size_t kBuckets = 17;  // last bucket = overflow
  uint64_t count[kBuckets]{};
  // Upper bound of bucket i in ns; kNone for the overflow bucket.
  static uint64_t UpperNs(std::size_t index);
};

struct LatencyStats {
  uint64_t count = 0;
  uint64_t min_ns = 0;
  uint64_t p50_ns = 0;
  uint64_t p90_ns = 0;
  uint64_t p99_ns = 0;
  uint64_t max_ns = 0;
  Histogram histogram;
  uint64_t window_ns = 0;
  bool frame_shaped = false;
  // "frame_header" (payload FrameHeader::mono_ns), "pinned_slot_slot"
  // (PinnedSlotHeader::mono_ns) or "none".
  std::string stamp_source{"none"};
};

struct EchoSample {
  uint64_t sequence = 0;
  uint64_t producer_ns = 0;
  uint64_t observed_ns = 0;
  uint32_t size = 0;           // full published payload size
  std::vector<uint8_t> bytes;  // truncated to the requested max_bytes
  uint64_t drop_count = kNone;
  uint32_t worker = 0;
  bool has_worker = false;
  uint32_t memory_flags = 0;
  bool has_memory_flags = false;
  bool truncated = false;
  bool has_header = false;  // payload decoded as a FrameHeader
  FrameHeader header{};
  uint16_t struct_id = 0;
  std::string struct_name;
};

struct ManifestEntry {
  std::string name;  // normalized to a leading '/'
  Kind kind = Kind::kUnknown;
  uint32_t slot_bytes = 0;
  uint32_t depth = 0;
  uint16_t struct_id = 0;
  std::string struct_name;
  bool frame_shaped = false;
};

struct Options {
  // /dev/shm entry prefixes to scan (without the leading '/'). The MP
  // supervisor default prefix is "c9_"; pass it explicitly with --prefix.
  std::vector<std::string> prefixes{"thyme_"};
  std::string manifest_path;  // empty = no manifest file
};

struct SampleOptions {
  int count = 0;               // 0 = until max_duration_ns / forever
  uint64_t period_ns = 1000000000ULL;
  uint64_t max_duration_ns = 0;  // 0 = no deadline
};

// One versioned object compared against this build's constant (OPS-06).
struct AbiCheck {
  std::string name;    // "/thyme_..." (companion pages keep their suffix)
  Kind kind = Kind::kUnknown;
  std::string object;  // matrix row object ("PinnedSlotPageHeader", ...)
  uint32_t expected = 0;
  uint32_t actual = 0;
  bool checked = false;     // a version word was readable
  bool compatible = true;   // checked, page geometry OK and actual == expected
  std::string note;
};

struct AbiReport {
  std::vector<AbiCheck> pages;      // sorted by name
  std::vector<std::string> unverifiable;  // no in-page version / no matrix row
  uint32_t compatible_count = 0;
  uint32_t mismatch_count = 0;

  bool ok() const { return mismatch_count == 0; }
};

class ProbeApi {
 public:
  ProbeApi();
  ~ProbeApi();
  ProbeApi(const ProbeApi&) = delete;
  ProbeApi& operator=(const ProbeApi&) = delete;

  void SetOptions(const Options& options);
  const Options& options() const;
  Status LoadManifest(const std::string& path);
  void AddManifestEntry(const ManifestEntry& entry);

  // Enumerate readable thyme pages under /dev/shm matching the prefixes.
  // Manifest-declared kinds fill in what the page cannot self-describe
  // (LatestSlot). Unknown magic + no manifest entry = skipped.
  Status List(std::vector<RegionInfo>& out);

  // Geometry/header summary for one page. kNotFound when the name is absent
  // or is not a thyme page; kBadGeometry when magic/version/geometry is
  // unsupported (out is still filled as far as the page could be read).
  Status Info(const std::string& name, RegionInfo& out);

  // Newest published sample, no cursor and no writes.
  Status Newest(const std::string& name, Sample* out);

  // Emit new samples at `period_ns`. The callback returning false stops.
  Status Watch(const std::string& name, const SampleOptions& options,
               const std::function<bool(const Sample&)>& on_sample,
               uint64_t* emitted = nullptr);

  // Rate/bandwidth over a sliding window of `period_ns` samples.
  Status Hz(const std::string& name, uint64_t window_ns, uint64_t period_ns,
            RateStats* out);
  Status Bw(const std::string& name, uint64_t window_ns, uint64_t period_ns,
            bool exact_bytes, RateStats* out);

  // Age at observation: observed_mono_ns - producer_stamp. This is NOT
  // consumer latency (time from publish to a consumer's read); see the README
  // section in the tool's --help.
  Status Delay(const std::string& name, uint64_t window_ns, uint64_t period_ns,
               LatencyStats* out);

  // Sample up to `count` newest payloads; deduplicated by sequence.
  Status Echo(const std::string& name, int count, uint32_t max_bytes,
              uint64_t timeout_ns, std::vector<EchoSample>* out);

  // OPS-06 pre-flight ABI check over every page List() would report:
  // compares each page's version word (and the PinnedSlot lease companion)
  // against the compiled-in matrix. Read-only. kOk when every checked page
  // matches, kBadGeometry when at least one does not, kNotFound when there
  // is nothing to check.
  Status CheckAbi(AbiReport* out);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace probe
}  // namespace thyme
