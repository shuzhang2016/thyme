#pragma once
// ARCH-11: described Create/Open for the v3 LatestSlot<T> SHM page.
//
// Split from latest_slot.h so the in-process container (and every TU that only
// needs the seqlock) does not pull in ShmRegion. Everything here is setup-path
// only: no allocation, and the hot path remains LatestSlot<T>::BeginWrite/
// CommitWrite on the mapped body.

#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/shm_region.h"

#include <cstddef>
#include <cstdint>
#include <new>

namespace thyme {
namespace detail {

inline bool LatestSlotPageError(ShmPageErrc code, uint64_t expected,
                                uint64_t actual, ShmPageError* error) {
  if (error != nullptr) *error = ShmPageError{code, expected, actual};
  return false;
}

// Shared Open body: build the exact spec, let the described region API map and
// pre-validate the header, then re-validate the complete page. Offsets are
// re-derived from the model after the mapping exists; nothing from a previous
// mapping (or a legacy v2 page) is trusted.
template <typename T>
void* OpenLatestSlotPageImpl(ShmRegion& region, const char* name,
                             uint64_t payload_layout_hash, uint64_t timeout_ns,
                             uint64_t feature_bits, bool read_only,
                             ShmPageError* error) {
  using Model = LatestSlotLayoutModel<T>;
  if (error != nullptr) *error = ShmPageError{};
  const LatestSlotPageSpec spec =
      Model::PageSpec(payload_layout_hash, feature_bits);
  if (name == nullptr || !Model::IsSpecValid(spec)) {
    LatestSlotPageError(ShmPageErrc::kInvalidSpec, 0, 0, error);
    return nullptr;
  }
  if (!region.OpenDescribed(name, spec.shm, timeout_ns, read_only, error)) {
    return nullptr;
  }
  if (ValidateLatestSlotPage<T>(region.data(), region.size(), spec, error) !=
      ShmPageErrc::kOk) {
    region.Unmap();
    return nullptr;
  }
  return region.data();
}

}  // namespace detail

// Creates a v3 LatestSlot<T> page: a described ShmSuperHeader followed by an
// in-place LatestSlot<T> body. payload_layout_hash == 0 is rejected
// (kInvalidSpec), because a zero identity would accept any same-size layout.
//
// The returned page is owned by the process-local object lifetime of the
// region; destroy nothing (LatestSlot<T> is trivially destructible) and call
// ShmRegion::Unmap()/UnlinkName when tearing the segment down.
template <typename T>
LatestSlotPage<T>* CreateLatestSlotPage(ShmRegion& region, const char* name,
                                        uint64_t payload_layout_hash,
                                        uint64_t feature_bits = 0,
                                        ShmPageError* error = nullptr) {
  using Model = LatestSlotLayoutModel<T>;
  if (error != nullptr) *error = ShmPageError{};
  const LatestSlotPageSpec spec =
      Model::PageSpec(payload_layout_hash, feature_bits);
  if (name == nullptr || !Model::IsSpecValid(spec)) {
    detail::LatestSlotPageError(ShmPageErrc::kInvalidSpec, 0, 0, error);
    return nullptr;
  }
  if (!region.Create(name, spec.shm.page_bytes)) {
    detail::LatestSlotPageError(ShmPageErrc::kNotReady, spec.shm.magic, 0,
                                error);
    return nullptr;
  }
  // The raw region has no published header yet. Construct and zero the whole
  // page first, then write the geometry block and publish magic last, so a
  // concurrent attacher can never observe a valid header while the body or
  // geometry is still being initialized.
  auto* page = new (region.data()) LatestSlotPage<T>();
  InitLatestSlotPageGeometry(&page->geometry, Model::kPayloadBytes,
                             Model::kCellStride, Model::kSlotCount);
  InitShmSuperHeader(&page->header, spec.shm);
  if (ValidateLatestSlotPage<T>(region.data(), region.size(), spec, error) !=
      ShmPageErrc::kOk) {
    region.Unmap();
    ShmRegion::UnlinkName(name);
    return nullptr;
  }
  return page;
}

// Attaches read-write to an existing v3 page (producer side). Validates the
// super-header, the backing/mapped size and the combined layout hash,
// including the caller's payload identity.
template <typename T>
LatestSlotPage<T>* OpenLatestSlotPage(ShmRegion& region, const char* name,
                                     uint64_t payload_layout_hash,
                                     uint64_t timeout_ns,
                                     uint64_t feature_bits = 0,
                                     ShmPageError* error = nullptr) {
  return static_cast<LatestSlotPage<T>*>(
      detail::OpenLatestSlotPageImpl<T>(region, name, payload_layout_hash,
                                        timeout_ns, feature_bits, false,
                                        error));
}

// Attaches read-only to an existing v3 page (consumer side). Same validation
// as OpenLatestSlotPage; the returned pointer must not be written through.
template <typename T>
const LatestSlotPage<T>* OpenLatestSlotPageReadOnly(
    ShmRegion& region, const char* name, uint64_t payload_layout_hash,
    uint64_t timeout_ns, uint64_t feature_bits = 0,
    ShmPageError* error = nullptr) {
  return static_cast<const LatestSlotPage<T>*>(
      detail::OpenLatestSlotPageImpl<T>(region, name, payload_layout_hash,
                                        timeout_ns, feature_bits, true,
                                        error));
}

}  // namespace thyme
