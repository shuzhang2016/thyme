#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>

namespace thyme {
namespace trigger_center {

class QuotaStateStore {
 public:
  QuotaStateStore() = default;
  ~QuotaStateStore();
  QuotaStateStore(const QuotaStateStore&) = delete;
  QuotaStateStore& operator=(const QuotaStateStore&) = delete;

  bool Open(const std::string& path, uint64_t now_realtime_ns,
            uint64_t now_mono_ns,
            std::map<std::string, std::deque<uint64_t>>* windows);
  bool Append(const std::string& quota_class, uint64_t realtime_ns);
  void Close();
  bool enabled() const { return enabled_; }

 private:
  int fd_ = -1;
  int lock_fd_ = -1;
  bool enabled_ = false;
};

}  // namespace trigger_center
}  // namespace thyme
