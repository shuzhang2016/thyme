#pragma once
#include "thyme/record/record_mirror_client.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/record_table.h"

#include <cstdint>

namespace thyme {
namespace app {
namespace record_mirror {

// Process-wide record mirror accessors for the mp runtime and generated node
// code. A session selects either the legacy Page() client or the RingPage()
// client; the supervisor creates it (or app main in single-process mode), and
// each role attaches the same page type read-write.
inline thyme::record_mirror::Client& Instance() {
  static thyme::record_mirror::Client client;
  return client;
}

inline bool Create(const char* shm_name, const thyme::ThymeRecordTable& table,
                   uint32_t session_id = 1) {
  return Instance().Create(shm_name, table, session_id);
}

inline bool AttachReadOnly(const char* shm_name,
                           uint64_t timeout_ns = 2000000000ULL) {
  return Instance().AttachReadOnly(shm_name, timeout_ns);
}

inline bool AttachReadWrite(const char* shm_name,
                            uint64_t timeout_ns = 2000000000ULL) {
  return Instance().AttachReadWrite(shm_name, timeout_ns);
}

inline thyme::RecordMirrorPage* Page() { return Instance().Page(); }

inline void Shutdown(bool unlink_name = false) {
  Instance().Shutdown(unlink_name);
}

// Ring pages are a separate v4 ABI, opt-in through CreateRing/AttachRing*.
// Page() and RingPage() are never aliases for the same SHM object.
inline thyme::record_mirror::RingClient& RingInstance() {
  static thyme::record_mirror::RingClient client;
  return client;
}

inline bool CreateRing(const char* shm_name, const thyme::ThymeRecordTable& table,
                       const thyme::RecordMirrorRingSpec* specs,
                       uint32_t session_id = 1) {
  return RingInstance().CreateRing(shm_name, table, specs, session_id);
}

inline bool AttachRingReadOnly(const char* shm_name,
                               uint64_t timeout_ns = 2000000000ULL,
                               uint64_t expected_config_hash = 0) {
  return RingInstance().AttachReadOnly(shm_name, timeout_ns, expected_config_hash);
}

inline bool AttachRingReadWrite(const char* shm_name,
                                uint64_t timeout_ns = 2000000000ULL,
                                uint64_t expected_config_hash = 0) {
  return RingInstance().AttachReadWrite(shm_name, timeout_ns, expected_config_hash);
}

inline thyme::RecordMirrorRingPage* RingPage() { return RingInstance().Page(); }

inline void ShutdownRing(bool unlink_name = false) {
  RingInstance().Shutdown(unlink_name);
}

}  // namespace record_mirror
}  // namespace app
}  // namespace thyme
