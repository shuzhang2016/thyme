#include "thyme/record/quota_state.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

#include "thyme/contracts/frame_header.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace thyme {
namespace trigger_center {

#if !defined(_WIN32)

namespace {

constexpr uint64_t kQuotaWindowNs = 3600000000000ull;
constexpr std::size_t kMaxQuotaClassSize = 63;

struct QuotaRecord {
  char magic[8];
  uint64_t realtime_ns;
  uint32_t crc32;
  uint16_t version;
  uint8_t class_size;
  uint8_t reserved[1];
  char quota_class[kMaxQuotaClassSize];
};

static_assert(sizeof(QuotaRecord) == 88, "quota record must stay 88 bytes");

uint32_t RecordCrc(const QuotaRecord& record) {
  QuotaRecord copy = record;
  copy.crc32 = 0;
  return Crc32Update(0xFFFFFFFFu, reinterpret_cast<const uint8_t*>(&copy),
                     sizeof(copy)) ^
         0xFFFFFFFFu;
}

bool IsExpired(uint64_t realtime_ns, uint64_t now_realtime_ns) {
  return realtime_ns <= now_realtime_ns &&
         now_realtime_ns - realtime_ns >= kQuotaWindowNs;
}

QuotaRecord MakeRecord(const std::string& quota_class, uint64_t realtime_ns) {
  QuotaRecord record{};
  std::memcpy(record.magic, "THYQUOTA", sizeof(record.magic));
  record.version = 1;
  record.class_size = static_cast<uint8_t>(quota_class.size());
  record.realtime_ns = realtime_ns;
  std::memcpy(record.quota_class, quota_class.data(), quota_class.size());
  record.crc32 = RecordCrc(record);
  return record;
}

bool ValidRecord(const QuotaRecord& record) {
  if (std::memcmp(record.magic, "THYQUOTA", sizeof(record.magic)) != 0 ||
      record.version != 1 || record.class_size == 0 ||
      record.class_size > kMaxQuotaClassSize || record.realtime_ns == 0) {
    return false;
  }
  for (std::size_t i = 0; i < sizeof(record.reserved); ++i) {
    if (record.reserved[i] != 0) {
      return false;
    }
  }
  for (std::size_t i = record.class_size; i < kMaxQuotaClassSize; ++i) {
    if (record.quota_class[i] != 0) {
      return false;
    }
  }
  return record.crc32 == RecordCrc(record);
}

int OpenFlags(int flags) {
  flags |= O_CLOEXEC;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  return flags;
}

bool OwnedRegularFile(int fd, int mode) {
  struct stat st {};
  return ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
         st.st_uid == ::geteuid() && ::fchmod(fd, mode) == 0;
}

bool WriteAll(int fd, const void* data, std::size_t size) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  std::size_t written = 0;
  while (written < size) {
    const ssize_t result = ::write(fd, bytes + written, size - written);
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

bool Compact(const std::string& path, const std::vector<QuotaRecord>& records) {
  const std::size_t slash = path.find_last_of('/');
  const std::string directory =
      slash == std::string::npos ? "." : path.substr(0, slash);
  const std::string basename =
      slash == std::string::npos ? path : path.substr(slash + 1);
  std::string temporary = directory.empty() ? "/" : directory;
  if (temporary.back() != '/') {
    temporary.push_back('/');
  }
  temporary += "." + basename + ".tmp.XXXXXX";

  const int fd = ::mkstemp(temporary.data());
  if (fd < 0) {
    return false;
  }
  bool ok = ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
  for (const QuotaRecord& record : records) {
    if (!ok) {
      break;
    }
    ok = WriteAll(fd, &record, sizeof(record));
  }
  if (ok) {
    ok = ::fdatasync(fd) == 0;
  }
  if (::close(fd) != 0) {
    ok = false;
  }
  if (!ok) {
    ::unlink(temporary.c_str());
    return false;
  }
  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return false;
  }
  return true;
}

}  // namespace

#endif

QuotaStateStore::~QuotaStateStore() { Close(); }

bool QuotaStateStore::Open(
    const std::string& path, uint64_t now_realtime_ns, uint64_t now_mono_ns,
    std::map<std::string, std::deque<uint64_t>>* windows) {
  Close();
  if (path.empty()) {
    return true;
  }
#if defined(_WIN32)
  (void)now_realtime_ns;
  (void)now_mono_ns;
  (void)windows;
  return false;
#else
  if (windows == nullptr) {
    return false;
  }

  const std::string lock_path = path + ".lock";
  lock_fd_ = ::open(lock_path.c_str(), OpenFlags(O_RDWR | O_CREAT), 0600);
  if (lock_fd_ < 0 || !OwnedRegularFile(lock_fd_, 0600) ||
      ::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    Close();
    return false;
  }

  const int state_fd = ::open(path.c_str(), OpenFlags(O_RDONLY));
  if (state_fd < 0 && errno != ENOENT) {
    Close();
    return false;
  }

  std::vector<QuotaRecord> records;
  bool ok = true;
  if (state_fd >= 0) {
    ok = OwnedRegularFile(state_fd, 0600);
    bool eof = false;
    while (ok && !eof) {
      QuotaRecord record{};
      std::size_t read_size = 0;
      while (read_size < sizeof(record)) {
        const ssize_t result =
            ::read(state_fd, reinterpret_cast<uint8_t*>(&record) + read_size,
                   sizeof(record) - read_size);
        if (result < 0) {
          if (errno == EINTR) {
            continue;
          }
          ok = false;
          break;
        }
        if (result == 0) {
          ok = read_size == 0;
          eof = true;
          break;
        }
        read_size += static_cast<std::size_t>(result);
      }
      if (!ok || eof) {
        continue;
      }
      if (!ValidRecord(record)) {
        ok = false;
        break;
      }
      if (!IsExpired(record.realtime_ns, now_realtime_ns)) {
        records.push_back(record);
      }
    }
  }
  if (state_fd >= 0 && ::close(state_fd) != 0) {
    ok = false;
  }
  if (!ok) {
    Close();
    return false;
  }

  std::map<std::string, std::vector<uint64_t>> sorted_records;
  for (const QuotaRecord& record : records) {
    sorted_records[std::string(record.quota_class, record.class_size)]
        .push_back(record.realtime_ns);
  }
  std::map<std::string, std::deque<uint64_t>> recovered;
  for (auto& item : sorted_records) {
    std::stable_sort(item.second.begin(), item.second.end());
    std::deque<uint64_t>& window = recovered[item.first];
    for (uint64_t realtime_ns : item.second) {
      if (realtime_ns > now_realtime_ns) {
        window.push_back(now_mono_ns);
        continue;
      }
      const uint64_t age = now_realtime_ns - realtime_ns;
      window.push_back(age > now_mono_ns ? 0 : now_mono_ns - age);
    }
  }

  if (!Compact(path, records)) {
    Close();
    return false;
  }
  fd_ = ::open(path.c_str(), OpenFlags(O_WRONLY | O_APPEND));
  if (fd_ < 0 || !OwnedRegularFile(fd_, 0600)) {
    Close();
    return false;
  }
  *windows = std::move(recovered);
  enabled_ = true;
  return true;
#endif
}

bool QuotaStateStore::Append(const std::string& quota_class,
                             uint64_t realtime_ns) {
#if defined(_WIN32)
  (void)quota_class;
  (void)realtime_ns;
  return false;
#else
  if (!enabled_ || fd_ < 0 || quota_class.empty() ||
      quota_class.size() > kMaxQuotaClassSize || realtime_ns == 0) {
    return false;
  }
  const QuotaRecord record = MakeRecord(quota_class, realtime_ns);
  return WriteAll(fd_, &record, sizeof(record));
#endif
}

void QuotaStateStore::Close() {
  enabled_ = false;
  if (fd_ >= 0) {
#if !defined(_WIN32)
    ::close(fd_);
#endif
    fd_ = -1;
  }
  if (lock_fd_ >= 0) {
#if !defined(_WIN32)
    ::close(lock_fd_);
#endif
    lock_fd_ = -1;
  }
}

}  // namespace trigger_center
}  // namespace thyme
