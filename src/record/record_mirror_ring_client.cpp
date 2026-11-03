#include "thyme/record/record_mirror_ring_client.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <new>
#include <cstring>
#include <unistd.h>

namespace thyme {
namespace record_mirror {

std::string DefaultRingShmName() {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "/thyme_recr_%d", static_cast<int>(getpid()));
  return buf;
}

bool RingClient::Create(const char* shm_name, const ThymeStructDesc* descs,
                        const RecordMirrorRingSpec* specs, uint32_t count,
                        uint32_t session_id) {
  if (shm_name == nullptr || shm_name[0] == '\0' || descs == nullptr ||
      specs == nullptr) {
    return false;
  }
  const uint64_t bytes =
      RecordMirrorRingPage::RequiredBytes(descs, specs, count);
  if (bytes == 0) return false;
  std::snprintf(shm_name_, sizeof(shm_name_), "%s", shm_name);
  // OPS-02: ShmRegion::Create proves staleness before reclaiming. Never unlink
  // a live ring from a second producer; that would detach its readers.
  if (!region_.Create(shm_name_, bytes)) {
    std::fprintf(stderr,
                 "RecordMirrorRingPage: Create(%s, %llu) failed: %s\n",
                 shm_name_, static_cast<unsigned long long>(bytes),
                 std::strerror(errno));
    return false;
  }
  page_ = new (region_.data()) RecordMirrorRingPage;
  if (!page_->Init(session_id, descs, specs, count)) {
    page_ = nullptr;
    region_.Unmap();
    ShmRegion::UnlinkName(shm_name_);
    return false;
  }
  return true;
}

bool RingClient::CreateRing(const char* shm_name, const ThymeRecordTable& table,
                            const RecordMirrorRingSpec* specs,
                            uint32_t session_id) {
  if (shm_name == nullptr || shm_name[0] == '\0' ||
      !ThymeRecordTableValid(table) || specs == nullptr) {
    return false;
  }
  ThymeStructDesc descs[kRecordMirrorRingMaxStructs];
  table.fill_struct_descs(descs, kRecordMirrorRingMaxStructs);
  return Create(shm_name, descs, specs, static_cast<uint32_t>(table.count),
                session_id);
}

bool RingClient::Attach(const char* shm_name, uint64_t timeout_ns,
                        bool read_only, uint64_t expected_config_hash) {
  std::snprintf(shm_name_, sizeof(shm_name_), "%s", shm_name);
  region_.Unmap();
  page_ = nullptr;
  // Phase 1: described open of the fixed page part. The super-header's
  // page_bytes covers the tables only; the arena is appended at creation.
  ShmPageError error;
  if (!region_.OpenDescribed(shm_name_, RecordMirrorRingPage::Spec(),
                             timeout_ns, true, &error)) {
    return false;
  }
  auto* probe = static_cast<const RecordMirrorRingPage*>(region_.data());
  if (!probe->ValidateConfig(expected_config_hash)) {
    std::fprintf(stderr, "RecordMirrorRingPage: %s config/super-header mismatch\n",
                 shm_name_);
    region_.Unmap();
    return false;
  }
  const uint64_t arena = probe->arena_bytes();
  if (arena > UINT64_MAX - sizeof(RecordMirrorRingPage)) {
    region_.Unmap();
    return false;
  }
  const uint64_t total = sizeof(RecordMirrorRingPage) + arena;
  region_.Unmap();
  // Full map at the real region size.
  if (!region_.OpenRetry(shm_name_, total, timeout_ns, read_only)) {
    return false;
  }
  page_ = static_cast<RecordMirrorRingPage*>(region_.data());
  if (!page_->ValidateConfig(expected_config_hash)) {
    region_.Unmap();
    page_ = nullptr;
    return false;
  }
  return true;
}

bool RingClient::AttachReadOnly(const char* shm_name, uint64_t timeout_ns,
                                uint64_t expected_config_hash) {
  if (shm_name == nullptr || shm_name[0] == '\0') return false;
  return Attach(shm_name, timeout_ns, true, expected_config_hash);
}

bool RingClient::AttachReadWrite(const char* shm_name, uint64_t timeout_ns,
                                 uint64_t expected_config_hash) {
  if (shm_name == nullptr || shm_name[0] == '\0') return false;
  return Attach(shm_name, timeout_ns, false, expected_config_hash);
}

void RingClient::Shutdown(bool unlink_name) {
  page_ = nullptr;
  region_.Unmap();
  if (unlink_name && shm_name_[0] != '\0') {
    ShmRegion::UnlinkName(shm_name_);
    shm_name_[0] = '\0';
  }
}

}  // namespace record_mirror
}  // namespace thyme
