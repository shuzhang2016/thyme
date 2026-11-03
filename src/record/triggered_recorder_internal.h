#pragma once

#include "thyme/record/triggered_recorder.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace thyme {
namespace triggered {

struct TriggeredRecorder::SourceState {
  SourceConfig cfg;
  uint64_t shm_ino = 0;
  uint64_t shm_size = 0;
  bool attached = false;
  bool ever_attached = false;
  uint64_t pinned_hash = 0;  // mirror only
  uint32_t struct_count = 0;  // mirror only
  record_mirror::RingClient mirror;
  PinnedSlotReader frames;
  // Post-window continuation cursors (valid during kPost): per-struct next
  // publish_index for mirrors, next frame_index for pinned_slot ([0]).
  std::vector<uint64_t> post_next;
  // h264 trigger post-window: set once the next IDR after t0+after is seen,
  // or the frame stream passes t0+after+gop_ns. Unused when gop_ns is 0.
  bool gop_post_done = false;
  // Continuous-mode cursors (valid while cont_armed_): same shape.
  std::vector<uint64_t> cont_next;
  // mono_ns of the last sample collected per struct. Stamps overrun-hole
  // descriptors (a hole's own timestamp is gone); 0 before the first sample.
  // Keeps hole rows inside the slice's mono order instead of pinning them to
  // the file start (a mono_ns-0 hole would regress the file — REP-07).
  std::vector<uint64_t> cont_last_mono;
  // Streaming big-frame drain: drop_count at the previous settlement (the
  // lag gap-skip difference is booked).
  uint64_t cont_bf_drops = 0;
};

namespace detail {

inline ThymeStructDesc MakeDesc(uint16_t sid, uint32_t payload,
                                const char* name) {
  ThymeStructDesc d{};
  d.struct_id = sid;
  d.contract_version = 1;
  d.payload_size = payload;
  std::snprintf(d.name, sizeof(d.name), "%s", name);
  return d;
}

// REC-26: file-level header for `frames/<channel>.raw`. A `.tbin` already
// carries a header, so `.raw` is the only data file that cannot identify
// itself; the header makes a single `.raw` copy explain which channel it
// belongs to. Offsets in the index are absolute, so a reader that ignores the
// header still lands on the right bytes — this is a pure additive change and
// old packages (no header, first offset 0) stay readable.
inline constexpr char kRawFileMagic[8] = {'T', 'H', 'Y', 'R', 'A', 'W', '1', '\0'};
inline constexpr uint32_t kRawFileVersionV1 = 1;
inline constexpr uint32_t kRawFileVersion = 2;
inline constexpr uint32_t kRawFlagHasProfile = 1u;

#pragma pack(push, 1)
struct RawFileHeader {
  char magic[8];
  uint32_t version;
  uint32_t header_bytes;
  uint32_t channel_key_bytes;
  uint32_t flags;
  char channel_key[32];
  uint32_t profile_version;
  uint32_t profile_bytes;
  uint64_t params_digest;
  char profile[56];
};
#pragma pack(pop)
static_assert(sizeof(RawFileHeader) == 128, "raw file header is 128 bytes");
static_assert(offsetof(RawFileHeader, channel_key) == 24,
              "v1/v2 share the channel key prefix");

inline RawFileHeader MakeRawFileHeader(const std::string& channel,
                                       const std::string& profile = std::string(),
                                       uint32_t profile_version = 0,
                                       uint64_t params_digest = 0) {
  RawFileHeader h{};
  for (std::size_t i = 0; i < sizeof(kRawFileMagic); ++i) h.magic[i] = kRawFileMagic[i];
  h.version = kRawFileVersion;
  h.header_bytes = sizeof(RawFileHeader);
  h.channel_key_bytes =
      static_cast<uint32_t>(channel.size() < sizeof(h.channel_key)
                                ? channel.size()
                                : sizeof(h.channel_key));
  std::memcpy(h.channel_key, channel.data(), h.channel_key_bytes);
  if (!profile.empty()) {
    h.flags = kRawFlagHasProfile;
    h.profile_version = profile_version;
    h.params_digest = params_digest;
    h.profile_bytes =
        static_cast<uint32_t>(profile.size() < sizeof(h.profile)
                                  ? profile.size()
                                  : sizeof(h.profile));
    std::memcpy(h.profile, profile.data(), h.profile_bytes);
  }
  return h;
}

inline RawFileHeader MakeRawFileHeader(const SourceConfig& cfg) {
  const std::string& channel =
      cfg.channel_name.empty() ? std::string("channel") : cfg.channel_name;
  return MakeRawFileHeader(channel, cfg.payload_profile,
                           cfg.payload_profile_version,
                           cfg.payload_profile_digest);
}

inline PayloadProfileRecord MakePayloadProfileRecord(const SourceConfig& cfg) {
  PayloadProfileRecord r{};
  const std::string& channel =
      cfg.channel_name.empty() ? std::string("channel") : cfg.channel_name;
  const std::size_t nchan =
      channel.size() < sizeof(r.channel) ? channel.size() : sizeof(r.channel);
  std::memcpy(r.channel, channel.data(), nchan);
  if (!cfg.payload_profile.empty()) {
    r.flags = kPayloadProfileFlagHasProfile;
    r.profile_version = cfg.payload_profile_version;
    r.params_digest = cfg.payload_profile_digest;
    const std::size_t nprof =
        cfg.payload_profile.size() < sizeof(r.profile)
            ? cfg.payload_profile.size()
            : sizeof(r.profile);
    std::memcpy(r.profile, cfg.payload_profile.data(), nprof);
  }
  return r;
}

// fsync a stdio stream (package durability: nothing counts as sealed until
// every package file is on stable storage).
inline bool SyncFile(FILE* f) {
  if (std::fflush(f) != 0) return false;
  return fsync(fileno(f)) == 0;
}

inline std::size_t WritePackageBytes(
    FILE* file, const void* data, std::size_t n,
    integrity::PackageHashState* hashes, const std::string& relative_path) {
  if (hashes != nullptr) {
    const long offset = std::ftell(file);
    if (offset < 0 ||
        static_cast<uint64_t>(offset) != hashes->Size(relative_path)) {
      return 0;  // REC-24: seek+overwrite must reset via Replace, not Append.
    }
  }
  const std::size_t written = std::fwrite(data, 1, n, file);
  if (written != 0) hashes->Append(relative_path, data, written);
  return written;
}

// fsync a directory (W3): after sealing a package the directory entry must
// be durable too; after an invalidate-rename the PARENT directory's entry
// change needs the fsync. Best-effort: the wal abort/seal rows remain the
// source of truth, but a failure is counted (Stats::sync_errors) instead of
// disappearing.
inline bool SyncDir(const std::string& dir) {
  const int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return false;
  const bool ok = fsync(fd) == 0;
  close(fd);
  return ok;
}

inline bool SyncParentDir(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  return SyncDir(slash == std::string::npos ? "." : path.substr(0, slash));
}

// SEC-03: evidence timestamps are wall clock (auditors correlate with logs);
// the package's data timeline stays CLOCK_MONOTONIC.
inline uint64_t RealTimeNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

}  // namespace detail

}  // namespace triggered
}  // namespace thyme
