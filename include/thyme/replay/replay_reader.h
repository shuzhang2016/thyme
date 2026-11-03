#pragma once

// replay_reader.h — package-level replay reader with a unified timeline
// (REP-01, roadmap §3.17).
//
// A capture package (triggered or continuous) is a directory:
//
//   <pkg>/meta.tbin                     header + kTrigger/kSession + meta records
//   <pkg>/small.tbin                    small-struct samples (+ GAP holes)
//   <pkg>/frames/<channel>.index.tbin   FrameIndexRecord rows (offset/len/...)
//   <pkg>/frames/<channel>.raw          big-frame payload bytes
//   <pkg>/wal.log                       drain journal (seal = complete)
//
// ReplayPackage opens every source, validates it, and exposes one merged
// stream ordered by mono_ns. Sources are enumerated deterministically:
//
//   channel 0            meta.tbin
//   channel 1            small.tbin (when present; captures without samples
//                        do not create the file)
//   following channels   frames/*.index.tbin sorted byte-wise by file name
//
// Channel ids are dense, so channels()[event.channel] is always valid.
//
// Every record of every source is an event, including kTrigger, GAP and
// kSession entries, so the timeline is lossless with respect to the package.
// kSession rows carry no mono_ns in the file format; the reader places them at
// the timestamp of the preceding record of the same source (0 when the session
// row is first), which keeps every source monotonic.
//
// Merge order: (mono_ns, channel id, record_sequence, source ordinal). Within
// one source the file order is preserved, so identical packages always
// produce the identical sequence. Records inside a source are expected to be
// non-decreasing in mono_ns (the recorder writes its window in order); the
// merge does not re-sort a source.
//
// Payload lifetime: event.payload points into a read-only mapping of the
// source file (small/meta records) or of the channel's .raw file (big
// frames). It is valid until Close()/destruction — and therefore always
// valid until the next NextEvent() — and is never copied. Big-frame payloads
// are zero-copy: the .raw file is mapped once at Open and the index record's
// offset/length select the segment (no per-event read, copy or allocation).
//
// Memory: one mmap per source plus the channel/registry tables (allocated at
// Open). The public event type is a POD copy.

#include "thyme/contracts/abi.h"
#include "thyme/contracts/struct_desc.h"
#include "thyme/record/record_schema.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace thyme {
namespace replay {

// Package/reader status. kEnd is the normal end of the stream, not an error.
enum class ReplayStatus : uint8_t {
  kOk = 0,
  kEnd,             // no more events
  kNotADirectory,   // Open() path missing or not a directory
  kMissingMeta,     // meta.tbin missing (required in every package)
  kMissingRaw,      // frames/<ch>.index.tbin without frames/<ch>.raw
  kOrphanRaw,       // frames/<ch>.raw without frames/<ch>.index.tbin
  kBadHeader,       // magic/version/registry rejected
  kTruncated,       // file ends inside a record/header/index
  kCorrupt,         // CRC/layout violation
  kTooManyChannels, // more than kReplayMaxChannels sources
  kInvalidArgument, // bad parameter (e.g. rate <= 0, null out)
  kStopped,         // ReplaySession stopped by the host
  kBusy,            // REP-10: a live replay already owns a target's gate
  kManifestMismatch,  // ABI-10: schema.json and/or the .tbin manifest block
                      // disagree — the package was produced under a different
                      // recorded-channel layout; refuse to decode
  kBadSchema,       // ABI-09: schema.json present but unparseable/tampered
};

const char* ReplayStatusName(ReplayStatus status);

inline constexpr uint32_t kReplayMaxChannels = 64;

enum class ReplayKind : uint8_t {
  kData = 0,
  kGap = 1,
  kTrigger = 2,
  kSession = 3,
};

const char* ReplayKindName(ReplayKind kind);

enum class ReplayChannelKind : uint8_t {
  kMeta = 0,
  kSmall = 1,
  kPinnedSlot = 2,
};

// One replay source (a .tbin file, optionally paired with a .raw file).
struct ReplayChannel {
  uint32_t id = 0;
  ReplayChannelKind kind = ReplayChannelKind::kMeta;
  std::string name;      // "meta", "small", or the frames/<name> stem
  std::string tbin_path; // meta.tbin, small.tbin, frames/<name>.index.tbin
  std::string raw_path;  // big-frame channels only
  // Struct registry from the .tbin header (mirror structs for meta/small,
  // FrameIdx/FrameGap for a big-frame index file).
  std::vector<ThymeStructDesc> registry;
  uint64_t first_mono_ns = 0;  // min timestamp in this source
  uint64_t last_mono_ns = 0;   // max timestamp in this source
  uint64_t record_count = 0;   // every record (data/gap/trigger/session)
  uint64_t data_count = 0;
  uint64_t gap_count = 0;
  bool has_footer_index = false;  // REC-04 footer present and validated
  // Byte offsets of this source's record stream (REP-06). `data_offset` is the
  // first record; `records_end_offset` is the footer index (or EOF when the
  // file is unindexed). first/last_record_offset bracket the records actually
  // present, so a tool can seek a raw reader without re-parsing it.
  uint64_t data_offset = 0;
  uint64_t records_end_offset = 0;
  uint64_t first_record_offset = 0;
  uint64_t last_record_offset = 0;
  // Seek index actually in use: `seek_entries` block points with `seek_stride`
  // records between them. A footer index uses kThymeBinIndexBlockSize; an
  // unindexed file gets a sparse table built by the Open validation scan
  // (kScanBlockRecords stride). SeekMonoNs upper_bounds this table, so both
  // paths are O(log n) + one bounded block scan; an unindexed package never
  // degrades to a per-seek full-file scan.
  uint32_t seek_entries = 0;
  uint32_t seek_stride = 0;
  // CRC status at Open: every record was verified (Open fails otherwise) and
  // the footer index, when present, was verified as a whole. Per-frame payload
  // CRCs are declared by the frame index rows and re-checked on demand by
  // `thyme-replay verify`; frame_crc_absent counts rows whose recorder policy
  // left payload_crc == 0 (REC-21 crc=off), which cannot be re-checked.
  uint64_t record_crc_checked = 0;
  uint64_t index_crc_checked = 0;
  uint64_t frame_crc_declared = 0;
  uint64_t frame_crc_absent = 0;
  // Largest payload in this source (big frames: the raw segment length from
  // FrameIndexRecord::len; other sources: the record payload size). Seeded by
  // the Open validation scan, so injectors can check target geometry once at
  // Open instead of discovering an oversize frame mid-stream.
  uint32_t max_data_len = 0;
  // Smallest non-empty payload in this source (same units as max_data_len).
  uint32_t min_data_len = 0;
  // Big-frame geometry: the .raw extent actually referenced by the index rows.
  // first_raw_offset is the first referenced byte; last_raw_end is one past the
  // last referenced byte (0 for non-big-frame sources).
  uint64_t first_raw_offset = 0;
  uint64_t last_raw_end = 0;
};

// One event on the unified timeline.
//
// `payload` (big frames: the raw segment; small/meta: the tbin record payload)
// is reader-owned, read-only, and stays valid until Close(). Big-frame events
// carry `frame_flags` / `payload_crc` straight from the FrameIndexRecord so
// the injector (REP-04) can verify .raw bytes without re-parsing the index.
struct ReplayEvent {
  uint64_t mono_ns = 0;
  uint32_t channel = 0;  // ReplayChannel::id
  ReplayKind kind = ReplayKind::kData;
  uint32_t struct_id = 0;
  uint64_t record_sequence = 0;  // per-file record ordinal (trigger/session id
                                 // for those entries, 0 for gaps)
  uint32_t len = 0;
  const uint8_t* payload = nullptr;

  uint32_t source_sequence = 0;  // data: producer sequence; gap: missed count
  uint16_t gap_reason = 0;       // kGap only (triggered::kGapReasonOverrun)
  uint16_t reserved = 0;
  uint32_t frame_flags = 0;      // big frame only (e.g. h264 IDR bit)
  uint32_t payload_crc = 0;      // big frame only: CRC over the raw segment
  uint64_t raw_offset = 0;       // big frame only: offset inside .raw

  uint64_t trigger_id = 0;   // kTrigger
  uint64_t pre_roll_ns = 0;  // kTrigger
  uint64_t post_roll_ns = 0; // kTrigger
  uint64_t session_id = 0;   // kSession
  uint16_t session_reason = 0;  // kSession
};

class ReplayPackage {
 public:
  ReplayPackage();
  ~ReplayPackage();
  ReplayPackage(const ReplayPackage&) = delete;
  ReplayPackage& operator=(const ReplayPackage&) = delete;
  ReplayPackage(ReplayPackage&&) noexcept;
  ReplayPackage& operator=(ReplayPackage&&) noexcept;

  // Discovers, maps and fully validates the package (record CRCs, registry,
  // footer index, frame offsets vs .raw size). On failure nothing stays open
  // and the status names the failing file; error() carries path + offset.
  // Validation is a full record scan, so Open costs one pass over every
  // .tbin; the scan also seeds the seek block table for unindexed files.
  ReplayStatus Open(const std::string& dir);
  void Close();
  bool is_open() const;

  // Next merged event. Returns kOk or kEnd (or the first I/O-integrity error
  // encountered while advancing a source).
  ReplayStatus NextEvent(ReplayEvent* out);

  // Repositions every source at its first record with mono_ns >= target
  // (blocks from the REC-04 footer index when present, otherwise from the
  // sparse block table built by the Open validation scan), then re-primes the
  // merge. A target before the first record starts at the beginning; a target
  // past the last record yields kEnd on the next NextEvent.
  ReplayStatus SeekMonoNs(uint64_t mono_ns);

  uint64_t first_mono_ns() const;  // 0 when the package holds no records
  uint64_t last_mono_ns() const;
  const std::string& dir() const;
  const std::vector<ReplayChannel>& channels() const;
  const ReplayChannel* FindChannel(uint32_t id) const;

  ReplayStatus status() const;
  const std::string& error() const;  // last failure detail ("path: reason")
  uint64_t events_delivered() const;  // since Open (not reset by Seek)

  // REP-06 seek observability: the number of SeekMonoNs() calls and the number
  // of records probed across all sources by the most recent one. With either
  // index (footer or scan-built) this is bounded by one block per source, not
  // by the record count — the test asserts the bound instead of the timing.
  uint64_t seek_calls() const;
  uint64_t seek_records_scanned() const;

  // wal.log presence / seal row (package completeness). A package without a
  // seal is still readable; this is informational, not an open-time error.
  bool wal_present() const;
  bool package_complete() const;

  // ---- ABI-09/ABI-10 record contract ----
  // Open() verifies the sidecar and manifest identity as part of validation:
  //   * an unparseable/tampered schema.json fails with kBadSchema;
  //   * schema.json and the .tbin manifest block must agree on hash/version,
  //     and every .tbin that declares a manifest must agree — otherwise
  //     kManifestMismatch (no event is ever delivered under a different id
  //     assignment).
  // A legacy package (no sidecar, no manifest block) opens normally and
  // reports schema_state() == kAbsent / manifest_declared() == false.
  schema::ReadState schema_state() const;
  const schema::PackageSchema& schema() const;
  const std::string& schema_error() const;  // set when schema_state != kOk
  bool manifest_declared() const;
  uint64_t manifest_hash() const;
  uint16_t manifest_version() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace replay
}  // namespace thyme
