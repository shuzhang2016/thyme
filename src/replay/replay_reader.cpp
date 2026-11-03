#include "thyme/replay/replay_reader.h"

// Package ABI constants (FrameIndexRecord, struct ids, wal rows) come from the
// recorder header: the reader must never fork the on-disk contract.
#include "thyme/contracts/frame_header.h"
#include "thyme/record/file_ptr.h"
#include "thyme/record/triggered_recorder.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace thyme {
namespace replay {

namespace {

constexpr uint64_t kFileHeaderBytes = sizeof(ThymeBinFileHeader);
static_assert(sizeof(ThymeBinFileHeader) == 40, "thyme_bin header is package ABI");
static_assert(sizeof(triggered::FrameIndexRecord) == 32,
              "frame index record v2 is package ABI; v1 rows are 24 bytes");
// REC-28: v1 packages recorded 24-byte FrameIdx rows (no producer frame
// index). A v2 reader accepts both and leaves the new field at 0 for v1.
constexpr uint32_t kFrameIndexBytesV1 = 24;

inline bool DecodeFrameIndex(const uint8_t* payload, uint32_t size,
                             triggered::FrameIndexRecord* out) {
  if (size != sizeof(*out) && size != kFrameIndexBytesV1) return false;
  *out = triggered::FrameIndexRecord{};
  std::memcpy(out, payload, size);
  return true;
}

// Sparse block table stride for recordings without a REC-04 footer.
constexpr uint64_t kScanBlockRecords = 256;

const uint8_t kIdxMagic[8] = {'T', 'H', 'Y', 'I', 'D', 'X', '1', '\0'};

template <typename T>
T Load(const uint8_t* p) {
  T v{};
  std::memcpy(&v, p, sizeof(T));
  return v;
}

bool StatRegular(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool StatDirectory(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<std::string> ListDir(const std::string& path) {
  std::vector<std::string> names;
  DIR* d = ::opendir(path.c_str());
  if (d == nullptr) {
    return names;
  }
  while (dirent* e = ::readdir(d)) {
    if (e->d_name[0] == '.') {
      continue;
    }
    names.emplace_back(e->d_name);
  }
  ::closedir(d);
  std::sort(names.begin(), names.end());
  return names;
}

bool EndsWith(const std::string& s, const char* suffix) {
  const std::size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Read-only file mapping. A package is immutable while replay reads it; the
// mapping is never written, so every payload pointer handed out is stable
// until Close().
struct MappedFile {
  int fd = -1;
  const uint8_t* data = nullptr;
  uint64_t size = 0;

  MappedFile() = default;
  ~MappedFile() { Reset(); }
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  void Reset() {
    if (data != nullptr) {
      ::munmap(const_cast<uint8_t*>(data), static_cast<std::size_t>(size));
      data = nullptr;
    }
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
    size = 0;
  }

  bool Map(const std::string& path, std::string* why) {
    Reset();
    fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      *why = std::string("open failed: ") + std::strerror(errno);
      return false;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
      *why = "not a regular file";
      Reset();
      return false;
    }
    size = static_cast<uint64_t>(st.st_size);
    if (size == 0) {
      return true;  // valid for .raw; a tbin header scan reports the truncation
    }
    void* p = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ,
                     MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
      *why = std::string("mmap failed: ") + std::strerror(errno);
      data = nullptr;
      Reset();
      return false;
    }
    data = static_cast<const uint8_t*>(p);
    return true;
  }
};

// Field layout of the thyme_bin records, mirroring ThymeBinEncoder and
// ThymeBinReader. The CRC never covers the pad bytes nor the data record's
// `reserved` word, so it is computed field by field.
uint32_t StartCrc(uint8_t type) {
  return Crc32Update(0xFFFFFFFFu, &type, 1);
}

uint32_t FinishCrc(uint32_t c) { return c ^ 0xFFFFFFFFu; }

uint32_t DataCrc(uint8_t type, const uint8_t* rec, uint32_t payload_size,
                 const uint8_t* payload) {
  uint32_t c = StartCrc(type);
  c = Crc32Update(c, rec, 8);       // record_sequence
  c = Crc32Update(c, rec + 8, 2);   // struct_id
  c = Crc32Update(c, rec + 12, 8);  // mono_ns
  c = Crc32Update(c, rec + 20, 4);  // source_sequence
  c = Crc32Update(c, rec + 24, 4);  // payload_size
  if (payload != nullptr && payload_size != 0) {
    c = Crc32Update(c, payload, payload_size);
  }
  return FinishCrc(c);
}

uint32_t SessionCrc(uint8_t type, const uint8_t* rec) {
  uint32_t c = StartCrc(type);
  c = Crc32Update(c, rec, 8);      // new_session_id
  c = Crc32Update(c, rec + 8, 2);  // reason
  return FinishCrc(c);
}

// Trigger and GAP bodies are fully covered by their contiguous fields (pad
// bytes are only before the body).
uint32_t ContiguousCrc(uint8_t type, const uint8_t* body, uint64_t body_bytes) {
  uint32_t c = StartCrc(type);
  c = Crc32Update(c, body, static_cast<std::size_t>(body_bytes));
  return FinishCrc(c);
}

uint32_t FooterCrc(const ThymeBinIndexFooter& f) {
  ThymeBinIndexFooter copy = f;
  copy.crc32 = 0;
  return Crc32(reinterpret_cast<const uint8_t*>(&copy), sizeof(copy));
}

uint32_t IndexEntryCrc(const ThymeBinIndexEntry& e) {
  return Crc32(reinterpret_cast<const uint8_t*>(&e),
               offsetof(ThymeBinIndexEntry, crc32));
}

enum class ParseStatus : uint8_t { kOk, kEnd, kTruncated, kCorrupt };

struct RecordView {
  uint8_t type = 0;
  uint64_t mono_ns = 0;
  uint64_t seq = 0;
  uint16_t struct_id = 0;
  uint16_t reason = 0;
  uint32_t source_sequence = 0;
  uint32_t payload_size = 0;
  uint64_t payload_off = 0;  // absolute offset in the .tbin
  uint64_t next_pos = 0;
  uint64_t trigger_id = 0;
  uint64_t pre_roll_ns = 0;
  uint64_t post_roll_ns = 0;
  uint64_t session_id = 0;
};

// Parses one record starting at pos. Validates bounds and the record CRC; the
// Open scan uses the same routine, so iteration never sees a record the Open
// did not validate (barring concurrent modification of the package).
ParseStatus ParseRecord(const MappedFile& f, uint64_t pos, uint64_t end,
                        RecordView* out, std::string* why) {
  const uint8_t* base = f.data;
  if (pos == end) {
    return ParseStatus::kEnd;
  }
  if (pos > end || end - pos < 4) {
    *why = "record header truncated";
    return ParseStatus::kTruncated;
  }
  const uint8_t type_byte = base[pos];
  const uint8_t* rec = base + pos + 4;
  const uint64_t avail = end - pos - 4;

  if (type_byte == static_cast<uint8_t>(ThymeBinEntryType::kData)) {
    if (avail < 28 + 4) {
      *why = "data record truncated";
      return ParseStatus::kTruncated;
    }
    out->type = type_byte;
    out->seq = Load<uint64_t>(rec);
    out->struct_id = Load<uint16_t>(rec + 8);
    out->mono_ns = Load<uint64_t>(rec + 12);
    out->source_sequence = Load<uint32_t>(rec + 20);
    out->payload_size = Load<uint32_t>(rec + 24);
    if (out->payload_size > kAbiMaxPayloadBytes) {
      *why = "data payload above kAbiMaxPayloadBytes";
      return ParseStatus::kCorrupt;
    }
    const uint64_t payload_off = pos + 4 + 28;
    const uint64_t next = payload_off + out->payload_size + 4;
    if (next > end || next < payload_off) {
      *why = "data payload truncated";
      return ParseStatus::kTruncated;
    }
    const uint32_t want =
        DataCrc(type_byte, rec, out->payload_size, base + payload_off);
    const uint32_t got = Load<uint32_t>(base + payload_off + out->payload_size);
    if (want != got) {
      *why = "data record crc mismatch";
      return ParseStatus::kCorrupt;
    }
    out->payload_off = payload_off;
    out->next_pos = next;
    return ParseStatus::kOk;
  }

  if (type_byte == static_cast<uint8_t>(ThymeBinEntryType::kTrigger)) {
    if (avail < 32 + 4) {
      *why = "trigger record truncated";
      return ParseStatus::kTruncated;
    }
    out->type = type_byte;
    out->trigger_id = Load<uint64_t>(rec);
    out->mono_ns = Load<uint64_t>(rec + 8);
    out->pre_roll_ns = Load<uint64_t>(rec + 16);
    out->post_roll_ns = Load<uint64_t>(rec + 24);
    out->seq = out->trigger_id;
    const uint32_t want = ContiguousCrc(type_byte, rec, 32);
    const uint32_t got = Load<uint32_t>(rec + 32);
    if (want != got) {
      *why = "trigger record crc mismatch";
      return ParseStatus::kCorrupt;
    }
    out->next_pos = pos + 4 + 32 + 4;
    return ParseStatus::kOk;
  }

  if (type_byte == static_cast<uint8_t>(ThymeBinEntryType::kGap)) {
    if (avail < 16 + 4) {
      *why = "gap record truncated";
      return ParseStatus::kTruncated;
    }
    out->type = type_byte;
    out->struct_id = Load<uint16_t>(rec);
    out->reason = Load<uint16_t>(rec + 2);
    out->mono_ns = Load<uint64_t>(rec + 4);
    out->source_sequence = Load<uint32_t>(rec + 12);
    const uint32_t want = ContiguousCrc(type_byte, rec, 16);
    const uint32_t got = Load<uint32_t>(rec + 16);
    if (want != got) {
      *why = "gap record crc mismatch";
      return ParseStatus::kCorrupt;
    }
    out->next_pos = pos + 4 + 16 + 4;
    return ParseStatus::kOk;
  }

  if (type_byte == static_cast<uint8_t>(ThymeBinEntryType::kSession)) {
    if (avail < 12 + 4) {
      *why = "session record truncated";
      return ParseStatus::kTruncated;
    }
    out->type = type_byte;
    out->session_id = Load<uint64_t>(rec);
    out->reason = Load<uint16_t>(rec + 8);
    out->seq = out->session_id;
    const uint32_t want = SessionCrc(type_byte, rec);
    const uint32_t got = Load<uint32_t>(rec + 12);
    if (want != got) {
      *why = "session record crc mismatch";
      return ParseStatus::kCorrupt;
    }
    out->next_pos = pos + 4 + 12 + 4;
    return ParseStatus::kOk;
  }

  *why = "unknown record type";
  return ParseStatus::kCorrupt;
}

struct BlockPoint {
  uint64_t offset = 0;
  uint64_t mono_ns = 0;
};

struct Source {
  ReplayChannel pub;
  MappedFile tbin;
  MappedFile raw;
  // Seek blocks: REC-04 footer entries when present, otherwise
  // kScanBlockRecords-spaced points recorded by the Open validation scan.
  std::vector<BlockPoint> blocks;
  uint64_t data_offset = 0;
  uint64_t records_end = 0;
  uint64_t pos = 0;
  // Timestamp of the last timestamped record seen (kSession rows carry none;
  // they inherit this position on the timeline).
  uint64_t prev_mono_ns = 0;
  bool has_head = false;
  ReplayEvent head;
  uint32_t ordinal = 0;
  bool failed = false;
  std::string fail_error;
};

struct HeapCmp {
  bool operator()(const Source* a, const Source* b) const {
    const ReplayEvent& ea = a->head;
    const ReplayEvent& eb = b->head;
    if (ea.mono_ns != eb.mono_ns) {
      return ea.mono_ns > eb.mono_ns;
    }
    if (ea.channel != eb.channel) {
      return ea.channel > eb.channel;
    }
    if (ea.record_sequence != eb.record_sequence) {
      return ea.record_sequence > eb.record_sequence;
    }
    return a->ordinal > b->ordinal;
  }
};

struct FooterInfo {
  bool present = false;
  uint64_t entries_offset = 0;
  uint64_t total_records = 0;
  std::vector<ThymeBinIndexEntry> entries;
};

// Validates the REC-04 footer at the tail of a mapped .tbin. A footer whose
// magic is absent means "unindexed" (stream capture, old file, crash before
// Finish); a footer that is present but inconsistent is a hard error.
ReplayStatus LoadFooter(const MappedFile& f, uint64_t data_offset,
                        FooterInfo* out, std::string* why) {
  out->present = false;
  out->entries.clear();
  out->entries_offset = 0;
  out->total_records = 0;
  if (f.size < data_offset + sizeof(ThymeBinIndexFooter)) {
    return ReplayStatus::kOk;
  }
  const uint64_t footer_off = f.size - sizeof(ThymeBinIndexFooter);
  ThymeBinIndexFooter footer{};
  std::memcpy(&footer, f.data + footer_off, sizeof(footer));
  if (std::memcmp(footer.magic, kIdxMagic, sizeof(kIdxMagic)) != 0) {
    return ReplayStatus::kOk;
  }
  if (footer.index_version != kThymeBinIndexVersion) {
    *why = "unsupported index version";
    return ReplayStatus::kCorrupt;
  }
  if (footer.block_size != kThymeBinIndexBlockSize) {
    *why = "unsupported index block size";
    return ReplayStatus::kCorrupt;
  }
  if (FooterCrc(footer) != footer.crc32) {
    *why = "index footer crc mismatch";
    return ReplayStatus::kCorrupt;
  }
  if (footer.data_offset != data_offset) {
    *why = "index data offset mismatch";
    return ReplayStatus::kCorrupt;
  }
  if (footer.entries_offset < data_offset ||
      footer.entries_offset > footer_off) {
    *why = "index entries offset out of range";
    return ReplayStatus::kCorrupt;
  }
  const uint64_t entry_bytes =
      static_cast<uint64_t>(footer.block_count) * sizeof(ThymeBinIndexEntry);
  if (footer_off - footer.entries_offset != entry_bytes) {
    *why = "index extent mismatch";
    return ReplayStatus::kCorrupt;
  }
  std::vector<ThymeBinIndexEntry> entries(footer.block_count);
  if (!entries.empty()) {
    std::memcpy(entries.data(), f.data + footer.entries_offset,
                static_cast<std::size_t>(entry_bytes));
  }
  uint64_t prev_offset = 0;
  uint64_t prev_mono = 0;
  uint32_t prev_index = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const ThymeBinIndexEntry& e = entries[i];
    if (IndexEntryCrc(e) != e.crc32) {
      *why = "index entry crc mismatch";
      return ReplayStatus::kCorrupt;
    }
    if (e.offset < data_offset || e.offset >= footer.entries_offset) {
      *why = "index entry offset out of range";
      return ReplayStatus::kCorrupt;
    }
    if (e.record_index % footer.block_size != 0 ||
        e.record_index >= footer.total_records) {
      *why = "index entry record ordinal mismatch";
      return ReplayStatus::kCorrupt;
    }
    if (i > 0 && (e.offset <= prev_offset || e.record_index <= prev_index ||
                  e.mono_ns < prev_mono)) {
      *why = "index entries not monotonic";
      return ReplayStatus::kCorrupt;
    }
    prev_offset = e.offset;
    prev_mono = e.mono_ns;
    prev_index = e.record_index;
  }
  out->present = true;
  out->entries_offset = footer.entries_offset;
  out->total_records = footer.total_records;
  out->entries = std::move(entries);
  return ReplayStatus::kOk;
}

// Fills the public event fields shared by every source from a parsed record.
ReplayStatus BuildEvent(const Source& src, const RecordView& r, ReplayEvent* ev,
                        std::string* why) {
  *ev = ReplayEvent{};
  ev->mono_ns = r.mono_ns;
  ev->channel = src.pub.id;
  ev->struct_id = r.struct_id;
  ev->record_sequence = r.seq;
  ev->source_sequence = r.source_sequence;
  switch (r.type) {
    case static_cast<uint8_t>(ThymeBinEntryType::kData): {
      ev->kind = ReplayKind::kData;
      if (src.pub.kind == ReplayChannelKind::kPinnedSlot) {
        triggered::FrameIndexRecord idx{};
        if (!DecodeFrameIndex(src.tbin.data + r.payload_off, r.payload_size,
                              &idx)) {
          *why = "frame index record has an unexpected size";
          return ReplayStatus::kCorrupt;
        }
        const uint64_t end = static_cast<uint64_t>(idx.offset) + idx.len;
        if (end < static_cast<uint64_t>(idx.offset) || end > src.raw.size) {
          *why = "frame index points past the .raw file";
          return ReplayStatus::kTruncated;
        }
        ev->len = idx.len;
        ev->payload = idx.len == 0 ? nullptr : src.raw.data + idx.offset;
        ev->raw_offset = idx.offset;
        ev->frame_flags = idx.flags;
        ev->payload_crc = idx.payload_crc;
      } else {
        ev->len = r.payload_size;
        ev->payload =
            r.payload_size == 0 ? nullptr : src.tbin.data + r.payload_off;
      }
      return ReplayStatus::kOk;
    }
    case static_cast<uint8_t>(ThymeBinEntryType::kGap):
      ev->kind = ReplayKind::kGap;
      ev->gap_reason = r.reason;
      return ReplayStatus::kOk;
    case static_cast<uint8_t>(ThymeBinEntryType::kTrigger):
      ev->kind = ReplayKind::kTrigger;
      ev->trigger_id = r.trigger_id;
      ev->pre_roll_ns = r.pre_roll_ns;
      ev->post_roll_ns = r.post_roll_ns;
      return ReplayStatus::kOk;
    case static_cast<uint8_t>(ThymeBinEntryType::kSession):
      ev->kind = ReplayKind::kSession;
      ev->session_id = r.session_id;
      ev->session_reason = r.reason;
      return ReplayStatus::kOk;
    default:
      *why = "unknown record type";
      return ReplayStatus::kCorrupt;
  }
}

ReplayStatus Advance(Source* src, std::string* why) {
  src->has_head = false;
  RecordView r;
  const ParseStatus ps =
      ParseRecord(src->tbin, src->pos, src->records_end, &r, why);
  if (ps == ParseStatus::kEnd) {
    return ReplayStatus::kOk;
  }
  if (ps == ParseStatus::kTruncated) {
    src->failed = true;
    src->fail_error = *why;
    return ReplayStatus::kTruncated;
  }
  if (ps == ParseStatus::kCorrupt) {
    src->failed = true;
    src->fail_error = *why;
    return ReplayStatus::kCorrupt;
  }
  src->pos = r.next_pos;
  if (r.type == static_cast<uint8_t>(ThymeBinEntryType::kSession)) {
    r.mono_ns = src->prev_mono_ns;
  } else {
    src->prev_mono_ns = r.mono_ns;
  }
  const ReplayStatus st = BuildEvent(*src, r, &src->head, why);
  if (st != ReplayStatus::kOk) {
    src->failed = true;
    src->fail_error = *why;
    return st;
  }
  src->has_head = true;
  return ReplayStatus::kOk;
}

}  // namespace

const char* ReplayStatusName(ReplayStatus status) {
  switch (status) {
    case ReplayStatus::kOk:
      return "ok";
    case ReplayStatus::kEnd:
      return "end";
    case ReplayStatus::kNotADirectory:
      return "not-a-directory";
    case ReplayStatus::kMissingMeta:
      return "missing-meta";
    case ReplayStatus::kMissingRaw:
      return "missing-raw";
    case ReplayStatus::kOrphanRaw:
      return "orphan-raw";
    case ReplayStatus::kBadHeader:
      return "bad-header";
    case ReplayStatus::kTruncated:
      return "truncated";
    case ReplayStatus::kCorrupt:
      return "corrupt";
    case ReplayStatus::kTooManyChannels:
      return "too-many-channels";
    case ReplayStatus::kInvalidArgument:
      return "invalid-argument";
    case ReplayStatus::kStopped:
      return "stopped";
    case ReplayStatus::kBusy:
      return "busy";
    case ReplayStatus::kManifestMismatch:
      return "manifest-mismatch";
    case ReplayStatus::kBadSchema:
      return "bad-schema";
  }
  return "unknown";
}

const char* ReplayKindName(ReplayKind kind) {
  switch (kind) {
    case ReplayKind::kData:
      return "data";
    case ReplayKind::kGap:
      return "gap";
    case ReplayKind::kTrigger:
      return "trigger";
    case ReplayKind::kSession:
      return "session";
  }
  return "unknown";
}

struct ReplayPackage::Impl {
  std::string dir;
  std::string error;
  std::vector<std::unique_ptr<Source>> sources;
  std::vector<Source*> heap;
  std::vector<ReplayChannel> channels;
  ReplayStatus status = ReplayStatus::kOk;
  uint64_t delivered = 0;
  bool open = false;
  bool wal_present = false;
  bool wal_sealed = false;
  uint64_t first_mono_ns = 0;
  uint64_t last_mono_ns = 0;
  bool has_records = false;
  // REP-06: seek observability (see ReplayPackage::seek_*).
  uint64_t seek_calls = 0;
  uint64_t last_seek_records = 0;
  // ABI-09/ABI-10 manifest + sidecar identity (see ReplayPackage accessors).
  schema::ReadState schema_state = schema::ReadState::kAbsent;
  schema::PackageSchema schema;
  std::string schema_error;
  bool manifest_declared = false;
  uint64_t manifest_hash = 0;
  uint16_t manifest_version = 0;
  std::string manifest_source;  // first file that declared it (diagnostics)

  void Reset() {
    heap.clear();
    sources.clear();
    channels.clear();
    dir.clear();
    error.clear();
    status = ReplayStatus::kOk;
    delivered = 0;
    open = false;
    wal_present = false;
    wal_sealed = false;
    first_mono_ns = 0;
    last_mono_ns = 0;
    has_records = false;
    seek_calls = 0;
    last_seek_records = 0;
    schema_state = schema::ReadState::kAbsent;
    schema = schema::PackageSchema{};
    schema_error.clear();
    manifest_declared = false;
    manifest_hash = 0;
    manifest_version = 0;
    manifest_source.clear();
  }

  ReplayStatus Fail(ReplayStatus st, const std::string& path,
                    const std::string& why) {
    error = path.empty() ? why : path + ": " + why;
    return Abort(st);
  }

  ReplayStatus Abort(ReplayStatus st) {
    status = st;
    heap.clear();
    sources.clear();
    channels.clear();
    wal_present = false;
    wal_sealed = false;
    return st;
  }

  bool OpenTbinSource(const std::string& tbin_path, const std::string& raw_path,
                      ReplayChannelKind kind, const std::string& name,
                      ReplayStatus* st);
  void Prime();
  // Returns the number of records parsed while repositioning (REP-06 bound).
  uint64_t SeekSource(Source* s, uint64_t target);
  void CheckWal();
};

bool ReplayPackage::Impl::OpenTbinSource(const std::string& tbin_path,
                                         const std::string& raw_path,
                                         ReplayChannelKind kind,
                                         const std::string& name,
                                         ReplayStatus* st) {
  if (sources.size() >= kReplayMaxChannels) {
    error = "more than " + std::to_string(kReplayMaxChannels) + " sources";
    *st = ReplayStatus::kTooManyChannels;
    return false;
  }
  std::unique_ptr<Source> src(new Source());
  src->pub.id = static_cast<uint32_t>(sources.size());
  src->pub.kind = kind;
  src->pub.name = name;
  src->pub.tbin_path = tbin_path;
  src->pub.raw_path = raw_path;
  src->ordinal = src->pub.id;
  std::string why;
  if (!src->tbin.Map(tbin_path, &why)) {
    error = tbin_path + ": " + why;
    *st = ReplayStatus::kTruncated;
    return false;
  }
  const MappedFile& f = src->tbin;
  if (f.size < kFileHeaderBytes) {
    error = tbin_path + ": file shorter than the thyme_bin header";
    *st = ReplayStatus::kTruncated;
    return false;
  }
  ThymeBinFileHeader hdr{};
  std::memcpy(&hdr, f.data, sizeof(hdr));
  if (std::memcmp(hdr.magic, kThymeBinMagic, sizeof(hdr.magic)) != 0 ||
      hdr.file_abi_version > kThymeBinFileAbiVersion ||
      hdr.file_abi_version == 0) {
    error = tbin_path + ": bad thyme_bin magic/version";
    *st = ReplayStatus::kBadHeader;
    return false;
  }
  if (hdr.struct_count > kThymeBinMaxStructCount) {
    error = tbin_path + ": struct registry too large";
    *st = ReplayStatus::kBadHeader;
    return false;
  }
  const uint16_t file_version = hdr.file_abi_version;
  const bool keyed_registry =
      file_version >= kThymeBinFileAbiVersionKeyedRegistry;
  const uint64_t entry_bytes =
      keyed_registry ? sizeof(ThymeStructDesc)
                     : static_cast<uint64_t>(kThymeStructDescLegacySize);
  const uint64_t registry_bytes =
      static_cast<uint64_t>(hdr.struct_count) * entry_bytes;
  if (kFileHeaderBytes + registry_bytes > f.size) {
    error = tbin_path + ": struct registry truncated";
    *st = ReplayStatus::kTruncated;
    return false;
  }
  src->pub.registry.resize(hdr.struct_count);
  for (uint32_t i = 0; i < hdr.struct_count; ++i) {
    const uint8_t* p = f.data + kFileHeaderBytes + i * entry_bytes;
    if (keyed_registry) {
      std::memcpy(&src->pub.registry[i], p, sizeof(ThymeStructDesc));
    } else {
      ThymeBinLegacyStructDesc legacy{};
      std::memcpy(&legacy, p, sizeof(legacy));
      src->pub.registry[i] = FromThymeBinLegacyDesc(legacy);
    }
  }
  // Same registry rule as ThymeBinReader: contract_version 0 is "unset" and no
  // writer emits it, so the descriptor cannot be matched to a contract
  // (docs/reference/abi-compatibility.md). Unknown non-zero versions stay metadata for
  // the consumer.
  for (const ThymeStructDesc& d : src->pub.registry) {
    if (d.contract_version == 0) {
      error = tbin_path + ": struct registry entry has no contract version";
      *st = ReplayStatus::kBadHeader;
      return false;
    }
  }
  uint64_t data_offset = kFileHeaderBytes + registry_bytes;
  if (file_version >= kThymeBinFileAbiVersionManifest) {
    // ABI-10: a version-3+ file must carry a valid manifest block. It is the
    // file's own declaration of the recorded-channel id assignment; a bad
    // block is corruption, not "legacy", and no record may be decoded.
    if (data_offset + sizeof(ThymeBinManifestBlock) > f.size) {
      error = tbin_path + ": record-manifest block truncated";
      *st = ReplayStatus::kTruncated;
      return false;
    }
    ThymeBinManifestBlock block{};
    std::memcpy(&block, f.data + data_offset, sizeof(block));
    if (std::memcmp(block.magic, kThymeBinManifestMagic, sizeof(block.magic)) !=
            0 ||
        block.manifest_version == 0 || block.reserved0 != 0 ||
        block.reserved1 != 0 || block.reserved2 != 0 ||
        block.crc32 !=
            Crc32(reinterpret_cast<const uint8_t*>(&block),
                  offsetof(ThymeBinManifestBlock, crc32))) {
      error = tbin_path + ": invalid record-manifest block";
      *st = ReplayStatus::kCorrupt;
      return false;
    }
    if (manifest_declared &&
        (block.manifest_hash != manifest_hash ||
         block.manifest_version != manifest_version)) {
      error = tbin_path + ": record manifest differs from " + manifest_source +
              " (RecordStructId values are not comparable)";
      *st = ReplayStatus::kManifestMismatch;
      return false;
    }
    if (!manifest_declared) {
      manifest_declared = true;
      manifest_hash = block.manifest_hash;
      manifest_version = block.manifest_version;
      manifest_source = tbin_path;
    }
    data_offset += sizeof(ThymeBinManifestBlock);
  }
  src->data_offset = data_offset;

  FooterInfo footer;
  const ReplayStatus fst = LoadFooter(f, src->data_offset, &footer, &why);
  if (fst != ReplayStatus::kOk) {
    error = tbin_path + ": " + why;
    *st = fst;
    return false;
  }
  src->pub.has_footer_index = footer.present;
  const bool pinned_slot = kind == ReplayChannelKind::kPinnedSlot;
  if (pinned_slot) {
    if (!src->raw.Map(raw_path, &why)) {
      error = raw_path + ": " + why;
      *st = ReplayStatus::kTruncated;
      return false;
    }
  }
  src->records_end = footer.present ? footer.entries_offset : f.size;

  // Full validation scan. Also cross-checks the footer index against the
  // record stream, but only for the entries it visits: an entry that does not
  // sit on a record boundary is rejected (a seek there could not be trusted).
  std::size_t next_entry = 0;
  uint64_t pos = src->data_offset;
  uint64_t count = 0;
  uint64_t first = 0;
  uint64_t last = 0;
  uint64_t prev_mono = 0;
  bool any = false;
  uint64_t first_record_off = 0;
  uint64_t last_record_off = 0;
  uint32_t min_len = 0;
  bool any_raw = false;
  uint64_t first_raw = 0;
  uint64_t last_raw_end = 0;
  while (pos < src->records_end) {
    RecordView r;
    const uint64_t record_off = pos;
    const ParseStatus ps = ParseRecord(f, pos, src->records_end, &r, &why);
    if (ps == ParseStatus::kEnd) {
      break;
    }
    if (ps != ParseStatus::kOk) {
      char detail[96];
      std::snprintf(detail, sizeof(detail), " at offset %llu",
                    static_cast<unsigned long long>(pos));
      error = tbin_path + ": " + why + detail;
      *st = ps == ParseStatus::kTruncated ? ReplayStatus::kTruncated
                                          : ReplayStatus::kCorrupt;
      return false;
    }
    if (count == 0) {
      first_record_off = record_off;
    }
    last_record_off = record_off;
    if (r.type == static_cast<uint8_t>(ThymeBinEntryType::kData)) {
      src->pub.data_count += 1;
      if (!pinned_slot) {
        src->pub.max_data_len =
            std::max(src->pub.max_data_len, r.payload_size);
        if (r.payload_size != 0 &&
            (min_len == 0 || r.payload_size < min_len)) {
          min_len = r.payload_size;
        }
      }
    }
    if (pinned_slot &&
        r.type == static_cast<uint8_t>(ThymeBinEntryType::kData)) {
      triggered::FrameIndexRecord idx{};
      if (!DecodeFrameIndex(f.data + r.payload_off, r.payload_size, &idx)) {
        error = tbin_path + ": frame index record has an unexpected size";
        *st = ReplayStatus::kCorrupt;
        return false;
      }
      const uint64_t end = static_cast<uint64_t>(idx.offset) + idx.len;
      if (end < static_cast<uint64_t>(idx.offset) || end > src->raw.size) {
        error = tbin_path + ": frame index points past the .raw file";
        *st = ReplayStatus::kTruncated;
        return false;
      }
      src->pub.max_data_len = std::max(src->pub.max_data_len, idx.len);
      if (idx.len != 0 && (min_len == 0 || idx.len < min_len)) {
        min_len = idx.len;
      }
      if (idx.payload_crc == 0) {
        src->pub.frame_crc_absent += 1;
      } else {
        src->pub.frame_crc_declared += 1;
      }
      if (!any_raw) {
        first_raw = idx.offset;
        last_raw_end = end;
        any_raw = true;
      } else {
        first_raw = std::min<uint64_t>(first_raw, idx.offset);
        last_raw_end = std::max<uint64_t>(last_raw_end, end);
      }
    }
    if (r.type == static_cast<uint8_t>(ThymeBinEntryType::kGap)) {
      src->pub.gap_count += 1;
    }
    uint64_t mono = r.mono_ns;
    if (r.type == static_cast<uint8_t>(ThymeBinEntryType::kSession)) {
      mono = prev_mono;
    } else {
      prev_mono = r.mono_ns;
    }
    if (!any) {
      first = mono;
      last = mono;
      any = true;
    } else {
      first = std::min(first, mono);
      last = std::max(last, mono);
    }
    if (footer.present) {
      if (next_entry < footer.entries.size() &&
          footer.entries[next_entry].offset == pos) {
        const ThymeBinIndexEntry& e = footer.entries[next_entry];
        if (e.record_index != count || e.mono_ns != mono) {
          char detail[96];
          std::snprintf(detail, sizeof(detail), " at record %llu",
                        static_cast<unsigned long long>(count));
          error = tbin_path + ": index entry disagrees with the record stream" +
                  std::string(detail);
          *st = ReplayStatus::kCorrupt;
          return false;
        }
        ++next_entry;
      } else if (next_entry < footer.entries.size() &&
                 footer.entries[next_entry].offset < pos) {
        error = tbin_path + ": index entry is not a record boundary";
        *st = ReplayStatus::kCorrupt;
        return false;
      }
    } else if (count % kScanBlockRecords == 0) {
      src->blocks.push_back(BlockPoint{pos, r.mono_ns});
    }
    ++count;
    pos = r.next_pos;
  }
  if (pos != src->records_end) {
    error = tbin_path + ": record stream does not reach the index";
    *st = ReplayStatus::kTruncated;
    return false;
  }
  if (footer.present) {
    if (next_entry != footer.entries.size()) {
      error = tbin_path + ": index has entries past the record stream";
      *st = ReplayStatus::kCorrupt;
      return false;
    }
    if (footer.total_records != count) {
      error = tbin_path + ": record count disagrees with the index footer";
      *st = ReplayStatus::kCorrupt;
      return false;
    }
    src->blocks.reserve(footer.entries.size());
    for (const ThymeBinIndexEntry& e : footer.entries) {
      src->blocks.push_back(BlockPoint{e.offset, e.mono_ns});
    }
  }
  src->pub.record_count = count;
  src->pub.first_mono_ns = any ? first : 0;
  src->pub.last_mono_ns = any ? last : 0;
  src->pub.data_offset = src->data_offset;
  src->pub.records_end_offset = src->records_end;
  src->pub.first_record_offset = count == 0 ? 0 : first_record_off;
  src->pub.last_record_offset = count == 0 ? 0 : last_record_off;
  src->pub.min_data_len = min_len;
  src->pub.first_raw_offset = any_raw ? first_raw : 0;
  src->pub.last_raw_end = any_raw ? last_raw_end : 0;
  // ParseRecord verifies the CRC of every record it returns, so a successful
  // scan means every record in this source was CRC-checked.
  src->pub.record_crc_checked = count;
  src->pub.index_crc_checked = footer.present ? 1 : 0;
  src->pub.seek_entries = static_cast<uint32_t>(src->blocks.size());
  src->pub.seek_stride = footer.present
                             ? static_cast<uint32_t>(kThymeBinIndexBlockSize)
                             : static_cast<uint32_t>(kScanBlockRecords);
  src->pos = src->data_offset;
  *st = ReplayStatus::kOk;
  sources.push_back(std::move(src));
  return true;
}

void ReplayPackage::Impl::Prime() {
  heap.clear();
  for (auto& sp : sources) {
    Source* s = sp.get();
    s->pos = s->data_offset;
    s->prev_mono_ns = 0;
    s->failed = false;
    s->fail_error.clear();
    s->has_head = false;
    std::string why;
    const ReplayStatus st = Advance(s, &why);
    if (st != ReplayStatus::kOk) {
      error = s->pub.tbin_path + ": " + s->fail_error;
      status = st;
    }
    if (s->has_head) {
      heap.push_back(s);
    }
  }
  std::make_heap(heap.begin(), heap.end(), HeapCmp{});
}

uint64_t ReplayPackage::Impl::SeekSource(Source* s, uint64_t target) {
  uint64_t start = s->data_offset;
  uint64_t start_mono = 0;
  if (!s->blocks.empty()) {
    // O(log n): pick the last block whose first timestamp is <= target, then
    // scan at most one block forward. Without a footer the block table came
    // from the Open validation scan (kScanBlockRecords stride), so the bound
    // holds for unindexed files too.
    const auto it = std::upper_bound(
        s->blocks.begin(), s->blocks.end(), target,
        [](uint64_t v, const BlockPoint& b) { return v < b.mono_ns; });
    if (it != s->blocks.begin()) {
      start = (it - 1)->offset;
      start_mono = (it - 1)->mono_ns;
    }
  }
  uint64_t scanned = 0;
  s->pos = start;
  s->prev_mono_ns = start_mono;
  s->has_head = false;
  s->failed = false;
  s->fail_error.clear();
  std::string why;
  while (s->pos < s->records_end) {
    RecordView r;
    const ParseStatus ps =
        ParseRecord(s->tbin, s->pos, s->records_end, &r, &why);
    if (ps == ParseStatus::kEnd) {
      return scanned;
    }
    if (ps != ParseStatus::kOk) {
      s->failed = true;
      s->fail_error = why;
      status = ps == ParseStatus::kTruncated ? ReplayStatus::kTruncated
                                             : ReplayStatus::kCorrupt;
      error = s->pub.tbin_path + ": " + why;
      return scanned;
    }
    scanned += 1;
    s->pos = r.next_pos;
    if (r.type == static_cast<uint8_t>(ThymeBinEntryType::kSession)) {
      r.mono_ns = s->prev_mono_ns;
    } else {
      s->prev_mono_ns = r.mono_ns;
    }
    if (r.mono_ns >= target) {
      std::string build_why;
      const ReplayStatus bst = BuildEvent(*s, r, &s->head, &build_why);
      if (bst != ReplayStatus::kOk) {
        s->failed = true;
        s->fail_error = build_why;
        status = bst;
        error = s->pub.tbin_path + ": " + build_why;
        return scanned;
      }
      s->has_head = true;
      return scanned;
    }
  }
  return scanned;
}

void ReplayPackage::Impl::CheckWal() {
  wal_present = false;
  wal_sealed = false;
  const std::string path = dir + "/wal.log";
  FilePtr f(std::fopen(path.c_str(), "rb"));
  if (f == nullptr) {
    return;
  }
  wal_present = true;
  triggered::WalHeader hdr{};
  if (std::fread(&hdr, sizeof(hdr), 1, f.get()) == 1 &&
      std::memcmp(hdr.magic, triggered::kWalMagic, sizeof(hdr.magic)) == 0) {
    triggered::WalRow row{};
    uint8_t last_op = 0;
    while (std::fread(&row, sizeof(row), 1, f.get()) == 1) {
      last_op = row.op;
    }
    wal_sealed = last_op == triggered::kWalOpSeal;
  }
}

ReplayPackage::ReplayPackage() : impl_(std::make_unique<Impl>()) {}

ReplayPackage::~ReplayPackage() = default;

ReplayPackage::ReplayPackage(ReplayPackage&&) noexcept = default;
ReplayPackage& ReplayPackage::operator=(ReplayPackage&&) noexcept = default;

ReplayStatus ReplayPackage::Open(const std::string& dir) {
  if (!impl_) {
    impl_ = std::make_unique<Impl>();
  } else {
    impl_->Reset();
  }
  impl_->dir = dir;
  if (!StatDirectory(dir)) {
    return impl_->Fail(ReplayStatus::kNotADirectory, dir, "not a directory");
  }
  const std::string meta_path = dir + "/meta.tbin";
  if (!StatRegular(meta_path)) {
    return impl_->Fail(ReplayStatus::kMissingMeta, meta_path,
                       "required package file missing");
  }
  ReplayStatus st = ReplayStatus::kOk;
  if (!impl_->OpenTbinSource(meta_path, "", ReplayChannelKind::kMeta, "meta",
                             &st)) {
    return impl_->Abort(st);
  }
  const std::string small_path = dir + "/small.tbin";
  if (StatRegular(small_path)) {
    if (!impl_->OpenTbinSource(small_path, "", ReplayChannelKind::kSmall,
                               "small", &st)) {
      return impl_->Abort(st);
    }
  } else {
    struct stat sst {};
    if (::stat(small_path.c_str(), &sst) == 0) {
      return impl_->Fail(ReplayStatus::kTruncated, small_path,
                         "not a regular file");
    }
  }

  const std::string frames_dir = dir + "/frames";
  if (StatDirectory(frames_dir)) {
    const std::vector<std::string> names = ListDir(frames_dir);
    std::vector<std::string> stems;
    for (const std::string& n : names) {
      if (EndsWith(n, ".index.tbin")) {
        stems.push_back(n.substr(0, n.size() - std::strlen(".index.tbin")));
      }
    }
    for (const std::string& stem : stems) {
      const std::string idx = frames_dir + "/" + stem + ".index.tbin";
      const std::string raw = frames_dir + "/" + stem + ".raw";
      if (!StatRegular(raw)) {
        return impl_->Fail(ReplayStatus::kMissingRaw, raw,
                           "frame index without its .raw segment file");
      }
      if (!impl_->OpenTbinSource(idx, raw, ReplayChannelKind::kPinnedSlot, stem,
                                 &st)) {
        return impl_->Abort(st);
      }
    }
    for (const std::string& n : names) {
      if (!EndsWith(n, ".raw")) {
        continue;
      }
      const std::string stem = n.substr(0, n.size() - std::strlen(".raw"));
      if (std::find(stems.begin(), stems.end(), stem) == stems.end()) {
        return impl_->Fail(ReplayStatus::kOrphanRaw, frames_dir + "/" + n,
                           ".raw without a .index.tbin");
      }
    }
  } else {
    struct stat sst {};
    if (::stat(frames_dir.c_str(), &sst) == 0) {
      return impl_->Fail(ReplayStatus::kNotADirectory, frames_dir,
                         "frames is not a directory");
    }
  }

  // ABI-09: the sidecar is part of the package contract. It is read after
  // the .tbin headers so the two independent manifest declarations can be
  // cross-checked: a sidecar without a manifest block (or vice versa) cannot
  // be verified and is refused rather than silently trusted.
  {
    std::string serr;
    impl_->schema_state =
        schema::ReadPackageSchema(dir, &impl_->schema, &serr);
    impl_->schema_error = serr;
    if (impl_->schema_state == schema::ReadState::kInvalid) {
      return impl_->Fail(ReplayStatus::kBadSchema, dir + "/" + schema::kFileName,
                         serr);
    }
    if (impl_->schema_state == schema::ReadState::kAbsent &&
        impl_->manifest_declared) {
      return impl_->Fail(
          ReplayStatus::kManifestMismatch, dir + "/" + schema::kFileName,
          "sidecar missing but the .tbin declares a record manifest (tamper "
          "or an incomplete package)");
    }
    if (impl_->schema_state == schema::ReadState::kOk) {
      const bool sidecar_declared = impl_->schema.manifest_declared;
      if (sidecar_declared != impl_->manifest_declared) {
        return impl_->Fail(
            ReplayStatus::kManifestMismatch, dir + "/" + schema::kFileName,
            sidecar_declared
                ? "schema declares a record manifest but no .tbin carries one"
                : "schema has no record manifest but the .tbin carries one");
      }
      if (sidecar_declared &&
          (impl_->schema.manifest_hash != impl_->manifest_hash ||
           impl_->schema.manifest_version != impl_->manifest_version)) {
        return impl_->Fail(
            ReplayStatus::kManifestMismatch, dir + "/" + schema::kFileName,
            "schema manifest " +
                schema::FormatManifestHash(impl_->schema.manifest_hash) +
                " v" + std::to_string(impl_->schema.manifest_version) +
                " differs from the meta.tbin block " +
                schema::FormatManifestHash(impl_->manifest_hash) + " v" +
                std::to_string(impl_->manifest_version));
      }
    }
  }

  impl_->channels.reserve(impl_->sources.size());
  for (const auto& sp : impl_->sources) {
    impl_->channels.push_back(sp->pub);
  }
  impl_->CheckWal();
  impl_->Prime();
  if (impl_->status != ReplayStatus::kOk) {
    return impl_->Abort(impl_->status);
  }
  for (const ReplayChannel& c : impl_->channels) {
    if (c.record_count == 0) {
      continue;
    }
    if (!impl_->has_records) {
      impl_->first_mono_ns = c.first_mono_ns;
      impl_->last_mono_ns = c.last_mono_ns;
      impl_->has_records = true;
    } else {
      impl_->first_mono_ns = std::min(impl_->first_mono_ns, c.first_mono_ns);
      impl_->last_mono_ns = std::max(impl_->last_mono_ns, c.last_mono_ns);
    }
  }
  impl_->open = true;
  impl_->status = ReplayStatus::kOk;
  return ReplayStatus::kOk;
}

void ReplayPackage::Close() {
  if (impl_) {
    impl_->Reset();
  }
}

bool ReplayPackage::is_open() const { return impl_ && impl_->open; }

ReplayStatus ReplayPackage::NextEvent(ReplayEvent* out) {
  if (!impl_ || !impl_->open) {
    return ReplayStatus::kInvalidArgument;
  }
  if (out == nullptr) {
    return ReplayStatus::kInvalidArgument;
  }
  if (impl_->status != ReplayStatus::kOk &&
      impl_->status != ReplayStatus::kEnd) {
    return impl_->status;
  }
  if (impl_->heap.empty()) {
    impl_->status = ReplayStatus::kEnd;
    return ReplayStatus::kEnd;
  }
  std::pop_heap(impl_->heap.begin(), impl_->heap.end(), HeapCmp{});
  Source* src = impl_->heap.back();
  impl_->heap.pop_back();
  *out = src->head;
  impl_->delivered += 1;
  std::string why;
  const ReplayStatus st = Advance(src, &why);
  if (st != ReplayStatus::kOk) {
    impl_->error = src->pub.tbin_path + ": " + why;
    impl_->status = st;
    return st;
  }
  if (src->has_head) {
    impl_->heap.push_back(src);
    std::push_heap(impl_->heap.begin(), impl_->heap.end(), HeapCmp{});
  }
  impl_->status = ReplayStatus::kOk;
  return ReplayStatus::kOk;
}

ReplayStatus ReplayPackage::SeekMonoNs(uint64_t mono_ns) {
  if (!impl_ || !impl_->open) {
    return ReplayStatus::kInvalidArgument;
  }
  impl_->status = ReplayStatus::kOk;
  impl_->error.clear();
  impl_->seek_calls += 1;
  impl_->last_seek_records = 0;
  for (auto& sp : impl_->sources) {
    impl_->last_seek_records += impl_->SeekSource(sp.get(), mono_ns);
    if (impl_->status != ReplayStatus::kOk) {
      return impl_->status;
    }
  }
  impl_->heap.clear();
  for (auto& sp : impl_->sources) {
    if (sp->has_head) {
      impl_->heap.push_back(sp.get());
    }
  }
  std::make_heap(impl_->heap.begin(), impl_->heap.end(), HeapCmp{});
  return ReplayStatus::kOk;
}

uint64_t ReplayPackage::first_mono_ns() const {
  return impl_ ? impl_->first_mono_ns : 0;
}

uint64_t ReplayPackage::last_mono_ns() const {
  return impl_ ? impl_->last_mono_ns : 0;
}

const std::string& ReplayPackage::dir() const {
  static const std::string kEmpty;
  return impl_ ? impl_->dir : kEmpty;
}

const std::vector<ReplayChannel>& ReplayPackage::channels() const {
  static const std::vector<ReplayChannel> kEmpty;
  return impl_ ? impl_->channels : kEmpty;
}

const ReplayChannel* ReplayPackage::FindChannel(uint32_t id) const {
  if (!impl_) {
    return nullptr;
  }
  for (const ReplayChannel& c : impl_->channels) {
    if (c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

ReplayStatus ReplayPackage::status() const {
  return impl_ ? impl_->status : ReplayStatus::kInvalidArgument;
}

const std::string& ReplayPackage::error() const {
  static const std::string kEmpty;
  return impl_ ? impl_->error : kEmpty;
}

uint64_t ReplayPackage::events_delivered() const {
  return impl_ ? impl_->delivered : 0;
}

uint64_t ReplayPackage::seek_calls() const {
  return impl_ ? impl_->seek_calls : 0;
}

uint64_t ReplayPackage::seek_records_scanned() const {
  return impl_ ? impl_->last_seek_records : 0;
}

bool ReplayPackage::wal_present() const { return impl_ && impl_->wal_present; }

bool ReplayPackage::package_complete() const {
  return impl_ && impl_->wal_sealed;
}

schema::ReadState ReplayPackage::schema_state() const {
  return impl_ ? impl_->schema_state : schema::ReadState::kAbsent;
}

const schema::PackageSchema& ReplayPackage::schema() const {
  static const schema::PackageSchema kEmpty;
  return impl_ ? impl_->schema : kEmpty;
}

const std::string& ReplayPackage::schema_error() const {
  static const std::string kEmpty;
  return impl_ ? impl_->schema_error : kEmpty;
}

bool ReplayPackage::manifest_declared() const {
  return impl_ && impl_->manifest_declared;
}

uint64_t ReplayPackage::manifest_hash() const {
  return impl_ ? impl_->manifest_hash : 0;
}

uint16_t ReplayPackage::manifest_version() const {
  return impl_ ? impl_->manifest_version : 0;
}

}  // namespace replay
}  // namespace thyme
