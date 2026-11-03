#pragma once

#include "generated/graph_ir.h"
#include "thyme/app/record_mirror.h"
#ifdef THYME_GRAPH_HAS_RECORD
#include "generated/record_table.h"
#endif

#include <cstdint>
#include <string>

// Dummy-app-local accessor for the generated app's explicit ring-page v4 ABI.
// The legacy RecordMirrorPage v3 client remains available to non-generated
// embedders; the two page types never share a client or an attach operation.
namespace record_mirror {

inline thyme::record_mirror::RingClient& Instance() {
  return thyme::app::record_mirror::RingInstance();
}

inline std::string DefaultShmName() {
  return thyme::record_mirror::DefaultRingShmName();
}

#ifdef THYME_GRAPH_HAS_RECORD
inline bool Create(const char* shm_name, uint32_t session_id = 1) {
  return Instance().CreateRing(shm_name, GetThymeRecordTable(),
                               kRecordMirrorRingSpecs, session_id);
}
#endif

inline bool AttachReadOnly(const char* shm_name,
                           uint64_t timeout_ns = 2000000000ULL) {
#ifdef THYME_GRAPH_HAS_RECORD
  return Instance().AttachReadOnly(shm_name, timeout_ns,
                                   ThymeRecordMirrorRingConfigHash());
#else
  return Instance().AttachReadOnly(shm_name, timeout_ns);
#endif
}

inline bool AttachReadWrite(const char* shm_name,
                            uint64_t timeout_ns = 2000000000ULL) {
#ifdef THYME_GRAPH_HAS_RECORD
  return Instance().AttachReadWrite(shm_name, timeout_ns,
                                    ThymeRecordMirrorRingConfigHash());
#else
  return Instance().AttachReadWrite(shm_name, timeout_ns);
#endif
}

inline thyme::RecordMirrorRingPage* Page() { return Instance().Page(); }

inline void Shutdown(bool unlink_name = false) {
  Instance().Shutdown(unlink_name);
}

}  // namespace record_mirror
