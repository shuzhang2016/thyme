#pragma once

#include "thyme/record/record_table.h"
#include "thyme/runtime/record_mirror.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <string>

namespace thyme {
namespace record_mirror {

// Default per-process mirror shm name ("/thyme_rec_<pid>").
std::string DefaultShmName();

// Owns one mirror SHM region. Instantiable — no global state; embedders keep
// an instance instead of sharing process-wide globals.
class Client {
 public:
  Client() = default;
  ~Client() = default;  // ShmRegion unmaps; Shutdown controls unlinking
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  RecordMirrorPage* Page() { return page_; }

  bool Create(const char* shm_name, const ThymeRecordTable& table,
              uint32_t session_id = 1);
  bool AttachReadOnly(const char* shm_name,
                      uint64_t timeout_ns = 2000000000ULL);
  bool AttachReadWrite(const char* shm_name,
                       uint64_t timeout_ns = 2000000000ULL);
  void Shutdown(bool unlink_name = false);

 private:
  ShmRegion region_;
  RecordMirrorPage* page_ = nullptr;
  char shm_name_[128] = {};
};

}  // namespace record_mirror
}  // namespace thyme
