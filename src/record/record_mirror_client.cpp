#include "thyme/record/record_mirror_client.h"

#include <cerrno>
#include <cstdio>
#include <new>
#include <cstring>
#include <unistd.h>

namespace thyme {
namespace record_mirror {

std::string DefaultShmName() {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "/thyme_rec_%d", static_cast<int>(getpid()));
  return buf;
}

bool Client::Create(const char* shm_name, const ThymeRecordTable& table,
                    uint32_t session_id) {
  if (shm_name == nullptr || shm_name[0] == '\0' ||
      !ThymeRecordTableValid(table)) {
    return false;
  }
  std::snprintf(shm_name_, sizeof(shm_name_), "%s", shm_name);
  // OPS-02: only ShmRegion::Create may reclaim a stale segment; a live page
  // must make this Create fail rather than detaching its readers.
  ThymeStructDesc descs[kRecordMirrorMaxSlots];
  table.fill_struct_descs(descs, kRecordMirrorMaxSlots);

  if (!region_.Create(shm_name_, sizeof(RecordMirrorPage))) {
    std::fprintf(stderr, "RecordMirrorPage: Create(%s, %zu) failed: %s\n",
                 shm_name_, sizeof(RecordMirrorPage), std::strerror(errno));
    return false;
  }
  page_ = new (region_.data()) RecordMirrorPage;
  if (!page_->Init(session_id, descs, static_cast<uint32_t>(table.count))) {
    page_ = nullptr;
    region_.Unmap();
    ShmRegion::UnlinkName(shm_name_);
    return false;
  }
  return true;
}

bool Client::AttachReadOnly(const char* shm_name, uint64_t timeout_ns) {
  if (shm_name == nullptr || shm_name[0] == '\0') return false;
  std::snprintf(shm_name_, sizeof(shm_name_), "%s", shm_name);
  region_.Unmap();
  page_ = nullptr;
  ShmPageError error;
  if (!region_.OpenDescribed(shm_name_, RecordMirrorPage::Spec(), timeout_ns,
                             true, &error)) {
    return false;
  }
  page_ = static_cast<RecordMirrorPage*>(region_.data());
  if (!page_->Validate(&error)) {
    region_.Unmap();
    page_ = nullptr;
    return false;
  }
  return true;
}

bool Client::AttachReadWrite(const char* shm_name, uint64_t timeout_ns) {
  if (shm_name == nullptr || shm_name[0] == '\0') return false;
  std::snprintf(shm_name_, sizeof(shm_name_), "%s", shm_name);
  region_.Unmap();
  page_ = nullptr;
  ShmPageError error;
  if (!region_.OpenDescribed(shm_name_, RecordMirrorPage::Spec(), timeout_ns,
                             false, &error)) {
    return false;
  }
  page_ = static_cast<RecordMirrorPage*>(region_.data());
  if (!page_->Validate(&error)) {
    region_.Unmap();
    page_ = nullptr;
    return false;
  }
  return true;
}

void Client::Shutdown(bool unlink_name) {
  page_ = nullptr;
  region_.Unmap();
  if (unlink_name && shm_name_[0] != '\0') {
    ShmRegion::UnlinkName(shm_name_);
    shm_name_[0] = '\0';
  }
}

}  // namespace record_mirror
}  // namespace thyme
