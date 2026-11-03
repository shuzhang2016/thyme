#include "thyme/record/thyme_bin.h"

#include "thyme/contracts/frame_header.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace thyme {

static_assert(sizeof(ThymeBinIndexEntry) == 32, "index entry on-disk layout");
static_assert(sizeof(ThymeBinIndexFooter) == 64, "index footer on-disk layout");

namespace {

uint32_t Crc32EntryFields(const ThymeBinDataEntry& e, const uint8_t* payload,
                          uint32_t payload_size) {
  uint32_t c = 0xFFFFFFFFu;
  const uint8_t type_byte = static_cast<uint8_t>(e.type);
  c = Crc32Update(c, &type_byte, 1);
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&e.record_sequence),
                  sizeof(e.record_sequence));
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&e.struct_id),
                  sizeof(e.struct_id));
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&e.mono_ns),
                  sizeof(e.mono_ns));
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&e.source_sequence),
                  sizeof(e.source_sequence));
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&e.payload_size),
                  sizeof(e.payload_size));
  if (payload != nullptr && payload_size != 0) {
    c = Crc32Update(c, payload, payload_size);
  }
  return c ^ 0xFFFFFFFFu;
}

uint32_t Crc32Entry(const ThymeBinDataEntry& e) {
  return Crc32EntryFields(e, e.payload.empty() ? nullptr : e.payload.data(),
                          static_cast<uint32_t>(e.payload.size()));
}

uint32_t Crc32Trigger(uint64_t trigger_id, uint64_t mono_ns, uint64_t pre_roll_ns,
                      uint64_t post_roll_ns) {
  uint32_t c = 0xFFFFFFFFu;
  const uint8_t type_byte = static_cast<uint8_t>(ThymeBinEntryType::kTrigger);
  c = Crc32Update(c, &type_byte, 1);
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&trigger_id),
                  sizeof(trigger_id));
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&mono_ns), sizeof(mono_ns));
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&pre_roll_ns),
                  sizeof(pre_roll_ns));
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&post_roll_ns),
                  sizeof(post_roll_ns));
  return c ^ 0xFFFFFFFFu;
}

uint32_t Crc32Session(uint64_t new_session_id, uint16_t reason) {
  uint32_t c = 0xFFFFFFFFu;
  const uint8_t type_byte = static_cast<uint8_t>(ThymeBinEntryType::kSession);
  c = Crc32Update(c, &type_byte, 1);
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&new_session_id),
                  sizeof(new_session_id));
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&reason), sizeof(reason));
  return c ^ 0xFFFFFFFFu;
}

uint32_t Crc32Gap(uint16_t struct_id, uint16_t reason, uint64_t mono_ns,
                  uint32_t missed_count) {
  uint32_t c = 0xFFFFFFFFu;
  const uint8_t type_byte = static_cast<uint8_t>(ThymeBinEntryType::kGap);
  c = Crc32Update(c, &type_byte, 1);
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&struct_id),
                  sizeof(struct_id));
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&reason), sizeof(reason));
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&mono_ns), sizeof(mono_ns));
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&missed_count),
                  sizeof(missed_count));
  return c ^ 0xFFFFFFFFu;
}

bool ReadExact(std::FILE* fp, void* buf, std::size_t n) {
  return std::fread(buf, 1, n, fp) == n;
}

bool SinkWrite(ThymeBinSink* sink, const void* buf, std::size_t n) {
  return sink != nullptr && sink->ok() && sink->Write(buf, n);
}

uint32_t Crc32Bytes(const void* data, std::size_t n) {
  return Crc32Update(0xFFFFFFFFu, static_cast<const uint8_t*>(data), n) ^
         0xFFFFFFFFu;
}

uint32_t IndexEntryCrc(const ThymeBinIndexEntry& e) {
  return Crc32Bytes(&e, offsetof(ThymeBinIndexEntry, crc32));
}

uint32_t IndexFooterCrc(const ThymeBinIndexFooter& f) {
  ThymeBinIndexFooter copy = f;
  copy.crc32 = 0;
  return Crc32Bytes(&copy, sizeof(copy));
}

uint32_t ManifestBlockCrc(const ThymeBinManifestBlock& b) {
  return Crc32Bytes(&b, offsetof(ThymeBinManifestBlock, crc32));
}

}  // namespace

ThymeBinFileSink::ThymeBinFileSink(const char* path)
    : ThymeBinFileSink(path, nullptr, {}) {}

ThymeBinFileSink::ThymeBinFileSink(const char* path,
                                   integrity::PackageHashState* hashes,
                                   std::string relative_path)
    : fp_(std::fopen(path, "wb")),
      hashes_(hashes),
      relative_path_(std::move(relative_path)) {
  if (fp_ && hashes_ != nullptr) hashes_->Track(relative_path_);
}

ThymeBinFileSink::~ThymeBinFileSink() = default;

bool ThymeBinFileSink::ok() const { return fp_ != nullptr; }

bool ThymeBinFileSink::Write(const void* data, std::size_t n) {
  if (!fp_) return false;
  if (hashes_ != nullptr) {
    const long offset = std::ftell(fp_.get());
    if (offset < 0 ||
        static_cast<uint64_t>(offset) != hashes_->Size(relative_path_)) {
      return false;  // REC-24: seek+overwrite must reset via Replace.
    }
  }
  const std::size_t written = std::fwrite(data, 1, n, fp_.get());
  if (hashes_ != nullptr && written != 0) {
    hashes_->Append(relative_path_, data, written);
  }
  return written == n;
}

int64_t ThymeBinFileSink::Tell() const {
  if (!fp_) return -1;
  const long pos = std::ftell(fp_.get());
  return pos < 0 ? -1 : static_cast<int64_t>(pos);
}

bool ThymeBinFileSink::Sync() {
  if (!fp_) return false;
  if (std::fflush(fp_.get()) != 0) return false;
#if !defined(_WIN32)
  return fsync(fileno(fp_.get())) == 0;
#else
  return true;
#endif
}

ThymeBinFdSink::ThymeBinFdSink(int fd, bool owned, bool non_blocking)
    : fd_(fd), owned_(owned), non_blocking_(non_blocking) {
#if !defined(_WIN32)
  if (non_blocking_ && fd_ >= 0) {
    const int flags = fcntl(fd_, F_GETFL, 0);
    if (flags >= 0) fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
  }
#else
  (void)non_blocking_;
#endif
}

ThymeBinFdSink::~ThymeBinFdSink() {
  if (owned_ && fd_ >= 0) {
#if defined(_WIN32)
    closesocket(fd_);
#else
    close(fd_);
#endif
  }
}

bool ThymeBinFdSink::Write(const void* data, std::size_t n) {
  if (fd_ < 0) return false;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  std::size_t off = 0;
  while (off < n) {
#if defined(_WIN32)
    const int w = send(fd_, reinterpret_cast<const char*>(p + off),
                       static_cast<int>(n - off), 0);
#else
    const int flags = non_blocking_ ? MSG_NOSIGNAL | MSG_DONTWAIT : MSG_NOSIGNAL;
    const ssize_t w = send(fd_, p + off, n - off, flags);
#endif
    if (w <= 0) {
      ++write_failures_;
#if !defined(_WIN32)
      if (non_blocking_ && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (off > 0 && fd_ >= 0) {
          // Partial frame already on the wire; disable sink to avoid a corrupt
          // stream. Close only when owned, but disable in both cases.
          if (owned_) close(fd_);
          fd_ = -1;
        }
        return false;
      }
#endif
      if (owned_ && fd_ >= 0) {
#if defined(_WIN32)
        closesocket(fd_);
#else
        close(fd_);
#endif
        fd_ = -1;
      }
      return false;
    }
    off += static_cast<std::size_t>(w);
  }
  return true;
}

void ThymeBinFanoutSink::Add(ThymeBinSink* sink) {
  if (sink == nullptr || count_ >= 4) return;
  sinks_[count_] = sink;
  sink_live_[count_] = true;
  ++count_;
}

bool ThymeBinFanoutSink::ok() const {
  if (count_ == 0) return false;
  // Primary (index 0) alone drives encoder ok()/backpressure; secondaries are
  // best-effort mirrors and must not mask a dead primary (silent TCP-only record).
  return sink_live_[0] && sinks_[0] != nullptr && sinks_[0]->ok();
}

bool ThymeBinFanoutSink::Seekable() const {
  return count_ > 0 && sink_live_[0] && sinks_[0] != nullptr &&
         sinks_[0]->Seekable();
}

int64_t ThymeBinFanoutSink::Tell() const {
  if (count_ == 0 || !sink_live_[0] || sinks_[0] == nullptr) return -1;
  return sinks_[0]->Tell();
}

bool ThymeBinFanoutSink::Write(const void* data, std::size_t n) {
  // Primary sink (index 0, typically ThymeBinFileSink) determines Write()
  // success and backpressure policy. Secondary sinks (e.g. TCP) are mirrored
  // best-effort; a secondary failure disables that sink but does not fail the write.
  bool primary_ok = false;
  for (std::size_t i = 0; i < count_; ++i) {
    if (!sink_live_[i] || sinks_[i] == nullptr) continue;
    const bool write_ok = sinks_[i]->Write(data, n);
    if (i == 0) {
      primary_ok = write_ok;
      if (!write_ok) sink_live_[0] = false;
    } else if (!write_ok && !sinks_[i]->ok()) {
      sink_live_[i] = false;
    }
  }
  return primary_ok;
}

ThymeBinEncoder::ThymeBinEncoder(ThymeBinSink* sink) : sink_(sink) {}

bool ThymeBinEncoder::ok() const { return sink_ != nullptr && sink_->ok(); }

bool ThymeBinEncoder::WriteHeader(uint64_t session_id, uint64_t start_mono_ns,
                                  const ThymeStructDesc* descs,
                                  std::size_t count,
                                  const ThymeBinManifestBlock* manifest,
                                  uint32_t device_id) {
  if (!ok()) return false;
  if (count > kThymeBinMaxStructCount) return false;
  if (manifest != nullptr && manifest->manifest_version == 0) return false;
  seekable_ = sink_->Seekable() && sink_->Tell() >= 0;
  index_entries_.clear();
  records_written_ = 0;
  data_offset_ = 0;
  first_mono_ns_ = 0;
  last_mono_ns_ = 0;
  finished_ = false;
  ThymeBinFileHeader h{};
  std::fill_n(reinterpret_cast<uint8_t*>(&h), sizeof(h), uint8_t{0});
  std::memcpy(h.magic, kThymeBinMagic, 4);
  if (manifest != nullptr) {
    h.file_abi_version = kThymeBinFileAbiVersion;
  } else {
    h.file_abi_version = seekable_ ? kThymeBinFileAbiVersionIndexed
                                   : kThymeBinFileAbiVersionUnindexed;
  }
  h.device_id = device_id;
  h.session_id = session_id;
  h.start_mono_ns = start_mono_ns;
  h.struct_count = static_cast<uint32_t>(count);
  if (!SinkWrite(sink_, &h, sizeof(h))) return false;
  const bool keyed_registry = manifest != nullptr;
  for (std::size_t i = 0; i < count; ++i) {
    if (keyed_registry) {
      if (!SinkWrite(sink_, &descs[i], sizeof(ThymeStructDesc))) return false;
    } else {
      const ThymeBinLegacyStructDesc legacy = ToThymeBinLegacyDesc(descs[i]);
      if (!SinkWrite(sink_, &legacy, sizeof(legacy))) return false;
    }
  }
  if (manifest != nullptr) {
    ThymeBinManifestBlock block = *manifest;
    // Magic and CRC are the writer's, never the caller's: a stale/zero field
    // in the caller's struct would otherwise produce an unreadable file.
    for (int i = 0; i < 8; ++i) block.magic[i] = kThymeBinManifestMagic[i];
    block.crc32 = ManifestBlockCrc(block);
    if (!SinkWrite(sink_, &block, sizeof(block))) return false;
  }
  if (seekable_) {
    const int64_t pos = sink_->Tell();
    if (pos >= 0) data_offset_ = static_cast<uint64_t>(pos);
  }
  return true;
}

bool ThymeBinEncoder::RecordWritten(int64_t start_offset, uint64_t mono_ns,
                                    uint64_t record_sequence) {
  const uint64_t ordinal = records_written_;
  if (ordinal == 0) first_mono_ns_ = mono_ns;
  last_mono_ns_ = mono_ns;
  ++records_written_;
  if (!seekable_ || start_offset < 0) return true;
  if (ordinal % kThymeBinIndexBlockSize != 0) return true;
  // Keep the index monotonic: a late sample below the last indexed timestamp
  // is reachable by scanning from the previous block, so do not index it.
  if (!index_entries_.empty() &&
      mono_ns < index_entries_.back().mono_ns) {
    return true;
  }
  ThymeBinIndexEntry e{};
  e.offset = static_cast<uint64_t>(start_offset);
  e.mono_ns = mono_ns;
  e.record_sequence = record_sequence;
  e.record_index = static_cast<uint32_t>(ordinal);
  e.crc32 = IndexEntryCrc(e);
  index_entries_.push_back(e);
  return true;
}

bool ThymeBinEncoder::WriteData(uint64_t record_sequence, uint16_t struct_id,
                                uint64_t mono_ns, uint32_t source_sequence,
                                const void* payload, uint32_t payload_size) {
  if (!ok() || finished_) return false;
  if (payload_size > kThymeBinMaxPayloadSize) return false;
  if (payload == nullptr && payload_size > 0) return false;
  const int64_t start_offset = seekable_ ? sink_->Tell() : -1;
  ThymeBinDataEntry e{};
  e.type = ThymeBinEntryType::kData;
  e.record_sequence = record_sequence;
  e.struct_id = struct_id;
  e.mono_ns = mono_ns;
  e.source_sequence = source_sequence;
  e.payload_size = payload_size;
  const auto* p = static_cast<const uint8_t*>(payload);
  e.crc32 = Crc32EntryFields(e, p, payload_size);

  const uint8_t type_byte = static_cast<uint8_t>(e.type);
  if (!SinkWrite(sink_, &type_byte, 1)) return false;
  uint8_t pad[3] = {};
  if (!SinkWrite(sink_, pad, 3)) return false;
  if (!SinkWrite(sink_, &e.record_sequence, sizeof(e.record_sequence)))
    return false;
  if (!SinkWrite(sink_, &e.struct_id, sizeof(e.struct_id))) return false;
  if (!SinkWrite(sink_, &e.reserved, sizeof(e.reserved))) return false;
  if (!SinkWrite(sink_, &e.mono_ns, sizeof(e.mono_ns))) return false;
  if (!SinkWrite(sink_, &e.source_sequence, sizeof(e.source_sequence)))
    return false;
  if (!SinkWrite(sink_, &e.payload_size, sizeof(e.payload_size))) return false;
  if (payload_size > 0) {
    if (!SinkWrite(sink_, payload, payload_size)) return false;
  }
  if (!SinkWrite(sink_, &e.crc32, sizeof(e.crc32))) return false;
  return RecordWritten(start_offset, mono_ns, record_sequence);
}

bool ThymeBinEncoder::WriteTrigger(uint64_t trigger_id, uint64_t mono_ns,
                                   uint64_t pre_roll_ns, uint64_t post_roll_ns) {
  if (!ok() || finished_) return false;
  const int64_t start_offset = seekable_ ? sink_->Tell() : -1;
  const uint8_t type_byte = static_cast<uint8_t>(ThymeBinEntryType::kTrigger);
  if (!SinkWrite(sink_, &type_byte, 1)) return false;
  uint8_t pad[3] = {};
  if (!SinkWrite(sink_, pad, 3)) return false;
  if (!SinkWrite(sink_, &trigger_id, sizeof(trigger_id))) return false;
  if (!SinkWrite(sink_, &mono_ns, sizeof(mono_ns))) return false;
  if (!SinkWrite(sink_, &pre_roll_ns, sizeof(pre_roll_ns))) return false;
  if (!SinkWrite(sink_, &post_roll_ns, sizeof(post_roll_ns))) return false;
  uint32_t crc = 0xFFFFFFFFu;
  crc = Crc32Update(crc, &type_byte, 1);
  crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&trigger_id),
                    sizeof(trigger_id));
  crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&mono_ns),
                    sizeof(mono_ns));
  crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&pre_roll_ns),
                    sizeof(pre_roll_ns));
  crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&post_roll_ns),
                    sizeof(post_roll_ns));
  crc ^= 0xFFFFFFFFu;
  if (!SinkWrite(sink_, &crc, sizeof(crc))) return false;
  return RecordWritten(start_offset, mono_ns, trigger_id);
}

bool ThymeBinEncoder::WriteGap(uint16_t struct_id, uint64_t mono_ns,
                               uint16_t reason, uint32_t missed_count) {
  if (!ok() || finished_) return false;
  const int64_t start_offset = seekable_ ? sink_->Tell() : -1;
  const uint8_t type_byte = static_cast<uint8_t>(ThymeBinEntryType::kGap);
  if (!SinkWrite(sink_, &type_byte, 1)) return false;
  uint8_t pad[3] = {};
  if (!SinkWrite(sink_, pad, 3)) return false;
  if (!SinkWrite(sink_, &struct_id, sizeof(struct_id))) return false;
  if (!SinkWrite(sink_, &reason, sizeof(reason))) return false;
  if (!SinkWrite(sink_, &mono_ns, sizeof(mono_ns))) return false;
  if (!SinkWrite(sink_, &missed_count, sizeof(missed_count))) return false;
  uint32_t crc = 0xFFFFFFFFu;
  crc = Crc32Update(crc, &type_byte, 1);
  crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&struct_id),
                    sizeof(struct_id));
  crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&reason),
                    sizeof(reason));
  crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&mono_ns),
                    sizeof(mono_ns));
  crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&missed_count),
                    sizeof(missed_count));
  crc ^= 0xFFFFFFFFu;
  if (!SinkWrite(sink_, &crc, sizeof(crc))) return false;
  return RecordWritten(start_offset, mono_ns, 0);
}

bool ThymeBinEncoder::WriteSession(uint64_t new_session_id, uint16_t reason) {
  if (!ok() || finished_) return false;
  const int64_t start_offset = seekable_ ? sink_->Tell() : -1;
  const uint8_t type_byte = static_cast<uint8_t>(ThymeBinEntryType::kSession);
  if (!SinkWrite(sink_, &type_byte, 1)) return false;
  uint8_t pad[3] = {};
  if (!SinkWrite(sink_, pad, 3)) return false;
  if (!SinkWrite(sink_, &new_session_id, sizeof(new_session_id))) return false;
  if (!SinkWrite(sink_, &reason, sizeof(reason))) return false;
  const uint16_t reserved = 0;
  if (!SinkWrite(sink_, &reserved, sizeof(reserved))) return false;
  const uint32_t crc = Crc32Session(new_session_id, reason);
  if (!SinkWrite(sink_, &crc, sizeof(crc))) return false;
  return RecordWritten(start_offset, last_mono_ns_, new_session_id);
}

bool ThymeBinEncoder::Finish() {
  if (finished_) return true;
  finished_ = true;
  if (sink_ == nullptr) return false;
  // Stream sinks cannot carry a trailing index; that is not an error.
  if (!seekable_ || !sink_->Seekable()) return true;
  if (index_entries_.size() > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  const int64_t pos = sink_->Tell();
  if (pos < 0) return true;
  const uint64_t entries_offset = static_cast<uint64_t>(pos);
  for (const ThymeBinIndexEntry& e : index_entries_) {
    if (!SinkWrite(sink_, &e, sizeof(e))) return false;
  }
  ThymeBinIndexFooter f{};
  std::memcpy(f.magic, kThymeBinIndexMagic, sizeof(f.magic));
  f.index_version = kThymeBinIndexVersion;
  f.block_size = kThymeBinIndexBlockSize;
  f.block_count = static_cast<uint32_t>(index_entries_.size());
  f.entries_offset = entries_offset;
  f.data_offset = data_offset_;
  f.total_records = records_written_;
  f.first_mono_ns = first_mono_ns_;
  f.last_mono_ns = last_mono_ns_;
  f.crc32 = IndexFooterCrc(f);
  return SinkWrite(sink_, &f, sizeof(f));
}

ThymeBinWriter::ThymeBinWriter(const char* path)
    : file_(path), encoder_(&file_) {}

ThymeBinWriter::~ThymeBinWriter() { encoder_.Finish(); }

ThymeBinReader::ThymeBinReader(const char* path)
    : fp_(std::fopen(path, "rb")) {
  if (!fp_) return;
  if (!ReadExact(fp_.get(), &header_, sizeof(header_))) {
    fp_.reset();
    return;
  }
  // Version 0 is undefined; versions 1..3 are the only ones a writer ever
  // emitted. Reject anything else instead of silently reading it with a
  // today's layout. The replay package reader applies the same rule.
  if (std::memcmp(header_.magic, kThymeBinMagic, 4) != 0 ||
      header_.file_abi_version == 0 ||
      header_.file_abi_version > kThymeBinFileAbiVersion ||
      header_.struct_count > kThymeBinMaxStructCount) {
    fp_.reset();
    return;
  }
  registry_.resize(header_.struct_count);
  const bool keyed_registry =
      header_.file_abi_version >= kThymeBinFileAbiVersionKeyedRegistry;
  for (auto& d : registry_) {
    if (keyed_registry) {
      if (!ReadExact(fp_.get(), &d, sizeof(d))) {
        fp_.reset();
        registry_.clear();
        return;
      }
    } else {
      ThymeBinLegacyStructDesc legacy{};
      if (!ReadExact(fp_.get(), &legacy, sizeof(legacy))) {
        fp_.reset();
        registry_.clear();
        return;
      }
      d = FromThymeBinLegacyDesc(legacy);
    }
    // contract_version 0 means "unset" (ThymeStructDesc defaults to 1). No
    // writer emits it, so a descriptor carrying it cannot be matched to a
    // contract: reject the header instead of handing the consumer a registry
    // entry it would have to guess about.
    if (d.contract_version == 0) {
      fp_.reset();
      registry_.clear();
      return;
    }
  }
  if (header_.file_abi_version >= kThymeBinFileAbiVersionManifest) {
    // ABI-10: version 3+ always carries the manifest block. A file that
    // claims v3+ without a valid block is corrupt, not "legacy": refuse it
    // so no record is decoded under an unverified id assignment.
    ThymeBinManifestBlock block{};
    if (!ReadExact(fp_.get(), &block, sizeof(block))) {
      manifest_state_ = ManifestState::kInvalid;
      manifest_error_ = "truncated record-manifest block";
      fp_.reset();
      registry_.clear();
      return;
    }
    if (std::memcmp(block.magic, kThymeBinManifestMagic,
                    sizeof(block.magic)) != 0) {
      manifest_state_ = ManifestState::kInvalid;
      manifest_error_ = "bad record-manifest magic";
      fp_.reset();
      registry_.clear();
      return;
    }
    if (block.manifest_version == 0 || block.reserved0 != 0 ||
        block.reserved1 != 0 || block.reserved2 != 0) {
      manifest_state_ = ManifestState::kInvalid;
      manifest_error_ = "unset manifest version or non-zero reserved field";
      fp_.reset();
      registry_.clear();
      return;
    }
    if (block.crc32 != ManifestBlockCrc(block)) {
      manifest_state_ = ManifestState::kInvalid;
      manifest_error_ = "record-manifest CRC mismatch";
      fp_.reset();
      registry_.clear();
      return;
    }
    manifest_state_ = ManifestState::kOk;
    manifest_hash_ = block.manifest_hash;
    manifest_version_ = block.manifest_version;
  } else {
    manifest_state_ = ManifestState::kAbsent;  // legacy (v1/v2) file
  }
  LoadIndex();
}

ThymeBinReader::~ThymeBinReader() = default;

void ThymeBinReader::LoadIndex() {
  index_status_ = IndexStatus::kAbsent;
  index_error_ = nullptr;
  index_entries_.clear();
  index_block_size_ = 0;
  index_first_mono_ns_ = 0;
  index_last_mono_ns_ = 0;
  indexed_record_count_ = 0;
  data_offset_ = 0;
  footer_start_ = 0;
  if (!fp_) return;

  auto io_error = [this](const char* why) {
    index_status_ = IndexStatus::kIoError;
    index_error_ = why;
    index_entries_.clear();
  };

  const long data_off_l = std::ftell(fp_.get());
  if (data_off_l < 0) {
    io_error("index data position query failed");
    return;
  }
  data_offset_ = static_cast<uint64_t>(data_off_l);
  if (std::fseek(fp_.get(), 0, SEEK_END) != 0) {
    io_error("index end seek failed");
    (void)std::fseek(fp_.get(), data_off_l, SEEK_SET);
    return;
  }
  const long file_size_l = std::ftell(fp_.get());
  if (file_size_l < data_off_l) {
    io_error("index file size query failed");
    (void)std::fseek(fp_.get(), data_off_l, SEEK_SET);
    return;
  }
  const uint64_t file_size = static_cast<uint64_t>(file_size_l);
  if (file_size - data_offset_ < sizeof(ThymeBinIndexFooter)) {
    (void)std::fseek(fp_.get(), data_off_l, SEEK_SET);
    return;  // old/truncated file: no footer
  }
  const uint64_t footer_off = file_size - sizeof(ThymeBinIndexFooter);
  if (std::fseek(fp_.get(), static_cast<long>(footer_off), SEEK_SET) != 0) {
    io_error("index footer seek failed");
    (void)std::fseek(fp_.get(), data_off_l, SEEK_SET);
    return;
  }
  ThymeBinIndexFooter f{};
  const bool read_ok = std::fread(&f, 1, sizeof(f), fp_.get()) == sizeof(f);
  (void)std::fseek(fp_.get(), data_off_l, SEEK_SET);
  if (!read_ok) {
    io_error("index footer read failed");
    return;
  }
  if (std::memcmp(f.magic, kThymeBinIndexMagic, sizeof(f.magic)) != 0) {
    return;  // no footer at the tail: absent, not corrupt
  }

  auto corrupt = [this](const char* why) {
    index_status_ = IndexStatus::kCorrupt;
    index_error_ = why;
    index_entries_.clear();
  };

  if (f.index_version != kThymeBinIndexVersion) {
    corrupt("unsupported index version");
    return;
  }
  if (f.block_size != kThymeBinIndexBlockSize) {
    corrupt("unsupported index block size");
    return;
  }
  if (IndexFooterCrc(f) != f.crc32) {
    corrupt("index footer crc mismatch");
    return;
  }
  if (f.data_offset != data_offset_) {
    corrupt("index data offset mismatch");
    return;
  }
  if (f.entries_offset < data_offset_ || f.entries_offset > footer_off) {
    corrupt("index entries offset out of range");
    return;
  }
  const uint64_t entry_bytes =
      static_cast<uint64_t>(f.block_count) * sizeof(ThymeBinIndexEntry);
  if (footer_off - f.entries_offset != entry_bytes) {
    corrupt("index extent mismatch");
    return;
  }
  // From here the footer extent is trustworthy: sequential reads stop here
  // even if an individual entry turns out to be corrupt.
  footer_start_ = f.entries_offset;

  std::vector<ThymeBinIndexEntry> entries(f.block_count);
  if (!entries.empty()) {
    const std::size_t bytes = entries.size() * sizeof(ThymeBinIndexEntry);
    if (std::fseek(fp_.get(), static_cast<long>(f.entries_offset), SEEK_SET) != 0) {
      io_error("index entries seek failed");
      (void)std::fseek(fp_.get(), data_off_l, SEEK_SET);
      return;
    }
    if (std::fread(entries.data(), 1, bytes, fp_.get()) != bytes) {
      (void)std::fseek(fp_.get(), data_off_l, SEEK_SET);
      corrupt("short index read");
      return;
    }
    (void)std::fseek(fp_.get(), data_off_l, SEEK_SET);
  }

  uint64_t prev_offset = 0;
  uint64_t prev_mono = 0;
  uint32_t prev_index = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const ThymeBinIndexEntry& e = entries[i];
    if (IndexEntryCrc(e) != e.crc32) {
      corrupt("index entry crc mismatch");
      return;
    }
    if (e.offset < data_offset_ || e.offset >= f.entries_offset) {
      corrupt("index entry offset out of range");
      return;
    }
    if (e.record_index % f.block_size != 0 ||
        e.record_index >= f.total_records) {
      corrupt("index entry record ordinal mismatch");
      return;
    }
    if (i > 0 && (e.offset <= prev_offset || e.record_index <= prev_index ||
                  e.mono_ns < prev_mono)) {
      corrupt("index entries not monotonic");
      return;
    }
    prev_offset = e.offset;
    prev_mono = e.mono_ns;
    prev_index = e.record_index;
  }

  index_entries_ = std::move(entries);
  index_block_size_ = f.block_size;
  index_first_mono_ns_ = f.first_mono_ns;
  index_last_mono_ns_ = f.last_mono_ns;
  indexed_record_count_ = f.total_records;
  index_status_ = IndexStatus::kOk;
}

bool ThymeBinReader::SeekToMonoNs(uint64_t mono_ns) {
  if (!fp_ || index_status_ != IndexStatus::kOk) return false;
  uint64_t target_offset = data_offset_;
  if (!index_entries_.empty()) {
    const auto it = std::upper_bound(
        index_entries_.begin(), index_entries_.end(), mono_ns,
        [](uint64_t t, const ThymeBinIndexEntry& e) { return t < e.mono_ns; });
    if (it != index_entries_.begin()) target_offset = (it - 1)->offset;
  }
  if (target_offset >
      static_cast<uint64_t>(std::numeric_limits<long>::max())) {
    return false;
  }
  return std::fseek(fp_.get(), static_cast<long>(target_offset), SEEK_SET) == 0;
}

ThymeBinReader::ReadResult ThymeBinReader::ReadNext(ThymeBinDataEntry& entry) {
  using R = ReadResult;
  if (!fp_) return R::kEof;
  const long rec_off = std::ftell(fp_.get());
  if (footer_start_ != 0 && rec_off >= 0 &&
      static_cast<uint64_t>(rec_off) >= footer_start_) {
    return R::kEof;  // reached the trailing index block
  }
  auto Corrupt = [&](uint32_t expected, uint32_t actual) {
    corrupt_offset_ = static_cast<uint64_t>(rec_off < 0 ? 0 : rec_off);
    corrupt_crc_expected_ = expected;
    corrupt_crc_actual_ = actual;
    return R::kCorrupt;
  };
  uint8_t type_byte = 0;
  if (!ReadExact(fp_.get(), &type_byte, 1)) {
    return std::feof(fp_.get()) ? R::kEof : Corrupt(0, 0);  // clean EOF
  }
  entry = ThymeBinDataEntry{};
  entry.type = static_cast<ThymeBinEntryType>(type_byte);
  uint8_t pad[3];
  if (!ReadExact(fp_.get(), pad, 3)) return Corrupt(0, 0);

  if (entry.type == ThymeBinEntryType::kTrigger) {
    uint64_t trigger_id = 0;
    if (!ReadExact(fp_.get(), &trigger_id, sizeof(trigger_id))) {
      return Corrupt(0, 0);
    }
    if (!ReadExact(fp_.get(), &entry.mono_ns, sizeof(entry.mono_ns))) {
      return Corrupt(0, 0);
    }
    if (!ReadExact(fp_.get(), &entry.pre_roll_ns, sizeof(entry.pre_roll_ns))) {
      return Corrupt(0, 0);
    }
    if (!ReadExact(fp_.get(), &entry.post_roll_ns,
                   sizeof(entry.post_roll_ns))) {
      return Corrupt(0, 0);
    }
    entry.record_sequence = trigger_id;
    if (!ReadExact(fp_.get(), &entry.crc32, sizeof(entry.crc32))) {
      return Corrupt(0, 0);
    }
    const uint32_t want =
        Crc32Trigger(trigger_id, entry.mono_ns, entry.pre_roll_ns,
                     entry.post_roll_ns);
    return want == entry.crc32 ? R::kOk : Corrupt(want, entry.crc32);
  }

  if (entry.type == ThymeBinEntryType::kGap) {
    uint16_t reason = 0;
    uint32_t missed_count = 0;
    if (!ReadExact(fp_.get(), &entry.struct_id, sizeof(entry.struct_id))) {
      return Corrupt(0, 0);
    }
    if (!ReadExact(fp_.get(), &reason, sizeof(reason))) return Corrupt(0, 0);
    if (!ReadExact(fp_.get(), &entry.mono_ns, sizeof(entry.mono_ns))) {
      return Corrupt(0, 0);
    }
    if (!ReadExact(fp_.get(), &missed_count, sizeof(missed_count))) {
      return Corrupt(0, 0);
    }
    entry.source_sequence = missed_count;
    entry.reserved = reason;
    if (!ReadExact(fp_.get(), &entry.crc32, sizeof(entry.crc32))) {
      return Corrupt(0, 0);
    }
    const uint32_t want =
        Crc32Gap(entry.struct_id, reason, entry.mono_ns, missed_count);
    return want == entry.crc32 ? R::kOk : Corrupt(want, entry.crc32);
  }

  if (entry.type == ThymeBinEntryType::kSession) {
    uint64_t new_session_id = 0;
    uint16_t reason = 0;
    uint16_t reserved = 0;
    if (!ReadExact(fp_.get(), &new_session_id, sizeof(new_session_id))) {
      return Corrupt(0, 0);
    }
    if (!ReadExact(fp_.get(), &reason, sizeof(reason))) return Corrupt(0, 0);
    if (!ReadExact(fp_.get(), &reserved, sizeof(reserved))) {
      return Corrupt(0, 0);
    }
    entry.record_sequence = new_session_id;
    entry.reserved = reason;
    if (!ReadExact(fp_.get(), &entry.crc32, sizeof(entry.crc32))) {
      return Corrupt(0, 0);
    }
    const uint32_t want = Crc32Session(new_session_id, reason);
    return want == entry.crc32 ? R::kOk : Corrupt(want, entry.crc32);
  }

  if (entry.type != ThymeBinEntryType::kData) {
    // Unknown entry type at the current file ABI — treat as corrupt (do not
    // mis-parse as DATA). Future ABI versions may add skippable entries.
    return Corrupt(0, 0);
  }

  if (!ReadExact(fp_.get(), &entry.record_sequence,
                 sizeof(entry.record_sequence))) {
    return Corrupt(0, 0);
  }
  if (!ReadExact(fp_.get(), &entry.struct_id, sizeof(entry.struct_id))) {
    return Corrupt(0, 0);
  }
  if (!ReadExact(fp_.get(), &entry.reserved, sizeof(entry.reserved))) {
    return Corrupt(0, 0);
  }
  if (!ReadExact(fp_.get(), &entry.mono_ns, sizeof(entry.mono_ns))) {
    return Corrupt(0, 0);
  }
  if (!ReadExact(fp_.get(), &entry.source_sequence,
                 sizeof(entry.source_sequence))) {
    return Corrupt(0, 0);
  }
  if (!ReadExact(fp_.get(), &entry.payload_size, sizeof(entry.payload_size))) {
    return Corrupt(0, 0);
  }
  if (entry.payload_size > kThymeBinMaxPayloadSize) return Corrupt(0, 0);
  if (entry.payload_size > 0) {
    entry.payload.resize(entry.payload_size);
    if (!ReadExact(fp_.get(), entry.payload.data(), entry.payload_size)) {
      return Corrupt(0, 0);
    }
  }
  if (!ReadExact(fp_.get(), &entry.crc32, sizeof(entry.crc32))) {
    return Corrupt(0, 0);
  }
  const uint32_t want = Crc32Entry(entry);
  return want == entry.crc32 ? R::kOk : Corrupt(want, entry.crc32);
}

}  // namespace thyme
