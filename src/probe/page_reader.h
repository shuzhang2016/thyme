#pragma once
// Internal read-only page reader for thyme_probe. Not installed; only
// thyme_probe_core and its test use it.

#include "thyme/contracts/frame_header.h"
#include "thyme/probe/probe.h"

#include <cstdint>
#include <string>
#include <vector>

namespace thyme {
namespace probe {
namespace detail {

// PROT_READ MAP_SHARED view of a POSIX shm object. Opening never modifies the
// object, its size, or its permissions.
class MappedPage {
 public:
  MappedPage() = default;
  ~MappedPage();
  MappedPage(const MappedPage&) = delete;
  MappedPage& operator=(const MappedPage&) = delete;

  Status Open(const std::string& shm_name);
  void Close();

  const uint8_t* base() const { return base_; }
  uint64_t size() const { return size_; }
  bool valid() const { return base_ != nullptr; }

 private:
  uint8_t* base_{nullptr};
  uint64_t size_{0};
  int fd_{-1};
};

// Geometry + header counters read from one page. Fields the page kind does not
// provide stay at kNone / 0.
struct Geometry {
  Kind kind = Kind::kUnknown;
  uint32_t abi_version = 0;
  bool compatible = true;
  uint32_t slots = 0;
  uint32_t slot_bytes = 0;
  uint32_t stride = 0;
  uint64_t region_bytes = 0;
  uint64_t head = kNone;
  uint64_t tail = kNone;
  uint64_t epoch = kNone;
  uint64_t drop = kNone;
  std::vector<uint64_t> reader_drop_slots;
  uint64_t skip = kNone;
  uint64_t steal = kNone;
  uint32_t session = 0;
  bool has_session = false;
  uint32_t workers = 0;
  std::vector<StructEntry> structs;
  bool frame_shaped = false;
  std::string source;  // "page" / "manifest"
  std::string note;
  uint64_t slots_offset = 0;  // page offset of slot 0
};

// Classify the page (magic/version) and fill geometry. `entry` may be null.
Status OpenGeometry(const std::string& name, const ManifestEntry* entry,
                    MappedPage* page, Geometry* geometry);

// Metadata-only FrameHeader recognition shared by the stamp and echo paths.
// The probe has no product magic/version constants, so self-comparison cannot
// reject a header from the wrong product; it only applies the structural rules.
// valid == 0 remains displayable metadata. Neither this check nor its result
// validates the CRC or permits payload consumption at a data boundary.
inline bool FrameHeaderMetadataOk(const FrameHeader& h, uint32_t cap) {
  return FrameHeaderStructurallyOk(h, h.magic, h.version) && h.size <= cap;
}

// One observation of the newest published sample. `payload_offset` /
// `payload_len` point into MappedPage::base().
struct RawSample {
  bool has = false;
  bool torn = false;
  uint64_t sequence = 0;
  uint32_t size = 0;
  bool has_stamp = false;
  uint64_t producer_ns = 0;
  uint64_t payload_offset = 0;
  uint32_t payload_len = 0;
};

// Read the newest sample without pinning, consuming, or writing.
bool ReadNewest(const MappedPage& page, const Geometry& geometry,
                RawSample* out);

// Copy the newest payload into dst, re-validating the slot afterwards.
bool ReadStablePayload(const MappedPage& page, const Geometry& geometry,
                       uint32_t cap, uint8_t* dst, RawSample* out,
                       uint32_t* copied);

// Read the payload size of an absolute PinnedSlot index without pinning it.
// Used by `bw --exact` to sum real lengths when the window still fits in the
// ring. Returns false when the frame is evicted or the slot is torn.
bool ReadFrameSize(const MappedPage& page, const Geometry& geometry,
                   uint64_t frame_index, uint32_t* size);

bool StreamKind(Kind kind);

}  // namespace detail
}  // namespace probe
}  // namespace thyme
