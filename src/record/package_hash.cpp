#include "thyme/record/package_hash.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include "sha256.h"
#include "thyme/contracts/frame_header.h"

namespace thyme {
namespace integrity {

struct PackageHashState::Impl {
  struct File {
    sha256_detail::Ctx hash{};
    uint64_t size = 0;
    bool hash_enabled = true;

    File() { sha256_detail::Init(&hash); }
  };

  std::map<std::string, File> files;
};

namespace {

constexpr uint32_t kCheckpointMagic = 0x50434850u;  // "PHCP" on disk.
constexpr uint16_t kCheckpointVersion = 1;
constexpr std::size_t kCheckpointHeaderBytes = 30;
constexpr std::size_t kCheckpointCrcOffset = 26;
constexpr std::size_t kCheckpointContextBytes = 108;
constexpr std::size_t kCheckpointEntryFixedBytes = 121;
constexpr std::size_t kMaxCheckpointBytes =
    std::size_t{64} * 1024 * 1024;

static_assert(sizeof(sha256_detail::Ctx) == 112,
              "unsupported SHA-256 context layout");
static_assert(offsetof(sha256_detail::Ctx, h) == 0,
              "unsupported SHA-256 context layout");
static_assert(offsetof(sha256_detail::Ctx, total) == 32,
              "unsupported SHA-256 context layout");
static_assert(offsetof(sha256_detail::Ctx, buf) == 40,
              "unsupported SHA-256 context layout");
static_assert(offsetof(sha256_detail::Ctx, buflen) == 104,
              "unsupported SHA-256 context layout");

void AppendUint16(std::vector<uint8_t>* out, uint16_t value) {
  for (uint32_t i = 0; i < 2; ++i) {
    out->push_back(static_cast<uint8_t>(value >> (8u * i)));
  }
}

void AppendUint32(std::vector<uint8_t>* out, uint32_t value) {
  for (uint32_t i = 0; i < 4; ++i) {
    out->push_back(static_cast<uint8_t>(value >> (8u * i)));
  }
}

void AppendUint64(std::vector<uint8_t>* out, uint64_t value) {
  for (uint32_t i = 0; i < 8; ++i) {
    out->push_back(static_cast<uint8_t>(value >> (8u * i)));
  }
}

void AppendContext(std::vector<uint8_t>* out,
                   const sha256_detail::Ctx& context) {
  for (const uint32_t word : context.h) {
    AppendUint32(out, word);
  }
  AppendUint64(out, context.total);
  out->insert(out->end(), context.buf, context.buf + sizeof(context.buf));
  AppendUint32(out, context.buflen);
}

class CheckpointReader {
 public:
  CheckpointReader(const std::vector<uint8_t>& bytes, std::size_t offset)
      : bytes_(bytes), offset_(offset) {}

  bool ReadUint8(uint8_t* value) {
    if (!CanRead(1)) {
      return false;
    }
    *value = bytes_[offset_++];
    return true;
  }

  bool ReadUint16(uint16_t* value) {
    if (!CanRead(2)) {
      return false;
    }
    *value = 0;
    for (uint32_t i = 0; i < 2; ++i) {
      *value = static_cast<uint16_t>(
          *value | (static_cast<uint16_t>(bytes_[offset_++]) << (8u * i)));
    }
    return true;
  }

  bool ReadUint32(uint32_t* value) {
    if (!CanRead(4)) {
      return false;
    }
    *value = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      *value |= static_cast<uint32_t>(bytes_[offset_++]) << (8u * i);
    }
    return true;
  }

  bool ReadUint64(uint64_t* value) {
    if (!CanRead(8)) {
      return false;
    }
    *value = 0;
    for (uint32_t i = 0; i < 8; ++i) {
      *value |= static_cast<uint64_t>(bytes_[offset_++]) << (8u * i);
    }
    return true;
  }

  bool ReadBytes(std::size_t size, const uint8_t** value) {
    if (!CanRead(size)) {
      return false;
    }
    *value = bytes_.data() + offset_;
    offset_ += size;
    return true;
  }

  bool ReadContext(sha256_detail::Ctx* context) {
    for (uint32_t& word : context->h) {
      if (!ReadUint32(&word)) {
        return false;
      }
    }
    if (!ReadUint64(&context->total)) {
      return false;
    }
    const uint8_t* buffer = nullptr;
    if (!ReadBytes(sizeof(context->buf), &buffer)) {
      return false;
    }
    std::memcpy(context->buf, buffer, sizeof(context->buf));
    return ReadUint32(&context->buflen);
  }

  std::size_t offset() const { return offset_; }
  std::size_t remaining() const { return bytes_.size() - offset_; }

 private:
  bool CanRead(std::size_t size) const {
    return size <= bytes_.size() - offset_;
  }

  const std::vector<uint8_t>& bytes_;
  std::size_t offset_;
};

bool ValidRelativePath(const std::string& path) {
  if (path.empty() || path == PackageHashState::kCheckpointName ||
      path.front() == '/' || path.find('\0') != std::string::npos) {
    return false;
  }
  std::size_t component_start = 0;
  while (component_start <= path.size()) {
    const std::size_t slash = path.find('/', component_start);
    const std::size_t component_end =
        slash == std::string::npos ? path.size() : slash;
    if (component_end - component_start == 2 &&
        path.compare(component_start, 2, "..") == 0) {
      return false;
    }
    if (slash == std::string::npos) {
      break;
    }
    component_start = slash + 1;
  }
  return true;
}

std::string PackagePath(const std::string& package_dir,
                        const std::string& relative_path) {
  if (package_dir.empty() || package_dir.back() == '/') {
    return package_dir + relative_path;
  }
  return package_dir + "/" + relative_path;
}

bool WriteAll(int fd, const uint8_t* data, std::size_t size) {
  std::size_t written = 0;
  while (written < size) {
    const ssize_t result = ::write(fd, data + written, size - written);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (result == 0) {
      return false;
    }
    written += static_cast<std::size_t>(result);
  }
  return true;
}

bool ReadAll(int fd, uint8_t* data, std::size_t size) {
  std::size_t read_bytes = 0;
  while (read_bytes < size) {
    const ssize_t result = ::read(fd, data + read_bytes, size - read_bytes);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (result == 0) {
      return false;
    }
    read_bytes += static_cast<std::size_t>(result);
  }
  uint8_t extra = 0;
  ssize_t result = 0;
  do {
    result = ::read(fd, &extra, 1);
  } while (result < 0 && errno == EINTR);
  return result == 0;
}

bool ReadCheckpoint(const std::string& path, std::vector<uint8_t>* bytes) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  struct stat status {};
  bool ok = ::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
            status.st_size >= 0 &&
            static_cast<uint64_t>(status.st_size) <= kMaxCheckpointBytes;
  if (ok) {
    bytes->resize(static_cast<std::size_t>(status.st_size));
    ok = ReadAll(fd, bytes->data(), bytes->size());
  }
  if (::close(fd) != 0) {
    ok = false;
  }
  return ok;
}

}  // namespace

PackageHashState::PackageHashState() : impl_(std::make_unique<Impl>()) {}

PackageHashState::~PackageHashState() = default;

PackageHashState::PackageHashState(PackageHashState&&) noexcept = default;

PackageHashState& PackageHashState::operator=(PackageHashState&&) noexcept =
    default;

void PackageHashState::Reset() { impl_->files.clear(); }

void PackageHashState::Track(const std::string& relative_path,
                             bool hash_enabled) {
  Impl::File file;
  file.hash_enabled = hash_enabled;
  impl_->files.erase(relative_path);
  impl_->files.emplace(relative_path, std::move(file));
}

uint64_t PackageHashState::Size(const std::string& relative_path) const {
  const auto it = impl_->files.find(relative_path);
  return it == impl_->files.end() ? 0 : it->second.size;
}

void PackageHashState::Append(const std::string& relative_path,
                              const void* data, std::size_t n) {
  Impl::File& file = impl_->files[relative_path];
  if (n != 0) {
    if (file.hash_enabled) {
      sha256_detail::Update(&file.hash, static_cast<const uint8_t*>(data), n);
    }
    file.size += n;
  }
}

void PackageHashState::Replace(const std::string& relative_path,
                               const void* data, std::size_t n) {
  const auto existing = impl_->files.find(relative_path);
  const bool hash_enabled =
      existing == impl_->files.end() || existing->second.hash_enabled;
  impl_->files.erase(relative_path);
  Track(relative_path, hash_enabled);
  Append(relative_path, data, n);
}

bool PackageHashState::RehashFile(const std::string& package_dir,
                                  const std::string& relative_path) {
  const auto existing = impl_->files.find(relative_path);
  if (existing == impl_->files.end() || !ValidRelativePath(relative_path)) {
    return false;
  }
  const int fd = ::open(PackagePath(package_dir, relative_path).c_str(),
                        O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat status {};
  bool ok = ::fstat(fd, &status) == 0 && S_ISREG(status.st_mode);
  Impl::File refreshed;
  refreshed.hash_enabled = existing->second.hash_enabled;
  sha256_detail::Init(&refreshed.hash);
  uint8_t buffer[16384];
  while (ok) {
    ssize_t n = 0;
    do {
      n = ::read(fd, buffer, sizeof(buffer));
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
      ok = false;
    } else if (n == 0) {
      break;
    } else {
      if (refreshed.hash_enabled) {
        sha256_detail::Update(&refreshed.hash, buffer,
                              static_cast<std::size_t>(n));
      }
      refreshed.size += static_cast<uint64_t>(n);
    }
  }
  if (::close(fd) != 0) ok = false;
  if (!ok) return false;
  existing->second = std::move(refreshed);
  return true;
}

std::vector<PackageFileDigest> PackageHashState::DigestFiles() const {
  return DigestFiles({}, nullptr, 0);
}

std::vector<PackageFileDigest> PackageHashState::DigestFiles(
    const std::string& uncommitted_path, const void* data,
    std::size_t n) const {
  std::vector<PackageFileDigest> out;
  out.reserve(impl_->files.size());
  for (const auto& item : impl_->files) {
    PackageFileDigest digest;
    digest.path = item.first;
    digest.size = item.second.size;
    sha256_detail::Ctx copy = item.second.hash;
    if (n != 0 && item.first == uncommitted_path) {
      if (item.second.hash_enabled) {
        sha256_detail::Update(&copy, static_cast<const uint8_t*>(data), n);
      }
      digest.size += n;
    }
    if (item.second.hash_enabled) {
      sha256_detail::Final(&copy, digest.sha256);
    }
    out.push_back(std::move(digest));
  }
  return out;
}

bool PackageHashState::SaveCheckpoint(const std::string& package_dir,
                                      uint64_t session_id) const {
  if (impl_->files.size() > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  uint64_t payload_bytes = 0;
  for (const auto& item : impl_->files) {
    const std::string& path = item.first;
    if (!ValidRelativePath(path) ||
        path.size() > std::numeric_limits<uint32_t>::max()) {
      return false;
    }
    const uint64_t entry_bytes = kCheckpointEntryFixedBytes + path.size();
    if (entry_bytes > kMaxCheckpointBytes - kCheckpointHeaderBytes ||
        payload_bytes >
            kMaxCheckpointBytes - kCheckpointHeaderBytes - entry_bytes) {
      return false;
    }
    payload_bytes += entry_bytes;
  }

  std::vector<uint8_t> bytes;
  bytes.reserve(kCheckpointHeaderBytes +
                static_cast<std::size_t>(payload_bytes));
  AppendUint32(&bytes, kCheckpointMagic);
  AppendUint16(&bytes, kCheckpointVersion);
  AppendUint64(&bytes, session_id);
  AppendUint32(&bytes, static_cast<uint32_t>(impl_->files.size()));
  AppendUint64(&bytes, payload_bytes);
  AppendUint32(&bytes, 0);
  for (const auto& item : impl_->files) {
    AppendUint32(&bytes, static_cast<uint32_t>(item.first.size()));
    bytes.push_back(item.second.hash_enabled ? 1u : 0u);
    AppendUint64(&bytes, item.second.size);
    AppendContext(&bytes, item.second.hash);
    bytes.insert(bytes.end(), item.first.begin(), item.first.end());
  }
  const uint32_t crc =
      Crc32Update(0xFFFFFFFFu, bytes.data(), bytes.size()) ^ 0xFFFFFFFFu;
  for (uint32_t i = 0; i < 4; ++i) {
    bytes[kCheckpointCrcOffset + i] = static_cast<uint8_t>(crc >> (8u * i));
  }

  std::string temporary =
      PackagePath(package_dir, kCheckpointTempPrefix) + "XXXXXX";
  const int fd = ::mkstemp(temporary.data());
  if (fd < 0) {
    return false;
  }
  bool ok = ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
            WriteAll(fd, bytes.data(), bytes.size()) && ::fdatasync(fd) == 0;
  if (::close(fd) != 0) {
    ok = false;
  }
  const std::string checkpoint = PackagePath(package_dir, kCheckpointName);
  if (ok) {
    ok = ::rename(temporary.c_str(), checkpoint.c_str()) == 0;
  }
  if (!ok) {
    ::unlink(temporary.c_str());
  }
  return ok;
}

bool PackageHashState::LoadCheckpoint(const std::string& package_dir,
                                      uint64_t expected_session_id) {
  std::vector<uint8_t> bytes;
  if (!ReadCheckpoint(PackagePath(package_dir, kCheckpointName), &bytes) ||
      bytes.size() < kCheckpointHeaderBytes) {
    return false;
  }

  CheckpointReader reader(bytes, 0);
  uint32_t magic = 0;
  uint16_t version = 0;
  uint64_t session_id = 0;
  uint32_t entry_count = 0;
  uint64_t payload_bytes = 0;
  uint32_t stored_crc = 0;
  if (!reader.ReadUint32(&magic) || !reader.ReadUint16(&version) ||
      !reader.ReadUint64(&session_id) || !reader.ReadUint32(&entry_count) ||
      !reader.ReadUint64(&payload_bytes) || !reader.ReadUint32(&stored_crc) ||
      magic != kCheckpointMagic || version != kCheckpointVersion ||
      session_id != expected_session_id ||
      payload_bytes != reader.remaining() ||
      entry_count > reader.remaining() / (kCheckpointEntryFixedBytes + 1)) {
    return false;
  }
  std::memset(bytes.data() + kCheckpointCrcOffset, 0, sizeof(stored_crc));
  const uint32_t actual_crc =
      Crc32Update(0xFFFFFFFFu, bytes.data(), bytes.size()) ^ 0xFFFFFFFFu;
  if (actual_crc != stored_crc) {
    return false;
  }

  PackageHashState loaded;
  for (uint32_t i = 0; i < entry_count; ++i) {
    uint32_t path_size = 0;
    uint8_t hash_enabled = 0;
    Impl::File file;
    if (!reader.ReadUint32(&path_size) || !reader.ReadUint8(&hash_enabled) ||
        hash_enabled > 1 || !reader.ReadUint64(&file.size) ||
        !reader.ReadContext(&file.hash) || path_size == 0 ||
        path_size > reader.remaining()) {
      return false;
    }
    const uint8_t* path_bytes = nullptr;
    if (!reader.ReadBytes(path_size, &path_bytes)) {
      return false;
    }
    std::string path(reinterpret_cast<const char*>(path_bytes), path_size);
    if (!ValidRelativePath(path) || file.hash.buflen >= sizeof(file.hash.buf) ||
        file.hash.total % sizeof(file.hash.buf) != file.hash.buflen ||
        (hash_enabled == 1 && file.hash.total != file.size)) {
      return false;
    }
    file.hash_enabled = hash_enabled == 1;
    if (!loaded.impl_->files.emplace(std::move(path), std::move(file)).second) {
      return false;
    }
  }
  if (reader.offset() != bytes.size()) {
    return false;
  }

  for (const auto& item : loaded.impl_->files) {
    struct stat status {};
    if (::stat(PackagePath(package_dir, item.first).c_str(), &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_size < 0) {
      return false;
    }
    const uint64_t actual_size = static_cast<uint64_t>(status.st_size);
    // Crash recovery may append lifecycle/abort rows after the last checkpoint.
    // The caller still rehashes wal.log; every other file must match exactly.
    if (item.first == "wal.log" ? actual_size < item.second.size
                                : actual_size != item.second.size) {
      return false;
    }
  }
  impl_ = std::move(loaded.impl_);
  return true;
}

}  // namespace integrity
}  // namespace thyme
