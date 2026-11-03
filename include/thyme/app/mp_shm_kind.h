#pragma once
// ARCH-11: per-kind SHM descriptor shared by the supervisor and role sides.
//
// The generated graph IR fills this from LatestSlotLayoutModel<T> (region
// bytes, payload bytes, cell stride, combined layout hash) and supplies the
// typed create/open callbacks, so neither side re-derives the page formula or
// constructs a bare LatestSlot<T> body.

#include "thyme/runtime/shm_region.h"

#include <cstddef>
#include <cstdint>

namespace thyme {
namespace app {

struct MpShmKindSpec {
  std::size_t region_bytes = 0;        // LatestSlotLayoutModel<T>::kRegionBytes
  std::size_t payload_bytes = 0;       // sizeof(T)
  std::size_t cell_stride = 0;         // RoundUp64(sizeof(T))
  uint64_t payload_layout_hash = 0;    // stable contract identity, never 0
  uint64_t feature_bits = 0;
  // Creates the described page (supervisor side). Returns false on any
  // validation failure; `error` carries the reason when non-null.
  bool (*create_page)(thyme::ShmRegion& region, const char* name,
                      uint64_t feature_bits,
                      thyme::ShmPageError* error) = nullptr;
  // Attaches read-write to an existing described page (role side).
  bool (*open_page)(thyme::ShmRegion& region, const char* name,
                    uint64_t timeout_ns, uint64_t feature_bits,
                    thyme::ShmPageError* error) = nullptr;
};

}  // namespace app
}  // namespace thyme
