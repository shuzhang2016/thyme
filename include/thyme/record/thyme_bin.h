#pragma once

#include "thyme/contracts/abi.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/contracts/struct_desc.h"
#include "thyme/record/file_ptr.h"
#include "thyme/record/package_hash.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace thyme {
inline constexpr char kThymeBinMagic[4] = {'T', 'H', 'Y', 'M'};
// Version history (ABI, docs/reference/abi-compatibility.md):
//   1 — streaming recording, no footer index;
//   2 — optional REC-04 footer index (seekable sinks only);
//   3 — ABI-10: a record-manifest block follows the struct registry (see
//       kThymeBinManifestMagic). Version 3 always carries the block; whether
//       the file also has a footer index is detected by the footer magic, so a
//       v3 byte stream is legal.
//   4 — ABI-11: the struct registry holds version-2 ThymeStructDesc entries
//       (layout_hash/channel/record_key) and the file always carries the
//       ABI-10 manifest block. RecordStructId remains a row index; the keys
//       let a newer topology map an old file by identity instead of id.
// Readers accept 1..4; version 0 is undefined and rejected, and a future
// version (> 4) is rejected rather than parsed with today's layout. Writers
// emit 4 only when the caller supplies a manifest block (a host with GEN-12
// codegen); otherwise they keep emitting 2 (seekable) / 1 (byte stream) with
// the legacy 40-byte registry, so a recorder without a manifest stays
// byte-identical to the pre-ABI-10 format.
inline constexpr uint16_t kThymeBinFileAbiVersion = 4;
inline constexpr uint16_t kThymeBinFileAbiVersionIndexed = 2;
inline constexpr uint16_t kThymeBinFileAbiVersionUnindexed = 1;
// First version that carries the ABI-10 block (and the last with the legacy
// 40-byte registry entry).
inline constexpr uint16_t kThymeBinFileAbiVersionManifest = 3;
// First version whose registry entries are ThymeStructDesc v2 (128 bytes).
inline constexpr uint16_t kThymeBinFileAbiVersionKeyedRegistry = 4;
inline constexpr uint32_t kThymeBinMaxStructCount = kAbiMaxStructCount;
// The .thyme.bin codec ceiling moved from the old 17024-byte limit to
// kAbiMaxPayloadBytes (512 KB). Recordings carrying payloads above the old
// ceiling are valid for new readers only; old readers reject them (expected —
// bumping a codec ceiling is not backward-compatible for the old reader side).
inline constexpr uint32_t kThymeBinMaxPayloadSize = kAbiMaxPayloadBytes;
enum class ThymeBinEntryType : uint8_t {
  kData = 0x01,
  kGap = 0x02,
  kTrigger = 0x03,
  kSession = 0x04,
};

struct ThymeBinFileHeader {
  char magic[4]{};
  uint16_t file_abi_version = kThymeBinFileAbiVersion;
  uint16_t reserved0 = 0;
  uint32_t device_id = 0;
  uint64_t session_id = 0;
  uint64_t start_mono_ns = 0;
  uint32_t struct_count = 0;
  uint32_t reserved1 = 0;
};

// The pre-ABI-11 registry entry (file versions 1..3). New readers convert it
// to ThymeStructDesc with the ABI-11 fields left empty, which is the
// documented legacy path: such a package maps by struct_id, not by key.
struct ThymeBinLegacyStructDesc {
  uint16_t struct_id = 0;
  uint16_t contract_version = 1;
  uint32_t payload_size = 0;
  char name[32]{};
};
static_assert(sizeof(ThymeBinLegacyStructDesc) == kThymeStructDescLegacySize,
              "legacy registry entry is the on-disk ABI (40 bytes)");

inline ThymeBinLegacyStructDesc ToThymeBinLegacyDesc(
    const ThymeStructDesc& d) {
  ThymeBinLegacyStructDesc out{};
  out.struct_id = d.struct_id;
  out.contract_version = d.contract_version;
  out.payload_size = d.payload_size;
  SetThymeStructText(out.name, sizeof(out.name), d.name);
  return out;
}

inline ThymeStructDesc FromThymeBinLegacyDesc(
    const ThymeBinLegacyStructDesc& d) {
  ThymeStructDesc out{};
  out.struct_id = d.struct_id;
  out.contract_version = d.contract_version;
  out.payload_size = d.payload_size;
  SetThymeStructText(out.name, sizeof(out.name), d.name);
  return out;
}

// ---- ABI-10 record-manifest block (file version 3+) -----------------------
// Written once, immediately after the struct registry, when the host supplies
// a GEN-12 record manifest. It pins the identity of the recorded channel set
// (kRecordManifestHash / version) inside the file itself, CRC-protected, so a
// reader can tell two recorders' id assignments apart before decoding a single
// record: RecordStructId is a *row index* into the sorted recorded-channel
// list, so adding/removing a channel renumbers rows and invalidates any
// decoder that was built against a different manifest.
//
// Legacy files (version 1/2) simply have no block: the manifest is then
// `absent` (documented legacy behaviour), never a decode failure — the struct
// registry still lets a tool decode by struct_id, but it must not assume the
// ids mean what the current build's RecordStructId enum says.
inline constexpr char kThymeBinManifestMagic[8] = {'T', 'H', 'Y',
                                                   'M', 'A', 'N', 'I', '1'};
inline constexpr uint16_t kThymeBinManifestBlockVersion = 1;

struct ThymeBinManifestBlock {
  char magic[8]{};          // kThymeBinManifestMagic
  uint16_t manifest_version = 0;  // record-manifest version (>= 1)
  uint16_t reserved0 = 0;   // must be 0
  uint32_t reserved1 = 0;   // must be 0
  uint64_t manifest_hash = 0;     // GEN-12 kRecordManifestHash
  uint32_t reserved2 = 0;   // must be 0
  uint32_t crc32 = 0;       // CRC32 over this struct with crc32 == 0
};
static_assert(sizeof(ThymeBinManifestBlock) == 32,
              "record-manifest block is on-disk ABI (32 bytes)");
static_assert(offsetof(ThymeBinManifestBlock, manifest_hash) == 16,
              "manifest block field order is on-disk ABI");

inline ThymeBinManifestBlock MakeThymeBinManifestBlock(uint64_t hash,
                                                       uint16_t version) {
  ThymeBinManifestBlock b{};
  for (int i = 0; i < 8; ++i) b.magic[i] = kThymeBinManifestMagic[i];
  b.manifest_version = version;
  b.manifest_hash = hash;
  return b;
}

// ---- Footer index (REC-04) ------------------------------------------------
// A seekable recording ends with:
//   [block entries: block_count * sizeof(ThymeBinIndexEntry)]
//   [ThymeBinIndexFooter]
// One entry is emitted every kThymeBinIndexBlockSize records, so a seek costs
// at most one block of sequential scanning.
inline constexpr char kThymeBinIndexMagic[8] = {'T', 'H', 'Y',
                                                'I', 'D', 'X', '1', '\0'};
inline constexpr uint16_t kThymeBinIndexVersion = 1;
inline constexpr uint32_t kThymeBinIndexBlockSize = 64;

struct ThymeBinIndexEntry {
  uint64_t offset = 0;           // absolute file offset of the record
  uint64_t mono_ns = 0;          // record timestamp (non-decreasing)
  uint64_t record_sequence = 0;  // data/trigger sequence, 0 for gaps
  uint32_t record_index = 0;     // global record ordinal (multiple of block size)
  uint32_t crc32 = 0;            // CRC over the first 24 bytes of this entry
};

struct ThymeBinIndexFooter {
  char magic[8]{};          // kThymeBinIndexMagic
  uint16_t index_version = kThymeBinIndexVersion;
  uint16_t reserved0 = 0;
  uint32_t block_size = kThymeBinIndexBlockSize;
  uint32_t block_count = 0;
  uint32_t crc32 = 0;       // CRC over this struct with crc32 == 0
  uint64_t entries_offset = 0;  // absolute offset of the first block entry
  uint64_t data_offset = 0;     // absolute offset of the first record
  uint64_t total_records = 0;   // records written through the encoder
  uint64_t first_mono_ns = 0;
  uint64_t last_mono_ns = 0;
};

struct ThymeBinDataEntry {
  ThymeBinEntryType type = ThymeBinEntryType::kData;
  uint64_t record_sequence = 0;
  uint16_t struct_id = 0;
  uint16_t reserved = 0;
  uint64_t mono_ns = 0;
  uint32_t source_sequence = 0;
  uint32_t payload_size = 0;
  std::vector<uint8_t> payload;
  uint32_t crc32 = 0;
  // kTrigger entries only: capture window parameters (0 for other types).
  // Previously crc-verified but discarded by ThymeBinReader; now surfaced so
  // replay tools can recover the trigger window from the package alone.
  uint64_t pre_roll_ns = 0;
  uint64_t post_roll_ns = 0;
};

// Byte sink for .thyme.bin (file, TCP socket, or fan-out).
class ThymeBinSink {
 public:
  virtual ~ThymeBinSink() = default;
  virtual bool ok() const = 0;
  virtual bool Write(const void* data, std::size_t n) = 0;
  virtual uint64_t write_failures() const { return 0; }
  // Footer index support: only sinks whose byte position is observable can
  // carry the trailing index. Stream sinks (TCP/fd) return false/-1.
  virtual bool Seekable() const { return false; }
  virtual int64_t Tell() const { return -1; }
};

class ThymeBinFileSink : public ThymeBinSink {
 public:
  explicit ThymeBinFileSink(const char* path);
  ThymeBinFileSink(const char* path, integrity::PackageHashState* hashes,
                   std::string relative_path);
  ~ThymeBinFileSink() override;
  bool ok() const override;
  bool Write(const void* data, std::size_t n) override;
  bool Seekable() const override { return fp_ != nullptr; }
  int64_t Tell() const override;
  bool Sync();

 private:
  FilePtr fp_;
  integrity::PackageHashState* hashes_ = nullptr;
  std::string relative_path_;
};

class ThymeBinFdSink : public ThymeBinSink {
 public:
  ThymeBinFdSink(int fd, bool owned, bool non_blocking = false);
  ~ThymeBinFdSink() override;
  bool ok() const override { return fd_ >= 0; }
  bool Write(const void* data, std::size_t n) override;
  uint64_t write_failures() const override { return write_failures_; }

 private:
  int fd_;
  bool owned_;
  bool non_blocking_;
  uint64_t write_failures_ = 0;
};

class ThymeBinFanoutSink : public ThymeBinSink {
 public:
  // Add sinks in priority order: index 0 = primary (typically file), index 1+ =
  // secondary best-effort (typically TCP). recorder_cli adds --out before --tcp.
  void Add(ThymeBinSink* sink);
  // Primary (index 0) only; secondary sinks must not make ok() true alone.
  bool ok() const override;
  // Returns primary (index 0) write result. Secondary failures disable that sink
  // but do not fail Write — backpressure follows the primary sink only.
  bool Write(const void* data, std::size_t n) override;
  // Index capability follows the primary sink: a file primary may index; a
  // TCP-only fan-out may not.
  bool Seekable() const override;
  int64_t Tell() const override;

 private:
  ThymeBinSink* sinks_[4]{};
  bool sink_live_[4]{true, true, true, true};
  std::size_t count_ = 0;
};

// Encodes THYM header + DATA entries onto any sink. Wire format == on-disk format.
class ThymeBinEncoder {
 public:
  explicit ThymeBinEncoder(ThymeBinSink* sink);
  bool ok() const;
  // `manifest` (ABI-10) is optional: when non-null the manifest-carrying file
  // ABI is selected and its CRC-protected block follows the struct registry.
  // Null keeps the legacy version (2 seekable / 1 stream) byte-for-byte.
  bool WriteHeader(uint64_t session_id, uint64_t start_mono_ns,
                   const ThymeStructDesc* descs, std::size_t count,
                   const ThymeBinManifestBlock* manifest = nullptr,
                   uint32_t device_id = 0);
  bool WriteData(uint64_t record_sequence, uint16_t struct_id, uint64_t mono_ns,
                 uint32_t source_sequence, const void* payload,
                 uint32_t payload_size);
  bool WriteTrigger(uint64_t trigger_id, uint64_t mono_ns, uint64_t pre_roll_ns,
                    uint64_t post_roll_ns);
  bool WriteGap(uint16_t struct_id, uint64_t mono_ns, uint16_t reason,
                uint32_t missed_count);
  bool WriteSession(uint64_t new_session_id, uint16_t reason);
  // Appends the footer index and marks the stream finished. Must be called
  // while the sink is still alive (clean close). Returns true for stream sinks
  // that simply cannot carry an index; false only on an actual write failure.
  // Idempotent. No records may be written after a successful Finish().
  bool Finish();
  uint64_t records_written() const { return records_written_; }
  bool has_index() const { return !index_entries_.empty(); }

 private:
  bool RecordWritten(int64_t start_offset, uint64_t mono_ns,
                     uint64_t record_sequence);
  ThymeBinSink* sink_;
  std::vector<ThymeBinIndexEntry> index_entries_;
  uint64_t records_written_ = 0;
  uint64_t data_offset_ = 0;
  uint64_t first_mono_ns_ = 0;
  uint64_t last_mono_ns_ = 0;
  bool seekable_ = false;
  bool finished_ = false;
};

// Convenience wrapper (local file only).
class ThymeBinWriter {
 public:
  explicit ThymeBinWriter(const char* path);
  ~ThymeBinWriter();
  ThymeBinWriter(const ThymeBinWriter&) = delete;
  ThymeBinWriter& operator=(const ThymeBinWriter&) = delete;
  bool ok() const { return encoder_.ok(); }
  bool WriteHeader(uint64_t session_id, uint64_t start_mono_ns,
                   const ThymeStructDesc* descs, std::size_t count) {
    return encoder_.WriteHeader(session_id, start_mono_ns, descs, count);
  }
  bool WriteHeader(uint64_t session_id, uint64_t start_mono_ns,
                   const ThymeStructDesc* descs, std::size_t count,
                   const ThymeBinManifestBlock* manifest) {
    return encoder_.WriteHeader(session_id, start_mono_ns, descs, count,
                                manifest);
  }
  bool WriteData(uint64_t record_sequence, uint16_t struct_id, uint64_t mono_ns,
                 uint32_t source_sequence, const void* payload,
                 uint32_t payload_size) {
    return encoder_.WriteData(record_sequence, struct_id, mono_ns,
                              source_sequence, payload, payload_size);
  }
  // Writes the footer index; also called automatically by the destructor.
  bool Finish() { return encoder_.Finish(); }

 private:
  ThymeBinFileSink file_;
  ThymeBinEncoder encoder_;
};

class ThymeBinReader {
 public:
  // R5: ReadNext is three-state — a corrupt record (bad CRC, truncated tail)
  // is reported with its offset and the expected/actual CRC instead of being
  // folded into EOF.
  enum class ReadResult : uint8_t { kOk, kEof, kCorrupt };

  explicit ThymeBinReader(const char* path);
  ~ThymeBinReader();
  ThymeBinReader(const ThymeBinReader&) = delete;
  ThymeBinReader& operator=(const ThymeBinReader&) = delete;
  bool ok() const { return fp_ != nullptr; }
  const ThymeBinFileHeader& header() const { return header_; }
  const std::vector<ThymeStructDesc>& registry() const { return registry_; }
  ReadResult ReadNext(ThymeBinDataEntry& entry);

  // ---- ABI-10 record manifest ----
  // kAbsent: legacy file (version 1/2) or a writer without a manifest. kOk:
  // the block parsed and its CRC matched. kInvalid: the block is present but
  // corrupt/unknown — the reader refused the file (ok() == false), so no
  // record is decoded under an unverified id assignment.
  enum class ManifestState : uint8_t { kAbsent, kOk, kInvalid };
  ManifestState manifest_state() const { return manifest_state_; }
  uint64_t manifest_hash() const { return manifest_hash_; }
  uint16_t manifest_version() const { return manifest_version_; }
  const char* manifest_error() const { return manifest_error_; }

  // ---- Footer index (REC-04) ----
  // kAbsent: old recording, crash-truncated file, or non-seekable capture.
  // kIoError: probing the index failed; kCorrupt: a footer was found but
  // failed validation. SeekToMonoNs refuses both non-ok outcomes.
  enum class IndexStatus : uint8_t { kAbsent, kOk, kCorrupt, kIoError };
  IndexStatus index_status() const { return index_status_; }
  bool HasIndex() const { return index_status_ == IndexStatus::kOk; }
  const char* index_error() const { return index_error_; }
  uint32_t index_block_count() const {
    return static_cast<uint32_t>(index_entries_.size());
  }
  uint32_t index_block_size() const { return index_block_size_; }
  // Summary over all records written through the encoder (0 when unknown).
  uint64_t first_mono_ns() const { return index_first_mono_ns_; }
  uint64_t last_mono_ns() const { return index_last_mono_ns_; }
  uint64_t indexed_record_count() const { return indexed_record_count_; }
  // Positions the reader at the last index block whose mono_ns <= target (or
  // at the first record when target precedes the recording). The caller then
  // advances with ReadNext(). Returns false when there is no usable index.
  bool SeekToMonoNs(uint64_t mono_ns);

  // Valid after a kCorrupt return: file offset of the record and the CRC
  // read vs recomputed (0 when the record was truncated before its CRC).
  uint64_t corrupt_offset() const { return corrupt_offset_; }
  uint32_t corrupt_crc_expected() const { return corrupt_crc_expected_; }
  uint32_t corrupt_crc_actual() const { return corrupt_crc_actual_; }

 private:
  void LoadIndex();
  FilePtr fp_;
  ThymeBinFileHeader header_{};
  std::vector<ThymeStructDesc> registry_;
  ManifestState manifest_state_ = ManifestState::kAbsent;
  uint64_t manifest_hash_ = 0;
  uint16_t manifest_version_ = 0;
  const char* manifest_error_ = nullptr;
  std::vector<ThymeBinIndexEntry> index_entries_;
  IndexStatus index_status_ = IndexStatus::kAbsent;
  const char* index_error_ = nullptr;
  uint32_t index_block_size_ = 0;
  uint64_t index_first_mono_ns_ = 0;
  uint64_t index_last_mono_ns_ = 0;
  uint64_t indexed_record_count_ = 0;
  uint64_t data_offset_ = 0;     // first record offset
  uint64_t footer_start_ = 0;    // first byte after records (0 = no footer)
  uint64_t corrupt_offset_ = 0;
  uint32_t corrupt_crc_expected_ = 0;
  uint32_t corrupt_crc_actual_ = 0;
};

}  // namespace thyme
