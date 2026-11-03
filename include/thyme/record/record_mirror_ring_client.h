#pragma once

#include "thyme/record/record_table.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <string>

namespace thyme {
namespace record_mirror {

// Default per-process ring mirror shm name ("/thyme_recr_<pid>"), distinct
// from the latest-value mirror's "/thyme_rec_<pid>".
std::string DefaultRingShmName();

// Owns one ring-mirror SHM region. Instantiable — no global state; embedders
// keep an instance instead of sharing process-wide globals. Coexists with
// record_mirror::Client (different page ABI, different shm name).
class RingClient {
 public:
  RingClient() = default;
  ~RingClient() = default;  // ShmRegion unmaps; Shutdown controls unlinking
  RingClient(const RingClient&) = delete;
  RingClient& operator=(const RingClient&) = delete;

  RecordMirrorRingPage* Page() { return page_; }

  // Low-level create: explicit desc/spec arrays (specs[i] pairs descs[i]).
  bool Create(const char* shm_name, const ThymeStructDesc* descs,
              const RecordMirrorRingSpec* specs, uint32_t count,
              uint32_t session_id = 1);
  // Table-driven create: descs come from table.fill_struct_descs, ring specs
  // (one per struct, same indexing) come from the trigger configuration.
  bool CreateRing(const char* shm_name, const ThymeRecordTable& table,
                  const RecordMirrorRingSpec* specs, uint32_t session_id = 1);
  // expected_config_hash != 0 rejects pages built from a different config.
  bool AttachReadOnly(const char* shm_name,
                      uint64_t timeout_ns = 2000000000ULL,
                      uint64_t expected_config_hash = 0);
  bool AttachReadWrite(const char* shm_name,
                       uint64_t timeout_ns = 2000000000ULL,
                       uint64_t expected_config_hash = 0);
  void Shutdown(bool unlink_name = false);

 private:
  bool Attach(const char* shm_name, uint64_t timeout_ns, bool read_only,
              uint64_t expected_config_hash);

  ShmRegion region_;
  RecordMirrorRingPage* page_ = nullptr;
  char shm_name_[128] = {};
};

}  // namespace record_mirror
}  // namespace thyme
